#include <string.h>

#include "seedxor.h"
#include "selfcheck.h"

#include <wally_bip39.h>
#include <wally_core.h>

// BBB-AIRGAP: vectors for the Seed XOR core (main/seedxor.c).  The entropies below are published
// BIP39 test vectors, so nothing here is anyone's wallet; the emulator can be handed them without
// a secret ever reaching this machine.  The split flow cannot be measured end to end on screen -
// the harness cannot read words back off the display and type them in again - so the round trip is
// measured here instead, against the same functions the screens call.

// Published BIP39 test entropies.  A and B are chosen so that their xor is a value that can be
// checked by eye, and C is an irregular one so that the three-part cases are not all symmetric.
static const uint8_t ENT_A16[16]
    = { 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f };
static const uint8_t ENT_B16[16]
    = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
static const uint8_t ENT_C16[16]
    = { 0x77, 0xc2, 0xb0, 0x07, 0x16, 0xce, 0xc7, 0x21, 0x38, 0x39, 0x15, 0x9e, 0x40, 0x4d, 0xb5, 0x0d };
static const uint8_t ENT_A32[32] = { 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f };

static const uint8_t A_XOR_B[16]
    = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t A_XOR_B_XOR_C[16]
    = { 0x88, 0x3d, 0x4f, 0xf8, 0xe9, 0x31, 0x38, 0xde, 0xc7, 0xc6, 0xea, 0x61, 0xbf, 0xb2, 0x4a, 0xf2 };

// The 12-word phrase for 16 zero bytes, ie. the wallet a part combined with itself gives.  This is
// the published all-zero BIP39 vector and the emulator's own test wallet; it is written out here
// because it is the one phrase that is public by construction.
static const char ZERO_MNEMONIC[]
    = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

static bool test_accumulate(void)
{
    uint8_t total[16] = { 0 };
    seedxor_accumulate(total, ENT_A16, sizeof(ENT_A16));
    seedxor_accumulate(total, ENT_B16, sizeof(ENT_B16));
    if (memcmp(total, A_XOR_B, sizeof(A_XOR_B))) {
        return false;
    }

    // A part combined with itself cancels: this is what the combine screen warns about.
    memset(total, 0, sizeof(total));
    seedxor_accumulate(total, ENT_C16, sizeof(ENT_C16));
    seedxor_accumulate(total, ENT_C16, sizeof(ENT_C16));
    return seedxor_is_zero(total, sizeof(total));
}

static bool test_order_independent(void)
{
    uint8_t forwards[16] = { 0 };
    seedxor_accumulate(forwards, ENT_A16, sizeof(ENT_A16));
    seedxor_accumulate(forwards, ENT_B16, sizeof(ENT_B16));
    seedxor_accumulate(forwards, ENT_C16, sizeof(ENT_C16));

    uint8_t backwards[16] = { 0 };
    seedxor_accumulate(backwards, ENT_C16, sizeof(ENT_C16));
    seedxor_accumulate(backwards, ENT_A16, sizeof(ENT_A16));
    seedxor_accumulate(backwards, ENT_B16, sizeof(ENT_B16));

    return !memcmp(forwards, A_XOR_B_XOR_C, sizeof(A_XOR_B_XOR_C))
        && !memcmp(backwards, A_XOR_B_XOR_C, sizeof(A_XOR_B_XOR_C));
}

static bool test_is_zero(void)
{
    uint8_t buf[32] = { 0 };
    if (!seedxor_is_zero(buf, sizeof(buf))) {
        return false;
    }

    // One bit set in turn at every position: a scan that stops early reports zero for the later
    // ones and passes a cancelled set off as a wallet.
    for (size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = 0x01;
        if (seedxor_is_zero(buf, sizeof(buf))) {
            return false;
        }
        buf[i] = 0x00;
    }
    return true;
}

static bool test_split_roundtrip(const uint8_t* entropy, const size_t entropy_len, const size_t num_parts)
{
    uint8_t parts[SEEDXOR_MAX_SPLIT_PARTS * SEEDXOR_MAX_PART_LEN];
    if (!seedxor_split(entropy, entropy_len, num_parts, parts)) {
        return false;
    }

    uint8_t total[SEEDXOR_MAX_PART_LEN] = { 0 };
    for (size_t i = 0; i < num_parts; ++i) {
        seedxor_accumulate(total, parts + (i * entropy_len), entropy_len);
    }
    if (memcmp(total, entropy, entropy_len)) {
        return false;
    }

    // No part may be the seed itself, and no two parts may be equal.  Both would still recombine,
    // so the round trip above does not see them; with the device rng behind the parts the odds of
    // a false failure here are those of guessing the seed.
    for (size_t i = 0; i < num_parts; ++i) {
        const uint8_t* const part = parts + (i * entropy_len);
        if (!memcmp(part, entropy, entropy_len)) {
            return false;
        }
        for (size_t j = i + 1; j < num_parts; ++j) {
            if (!memcmp(part, parts + (j * entropy_len), entropy_len)) {
                return false;
            }
        }
    }
    return true;
}

static bool test_split_every_shape(void)
{
    for (size_t num_parts = SEEDXOR_MIN_SPLIT_PARTS; num_parts <= SEEDXOR_MAX_SPLIT_PARTS; ++num_parts) {
        if (!test_split_roundtrip(ENT_C16, sizeof(ENT_C16), num_parts)
            || !test_split_roundtrip(ENT_A32, sizeof(ENT_A32), num_parts)) {
            return false;
        }
    }
    return true;
}

static bool test_cancelled_pair_is_the_zero_wallet(void)
{
    // The discriminating vector: combining a part with itself must give the all-zero wallet, not
    // the part's own wallet.  An implementation that kept the first part instead of xoring would
    // pass every test above that only looks at bytes, and fail here.
    uint8_t total[16] = { 0 };
    seedxor_accumulate(total, ENT_C16, sizeof(ENT_C16));
    seedxor_accumulate(total, ENT_C16, sizeof(ENT_C16));

    char* mnemonic = NULL;
    if (bip39_mnemonic_from_bytes(NULL, total, sizeof(total), &mnemonic) != WALLY_OK || !mnemonic) {
        return false;
    }
    const bool matches = !strcmp(mnemonic, ZERO_MNEMONIC);
    wally_free_string(mnemonic);
    return matches;
}

bool test_seedxor(void)
{
    JADE_LOGI("Testing SeedXOR split and combine");

    return test_accumulate() && test_order_independent() && test_is_zero() && test_split_every_shape()
        && test_cancelled_pair_is_the_zero_wallet();
}

bool debug_selfcheck(jade_process_t* process)
{
    if (!test_seedxor()) {
        FAIL();
    }
    return true;
}
