#ifndef AMALGAMATED_BUILD
#include "registration_seal.h"
#include "jade_assert.h"
#include "sensitive.h"
#include "wallet.h"

#include <sodium/utils.h>
#include <string.h>

// BBB-AIRGAP: label hashed under the master key to make the record key (spec B1).  Changing it
// invalidates every sealed record, which is what a key change should do.
static const char REGISTRATION_KEY_LABEL[] = "pijade-registered-wallet-aes-v1";

static bool get_registration_key(uint8_t* key, const size_t key_len)
{
    JADE_ASSERT(key);
    JADE_ASSERT(key_len == AES_KEY_LEN_256);
    return wallet_hmac_with_master_key(
        (const uint8_t*)REGISTRATION_KEY_LABEL, sizeof(REGISTRATION_KEY_LABEL) - 1, key, key_len);
}

bool registration_seal(
    const uint8_t version, const uint8_t* body, const size_t body_len, uint8_t* output, const size_t output_len)
{
    JADE_ASSERT(body);
    JADE_ASSERT(body_len);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len == REGISTRATION_SEALED_LEN(body_len));

    bool ret = false;
    uint8_t key[AES_KEY_LEN_256];
    SENSITIVE_PUSH(key, sizeof(key));
    if (!get_registration_key(key, sizeof(key))) {
        goto cleanup;
    }

    output[0] = version;
    uint8_t* const encrypted = output + sizeof(version);
    const size_t encrypted_len = AES_ENCRYPTED_LEN(body_len);
    if (!aes_encrypt_bytes(key, sizeof(key), body, body_len, encrypted, encrypted_len)) {
        goto cleanup;
    }

    uint8_t* const hmac = encrypted + encrypted_len;
    JADE_ASSERT(hmac + HMAC_SHA256_LEN == output + output_len);
    ret = wallet_hmac_with_master_key(output, output_len - HMAC_SHA256_LEN, hmac, HMAC_SHA256_LEN);

cleanup:
    SENSITIVE_POP(key);
    return ret;
}

bool registration_open(const uint8_t expected_version, const uint8_t* bytes, const size_t bytes_len, uint8_t* body,
    const size_t body_len, size_t* written)
{
    JADE_ASSERT(bytes);
    JADE_ASSERT(body);
    JADE_ASSERT(body_len);
    JADE_INIT_OUT_SIZE(written);

    // BBB-AIRGAP: a record written before sealing (multisig v3 from 114 bytes, descriptor v0 from
    // 41 bytes) still passes the settings file floor and reaches this reader, so length is a
    // gate and not an assert: the record shows as not readable instead of taking the device down.
    if (bytes_len < REGISTRATION_MIN_SEALED_LEN || (bytes_len - REGISTRATION_MIN_SEALED_LEN) % AES_BLOCK_LEN) {
        JADE_LOGW("Registered wallet record has unexpected length %u", bytes_len);
        return false;
    }

    uint8_t hmac_calculated[HMAC_SHA256_LEN];
    if (!wallet_hmac_with_master_key(bytes, bytes_len - HMAC_SHA256_LEN, hmac_calculated, sizeof(hmac_calculated))
        || sodium_memcmp(bytes + bytes_len - HMAC_SHA256_LEN, hmac_calculated, sizeof(hmac_calculated)) != 0) {
        JADE_LOGW("Registered wallet record HMAC error/mismatch");
        return false;
    }

    if (bytes[0] != expected_version) {
        JADE_LOGE("Bad version byte %u in registered wallet record (expected %u)", bytes[0], expected_version);
        return false;
    }

    bool ret = false;
    uint8_t key[AES_KEY_LEN_256];
    SENSITIVE_PUSH(key, sizeof(key));
    if (get_registration_key(key, sizeof(key))) {
        const uint8_t* const encrypted = bytes + sizeof(uint8_t);
        const size_t encrypted_len = bytes_len - sizeof(uint8_t) - HMAC_SHA256_LEN;
        ret = aes_decrypt_bytes(key, sizeof(key), encrypted, encrypted_len, body, body_len, written);
    }
    SENSITIVE_POP(key);
    return ret;
}
#endif // AMALGAMATED_BUILD
