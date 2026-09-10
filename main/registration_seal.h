#ifndef REGISTRATION_SEAL_H_
#define REGISTRATION_SEAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wally_crypto.h>

#include "aes.h"
#include "jade_assert.h"

// BBB-AIRGAP: registered wallet records (multisig and descriptor) live on an SD card that has
// no flash encryption, so their body is sealed with a key derived from the wallet master key:
//     version(1) | IV(16) + AES-256-CBC(body) | HMAC-SHA256(32)
// The HMAC covers the version byte and the ciphertext.  A record sealed by another wallet
// fails the HMAC; a record from before sealing fails the version check.  Neither is migrated.
#define REGISTRATION_SEALED_LEN(body_len) (sizeof(uint8_t) + AES_ENCRYPTED_LEN(body_len) + HMAC_SHA256_LEN)

// The shortest well-formed record: version, IV, one padding block, HMAC
#define REGISTRATION_MIN_SEALED_LEN REGISTRATION_SEALED_LEN(0)

// Seal 'body' into 'output' (output_len must be exactly REGISTRATION_SEALED_LEN(body_len))
WARN_UNUSED_RESULT bool registration_seal(
    uint8_t version, const uint8_t* body, size_t body_len, uint8_t* output, size_t output_len);

// Verify and open a sealed record into 'body' (body_len is the capacity; 'written' the body size).
// Returns false, never asserts, for a record of the wrong length, HMAC or version.
WARN_UNUSED_RESULT bool registration_open(uint8_t expected_version, const uint8_t* bytes, size_t bytes_len,
    uint8_t* body, size_t body_len, size_t* written);

#endif /* REGISTRATION_SEAL_H_ */
