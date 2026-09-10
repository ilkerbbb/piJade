#ifndef JADE_SEEDQR_H_
#define JADE_SEEDQR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jade_assert.h"

// BBB-AIRGAP: Standard SeedQR (SeedSigner) digit-string generation.
// The output is four zero-padded digits per BIP39 word index, no separators:
// 48 characters for 12 words, 96 for 24.  It is derived from the entropy per the
// BIP39 definition (11-bit slices of entropy || SHA256(entropy)), not by looking
// words up in a wordlist.
//
// NOTE: the output is seed-equivalent and therefore SECRET.  The caller must
// SENSITIVE_PUSH its own buffer and wipe it when done.
//
// entropy_len must be 16 or 32; digits_len must leave room for the NUL.
// 'written' receives the number of digits excluding the NUL.
WARN_UNUSED_RESULT bool seedqr_digits_from_entropy(
    const uint8_t* entropy, size_t entropy_len, char* digits_out, size_t digits_len, size_t* written);

#endif /* JADE_SEEDQR_H_ */
