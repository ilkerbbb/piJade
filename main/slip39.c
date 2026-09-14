#ifndef AMALGAMATED_BUILD
#include "slip39.h"
#include "jade_assert.h"
#include "jade_wally_verify.h"
#include "keychain.h"
#include "sensitive.h"
#include "shamir.h"

#include <string.h>

#include <sodium/utils.h>
#include <wally_core.h>
#include <wally_crypto.h>

// BBB-AIRGAP: SLIP-0039 recovery.  Every constant and every rejection below is measured
// against Trezor's reference implementation, vendor-audit/trezor at
// 7ab83a2af010e61a53298938112a4e8f850f0d8c, core/src/trezor/crypto/slip39.py; the function
// names in the comments are that file's.  No check beyond the reference's is added, with one
// exception noted at slip39_add_share().
//
// That reference sits under core/, which the monorepo's LICENSE.md makes GPLv3 by default,
// but it carries MIT in its own header (Andrew R. Kozlik, 2018) and the header decides.  It
// is read here, not copied: this is C written against it, and the only Trezor code taken
// verbatim is main/shamir.c and main/slip39_english.c, both MIT out of crypto/.

#define RADIX_BITS 10
#define ID_EXP_LENGTH_WORDS 2
#define CHECKSUM_LENGTH_WORDS 3
#define METADATA_LENGTH_WORDS (ID_EXP_LENGTH_WORDS + 2 + CHECKSUM_LENGTH_WORDS)
#define ITERATION_EXP_LENGTH_BITS 4
#define DIGEST_LENGTH_BYTES 4
#define BASE_ITERATION_COUNT 10000
#define ROUND_COUNT 4
#define SECRET_INDEX 255
#define DIGEST_INDEX 254

// The customization string is also the salt prefix for non-extendable backups (_get_salt()).
static const char CUSTOMIZATION_ORIG[] = "shamir";
static const char CUSTOMIZATION_EXTENDABLE[] = "shamir_extendable";
#define CUSTOMIZATION_ORIG_LEN (sizeof(CUSTOMIZATION_ORIG) - 1)
#define SALT_PREFIX_MAX_LEN (CUSTOMIZATION_ORIG_LEN + 2)

// _rs1024_polymod(), fed the customization string first as _rs1024_verify_checksum() does.
static uint32_t rs1024_polymod(
    const char* customization, const size_t customization_len, const uint16_t* values, const size_t values_len)
{
    static const uint32_t GEN[10] = { 0x00E0E040, 0x01C1C080, 0x03838100, 0x07070200, 0x0E0E0009, 0x1C0C2412,
        0x38086C24, 0x3090FC48, 0x21B1F890, 0x03F3F120 };

    uint32_t chk = 1;
    for (size_t i = 0; i < customization_len + values_len; ++i) {
        const uint32_t v = i < customization_len ? (uint8_t)customization[i] : values[i - customization_len];
        const uint32_t b = chk >> 20;
        chk = ((chk & 0x000FFFFF) << RADIX_BITS) ^ v;
        for (size_t j = 0; j < 10; ++j) {
            if ((b >> j) & 1) {
                chk ^= GEN[j];
            }
        }
    }
    return chk;
}

// Wordlist entries are lowercase ascii; the reference lowercases the input word before lookup
// (_mnemonic_to_indices()), and a QR code in alphanumeric mode can only carry uppercase.
static int wordlist_cmp(const char* entry, const char* word, const size_t word_len)
{
    for (size_t i = 0; i < word_len; ++i) {
        const unsigned char e = (unsigned char)entry[i];
        unsigned char c = (unsigned char)word[i];
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        if (e != c) {
            return e < c ? -1 : 1;
        }
        if (!e) {
            // Both ended early; treat the entry as the shorter one rather than read past it.
            return -1;
        }
    }
    return entry[word_len] ? 1 : 0;
}

bool slip39_word_index(const char* word, const size_t word_len, uint16_t* index)
{
    JADE_ASSERT(word);
    JADE_ASSERT(index);

    if (!word_len) {
        return false;
    }

    size_t lo = 0;
    size_t hi = SLIP39_WORD_COUNT;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int cmp = wordlist_cmp(SLIP39_WORDLIST[mid], word, word_len);
        if (!cmp) {
            *index = (uint16_t)mid;
            return true;
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

// decode_mnemonic()
slip39_err_t slip39_parse_share(const char* mnemonic, const size_t mnemonic_len, slip39_share_t* share)
{
    JADE_ASSERT(mnemonic);
    JADE_ASSERT(mnemonic_len);
    JADE_ASSERT(share);

    memset(share, 0, sizeof(slip39_share_t));

    uint16_t indices[SLIP39_LONG_SHARE_WORDS];
    SENSITIVE_PUSH(indices, sizeof(indices));

    slip39_err_t rc = SLIP39_OK;
    size_t num_words = 0;
    // Any control character or space separates words, as the reference's str.split() does;
    // a share arriving as a text QR can be newline separated.
    for (size_t pos = 0; pos < mnemonic_len; /* advanced below */) {
        if ((unsigned char)mnemonic[pos] <= ' ') {
            ++pos;
            continue;
        }
        size_t end = pos;
        while (end < mnemonic_len && (unsigned char)mnemonic[end] > ' ') {
            ++end;
        }
        if (num_words == SLIP39_LONG_SHARE_WORDS) {
            rc = SLIP39_ERR_LENGTH;
            goto cleanup;
        }
        if (!slip39_word_index(mnemonic + pos, end - pos, &indices[num_words])) {
            rc = SLIP39_ERR_WORD;
            goto cleanup;
        }
        ++num_words;
        pos = end;
    }

    // The format allows other lengths; this fork does not, see slip39.h.
    if (num_words != SLIP39_SHORT_SHARE_WORDS && num_words != SLIP39_LONG_SHARE_WORDS) {
        rc = SLIP39_ERR_LENGTH;
        goto cleanup;
    }

    // The extendable flag selects the customization string, so it is read before the checksum.
    const uint32_t id_exp = ((uint32_t)indices[0] << RADIX_BITS) | indices[1];
    share->identifier = (uint16_t)(id_exp >> (1 + ITERATION_EXP_LENGTH_BITS));
    share->extendable = (id_exp >> ITERATION_EXP_LENGTH_BITS) & 1;
    share->iteration_exponent = id_exp & ((1 << ITERATION_EXP_LENGTH_BITS) - 1);

    const char* const customization = share->extendable ? CUSTOMIZATION_EXTENDABLE : CUSTOMIZATION_ORIG;
    if (rs1024_polymod(customization, strlen(customization), indices, num_words) != 1) {
        rc = SLIP39_ERR_CHECKSUM;
        goto cleanup;
    }

    // Five 4-bit fields packed into the next two words, big endian (_int_to_indices()).
    const uint32_t meta = ((uint32_t)indices[ID_EXP_LENGTH_WORDS] << RADIX_BITS) | indices[ID_EXP_LENGTH_WORDS + 1];
    const uint8_t group_threshold = (meta >> 12) & 0x0F;
    const uint8_t group_count = (meta >> 8) & 0x0F;
    if (group_count < group_threshold) {
        rc = SLIP39_ERR_FORMAT;
        goto cleanup;
    }
    share->group_index = (meta >> 16) & 0x0F;
    share->group_threshold = group_threshold + 1;
    share->group_count = group_count + 1;
    share->member_index = (meta >> 4) & 0x0F;
    share->member_threshold = (meta & 0x0F) + 1;

    const size_t value_words = num_words - METADATA_LENGTH_WORDS;
    const size_t padding_len = (RADIX_BITS * value_words) % 16;
    const size_t first = ID_EXP_LENGTH_WORDS + 2;
    if (indices[first] >= (1u << (RADIX_BITS - padding_len))) {
        rc = SLIP39_ERR_FORMAT;
        goto cleanup;
    }

    uint64_t acc = 0;
    size_t bits = 0;
    size_t out = 0;
    for (size_t i = 0; i < value_words; ++i) {
        // The padding bits of the first word were just checked to be zero, so no mask is needed.
        const size_t nbits = i ? RADIX_BITS : RADIX_BITS - padding_len;
        acc = (acc << nbits) | indices[first + i];
        bits += nbits;
        while (bits >= 8) {
            bits -= 8;
            share->value[out++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    JADE_ASSERT(!bits);
    JADE_ASSERT(out && out <= SLIP39_SHARE_VALUE_MAX);
    share->value_len = (uint8_t)out;

cleanup:
    if (rc != SLIP39_OK) {
        JADE_WALLY_VERIFY(wally_bzero(share, sizeof(slip39_share_t)));
    }
    SENSITIVE_POP(indices);
    return rc;
}

void slip39_init(slip39_ctx_t* ctx)
{
    JADE_ASSERT(ctx);
    memset(ctx, 0, sizeof(slip39_ctx_t));
}

void slip39_clear(slip39_ctx_t* ctx)
{
    JADE_ASSERT(ctx);
    JADE_WALLY_VERIFY(wally_bzero(ctx, sizeof(slip39_ctx_t)));
}

// _decode_mnemonics() does these consistency checks over the whole list at once; doing them as
// each share arrives gives the same set, and tells the user which share was the odd one out.
slip39_err_t slip39_add_share(slip39_ctx_t* ctx, const slip39_share_t* share)
{
    JADE_ASSERT(ctx);
    JADE_ASSERT(share);
    JADE_ASSERT(share->value_len && share->value_len <= SLIP39_SHARE_VALUE_MAX);
    JADE_ASSERT(share->group_index < SLIP39_MAX_GROUPS);
    JADE_ASSERT(share->member_index < SLIP39_MAX_MEMBERS);
    JADE_ASSERT(share->member_threshold && share->member_threshold <= SLIP39_MAX_MEMBERS);

    if (!ctx->started) {
        ctx->identifier = share->identifier;
        ctx->extendable = share->extendable;
        ctx->iteration_exponent = share->iteration_exponent;
        ctx->group_threshold = share->group_threshold;
        ctx->group_count = share->group_count;
        ctx->value_len = share->value_len;
        ctx->started = true;
    } else if (ctx->identifier != share->identifier || ctx->extendable != share->extendable
        || ctx->iteration_exponent != share->iteration_exponent || ctx->group_threshold != share->group_threshold
        || ctx->group_count != share->group_count || ctx->value_len != share->value_len) {
        // The value length is the one check the reference leaves to a trezorcrypto assertion.
        return SLIP39_ERR_SET;
    }

    slip39_group_t* const group = &ctx->groups[share->group_index];
    if (group->count && group->threshold != share->member_threshold) {
        return SLIP39_ERR_SET;
    }
    for (uint8_t i = 0; i < group->count; ++i) {
        if (group->index[i] == share->member_index) {
            // The reference collects shares into a set, so entering the same share twice is
            // dropped silently.  Saying so instead leaves the same set of shares behind, and a
            // screen that shows a share counter otherwise looks stuck.
            return memcmp(group->value[i], share->value, share->value_len) ? SLIP39_ERR_SET : SLIP39_ERR_DUPLICATE;
        }
    }
    if (group->count && group->count == group->threshold) {
        return SLIP39_ERR_GROUP_FULL;
    }
    if (!group->count && ctx->groups_started == ctx->group_threshold) {
        return SLIP39_ERR_GROUPS_FULL;
    }

    if (!group->count) {
        group->threshold = share->member_threshold;
        ++ctx->groups_started;
    }
    group->index[group->count] = share->member_index;
    memcpy(group->value[group->count], share->value, share->value_len);
    ++group->count;
    if (group->count == group->threshold) {
        ++ctx->groups_complete;
    }
    return SLIP39_OK;
}

bool slip39_is_complete(const slip39_ctx_t* ctx)
{
    JADE_ASSERT(ctx);
    return ctx->started && ctx->groups_complete == ctx->group_threshold;
}

// _recover_secret()
static slip39_err_t recover_secret(
    const uint8_t threshold, const uint8_t* indices, const uint8_t** values, const size_t len, uint8_t* secret)
{
    JADE_ASSERT(threshold);

    if (threshold == 1) {
        // A threshold of one carries no digest - that is the format's rule, not a relaxation.
        memcpy(secret, values[0], len);
        return SLIP39_OK;
    }

    uint8_t digest_share[SLIP39_SHARE_VALUE_MAX];
    uint8_t digest[HMAC_SHA256_LEN];
    SENSITIVE_PUSH(digest_share, sizeof(digest_share));
    SENSITIVE_PUSH(digest, sizeof(digest));

    slip39_err_t rc = SLIP39_ERR_DIGEST;
    if (shamir_interpolate(secret, SECRET_INDEX, indices, values, threshold, len)
        && shamir_interpolate(digest_share, DIGEST_INDEX, indices, values, threshold, len)) {
        // _create_digest(): the random part of the digest share keys the hmac, the recovered
        // secret is the message, and the first four bytes must match.
        JADE_WALLY_VERIFY(wally_hmac_sha256(
            digest_share + DIGEST_LENGTH_BYTES, len - DIGEST_LENGTH_BYTES, secret, len, digest, sizeof(digest)));
        if (!sodium_memcmp(digest, digest_share, DIGEST_LENGTH_BYTES)) {
            rc = SLIP39_OK;
        }
    }

    SENSITIVE_POP(digest);
    SENSITIVE_POP(digest_share);
    return rc;
}

// decrypt(): a four round Feistel cipher, run in reverse.  A wrong passphrase is not an error
// here, it simply yields a different master secret; see the C2 design note section 4.2.
static void decrypt_ems(const uint8_t* ems, const size_t len, const char* passphrase, const uint8_t exponent,
    const uint16_t identifier, const bool extendable, uint8_t* master_secret)
{
    JADE_ASSERT(ems);
    JADE_ASSERT(len && !(len % 2) && len <= SLIP39_MASTER_SECRET_MAX);
    JADE_ASSERT(master_secret);

    const size_t half = len / 2;
    const size_t passphrase_len = passphrase ? strlen(passphrase) : 0;
    JADE_ASSERT(passphrase_len <= PASSPHRASE_MAX_LEN);

    uint8_t pass[1 + PASSPHRASE_MAX_LEN];
    uint8_t salt[SALT_PREFIX_MAX_LEN + SLIP39_MASTER_SECRET_MAX / 2];
    uint8_t half_l[SLIP39_MASTER_SECRET_MAX / 2];
    uint8_t half_r[SLIP39_MASTER_SECRET_MAX / 2];
    uint8_t f[PBKDF2_HMAC_SHA256_LEN];
    SENSITIVE_PUSH(pass, sizeof(pass));
    SENSITIVE_PUSH(salt, sizeof(salt));
    SENSITIVE_PUSH(half_l, sizeof(half_l));
    SENSITIVE_PUSH(half_r, sizeof(half_r));
    SENSITIVE_PUSH(f, sizeof(f));

    // _get_salt(): empty for extendable backups, otherwise "shamir" and the 15 bit identifier.
    size_t salt_prefix_len = 0;
    if (!extendable) {
        memcpy(salt, CUSTOMIZATION_ORIG, CUSTOMIZATION_ORIG_LEN);
        salt[CUSTOMIZATION_ORIG_LEN] = (uint8_t)(identifier >> 8);
        salt[CUSTOMIZATION_ORIG_LEN + 1] = (uint8_t)(identifier & 0xFF);
        salt_prefix_len = SALT_PREFIX_MAX_LEN;
    }
    if (passphrase_len) {
        memcpy(pass + 1, passphrase, passphrase_len);
    }

    // _round_function().  wally always writes a full 32 byte block, and the first block of
    // PBKDF2 does not depend on the requested output length, so taking its first 'half' bytes
    // is the reference's .key()[:len(r)].
    const uint32_t cost = ((uint32_t)BASE_ITERATION_COUNT << exponent) / ROUND_COUNT;

    memcpy(half_l, ems, half);
    memcpy(half_r, ems + half, half);
    for (uint8_t round = ROUND_COUNT; round > 0; --round) {
        pass[0] = round - 1;
        memcpy(salt + salt_prefix_len, half_r, half);
        JADE_WALLY_VERIFY(
            wally_pbkdf2_hmac_sha256(pass, 1 + passphrase_len, salt, salt_prefix_len + half, 0, cost, f, sizeof(f)));
        for (size_t i = 0; i < half; ++i) {
            f[i] ^= half_l[i];
        }
        memcpy(half_l, half_r, half);
        memcpy(half_r, f, half);
    }
    memcpy(master_secret, half_r, half);
    memcpy(master_secret + half, half_l, half);

    SENSITIVE_POP(f);
    SENSITIVE_POP(half_r);
    SENSITIVE_POP(half_l);
    SENSITIVE_POP(salt);
    SENSITIVE_POP(pass);
}

// recover_ems() followed by decrypt()
slip39_err_t slip39_combine(
    const slip39_ctx_t* ctx, const char* passphrase, uint8_t* master_secret, const size_t master_secret_len)
{
    JADE_ASSERT(ctx);
    JADE_ASSERT(master_secret);

    if (!slip39_is_complete(ctx)) {
        return SLIP39_ERR_INCOMPLETE;
    }
    JADE_ASSERT(master_secret_len == ctx->value_len);

    uint8_t group_indices[SLIP39_MAX_GROUPS];
    uint8_t group_secrets[SLIP39_MAX_GROUPS][SLIP39_SHARE_VALUE_MAX];
    const uint8_t* group_values[SLIP39_MAX_GROUPS];
    uint8_t ems[SLIP39_SHARE_VALUE_MAX];
    SENSITIVE_PUSH(group_secrets, sizeof(group_secrets));
    SENSITIVE_PUSH(ems, sizeof(ems));

    slip39_err_t rc = SLIP39_OK;
    uint8_t num_groups = 0;
    for (uint8_t g = 0; g < SLIP39_MAX_GROUPS && rc == SLIP39_OK; ++g) {
        const slip39_group_t* const group = &ctx->groups[g];
        if (!group->count) {
            continue;
        }
        JADE_ASSERT(group->count == group->threshold);

        const uint8_t* member_values[SLIP39_MAX_MEMBERS];
        for (uint8_t m = 0; m < group->count; ++m) {
            member_values[m] = group->value[m];
        }
        rc = recover_secret(group->threshold, group->index, member_values, ctx->value_len, group_secrets[num_groups]);
        group_indices[num_groups] = g;
        group_values[num_groups] = group_secrets[num_groups];
        ++num_groups;
    }

    if (rc == SLIP39_OK) {
        JADE_ASSERT(num_groups == ctx->group_threshold);
        rc = recover_secret(ctx->group_threshold, group_indices, group_values, ctx->value_len, ems);
    }
    if (rc == SLIP39_OK) {
        decrypt_ems(
            ems, ctx->value_len, passphrase, ctx->iteration_exponent, ctx->identifier, ctx->extendable, master_secret);
    }

    SENSITIVE_POP(ems);
    SENSITIVE_POP(group_secrets);
    return rc;
}

const char* slip39_error_message(const slip39_err_t err)
{
    switch (err) {
    case SLIP39_OK:
        return "OK";
    case SLIP39_ERR_WORD:
        return "Not a SLIP39 word";
    case SLIP39_ERR_LENGTH:
        return "Must be 20 or 33 words";
    case SLIP39_ERR_CHECKSUM:
        return "Checksum failed - check the words";
    case SLIP39_ERR_FORMAT:
        return "Invalid SLIP39 share";
    case SLIP39_ERR_SET:
        return "Not from this backup";
    case SLIP39_ERR_DUPLICATE:
        return "Share already entered";
    case SLIP39_ERR_GROUP_FULL:
        return "That group is full";
    case SLIP39_ERR_GROUPS_FULL:
        return "Enough groups already entered";
    case SLIP39_ERR_INCOMPLETE:
        return "More shares needed";
    case SLIP39_ERR_DIGEST:
        return "Shares do not match";
    }
    return "Unknown error";
}
#endif /* AMALGAMATED_BUILD */
