#ifndef KEYCHAIN_H_
#define KEYCHAIN_H_

#include "jade_assert.h"
#include "utils/network.h"

#include <stdbool.h>
#include <wally_bip32.h>
#include <wally_crypto.h>

#define PASSPHRASE_MAX_LEN 100
#define GASERVICE_PATH_LEN (HMAC_SHA512_LEN / 2)

typedef struct {
    uint32_t gaservice_path[GASERVICE_PATH_LEN];
    uint8_t master_unblinding_key[HMAC_SHA512_LEN];
    uint8_t seed[BIP32_ENTROPY_LEN_512];
    struct ext_key xpriv;
    struct ext_key cached_gaservice_main_root;
    struct ext_key cached_gaservice_subact_root;
    const struct ext_key* cached_service;
    size_t seed_len;
} keychain_t;

typedef enum { PASSPHRASE_NEVER, PASSPHRASE_ONCE, PASSPHRASE_ALWAYS } passphrase_freq_t;
typedef enum { PASSPHRASE_WORDLIST, PASSPHRASE_FREETEXT } passphrase_type_t;

void keychain_init_cache(void);
void keychain_set(const keychain_t* src, uint8_t userdata, bool temporary);

// BBB-AIRGAP: keychain_set() is the single-wallet entry point - it drops every wallet held.  These
// work on the slot table instead: a wallet is loaded alongside the ones already in memory, into the
// first free slot, and becomes the wallet in use.  keychain_get() keeps its meaning throughout, it
// returns the wallet in use.  Loading fails only when the table is full.
bool keychain_has_free_slot(void);
WARN_UNUSED_RESULT bool keychain_load_into_free_slot(const keychain_t* src, uint8_t userdata, bool temporary);

// BBB-AIRGAP: how many wallets can be held in memory at once. Out here because a caller building
// a screen of them needs the bound at compile time; the slots themselves stay in keychain.c.
#define MAX_SEED_SLOTS 8

// BBB-AIRGAP: the wallets held, as the user sees them - addressed by position in that list, from
// zero to keychain_slot_count() - 1. Activating one only changes which wallet keychain_get()
// returns; nothing is loaded, dropped or written to storage.
size_t keychain_slot_count(void);
void keychain_slot_fingerprint(size_t position, uint8_t* output, size_t output_len);

// BBB-AIRGAP: is this derived wallet one of the wallets held?  On true 'position_out' is where it
// sits in that same list.  The comparison is on the master key rather than on the fingerprint the
// screens show: a fingerprint is four bytes of a hash, a collision can be searched for on purpose,
// and answering a scanned wallet with a different one that merely displays the same is the failure
// that identity check has to rule out.
bool keychain_find_slot(const keychain_t* candidate, size_t* position_out);
bool keychain_slot_is_temporary(size_t position);
void keychain_slot_activate(size_t position);

// BBB-AIRGAP: drops the wallet in use and hands over to another one held, or empties the table if
// it was the last.  Only the wallet in use can be forgotten, so a caller does not have to hold a
// position that could go stale while the screen offering this is open.
void keychain_slot_forget_active(void);

// BBB-AIRGAP: SeedQR export.  Upstream offers export once, during setup, while the mnemonic the
// user typed is still on the stack (main/process/mnemonic.c); afterwards the words are gone and
// the wallet cannot be drawn again.  These three calls give the wallet in use a place to keep the
// entropy it was built from, so the same export screens can be reached from the session menu.
// Only a wallet whose words were presented in this session has entropy: a persisted wallet read
// back from the blob holds a serialised xpriv, and no mnemonic can be recovered from that.
void keychain_set_entropy(const char* mnemonic);
bool keychain_slot_has_entropy(size_t position);
// Returns false when the wallet in use has no entropy.  On true the caller owns the string and
// must free it with wally_free_string().
WARN_UNUSED_RESULT bool keychain_export_mnemonic(char** mnemonic);

// BBB-AIRGAP: keychain_get_userdata() answers for the wallet in use.  This answers for the
// persisted wallet whichever wallet is in use, which is what the connection-lost guard has to ask
// while a temporary wallet is loaded on top of a PIN-unlocked one.  Returns 0 (SOURCE_NONE) when
// no persisted wallet is held, or when it is not associated with a connection yet.
uint8_t keychain_get_persisted_userdata(void);
const struct ext_key* keychain_cached_service(const struct ext_key* service, bool subaccount_root);
void keychain_clear(void);

const keychain_t* keychain_get(void);
bool keychain_requires_passphrase(void);

// key flags
void keychain_set_passphrase_frequency(passphrase_freq_t freq);
passphrase_freq_t keychain_get_passphrase_freq();
void keychain_set_passphrase_type(passphrase_type_t type);
passphrase_type_t keychain_get_passphrase_type();
void keychain_set_confirm_export_blinding_key(const bool confirm_export);
bool keychain_get_confirm_export_blinding_key(void);
void keychain_persist_key_flags(void);

void keychain_set_temporary(void);
bool keychain_has_temporary(void);
uint8_t keychain_get_userdata(void);

// Temporarily cache mnemonic entropy (if using passphrase)
void keychain_cache_mnemonic_entropy(const char* mnemonic);

// Clear/set/get/compare the pinned/restricted network type
void keychain_clear_network_type_restriction(void);
void keychain_set_network_type_restriction(const network_type_t network_type);
network_type_t keychain_get_network_type_restriction(void);
bool keychain_is_network_id_consistent(const network_t network_id);
bool keychain_is_network_type_consistent(const network_type_t network_type);

// mnemonic returned should be freed by caller with wally_free_string
void keychain_get_new_mnemonic(char** mnemonic, size_t nwords);
WARN_UNUSED_RESULT bool keychain_get_new_privatekey(uint8_t* privatekey, size_t size);

bool keychain_has_pin(void);
uint8_t keychain_pin_attempts_remaining(void);
WARN_UNUSED_RESULT bool keychain_erase_encrypted(void);

void keychain_derive_from_seed(const uint8_t* seed, size_t seed_len, keychain_t* keydata);
WARN_UNUSED_RESULT bool keychain_derive_from_mnemonic(
    const char* mnemonic, const char* passphrase, keychain_t* keydata);
WARN_UNUSED_RESULT bool keychain_complete_derivation_with_passphrase(const char* passphrase);

WARN_UNUSED_RESULT bool keychain_store(const uint8_t* aeskey, size_t aeslen);
WARN_UNUSED_RESULT bool keychain_load(const uint8_t* aeskey, size_t aeslen);
WARN_UNUSED_RESULT bool keychain_reencrypt(
    const uint8_t* curr_aeskey, size_t curr_aeslen, const uint8_t* new_aeskey, size_t new_aeslen);

#endif /* KEYCHAIN_H_ */
