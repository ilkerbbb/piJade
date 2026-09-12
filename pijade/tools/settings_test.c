/*
 * Checks the default-namespace allowlist and all four registration namespaces, including their
 * framing, key and value bounds, count caps, commit path, and host file-size cap. It also checks
 * that failed persistence is reported rather than swallowed.
 *
 * The list is still what bounds this: the store holds more than the file may carry, and which
 * fields reach the card is a property of a table, which is exactly the kind of thing that quietly
 * stops being true. So it is asserted here rather than argued in a comment.
 *
 * Build and run: see pijade/UPSTREAM.md.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wally_core.h> // for WALLY_CORE_API, which wally_map.h uses but does not include
#include <wally_crypto.h>
#include <wally_map.h>

#include "libjade.h"
#include "pijade_settings.h"

// The unit-test command in this file's spec links only libjade. Include the host store directly so
// the same settings_store_read()/settings_store_write() implementation and size cap are exercised.
#include "../host/settings_store.c"

#define TEST_MAGIC_LEN 8
#define TEST_DIGEST_LEN 4
#define TEST_HEADER_LEN (TEST_MAGIC_LEN + TEST_DIGEST_LEN)

// nvs_commit()/nvs_flash_erase() are declared in libjade/include/nvs_flash.h, which is outside the
// -I path this test is built with (see pijade/UPSTREAM.md); esp_err_t is `int` (ESP_OK=0, ESP_FAIL=1)
// and nvs_handle_t is `struct wally_map*` (both from libjade/include), so the two entry points under
// test are declared directly here rather than pulling in a header meant for libjade's own build.
#include <nvs_flash.h> // for the nvs_commit()/nvs_flash_erase() chain below

static int failures = 0;

static void check(const bool ok, const char* const what)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++failures;
    }
}

// A stand-in for a host whose store has gone read-only: settings writes and erases in this state
// return false, and nvs_commit()/nvs_flash_erase() must pass that up rather than report success.
static bool refusing_settings_handler(const uint8_t* const data, const size_t len, void* const ctx)
{
    (void)data;
    (void)len;
    (void)ctx;
    return false;
}

static bool accepting_settings_handler(const uint8_t* const data, const size_t len, void* const ctx)
{
    (void)data;
    (void)len;
    (void)ctx;
    return true;
}

static bool saw_erase_request = false;

static bool erase_settings_handler(const uint8_t* const data, const size_t len, void* const ctx)
{
    (void)ctx;
    saw_erase_request = data == NULL && len == 0;
    return saw_erase_request;
}

static void put(struct wally_map* const m, const char* const key, const void* const value, const size_t len)
{
    if (wally_map_replace(m, (const unsigned char*)key, strlen(key), value, len) != WALLY_OK) {
        printf("cannot seed the store with %s\n", key);
        exit(EXIT_FAILURE);
    }
}

static void put_key(struct wally_map* const m, const uint8_t* const key, const size_t key_len,
    const void* const value, const size_t value_len)
{
    if (wally_map_replace(m, key, key_len, value, value_len) != WALLY_OK) {
        printf("cannot seed a namespace map\n");
        exit(EXIT_FAILURE);
    }
}

static void clear_store(void)
{
    for (size_t ns = 0; ns < 5; ++ns) {
        struct wally_map* const storage = pijade_settings_storage(ns);
        if (!storage) {
            printf("cannot access namespace %zu\n", ns);
            exit(EXIT_FAILURE);
        }
        wally_map_clear(storage);
    }
}

static bool contains(const uint8_t* const haystack, const size_t len, const char* const needle)
{
    const size_t needle_len = strlen(needle);
    for (size_t i = 0; needle_len <= len && i <= len - needle_len; ++i) {
        if (!memcmp(haystack + i, needle, needle_len)) {
            return true;
        }
    }
    return false;
}

static bool refresh_digest(uint8_t* const bytes, const size_t len)
{
    uint8_t digest[SHA256_LEN];
    const bool ok = len >= TEST_HEADER_LEN
        && wally_sha256(bytes + TEST_HEADER_LEN, len - TEST_HEADER_LEN, digest, sizeof(digest)) == WALLY_OK;
    if (ok) {
        memcpy(bytes + TEST_MAGIC_LEN, digest, TEST_DIGEST_LEN);
    }
    wally_bzero(digest, sizeof(digest));
    return ok;
}

static void wipe_free(uint8_t* data, size_t len);

struct raw_entry {
    uint8_t ns;
    const uint8_t* key;
    size_t key_len;
    const uint8_t* value;
    size_t value_len;
};

static uint8_t* make_file(const struct raw_entry* const entries, const size_t num_entries, size_t* const len)
{
    size_t body_len = 0;
    for (size_t i = 0; i < num_entries; ++i) {
        body_len += 1 + 1 + entries[i].key_len + 2 + entries[i].value_len;
    }
    *len = TEST_HEADER_LEN + body_len;
    uint8_t* const bytes = malloc(*len);
    if (!bytes) {
        return NULL;
    }
    memcpy(bytes, "PIJADES4", TEST_MAGIC_LEN); // must track SETTINGS_MAGIC (libjade/pijade_settings.c)
    uint8_t* p = bytes + TEST_HEADER_LEN;
    for (size_t i = 0; i < num_entries; ++i) {
        *p++ = entries[i].ns;
        *p++ = (uint8_t)entries[i].key_len;
        memcpy(p, entries[i].key, entries[i].key_len);
        p += entries[i].key_len;
        *p++ = (uint8_t)entries[i].value_len;
        *p++ = (uint8_t)(entries[i].value_len >> 8);
        memcpy(p, entries[i].value, entries[i].value_len);
        p += entries[i].value_len;
    }
    if ((size_t)(p - bytes) != *len || !refresh_digest(bytes, *len)) {
        wipe_free(bytes, *len);
        *len = 0;
        return NULL;
    }
    return bytes;
}

static bool entry_case(const uint8_t ns, const uint8_t* const key, const size_t key_len,
    const size_t value_len, const bool expected)
{
    uint8_t* const value = calloc(value_len ? value_len : 1, 1);
    if (!value) {
        return false;
    }
    const struct raw_entry entry = { ns, key, key_len, value, value_len };
    size_t file_len = 0;
    uint8_t* const file = make_file(&entry, 1, &file_len);

    clear_store();
    const bool loader_ok = file && pijade_settings_deserialize(pijade_settings_storage(0), file, file_len);
    clear_store();
    bool serializer_ok = false;
    if (ns < 5) {
        put_key(pijade_settings_storage(ns), key, key_len, value, value_len);
        uint8_t* serialized = NULL;
        size_t serialized_len = 0;
        serializer_ok = pijade_settings_serialize(pijade_settings_storage(0), &serialized, &serialized_len);
        wipe_free(serialized, serialized_len);
    }

    clear_store();
    wipe_free(file, file_len);
    wally_bzero(value, value_len);
    free(value);
    return loader_ok == expected && serializer_ok == expected;
}

struct captured_settings {
    size_t calls;
    uint8_t* data;
    size_t len;
};

static bool capturing_settings_handler(const uint8_t* const data, const size_t len, void* const ctx)
{
    struct captured_settings* const captured = ctx;
    wipe_free(captured->data, captured->len);
    captured->data = malloc(len);
    captured->len = captured->data ? len : 0;
    ++captured->calls;
    if (!captured->data) {
        return false;
    }
    memcpy(captured->data, data, len);
    return true;
}

static bool populate_largest_store(void)
{
    static const struct {
        const char* key;
        size_t len;
        bool nul_terminated;
    } fields[] = {
        { "guiflags", 1, false },
        { "idletimeout", 2, false },
        { "screentimeout", 2, false },
        { "brightness", 1, false },
        { "qrflags", 4, false },
        { "privatekey", 32, false },
        { "blob", 256, false },
        { "counter", 1, false },
        { "antireplay", 4, false },
        { "keyflags", 1, false },
        { "featflags", 1, false },
        { "walleterasepin", 48, false },
        { "networktype", 4, false },
        { "pinsvrurlA", 120, true },
        { "pinsvrurlB", 120, true },
        { "pinsvrpubkey", 33, false },
        { "pinsvrcert", 2048, true },
    };
    static const size_t maximums[] = { 3250, 3249, 288, 8 };

    clear_store();
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        uint8_t* const value = calloc(fields[i].len, 1);
        if (!value) {
            return false;
        }
        if (fields[i].nul_terminated && fields[i].len > 1) {
            memset(value, 'x', fields[i].len - 1);
        }
        put(pijade_settings_storage(0), fields[i].key, value, fields[i].len);
        wipe_free(value, fields[i].len);
    }
    for (size_t ns = 1; ns < 5; ++ns) {
        uint8_t* const value = calloc(maximums[ns - 1], 1);
        if (!value) {
            return false;
        }
        for (size_t i = 0; i < 16; ++i) {
            char key[16];
            snprintf(key, sizeof(key), "record%02zu", i);
            put(pijade_settings_storage(ns), key, value, maximums[ns - 1]);
        }
        wipe_free(value, maximums[ns - 1]);
    }
    return true;
}

static uint8_t* find_entry_key(uint8_t* const bytes, const size_t len, const char* const wanted)
{
    uint8_t* p = bytes + TEST_HEADER_LEN;
    uint8_t* const end = bytes + len;
    const size_t wanted_len = strlen(wanted);
    while (p < end) {
        ++p; // namespace byte
        if (p == end) {
            return NULL;
        }
        const size_t key_len = *p++;
        if (!key_len || (size_t)(end - p) < key_len + 2) {
            return NULL;
        }
        uint8_t* const key = p;
        p += key_len;
        const size_t value_len = (size_t)p[0] | ((size_t)p[1] << 8);
        p += 2;
        if ((size_t)(end - p) < value_len) {
            return NULL;
        }
        if (key_len == wanted_len && !memcmp(key, wanted, key_len)) {
            return key;
        }
        p += value_len;
    }
    return NULL;
}

static void wipe_free(uint8_t* const data, const size_t len)
{
    if (data) {
        wally_bzero(data, len);
        free(data);
    }
}

int main(void)
{
    struct wally_map* const store = pijade_settings_storage(0);
    wally_map_clear(store);

    uint8_t gui_flags[] = { 0x25 };
    uint8_t idle_timeout[] = { 0x58, 0x02 };
    uint8_t screen_timeout[] = { 0x1e, 0x00 };
    uint8_t brightness[] = { 7 };
    uint8_t qr_flags[] = { 0x44, 0x33, 0x22, 0x11 };
    uint8_t private_key[32];
    uint8_t wallet[256];
    uint8_t counter[] = { 3 };
    uint8_t antireplay[] = { 0x78, 0x56, 0x34, 0x12 };
    uint8_t key_flags[] = { 0xa5 };
    uint8_t feat_flags[] = { 0x5a };
    // Since the S4 format this field is WALLET_ERASE_PIN_RECORD_LEN bytes (main/storage.h): a
    // 16-byte salt followed by a 32-byte PBKDF2 verifier.  Both halves are opaque, so the fixture
    // only has to be the right width and to contain byte values a digit field would have refused.
    uint8_t wallet_erase_pin[48];
    for (size_t i = 0; i < sizeof(wallet_erase_pin); ++i) {
        wallet_erase_pin[i] = (uint8_t)(0x40 + i);
    }
    uint8_t network_type[] = { 2, 0, 0, 0 };
    uint8_t pinsvr_url_a[] = "https://pin.example/a";
    // The empty string is the "explicitly no second url" state (main/process/pinclient.c:104), and
    // it is the narrowest value any string field can take, so it is what the list is tested with.
    uint8_t pinsvr_url_b[] = "";
    uint8_t pinsvr_pubkey[33];
    uint8_t pinsvr_cert[] = "test pinserver certificate";
    uint8_t unlisted[] = { 0xde, 0xad, 0xbe, 0xef };

    for (size_t i = 0; i < sizeof(private_key); ++i) {
        private_key[i] = (uint8_t)(0x40 + i);
    }
    for (size_t i = 0; i < sizeof(wallet); ++i) {
        wallet[i] = (uint8_t)i;
    }
    for (size_t i = 0; i < sizeof(pinsvr_pubkey); ++i) {
        pinsvr_pubkey[i] = (uint8_t)(0x80 + i);
    }

    struct test_field {
        const char* key;
        uint8_t* value;
        size_t len;
    } fields[] = {
        { "guiflags", gui_flags, sizeof(gui_flags) },
        { "idletimeout", idle_timeout, sizeof(idle_timeout) },
        { "screentimeout", screen_timeout, sizeof(screen_timeout) },
        { "brightness", brightness, sizeof(brightness) },
        { "qrflags", qr_flags, sizeof(qr_flags) },
        { "privatekey", private_key, sizeof(private_key) },
        { "blob", wallet, sizeof(wallet) },
        { "counter", counter, sizeof(counter) },
        { "antireplay", antireplay, sizeof(antireplay) },
        { "keyflags", key_flags, sizeof(key_flags) },
        { "featflags", feat_flags, sizeof(feat_flags) },
        { "walleterasepin", wallet_erase_pin, sizeof(wallet_erase_pin) },
        { "networktype", network_type, sizeof(network_type) },
        { "pinsvrurlA", pinsvr_url_a, sizeof(pinsvr_url_a) },
        { "pinsvrurlB", pinsvr_url_b, sizeof(pinsvr_url_b) },
        { "pinsvrpubkey", pinsvr_pubkey, sizeof(pinsvr_pubkey) },
        { "pinsvrcert", pinsvr_cert, sizeof(pinsvr_cert) },
    };

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        put(store, fields[i].key, fields[i].value, fields[i].len);
    }
    put(store, "notalisted", unlisted, sizeof(unlisted));

    uint8_t* blob = NULL;
    size_t blob_len = 0;
    check(pijade_settings_serialize(store, &blob, &blob_len), "serialise every allowed field");
    check(blob && !contains(blob, blob_len, "notalisted"), "an unlisted field never appears in the output");

    wally_map_clear(store);
    check(blob && pijade_settings_deserialize(store, blob, blob_len), "deserialise every allowed field");
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        char what[80];
        const struct wally_map_item* const item = wally_map_get(
            store, (const unsigned char*)fields[i].key, strlen(fields[i].key));
        snprintf(what, sizeof(what), "%s round-trips unchanged", fields[i].key);
        check(item && item->value_len == fields[i].len
                && !memcmp(item->value, fields[i].value, fields[i].len),
            what);
    }
    check(!wally_map_get(store, (const unsigned char*)"notalisted", 10), "the unlisted field does not come back");

    // The wallet blob's floor moved from 1 to 80 (ENCRYPTED_DATA_LEN(16), the shortest ciphertext
    // keychain.c ever produces). Both shapes it actually emits below the old floor - 80 bytes for a
    // 16-byte mnemonic entropy and 96 bytes for a 32-byte one - must still round-trip.
    {
        const size_t widths[] = { 80, 96 };
        for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w) {
            const size_t width = widths[w];
            uint8_t value[96];
            for (size_t i = 0; i < width; ++i) {
                value[i] = (uint8_t)(0x10 + w * 16 + i);
            }
            wally_map_clear(store);
            put(store, "blob", value, width);
            uint8_t* width_bytes = NULL;
            size_t width_len = 0;
            char what[64];
            snprintf(what, sizeof(what), "serialise an %zu-byte blob", width);
            check(pijade_settings_serialize(store, &width_bytes, &width_len), what);
            wally_map_clear(store);
            snprintf(what, sizeof(what), "deserialise an %zu-byte blob", width);
            check(width_bytes && pijade_settings_deserialize(store, width_bytes, width_len), what);
            const struct wally_map_item* const item
                = wally_map_get(store, (const unsigned char*)"blob", strlen("blob"));
            snprintf(what, sizeof(what), "the %zu-byte blob round-trips unchanged", width);
            check(item && item->value_len == width && !memcmp(item->value, value, width), what);
            wally_bzero(value, sizeof(value));
            wipe_free(width_bytes, width_len);
        }
    }

    uint8_t* const doctored = blob ? malloc(blob_len) : NULL;
    check(blob && doctored, "allocate the doctored-file buffer");
    if (blob && doctored) {
        memcpy(doctored, blob, blob_len);
        uint8_t* entry = find_entry_key(doctored, blob_len, "brightness");
        check(entry != NULL, "find an entry to doctor into an unknown field");
        if (entry) {
            memcpy(entry, "notalisted", 10);
        }
        check(entry && refresh_digest(doctored, blob_len), "recompute the doctored-file digest");
        wally_map_clear(store);
        check(entry && !pijade_settings_deserialize(store, doctored, blob_len),
            "a doctored unlisted field is refused");
        check(!store->num_items, "no fields are stored before the refusal");

        memcpy(doctored, blob, blob_len);
        entry = find_entry_key(doctored, blob_len, "privatekey");
        check(entry != NULL, "find a fixed-width entry to resize");
        if (entry) {
            entry[strlen("privatekey")] = 31;
            entry[strlen("privatekey") + 1] = 0;
        }
        check(entry && refresh_digest(doctored, blob_len), "recompute the invalid-width digest");
        wally_map_clear(store);
        check(entry && !pijade_settings_deserialize(store, doctored, blob_len),
            "a value outside its allowed width is refused");

        memcpy(doctored, blob, blob_len);
        doctored[TEST_MAGIC_LEN - 1] = '1';
        wally_map_clear(store);
        check(!pijade_settings_deserialize(store, doctored, blob_len), "a v1-magic file is refused");

        memcpy(doctored, blob, blob_len);
        doctored[TEST_MAGIC_LEN - 1] = '2';
        wally_map_clear(store);
        check(!pijade_settings_deserialize(store, doctored, blob_len), "a PIJADES2 magic file is refused");

        // S3 held 'walleterasepin' as six raw digits; S4 holds a salt and a verifier, so an S3 file
        // has to be refused rather than read with the old width.
        memcpy(doctored, blob, blob_len);
        doctored[TEST_MAGIC_LEN - 1] = '3';
        wally_map_clear(store);
        check(!pijade_settings_deserialize(store, doctored, blob_len), "a PIJADES3 magic file is refused");

        memcpy(doctored, blob, blob_len);
        doctored[blob_len - 1] ^= 0xff;
        wally_map_clear(store);
        check(!pijade_settings_deserialize(store, doctored, blob_len), "the digest catches a flipped byte");

        memcpy(doctored, blob, blob_len);
        entry = find_entry_key(doctored, blob_len, "blob");
        check(entry != NULL, "find the blob entry to shrink its declared width");
        if (entry) {
            entry[strlen("blob")] = 1;
            entry[strlen("blob") + 1] = 0;
        }
        check(entry && refresh_digest(doctored, blob_len), "recompute the shrunk-blob digest");
        wally_map_clear(store);
        check(entry && !pijade_settings_deserialize(store, doctored, blob_len),
            "a blob declared narrower than 80 bytes is refused");

        memcpy(doctored, blob, blob_len);
        entry = find_entry_key(doctored, blob_len, "pinsvrurlA");
        check(entry != NULL, "find pinsvrurlA to strip its terminator");
        if (entry) {
            const size_t key_len = strlen("pinsvrurlA");
            const size_t value_len = (size_t)entry[key_len] | ((size_t)entry[key_len + 1] << 8);
            entry[key_len + 2 + value_len - 1] = 'x';
        }
        check(entry && refresh_digest(doctored, blob_len), "recompute the non-terminated digest");
        wally_map_clear(store);
        check(entry && !pijade_settings_deserialize(store, doctored, blob_len),
            "a pinsvrurlA not ending in NUL is refused");

        memcpy(doctored, blob, blob_len);
        entry = find_entry_key(doctored, blob_len, "antireplay");
        check(entry != NULL, "find antireplay to set the sentinel value");
        if (entry) {
            const size_t key_len = strlen("antireplay");
            memset(entry + key_len + 2, 0xff, sizeof(uint32_t));
        }
        check(entry && refresh_digest(doctored, blob_len), "recompute the sentinel-antireplay digest");
        wally_map_clear(store);
        check(entry && !pijade_settings_deserialize(store, doctored, blob_len), "antireplay = UINT32_MAX is refused");

        memcpy(doctored, blob, blob_len);
        entry = find_entry_key(doctored, blob_len, "networktype");
        check(entry != NULL, "find networktype to set an unknown value");
        if (entry) {
            const size_t key_len = strlen("networktype");
            entry[key_len + 2] = 3;
            entry[key_len + 3] = 0;
            entry[key_len + 4] = 0;
            entry[key_len + 5] = 0;
        }
        check(entry && refresh_digest(doctored, blob_len), "recompute the unknown-networktype digest");
        wally_map_clear(store);
        check(entry && !pijade_settings_deserialize(store, doctored, blob_len), "networktype = 3 is refused");

        memcpy(doctored, blob, blob_len);
        entry = find_entry_key(doctored, blob_len, "networktype");
        check(entry != NULL, "find networktype to set a valid value");
        if (entry) {
            // MAIN rather than the fixture's own TEST: writing back the value the round-trip case
            // already stored would prove nothing about the new range check.
            const size_t key_len = strlen("networktype");
            entry[key_len + 2] = 1;
            entry[key_len + 3] = 0;
            entry[key_len + 4] = 0;
            entry[key_len + 5] = 0;
        }
        check(entry && refresh_digest(doctored, blob_len), "recompute the valid-networktype digest");
        wally_map_clear(store);
        check(entry && pijade_settings_deserialize(store, doctored, blob_len), "networktype = 1 (MAIN) is accepted");

        memcpy(doctored, blob, blob_len);
        entry = find_entry_key(doctored, blob_len, "pinsvrurlB");
        check(entry != NULL, "find pinsvrurlB to remove it");
        size_t trimmed_len = blob_len;
        if (entry) {
            const size_t key_len = strlen("pinsvrurlB");
            const size_t value_len = (size_t)entry[key_len] | ((size_t)entry[key_len + 1] << 8);
            uint8_t* const entry_start = entry - 2; // back up over the namespace and key-length bytes
            uint8_t* const entry_end = entry + key_len + 2 + value_len;
            const size_t tail_len = (size_t)((doctored + blob_len) - entry_end);
            memmove(entry_start, entry_end, tail_len);
            trimmed_len = blob_len - (size_t)(entry_end - entry_start);
        }
        check(entry && refresh_digest(doctored, trimmed_len), "recompute the digest after removing pinsvrurlB");
        wally_map_clear(store);
        check(entry && !pijade_settings_deserialize(store, doctored, trimmed_len),
            "pinsvrurlA without pinsvrurlB is refused");
        check(!store->num_items, "no fields are stored before the url-pairing refusal");
    }

    wally_map_clear(store);
    uint8_t short_private_key[31] = { 0 };
    put(store, "privatekey", short_private_key, sizeof(short_private_key));
    uint8_t* skipped = NULL;
    size_t skipped_len = 0;
    check(pijade_settings_serialize(store, &skipped, &skipped_len), "serialise a store with an invalid width");
    check(skipped && !contains(skipped, skipped_len, "privatekey"), "serialisation skips an invalid width");

    {
        static const uint8_t valid_key[] = "record";
        static const uint8_t key_16[] = "abcdefghijklmnop";
        static const uint8_t key_space[] = { 'a', ' ', 'b' };
        static const uint8_t key_del[] = { 'a', 0x7f, 'b' };
        static const uint8_t key_nul[] = { 'a', '\0', 'b' };
        static const uint8_t key_15[] = "abcdefghijklmno";

        check(entry_case(1, key_16, sizeof(key_16) - 1, 114, false), "a 16-byte registration key is refused on both sides");
        check(entry_case(1, key_space, sizeof(key_space), 114, false), "a registration key containing space is refused on both sides");
        check(entry_case(1, key_del, sizeof(key_del), 114, false), "a registration key containing DEL is refused on both sides");
        check(entry_case(1, key_nul, sizeof(key_nul), 114, false), "a registration key containing NUL is refused on both sides");
        check(entry_case(1, key_15, sizeof(key_15) - 1, 114, true), "a printable 15-byte registration key is accepted on both sides");

        check(entry_case(1, valid_key, sizeof(valid_key) - 1, 113, false), "a 113-byte multisig is refused on both sides");
        check(entry_case(1, valid_key, sizeof(valid_key) - 1, 114, true), "a 114-byte multisig is accepted on both sides");
        check(entry_case(1, valid_key, sizeof(valid_key) - 1, 3251, false), "a 3251-byte multisig is refused on both sides");
        check(entry_case(2, valid_key, sizeof(valid_key) - 1, 40, false), "a 40-byte descriptor is refused on both sides");
        check(entry_case(2, valid_key, sizeof(valid_key) - 1, 41, true), "a 41-byte descriptor is accepted on both sides");
        check(entry_case(2, valid_key, sizeof(valid_key) - 1, 3250, false), "a 3250-byte descriptor is refused on both sides");
        check(entry_case(3, valid_key, sizeof(valid_key) - 1, 31, false), "a 31-byte OTP record is refused on both sides");
        check(entry_case(3, valid_key, sizeof(valid_key) - 1, 33, false), "a non-block-aligned OTP record is refused on both sides");
        check(entry_case(3, valid_key, sizeof(valid_key) - 1, 32, true), "a 32-byte OTP record is accepted on both sides");
        check(entry_case(3, valid_key, sizeof(valid_key) - 1, 288, true), "a 288-byte OTP record is accepted on both sides");
        check(entry_case(3, valid_key, sizeof(valid_key) - 1, 304, false), "a 304-byte OTP record is refused on both sides");
        check(entry_case(4, valid_key, sizeof(valid_key) - 1, 7, false), "a 7-byte HOTP counter is refused on both sides");
        check(entry_case(4, valid_key, sizeof(valid_key) - 1, 8, true), "an 8-byte HOTP counter is accepted on both sides");
        check(entry_case(4, valid_key, sizeof(valid_key) - 1, 9, false), "a 9-byte HOTP counter is refused on both sides");
        check(entry_case(5, valid_key, sizeof(valid_key) - 1, 114, false), "namespace byte 5 is refused");
    }

    {
        uint8_t value[114] = { 0 };
        uint8_t keys[17][4];
        struct raw_entry entries[17];
        for (size_t i = 0; i < 17; ++i) {
            snprintf((char*)keys[i], sizeof(keys[i]), "r%02zu", i);
            entries[i] = (struct raw_entry) { 1, keys[i], strlen((char*)keys[i]), value, sizeof(value) };
        }
        size_t sixteen_len = 0;
        size_t seventeen_len = 0;
        uint8_t* const sixteen = make_file(entries, 16, &sixteen_len);
        uint8_t* const seventeen = make_file(entries, 17, &seventeen_len);
        clear_store();
        check(sixteen && pijade_settings_deserialize(store, sixteen, sixteen_len),
            "the loader accepts 16 records in one namespace");
        clear_store();
        check(seventeen && !pijade_settings_deserialize(store, seventeen, seventeen_len),
            "the loader refuses a 17th record in one namespace");

        clear_store();
        for (size_t i = 0; i < 16; ++i) {
            put_key(pijade_settings_storage(1), keys[i], strlen((char*)keys[i]), value, sizeof(value));
        }
        uint8_t* count_blob = NULL;
        size_t count_blob_len = 0;
        check(pijade_settings_serialize(store, &count_blob, &count_blob_len),
            "the serializer accepts 16 records in one namespace");
        wipe_free(count_blob, count_blob_len);
        put_key(pijade_settings_storage(1), keys[16], strlen((char*)keys[16]), value, sizeof(value));
        count_blob = NULL;
        count_blob_len = 0;
        check(!pijade_settings_serialize(store, &count_blob, &count_blob_len),
            "the serializer refuses a 17th record in one namespace");
        wipe_free(count_blob, count_blob_len);
        wipe_free(sixteen, sixteen_len);
        wipe_free(seventeen, seventeen_len);
        wally_bzero(value, sizeof(value));
    }

    {
        const uint8_t value[114] = { 0 };
        const uint8_t url_a[] = "https://pin.example/a";
        clear_store();
        put(pijade_settings_storage(0), "pinsvrurlA", url_a, sizeof(url_a));
        uint8_t* invalid_pair = NULL;
        size_t invalid_pair_len = 0;
        check(!pijade_settings_serialize(store, &invalid_pair, &invalid_pair_len),
            "the serializer refuses an unpaired pinserver URL");
        wipe_free(invalid_pair, invalid_pair_len);

        clear_store();
        put(pijade_settings_storage(0), "brightness", value, 1);
        put(pijade_settings_storage(1), "multisig", value, 114);
        put(pijade_settings_storage(2), "descriptor", value, 41);
        put(pijade_settings_storage(3), "otp", value, 32);
        put(pijade_settings_storage(4), "hotpc", value, 8);
        uint8_t* all_blob = NULL;
        size_t all_blob_len = 0;
        const bool serialized = pijade_settings_serialize(store, &all_blob, &all_blob_len);
        clear_store();
        const bool loaded = serialized && pijade_settings_deserialize(store, all_blob, all_blob_len);
        bool all_present = loaded;
        static const char* const keys[] = { "brightness", "multisig", "descriptor", "otp", "hotpc" };
        for (size_t ns = 0; all_present && ns < 5; ++ns) {
            all_present = wally_map_get(pijade_settings_storage(ns),
                              (const unsigned char*)keys[ns], strlen(keys[ns]))
                != NULL;
        }
        check(all_present, "records in all five namespaces round-trip together");
        wipe_free(all_blob, all_blob_len);
    }

    {
        static const char* const namespace_names[] = { "PIN", "MULTISIGS", "DESCRIPTORS", "OTP", "HOTPC" };
        static const char* const keys[] = { "brightness", "multisig", "descriptor", "otp", "hotpc" };
        static const size_t lengths[] = { 1, 114, 41, 32, 8 };
        uint8_t value[114] = { 0 };
        struct captured_settings captured = { 0 };
        libjade_set_settings_handler(capturing_settings_handler, &captured);
        for (size_t ns = 0; ns < 5; ++ns) {
            clear_store();
            nvs_handle_t handle = NULL;
            const bool opened = nvs_open(namespace_names[ns], NVS_READWRITE, &handle) == ESP_OK;
            const bool stored = opened && nvs_set_blob(handle, keys[ns], value, lengths[ns]) == ESP_OK;
            const bool committed = stored && nvs_commit(handle) == ESP_OK;
            clear_store();
            const bool loaded = committed && captured.calls == ns + 1
                && libjade_load_settings(captured.data, captured.len);
            const struct wally_map_item* const item = loaded
                ? wally_map_get(pijade_settings_storage(ns), (const unsigned char*)keys[ns], strlen(keys[ns]))
                : NULL;
            char what[96];
            snprintf(what, sizeof(what), "namespace %zu commit reaches the host and reloads", ns);
            check(item && item->value_len == lengths[ns], what);
        }
        libjade_set_settings_handler(NULL, NULL);
        wipe_free(captured.data, captured.len);
        wally_bzero(value, sizeof(value));
    }

    {
        char path[] = "/tmp/pijade-settings-test.XXXXXX";
        const int fd = mkstemp(path);
        if (fd >= 0) {
            close(fd);
            unlink(path);
        }
        uint8_t* maximum = NULL;
        size_t maximum_len = 0;
        uint8_t* read_back = NULL;
        size_t read_back_len = 0;
        const bool populated = fd >= 0 && populate_largest_store();
        const bool serialized = populated && pijade_settings_serialize(store, &maximum, &maximum_len);
        settings_store_t* const host_store = settings_store_open(path);
        const bool written = serialized && maximum_len > 8192 && host_store
            && settings_store_write(host_store, maximum, maximum_len);
        const bool read = written && settings_store_read(host_store, &read_back, &read_back_len);
        clear_store();
        check(read && read_back_len == maximum_len && !memcmp(read_back, maximum, maximum_len)
                && libjade_load_settings(read_back, read_back_len),
            "the largest valid store survives the host write/read/load path");

        uint8_t* const over_cap = calloc(SETTINGS_MAX_LEN + 1, 1);
        check(over_cap && host_store && !settings_store_write(host_store, over_cap, SETTINGS_MAX_LEN + 1),
            "the host refuses a blob one byte over the 131072-byte cap");
        settings_store_close(host_store);
        wipe_free(over_cap, SETTINGS_MAX_LEN + 1);
        wipe_free(read_back, read_back_len);
        wipe_free(maximum, maximum_len);
        char slot_path[sizeof(path) + 2];
        snprintf(slot_path, sizeof(slot_path), "%s.a", path);
        unlink(slot_path);
        snprintf(slot_path, sizeof(slot_path), "%s.b", path);
        unlink(slot_path);
    }

    clear_store();
    {
        uint8_t marker[] = { 0x01 };

        libjade_set_settings_handler(refusing_settings_handler, NULL);
        put(store, "brightness", marker, sizeof(marker));
        check(nvs_commit(store) == ESP_FAIL, "nvs_commit fails when the settings handler refuses the write");

        libjade_set_settings_handler(accepting_settings_handler, NULL);
        check(nvs_commit(store) == ESP_OK, "nvs_commit succeeds when the settings handler accepts the write");

        libjade_set_settings_handler(NULL, NULL);
        check(nvs_commit(store) == ESP_OK, "nvs_commit succeeds with no settings handler registered");

        libjade_set_settings_handler(refusing_settings_handler, NULL);
        check(nvs_flash_erase() == ESP_FAIL, "nvs_flash_erase fails when the settings handler refuses the erase");

        libjade_set_settings_handler(erase_settings_handler, NULL);
        check(nvs_flash_erase() == ESP_OK && saw_erase_request,
            "nvs_flash_erase sends the host a NULL, zero-length erase request");

        // Reset so the handler does not leak into whatever runs after this test.
        libjade_set_settings_handler(NULL, NULL);
        wally_bzero(marker, sizeof(marker));
    }

    clear_store();
    wally_bzero(short_private_key, sizeof(short_private_key));
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        wally_bzero(fields[i].value, fields[i].len);
    }
    wally_bzero(unlisted, sizeof(unlisted));
    wipe_free(doctored, blob_len);
    wipe_free(skipped, skipped_len);
    wipe_free(blob, blob_len);

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
