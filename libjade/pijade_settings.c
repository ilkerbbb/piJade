#include "pijade_settings.h"

#include <stdlib.h>
#include <string.h>
#include <wally_core.h>
#include <wally_crypto.h>
#include <wally_map.h>

/*
 * BBB-AIRGAP: which NVS fields are written to disk, and their allowed lengths.
 *
 * The file holds the encrypted wallet, the PIN private key, and the remaining-attempt counter. The
 * wallet blob is useless without the pinserver half of its key. Unlike the ESP32's encrypted flash,
 * however, an attacker holding the card can copy these fields and roll the attempt counter back, so
 * the remaining defence is the pinserver's own rate limiting.
 *
 * The lengths mirror the fixed types and string bounds that main/storage.c reads back. A stored
 * value outside its range is refused rather than handed to code that does not expect that width.
 */
static const struct {
    const char* key;
    uint16_t min_len;
    uint16_t max_len;
    // nvs_get_str() is really nvs_get_blob() (nvs_flash.c), so there is no NUL guarantee from the
    // storage layer itself; pinclient.c:96,108 snprintf("%s", ...) these buffers directly, so a
    // stored value that is not NUL-terminated is an out-of-bounds read waiting to happen.
    bool nul_terminated;
} PERSISTED_FIELDS[] = {
    { "guiflags", 1, 1, false }, // theme, flipped orientation, camera rotation; see main/storage.h
    { "idletimeout", 2, 2, false }, // uint16_t seconds
    // BBB-AIRGAP: the screen dimming threshold (Preferences > Screen Timeout), added here with
    // the setting itself rather than after a device round measured it reverting; same width and
    // same writer shape as idletimeout above (storage_set_screen_timeout(), main/storage.c).
    { "screentimeout", 2, 2, false }, // uint16_t seconds
    { "brightness", 1, 1, false },
    { "qrflags", 4, 4, false }, // uint32_t; QR mode preferences, see main/qrmode.c
    { "privatekey", 32, 32, false },
    // keychain.c only ever encrypts the mnemonic entropy (16 or 32 bytes) or the serialised key
    // (206 bytes) through ENCRYPTED_DATA_LEN(), which is AES_BLOCK_LEN + AES_PADDED_LEN(len) +
    // HMAC_SHA256_LEN (aes.h, keychain.c:22): 80, 96, and 256 bytes respectively. 80 is the floor
    // so a shorter value can never reach keychain.c:441's JADE_ASSERT(bytes_len > HMAC_SHA256_LEN).
    { "blob", 80, 256, false },
    { "counter", 1, 1, false },
    { "antireplay", 4, 4, false },
    { "keyflags", 1, 1, false },
    // BBB-AIRGAP: the seven Wallet Options flags (main/storage.h FEATURE_FLAGS_*: bip85, sign
    // message, harsh warnings, xpub details, singlesig, multisig, denomination).  Left out of this
    // list until the device round of 2026-09-02 measured the result: the screen worked for the rest
    // of the session and then silently reverted on the next boot, because a value nvs holds but
    // this list does not name never reaches the card.  One byte, as storage_set_feature_flags()
    // (main/storage.c:678) writes sizeof(uint8_t).
    //
    // Two other fields main/storage.c names are deliberately still absent, and the reason is that
    // nothing on this port writes them: 'bleflags' because ble is not compiled here (the radio is
    // physically cut and main/ble/ble.h:27 supplies the empty implementation), and 'clickevent'
    // because only storage.c:358 touches it, to erase it.  Either would need its writer back
    // before persisting it would mean anything.
    { "featflags", 1, 1, false },
    { "walleterasepin", 48, 48, false }, // WALLET_ERASE_PIN_RECORD_LEN (main/storage.h): 16 salt + 32 verifier
    { "networktype", 4, 4, false },
    // The three string fields start at 1: nvs_set_str() stores strlen + 1, so an empty string is a
    // one-byte value, and for pinsvrurlB that is a state Jade sets on purpose ("explicitly no second
    // url", main/process/pinclient.c:104). Dropping it would leave urlA present and urlB absent, and
    // pinclient.c:113 asserts the two are set together.
    { "pinsvrurlA", 1, 120, true },
    { "pinsvrurlB", 1, 120, true },
    { "pinsvrpubkey", 33, 33, false },
    { "pinsvrcert", 1, 2048, true },
};
#define NUM_PERSISTED_FIELDS (sizeof(PERSISTED_FIELDS) / sizeof(PERSISTED_FIELDS[0]))

/*
 * Format: magic(8), digest(4), then entries of ns(1), key_len(1), key, value_len(2 LE), value.
 *
 * The digest is the first bytes of the SHA256 of everything after it. It is not a security measure
 * - anyone who can edit the file can recompute it - but the settings file lives on the FAT boot
 * partition, which has no journal, so a power cut part way through a write can leave a file that
 * parses yet holds nonsense. Four bytes are enough to send that file back to the defaults.
 *
 * The trailing digit of the magic is the format version: change it if the layout below changes, and
 * old files are ignored rather than misread.
 */
static const char SETTINGS_MAGIC[8] = { 'P', 'I', 'J', 'A', 'D', 'E', 'S', '4' };
#define SETTINGS_DIGEST_LEN 4
#define SETTINGS_HEADER_LEN (sizeof(SETTINGS_MAGIC) + SETTINGS_DIGEST_LEN)

static const struct {
    uint16_t min_len;
    uint16_t max_len;
    uint8_t modulo;
    uint8_t max_entries;
} PERSISTED_NAMESPACES[] = {
    // BBB-AIRGAP: multisig.h MAX_MULTISIG_BYTES_LEN measures 3281 bytes (sealed body, see
    // main/registration_seal.h) and a compile-time assert in multisig.c holds it there;
    // multisig.h caps registrations at 16.  The floor stays at the pre-sealing 114 so a record
    // written by an older image is refused by registration_open() as "not readable" rather than
    // taking the whole settings file down with it.
    { 114, 3281, 0, 16 },
    // BBB-AIRGAP: descriptor.h MAX_DESCRIPTOR_BYTES_LEN measures 3281 bytes, likewise asserted in
    // descriptor.c; floor 41 kept for the same reason as above; descriptor.h caps records at 16.
    { 41, 3281, 0, 16 },
    // otpauth.c:809 passes the stored value to aes_decrypt_bytes(); aes.c:45,51 require more than
    // AES_BLOCK_LEN bytes and a multiple of AES_BLOCK_LEN after the first block. otpauth.c:808-809
    // measures the destination at 288 bytes, and otpauth.h:14 caps records at 16.
    { 32, 288, 16, 16 },
    // storage.c:773-783 stores and reads a fixed uint64_t; otpauth.h:14 caps its OTP names at 16.
    { 8, 8, 0, 16 },
};
#define NUM_PERSISTED_NAMESPACES (sizeof(PERSISTED_NAMESPACES) / sizeof(PERSISTED_NAMESPACES[0]))
#define SETTINGS_NAMESPACE_COUNT (1 + NUM_PERSISTED_NAMESPACES)

// Index into PERSISTED_FIELDS, or -1 if the key is not persisted.
static int find_field(const unsigned char* key, const size_t key_len)
{
    for (size_t i = 0; i < NUM_PERSISTED_FIELDS; ++i) {
        const size_t len = strlen(PERSISTED_FIELDS[i].key);
        if (len == key_len && !memcmp(key, PERSISTED_FIELDS[i].key, len)) {
            return (int)i;
        }
    }
    return -1;
}

static bool user_entry_valid(
    const uint8_t ns, const unsigned char* const key, const size_t key_len, const size_t value_len)
{
    if (ns == 0 || ns >= SETTINGS_NAMESPACE_COUNT || ns - 1 >= NUM_PERSISTED_NAMESPACES) {
        return false;
    }
    // storage.c:398-414 storage_key_name_valid() requires 1..15 bytes and isgraph() for every byte.
    // nvs_flash.c:243 aborts when a listed key is 16 bytes or longer; bytes 33..126 also exclude an
    // embedded NUL that C-string NVS lookups could never match or erase.
    if (!key_len || key_len >= NVS_KEY_NAME_MAX_SIZE) {
        return false;
    }
    for (size_t i = 0; i < key_len; ++i) {
        if (key[i] < 33 || key[i] > 126) {
            return false;
        }
    }

    const size_t index = ns - 1;
    if (value_len < PERSISTED_NAMESPACES[index].min_len
        || value_len > PERSISTED_NAMESPACES[index].max_len) {
        return false;
    }
    return !PERSISTED_NAMESPACES[index].modulo
        || (value_len - AES_BLOCK_LEN) % PERSISTED_NAMESPACES[index].modulo == 0;
}

// Writes the digest of `body` into the header of `output`.
static bool set_digest(uint8_t* const output, const uint8_t* const body, const size_t body_len)
{
    uint8_t digest[SHA256_LEN];
    if (wally_sha256(body, body_len, digest, sizeof(digest)) != WALLY_OK) {
        return false;
    }
    memcpy(output + sizeof(SETTINGS_MAGIC), digest, SETTINGS_DIGEST_LEN);
    wally_bzero(digest, sizeof(digest));
    return true;
}

static bool walk_entries(struct wally_map* prefs, const uint8_t* body, size_t body_len);

bool pijade_settings_serialize(const struct wally_map* const prefs, uint8_t** output, size_t* output_len)
{
    if (!prefs || !output || !output_len) {
        return false;
    }
    *output = NULL;
    *output_len = 0;

    size_t body_len = 0;
    for (size_t i = 0; i < NUM_PERSISTED_FIELDS; ++i) {
        const size_t key_len = strlen(PERSISTED_FIELDS[i].key);
        const struct wally_map_item* const item
            = wally_map_get(prefs, (const unsigned char*)PERSISTED_FIELDS[i].key, key_len);
        // A field Jade has never written, or one whose width is outside the range above,
        // is left out. Leaving it out means the device keeps its default for that setting.
        if (item && item->value_len >= PERSISTED_FIELDS[i].min_len
            && item->value_len <= PERSISTED_FIELDS[i].max_len) {
            body_len += 1 + 1 + key_len + 2 + item->value_len;
        }
    }
    for (size_t ns = 1; ns < SETTINGS_NAMESPACE_COUNT; ++ns) {
        const struct wally_map* const records = pijade_settings_storage(ns);
        if (!records) {
            return false;
        }
        for (size_t i = 0; i < records->num_items; ++i) {
            const struct wally_map_item* const item = &records->items[i];
            // register_multisig.c:39,317,673, register_descriptor.c:42,228, and
            // register_otp.c:25,113 validate names, while their records are built to the bounds
            // cited in PERSISTED_NAMESPACES. A violation is therefore an impossible state. Refuse
            // the write instead of silently recreating T6's user-data loss class.
            if (!user_entry_valid((uint8_t)ns, item->key, item->key_len, item->value_len)) {
                return false;
            }
            body_len += 1 + 1 + item->key_len + 2 + item->value_len;
        }
    }

    uint8_t* const out = malloc(SETTINGS_HEADER_LEN + body_len);
    if (!out) {
        return false;
    }
    memcpy(out, SETTINGS_MAGIC, sizeof(SETTINGS_MAGIC));

    uint8_t* p = out + SETTINGS_HEADER_LEN;
    for (size_t i = 0; i < NUM_PERSISTED_FIELDS; ++i) {
        const size_t key_len = strlen(PERSISTED_FIELDS[i].key);
        const struct wally_map_item* const item
            = wally_map_get(prefs, (const unsigned char*)PERSISTED_FIELDS[i].key, key_len);
        if (!item || item->value_len < PERSISTED_FIELDS[i].min_len
            || item->value_len > PERSISTED_FIELDS[i].max_len) {
            continue;
        }
        *p++ = 0;
        *p++ = (uint8_t)key_len;
        memcpy(p, PERSISTED_FIELDS[i].key, key_len);
        p += key_len;
        *p++ = (uint8_t)item->value_len;
        *p++ = (uint8_t)(item->value_len >> 8);
        memcpy(p, item->value, item->value_len);
        p += item->value_len;
    }
    for (size_t ns = 1; ns < SETTINGS_NAMESPACE_COUNT; ++ns) {
        const struct wally_map* const records = pijade_settings_storage(ns);
        for (size_t i = 0; i < records->num_items; ++i) {
            const struct wally_map_item* const item = &records->items[i];
            *p++ = (uint8_t)ns;
            *p++ = (uint8_t)item->key_len;
            memcpy(p, item->key, item->key_len);
            p += item->key_len;
            *p++ = (uint8_t)item->value_len;
            *p++ = (uint8_t)(item->value_len >> 8);
            memcpy(p, item->value, item->value_len);
            p += item->value_len;
        }
    }

    if ((size_t)(p - out) != SETTINGS_HEADER_LEN + body_len
        || !set_digest(out, out + SETTINGS_HEADER_LEN, body_len)
        // The checks below and the pinsvrurlA/urlB pairing check were loader-only in T5. That let
        // an invalid default-namespace map serialize successfully, then fail at the next boot and
        // move the wallet aside. Run the loader's validation pass here so every file written can
        // be read back, and the current write reports the failure instead. See pinclient.c:104,113.
        || !walk_entries(NULL, out + SETTINGS_HEADER_LEN, body_len)) {
        wally_bzero(out, SETTINGS_HEADER_LEN + body_len);
        free(out);
        return false;
    }

    *output = out;
    *output_len = SETTINGS_HEADER_LEN + body_len;
    return true;
}

/*
 * Walks the entries of `body`, checking each against the allowlist.
 *
 * With `prefs` NULL this only validates; with `prefs` set it also stores. Callers make one pass of
 * each so a file that turns out to be bad part way through has not already changed anything.
 */
static bool walk_entries(struct wally_map* const prefs, const uint8_t* const body, const size_t body_len)
{
    // Set together only on the validating pass (prefs == NULL): pinclient.c:113 asserts the two
    // pinserver urls are either both present or both absent, and that has to be caught before the
    // storing pass touches `prefs`, or a rejected file would still leave one of the pair stored.
    bool urlA_seen = false;
    bool urlB_seen = false;
    size_t namespace_counts[SETTINGS_NAMESPACE_COUNT] = { 0 };

    const uint8_t* p = body;
    const uint8_t* const end = body + body_len;
    while (p < end) {
        const uint8_t ns = *p++;
        if (ns >= SETTINGS_NAMESPACE_COUNT || p == end) {
            return false;
        }
        const size_t key_len = *p++;
        if (!key_len || (size_t)(end - p) < key_len + 2) {
            return false;
        }
        const unsigned char* const key = p;
        p += key_len;

        const size_t value_len = (size_t)p[0] | ((size_t)p[1] << 8);
        p += 2;
        if ((size_t)(end - p) < value_len) {
            return false;
        }

        if (ns == 0) {
            const int field = find_field(key, key_len);
            if (field < 0 || value_len < PERSISTED_FIELDS[field].min_len
                || value_len > PERSISTED_FIELDS[field].max_len) {
                return false;
            }
            if (PERSISTED_FIELDS[field].nul_terminated && p[value_len - 1] != '\0') {
                return false;
            }
            // storage.c:539 asserts the replay counter stays below UINT32_MAX; refuse a file that would
            // hand that assert a value it cannot accept.
            if (!strcmp(PERSISTED_FIELDS[field].key, "antireplay")) {
                const uint32_t antireplay = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                    | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                if (antireplay == UINT32_MAX) {
                    return false;
                }
            }
            // No range check for 'walleterasepin': since the S4 format the field is a salt and a
            // PBKDF2 verifier (main/storage.h), so every byte value is legitimate.  It used to hold
            // raw digits, which format_pin() asserted were < 10; keeping that check would now reject
            // ordinary hash bytes and, because the refusal is per file (see below), would take the
            // whole settings file and the stored wallet blob down with it.
            // keychain.c:213-215 JADE_ASSERT(keychain_is_network_type_consistent(network_type)) on every
            // successful PIN unlock (auth_user.c:435-440); network_type_t (network.h:23) only defines
            // 0, 1, 2, so refuse a stored value outside that range.
            if (!strcmp(PERSISTED_FIELDS[field].key, "networktype")) {
                const uint32_t networktype = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                    | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                if (networktype > 2) {
                    return false;
                }
            }
            if (!prefs) {
                if (!strcmp(PERSISTED_FIELDS[field].key, "pinsvrurlA")) {
                    urlA_seen = true;
                } else if (!strcmp(PERSISTED_FIELDS[field].key, "pinsvrurlB")) {
                    urlB_seen = true;
                }
            }
        } else {
            if (!user_entry_valid(ns, key, key_len, value_len)
                || ++namespace_counts[ns] > PERSISTED_NAMESPACES[ns - 1].max_entries) {
                return false;
            }
        }
        if (prefs) {
            struct wally_map* const storage = ns == 0 ? prefs : pijade_settings_storage(ns);
            if (!storage || wally_map_replace(storage, key, key_len, p, value_len) != WALLY_OK) {
                return false;
            }
        }
        p += value_len;
    }
    if (!prefs && urlA_seen != urlB_seen) {
        return false;
    }
    return true;
}

bool pijade_settings_deserialize(
    struct wally_map* const prefs, const uint8_t* const bytes, const size_t bytes_len)
{
    if (!prefs || !bytes || bytes_len < SETTINGS_HEADER_LEN
        || memcmp(bytes, SETTINGS_MAGIC, sizeof(SETTINGS_MAGIC))) {
        return false;
    }

    const uint8_t* const body = bytes + SETTINGS_HEADER_LEN;
    const size_t body_len = bytes_len - SETTINGS_HEADER_LEN;

    uint8_t digest[SHA256_LEN];
    if (wally_sha256(body, body_len, digest, sizeof(digest)) != WALLY_OK) {
        return false;
    }
    const bool digest_ok = !memcmp(bytes + sizeof(SETTINGS_MAGIC), digest, SETTINGS_DIGEST_LEN);
    wally_bzero(digest, sizeof(digest));
    if (!digest_ok) {
        return false;
    }

    // Validate everything before storing anything; see walk_entries().
    return walk_entries(NULL, body, body_len) && walk_entries(prefs, body, body_len);
}
