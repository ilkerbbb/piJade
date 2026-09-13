#ifndef AMALGAMATED_BUILD
#include "seedxor.h"
#include "jade_assert.h"
#include "jade_wally_verify.h"
#include "random.h"
#include "sensitive.h"

#include <string.h>

#include <sodium/utils.h>
#include <wally_core.h>

void seedxor_accumulate(uint8_t* total, const uint8_t* part, const size_t len)
{
    JADE_ASSERT(total);
    JADE_ASSERT(part);
    JADE_ASSERT(len);

    for (size_t i = 0; i < len; ++i) {
        total[i] ^= part[i];
    }
}

bool seedxor_is_zero(const uint8_t* entropy, const size_t len)
{
    JADE_ASSERT(entropy);
    JADE_ASSERT(len);

    uint8_t bits = 0;
    for (size_t i = 0; i < len; ++i) {
        bits |= entropy[i];
    }
    return bits == 0;
}

bool seedxor_split(const uint8_t* entropy, const size_t entropy_len, const size_t num_parts, uint8_t* parts_out)
{
    JADE_ASSERT(entropy);
    JADE_ASSERT(entropy_len);
    JADE_ASSERT(entropy_len <= SEEDXOR_MAX_PART_LEN);
    JADE_ASSERT(num_parts >= SEEDXOR_MIN_SPLIT_PARTS);
    JADE_ASSERT(num_parts <= SEEDXOR_MAX_SPLIT_PARTS);
    JADE_ASSERT(parts_out);

    // Every part but the last is random; the last is whatever closes the xor, so it is built by
    // starting from the seed and xoring each random part into it as that part is made.
    //
    // Coldcard offers a second mode that derives the random parts from the seed itself, so that
    // repeating the split shows the same words again.  It is not taken here: it ties the parts to
    // the seed, so one part plus the knowledge that the mode was used gives the others back, and
    // the string it hashes in is Coldcard's own, which buys this fork no compatibility.
    uint8_t* const last = parts_out + ((num_parts - 1) * entropy_len);
    memcpy(last, entropy, entropy_len);
    for (size_t i = 0; i < num_parts - 1; ++i) {
        uint8_t* const part = parts_out + (i * entropy_len);
        get_random(part, entropy_len);
        seedxor_accumulate(last, part, entropy_len);
    }

    // Measure this function rather than trust it: combine what it just wrote and compare.  A
    // caller that gets 'false' here has parts that do not rebuild the wallet, which is the one
    // failure that must never reach paper.
    uint8_t check[SEEDXOR_MAX_PART_LEN];
    SENSITIVE_PUSH(check, sizeof(check));
    memset(check, 0, sizeof(check));
    for (size_t i = 0; i < num_parts; ++i) {
        seedxor_accumulate(check, parts_out + (i * entropy_len), entropy_len);
    }
    const bool recombines = !sodium_memcmp(check, entropy, entropy_len);
    SENSITIVE_POP(check);

    if (!recombines) {
        JADE_LOGE("SeedXOR split did not recombine to the original entropy");
        JADE_WALLY_VERIFY(wally_bzero(parts_out, num_parts * entropy_len));
    }
    return recombines;
}

#endif /* AMALGAMATED_BUILD */
