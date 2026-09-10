/*
 * Duress ('wallet-erase') PIN storage test.
 *
 * pijade/tools/settings_test.c checks that the 'walleterasepin' field is the right width on the
 * card; it cannot check what the bytes are, because it never goes through main/storage.c.  This
 * test does: it sets a PIN through the real storage path and then reads the raw record back out
 * of the NVS shim, so a change that quietly went back to storing the digits would fail here.
 *
 * Build (from the repo root, after a build_linux):
 *
 *   gcc -Wall -Wextra -O1 -o /tmp/duress_pin_test pijade/tools/duress_pin_test.c \
 *       -I main -I libjade -isystem libjade/include \
 *       -isystem components/libwally-core/upstream/include \
 *       -Wl,--start-group \
 *         build_linux/libjade/libjade_static.a \
 *         build_linux/libjade/libcbor_target.a \
 *         build_linux/libjade/libotpauth_migrate_target.a \
 *         build_linux/libjade/bcur/libbcur.a \
 *         build_linux/libjade/mbedtls/library/libmbedcrypto.a \
 *       -Wl,--end-group \
 *       -lpthread -lz -lm -lstdc++
 */
#include "storage.h"
#include <nvs_flash.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
static void check(const bool ok, const char* what)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++fails;
    }
}

static bool contains(const uint8_t* haystack, const size_t haystack_len, const uint8_t* needle, const size_t needle_len)
{
    if (needle_len > haystack_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= haystack_len; ++i) {
        if (!memcmp(haystack + i, needle, needle_len)) {
            return true;
        }
    }
    return false;
}

// Read the record the way a card carver would, rather than through the storage API
static bool read_record(uint8_t* out, size_t len)
{
    nvs_handle handle;
    if (nvs_open("PIN", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t written = len;
    const esp_err_t err = nvs_get_blob(handle, "walleterasepin", out, &written);
    nvs_close(handle);
    return err == ESP_OK && written == len;
}

int main(void)
{
    if (nvs_flash_init() != ESP_OK) {
        printf("nvs_flash_init failed\n");
        return 1;
    }

    const uint8_t pin[6] = { 1, 2, 3, 4, 5, 6 };
    const uint8_t wrong[6] = { 1, 2, 3, 4, 5, 7 };

    check(!storage_wallet_erase_pin_exists(), "no duress PIN on a fresh device");
    check(!storage_verify_wallet_erase_pin(pin, sizeof(pin)), "verify says no when none is set");

    check(storage_set_wallet_erase_pin(pin, sizeof(pin)), "setting a duress PIN succeeds");
    check(storage_wallet_erase_pin_exists(), "the device now reports one is set");

    uint8_t record[WALLET_ERASE_PIN_RECORD_LEN];
    check(read_record(record, sizeof(record)), "the stored record is 48 bytes wide");
    check(!contains(record, sizeof(record), pin, sizeof(pin)), "the digits themselves are nowhere in the record");

    check(storage_verify_wallet_erase_pin(pin, sizeof(pin)), "the right PIN verifies");
    check(!storage_verify_wallet_erase_pin(wrong, sizeof(wrong)), "a wrong PIN does not");
    check(!storage_verify_wallet_erase_pin(pin, 5), "a short PIN does not");

    // Setting the same PIN a second time must produce a different record (random salt)
    uint8_t again[WALLET_ERASE_PIN_RECORD_LEN];
    check(storage_set_wallet_erase_pin(pin, sizeof(pin)) && read_record(again, sizeof(again)),
        "the same PIN can be set a second time");
    check(memcmp(record, again, sizeof(record)) != 0, "the same PIN produces a different record each time");
    check(storage_verify_wallet_erase_pin(pin, sizeof(pin)), "and still verifies afterwards");

    check(storage_erase_wallet_erase_pin(), "erasing the duress PIN succeeds");
    check(!storage_wallet_erase_pin_exists(), "the device reports none is set again");
    check(!storage_verify_wallet_erase_pin(pin, sizeof(pin)), "and the old PIN no longer verifies");

    printf("\n%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
