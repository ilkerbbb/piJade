#ifndef SLIP39_H_
#define SLIP39_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// BBB-AIRGAP: SLIP-0039 share recovery.  Recovery only - this fork never splits a secret
// into shares, see local-docs C2 design note section 2.1.  The reference implementation this
// is measured against is Trezor's core/src/trezor/crypto/slip39.py; the Shamir interpolation
// itself is the vendored main/shamir.c that reference calls into.

#define SLIP39_WORD_COUNT 1024

// SLIP-0039 caps both at 16.
#define SLIP39_MAX_GROUPS 16
#define SLIP39_MAX_MEMBERS 16

// Only 128 and 256 bit master secrets are accepted, ie 20 and 33 word shares.  The format
// allows other lengths, but the resulting master secret would not be a length
// bip32_key_from_seed() accepts, so such a backup could not be turned into a wallet here.
// See the C2 design note section 6.3.
#define SLIP39_SHORT_SHARE_WORDS 20
#define SLIP39_LONG_SHARE_WORDS 33
#define SLIP39_SHARE_VALUE_MAX 32
#define SLIP39_MASTER_SECRET_MAX 32

// The wordlist is vendored in main/slip39_english.c.
extern const char* const SLIP39_WORDLIST[SLIP39_WORD_COUNT];

typedef enum {
    SLIP39_OK = 0,
    SLIP39_ERR_WORD, // a word is not in the SLIP-0039 wordlist
    SLIP39_ERR_LENGTH, // not 20 or 33 words
    SLIP39_ERR_CHECKSUM, // RS1024 checksum failed - a word is wrong
    SLIP39_ERR_FORMAT, // padding bits set, or group count below group threshold
    SLIP39_ERR_SET, // a valid share, but not from the backup already being collected
    SLIP39_ERR_DUPLICATE, // this exact share was entered before
    SLIP39_ERR_GROUP_FULL, // that group already has all the shares it needs
    SLIP39_ERR_GROUPS_FULL, // enough groups have been collected already
    SLIP39_ERR_INCOMPLETE, // combine called before enough shares were collected
    SLIP39_ERR_DIGEST, // the shares do not belong together
} slip39_err_t;

typedef struct {
    uint16_t identifier;
    uint8_t iteration_exponent;
    uint8_t group_index;
    uint8_t group_threshold;
    uint8_t group_count;
    uint8_t member_index;
    uint8_t member_threshold;
    bool extendable;
    uint8_t value_len;
    uint8_t value[SLIP39_SHARE_VALUE_MAX];
} slip39_share_t;

typedef struct {
    uint8_t threshold; // member threshold of this group; 0 until its first share arrives
    uint8_t count;
    uint8_t index[SLIP39_MAX_MEMBERS];
    uint8_t value[SLIP39_MAX_MEMBERS][SLIP39_SHARE_VALUE_MAX];
} slip39_group_t;

// Collects shares of one backup.  Holds share values, so it is secret: callers zero it with
// slip39_clear() and keep it under SENSITIVE_PUSH/POP or free it through that path.
typedef struct {
    bool started;
    uint16_t identifier;
    bool extendable;
    uint8_t iteration_exponent;
    uint8_t group_threshold;
    uint8_t group_count;
    uint8_t value_len;
    uint8_t groups_started; // groups with at least one share
    uint8_t groups_complete; // groups that have reached their member threshold
    slip39_group_t groups[SLIP39_MAX_GROUPS];
} slip39_ctx_t;

// Decodes one share mnemonic: wordlist lookup, RS1024 checksum, padding and metadata.
// Says nothing about whether the share fits any other share; that is slip39_add_share().
slip39_err_t slip39_parse_share(const char* mnemonic, size_t mnemonic_len, slip39_share_t* share);

// Adds a parsed share to the set being collected, rejecting anything that does not belong.
void slip39_init(slip39_ctx_t* ctx);
slip39_err_t slip39_add_share(slip39_ctx_t* ctx, const slip39_share_t* share);
bool slip39_is_complete(const slip39_ctx_t* ctx);
void slip39_clear(slip39_ctx_t* ctx);

// Recovers the master secret.  'passphrase' is applied here, not by any caller: in SLIP-0039
// the passphrase is part of decryption, and a wrong one yields a different master secret
// rather than an error, see the C2 design note section 4.2.  Writes value_len bytes.
slip39_err_t slip39_combine(
    const slip39_ctx_t* ctx, const char* passphrase, uint8_t* master_secret, size_t master_secret_len);

// Short screen text for a rejection reason.
const char* slip39_error_message(slip39_err_t err);

// Exact-match lookup in the SLIP-0039 wordlist.
bool slip39_word_index(const char* word, size_t word_len, uint16_t* index);

#endif /* SLIP39_H_ */
