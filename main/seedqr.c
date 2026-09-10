#ifndef AMALGAMATED_BUILD

#include "seedqr.h"
#include "jade_assert.h"
#include "jade_wally_verify.h"
#include "sensitive.h"

#include <wally_bip32.h>
#include <wally_crypto.h>

#include <stdio.h>
#include <string.h>

// Number of bits in a BIP39 word index
#define SEEDQR_INDEX_BITS 11

// Digits emitted per word index, plus the NUL that snprintf() writes
#define SEEDQR_DIGITS_PER_WORD 4

bool seedqr_digits_from_entropy(const uint8_t* entropy, const size_t entropy_len, char* digits_out,
    const size_t digits_len, size_t* written)
{
    if (!entropy || !digits_out || !written) {
        return false;
    }
    if (entropy_len != BIP32_ENTROPY_LEN_128 && entropy_len != BIP32_ENTROPY_LEN_256) {
        return false;
    }

    // 16 bytes -> 12 words, 32 bytes -> 24 words
    const size_t num_words = entropy_len * 3 / 4;
    const size_t num_digits = num_words * SEEDQR_DIGITS_PER_WORD;
    if (digits_len < num_digits + 1) {
        return false;
    }

    // The final index of a 24-word phrase starts at bit 253 and so reads work[31..33],
    // making 34 the smallest safe size.  35 is deliberately conservative; the whole
    // buffer is wiped on the way out either way.
    uint8_t work[35] = { 0 };
    uint8_t hash[SHA256_LEN];
    SENSITIVE_PUSH(work, sizeof(work));
    SENSITIVE_PUSH(hash, sizeof(hash));

    // BIP39: the checksum is the first (entropy_len / 4) bits of SHA256(entropy), appended
    // to the entropy.  Only hash[0] is ever consumed (4 bits for 12 words, 8 for 24).
    JADE_WALLY_VERIFY(wally_sha256(entropy, entropy_len, hash, sizeof(hash)));
    memcpy(work, entropy, entropy_len);
    work[entropy_len] = hash[0];

    for (size_t i = 0; i < num_words; ++i) {
        const size_t bit_offset = SEEDQR_INDEX_BITS * i;
        const size_t byte_offset = bit_offset / 8;

        // Read three bytes and right-align the 11 bits we want.  bit_offset % 8 is 0..7,
        // so the shift is 13..6 and never negative.
        const uint8_t shift = 24 - SEEDQR_INDEX_BITS - (bit_offset % 8);
        JADE_ASSERT(byte_offset + 2 < sizeof(work));

        const uint32_t triple = ((uint32_t)work[byte_offset] << 16) | ((uint32_t)work[byte_offset + 1] << 8)
            | (uint32_t)work[byte_offset + 2];
        const uint32_t index = (triple >> shift) & 0x7FF;

        char* const dst = digits_out + (i * SEEDQR_DIGITS_PER_WORD);
        const int nwritten = snprintf(dst, SEEDQR_DIGITS_PER_WORD + 1, "%04u", (unsigned int)index);
        JADE_ASSERT(nwritten == SEEDQR_DIGITS_PER_WORD);
    }

    // NOTE: SENSITIVE_POP wipes the buffer (main/sensitive.c), so no explicit bzero here.
    SENSITIVE_POP(hash);
    SENSITIVE_POP(work);

    *written = num_digits;
    return true;
}

#endif /* AMALGAMATED_BUILD */
