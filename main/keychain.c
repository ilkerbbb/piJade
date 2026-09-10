#ifndef AMALGAMATED_BUILD
#include "keychain.h"
#include "aes.h"
#include "jade_assert.h"
#include "jade_wally_verify.h"
#include "random.h"
#include "sensitive.h"
#include "storage.h"
#include "utils/malloc_ext.h"
#include "utils/network.h"
#include "wallet.h"

#include <sodium/crypto_verify_32.h>
#include <sodium/utils.h>
#include <string.h>
#include <wally_bip39.h>
#include <wally_elements.h>

// Size of keydata_t elements - ext-key, ga-path, master-blinding-key
#define SERIALIZED_KEY_LEN (BIP32_SERIALIZED_LEN + HMAC_SHA512_LEN + HMAC_SHA512_LEN)

// Encrypted length plus hmac (input length given)
#define ENCRYPTED_DATA_LEN(len) (AES_ENCRYPTED_LEN(len) + HMAC_SHA256_LEN)

// BBB-AIRGAP: upstream holds one wallet in one static struct, so a second seed can only be
// loaded by overwriting the first.  The struct becomes a fixed table and 'keychain_data' points
// at the entry currently in use.  Nothing above this file changes: keychain_get() still returns
// that pointer, so every call site keeps reading "the wallet in use", and NULL still means "no
// wallet loaded".  The table stays static for the reason the single struct was static - a
// malloc'd block of this size is never freed and would fragment DRAM permanently.
typedef struct {
    keychain_t keydata;
    // BBB-AIRGAP: the entropy the wallet was built from, kept so the user can draw its SeedQR
    // later rather than only during setup.  It lives in the slot, not in a global, for two
    // measured reasons: the two wipes below already clear a slot in one call
    // (keychain_slot_forget_active, keychain_clear), so no new wipe path can be forgotten; and
    // keychain_set() is a no-op when copying from self, so the re-bind calls in auth_user.c and
    // dashboard.c leave it alone - the global mnemonic_entropy is cleared by finalise_slot() on
    // exactly those calls.  Zero length means this wallet cannot be exported, which is the case
    // for a persisted wallet read back from the blob: that holds a serialised xpriv, and a
    // mnemonic cannot be recovered from it.
    uint8_t entropy[BIP39_ENTROPY_LEN_256];
    size_t entropy_len;
    uint8_t userdata;
    bool temporary;
    bool in_use;
} keychain_slot_t;

// Internal variables - the keychain slots, and the one currently in use
static keychain_slot_t keychain_slots[MAX_SEED_SLOTS] = { 0 };
static size_t active_slot = 0; // only meaningful while keychain_data is set
static keychain_t* keychain_data = NULL;
static network_type_t network_type_restriction = NETWORK_TYPE_NONE;
static bool has_encrypted_blob = false;

// If using a passphrase we may need to cache the mnemonic entropy
// while the passphrase is entered and the wallet master key derived.
static uint8_t mnemonic_entropy[BIP39_ENTROPY_LEN_256]; // Maximum supported entropy is 24 words
static size_t mnemonic_entropy_len = 0;

// Cached key flags
static uint8_t key_flags = 0;

// Places the passed key data in the given free slot, and makes that the slot in use
static void occupy_slot(const size_t slot, const keychain_t* src)
{
    JADE_ASSERT(slot < MAX_SEED_SLOTS);
    JADE_ASSERT(src);

    active_slot = slot;
    keychain_slots[slot].in_use = true;
    keychain_data = &keychain_slots[slot].keydata;
    memcpy(keychain_data, src, sizeof(keychain_t));

    // BBB-AIRGAP: a wallet arriving in this slot has no entropy until the caller says otherwise,
    // so export starts off and is turned on deliberately by keychain_set_entropy().  The
    // slot is wiped when it is given up, so this is not clearing anything that should still be
    // there - it states the invariant rather than relying on every path that frees a slot.
    JADE_WALLY_VERIFY(wally_bzero(keychain_slots[slot].entropy, sizeof(keychain_slots[slot].entropy)));
    keychain_slots[slot].entropy_len = 0;
}

// Drops any cached mnemonic entropy and records what the slot in use is associated with
static void finalise_slot(const uint8_t userdata, const bool temporary)
{
    // Clear any mnemonic entropy we may have been holding
    JADE_WALLY_VERIFY(wally_bzero(mnemonic_entropy, sizeof(mnemonic_entropy)));
    mnemonic_entropy_len = 0;

    // Reload key flags
    key_flags = storage_get_key_flags();

    // Hold the associated userdata
    keychain_slots[active_slot].userdata = userdata;

    // Store whether this is intended to be a temporary keychain
    keychain_slots[active_slot].temporary = temporary;
}

void keychain_set(const keychain_t* src, const uint8_t userdata, const bool temporary)
{
    JADE_ASSERT(src);

    // Copy-from-self is no-op for keys (but we may override 'userdata' below)
    if (src != keychain_data) {
        // BBB-AIRGAP: this remains the single-wallet entry point and keeps that meaning - every
        // slot is dropped and the new wallet becomes the only one loaded.
        keychain_clear();
        occupy_slot(0, src);
    }

    finalise_slot(userdata, temporary);
}

bool keychain_has_free_slot(void)
{
    for (size_t i = 0; i < MAX_SEED_SLOTS; ++i) {
        if (!keychain_slots[i].in_use) {
            return true;
        }
    }
    return false;
}

bool keychain_load_into_free_slot(const keychain_t* src, const uint8_t userdata, const bool temporary)
{
    JADE_ASSERT(src);

    // BBB-AIRGAP: unlike keychain_set() this keeps the wallets already loaded - the new wallet
    // takes the first free slot and becomes the one in use.  Returning false when the table is
    // full lets the caller say so, rather than silently dropping a wallet the user still has.
    for (size_t i = 0; i < MAX_SEED_SLOTS; ++i) {
        if (!keychain_slots[i].in_use) {
            occupy_slot(i, src);
            finalise_slot(userdata, temporary);
            return true;
        }
    }
    return false;
}

// BBB-AIRGAP: the wallets held are addressed by their position in the list the user sees, not by
// their slot in the table, so MAX_SEED_SLOTS stays inside this file and the caller just counts
// from zero. Positions shift when a wallet is forgotten, which is correct: the list is rebuilt.
static size_t slot_at_position(const size_t position)
{
    size_t seen = 0;
    size_t slot = 0;
    for (; slot < MAX_SEED_SLOTS; ++slot) {
        if (keychain_slots[slot].in_use && seen++ == position) {
            break;
        }
    }
    JADE_ASSERT(slot < MAX_SEED_SLOTS); // the caller's position must exist
    return slot;
}

size_t keychain_slot_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < MAX_SEED_SLOTS; ++i) {
        if (keychain_slots[i].in_use) {
            ++count;
        }
    }
    return count;
}

void keychain_slot_fingerprint(const size_t position, uint8_t* output, const size_t output_len)
{
    JADE_ASSERT(output);
    JADE_ASSERT(output_len == BIP32_KEY_FINGERPRINT_LEN);

    // The same value wallet_get_fingerprint() reads for the wallet in use: it is already there in
    // the derived key, so a slot does not have to carry a copy of it.
    memcpy(output, keychain_slots[slot_at_position(position)].keydata.xpriv.hash160, output_len);
}

bool keychain_find_slot(const keychain_t* candidate, size_t* position_out)
{
    JADE_ASSERT(candidate);
    JADE_ASSERT(position_out);

    // The master key is the wallet: the private key and the chain code together are what every
    // other key is derived from, so two wallets agreeing on both are the same wallet and no two
    // different ones can be made to agree.  Each comparison is constant time, so a candidate that
    // somebody else prepared cannot be walked towards a held key byte by byte; the loop itself is
    // not, but what it gives away - whether a wallet matched, and which one - is what the screen
    // that follows says out loud anyway.
    const size_t num_slots = keychain_slot_count();
    for (size_t i = 0; i < num_slots; ++i) {
        const keychain_t* const held = &keychain_slots[slot_at_position(i)].keydata;
        if (!sodium_memcmp(held->xpriv.priv_key, candidate->xpriv.priv_key, sizeof(held->xpriv.priv_key))
            && !sodium_memcmp(held->xpriv.chain_code, candidate->xpriv.chain_code, sizeof(held->xpriv.chain_code))) {
            *position_out = i;
            return true;
        }
    }
    return false;
}

bool keychain_slot_is_temporary(const size_t position) { return keychain_slots[slot_at_position(position)].temporary; }

void keychain_slot_activate(const size_t position)
{
    // Only which wallet is in use changes. The key flags are the device's, read from storage, so
    // they do not follow the wallet and are left alone.
    active_slot = slot_at_position(position);
    keychain_data = &keychain_slots[active_slot].keydata;
}

void keychain_slot_forget_active(void)
{
    JADE_ASSERT(keychain_data);

    // Wipe the key material together with the flags that describe it, in a single call, so no flag
    // can be left behind pointing at a slot that no longer holds a wallet.  keychain_data pointed
    // into that slot, so it stops meaning anything the moment the slot is wiped.
    JADE_WALLY_VERIFY(wally_bzero(&keychain_slots[active_slot], sizeof(keychain_slots[active_slot])));
    keychain_data = NULL;

    // One of the wallets still held takes over.  If none are left the device is in the state a log
    // out leaves it in, so say that with keychain_clear() rather than writing a second way to empty
    // the table - that keeps the two paths from drifting apart.
    if (keychain_slot_count()) {
        keychain_slot_activate(0);
    } else {
        keychain_clear();
    }
}

// BBB-AIRGAP: the only way entropy enters a slot.  Called once, by the path that has just built
// a wallet from a mnemonic the user supplied, so the device never holds entropy for a wallet the
// user did not present the words for in this session.
void keychain_set_entropy(const char* mnemonic)
{
    JADE_ASSERT(mnemonic);
    JADE_ASSERT(keychain_data);
    JADE_ASSERT(!keychain_slots[active_slot].entropy_len);

    size_t written = 0;
    JADE_WALLY_VERIFY(bip39_mnemonic_to_bytes(
        NULL, mnemonic, keychain_slots[active_slot].entropy, sizeof(keychain_slots[active_slot].entropy), &written));

    // Only 12 and 24 word mnemonics can be drawn as a SeedQR, and they are the only lengths the
    // rest of the code supports.  Anything else leaves the slot unexportable rather than half set.
    if (written != BIP39_ENTROPY_LEN_128 && written != BIP39_ENTROPY_LEN_256) {
        JADE_LOGW("Mnemonic entropy length %u not exportable", written);
        JADE_WALLY_VERIFY(
            wally_bzero(keychain_slots[active_slot].entropy, sizeof(keychain_slots[active_slot].entropy)));
        return;
    }
    keychain_slots[active_slot].entropy_len = written;
}

// Whether the wallet at the given position can be exported as a SeedQR
bool keychain_slot_has_entropy(const size_t position)
{
    return keychain_slots[slot_at_position(position)].entropy_len != 0;
}

// BBB-AIRGAP: the only way entropy leaves a slot.  The mnemonic is built on demand and handed to
// the caller, which must free it with wally_free_string(); it is never held anywhere else.  A
// false return means this wallet has no entropy, which is the normal state for a persisted wallet
// read back from the blob - the caller must not offer export for it.
bool keychain_export_mnemonic(char** mnemonic)
{
    JADE_ASSERT(mnemonic);
    JADE_INIT_OUT_PPTR(mnemonic);
    JADE_ASSERT(keychain_data);

    const keychain_slot_t* const slot = &keychain_slots[active_slot];
    if (!slot->entropy_len) {
        return false;
    }

    if (bip39_mnemonic_from_bytes(NULL, slot->entropy, slot->entropy_len, mnemonic) != WALLY_OK) {
        JADE_LOGE("Failed to convert entropy bytes to mnemonic string");
        return false;
    }
    JADE_ASSERT(*mnemonic);
    return true;
}

uint8_t keychain_get_persisted_userdata(void)
{
    // BBB-AIRGAP: keychain_get_userdata() answers for the wallet in use; this answers for the
    // persisted wallet whichever wallet is in use.  Returns 0 (SOURCE_NONE) when no persisted
    // wallet is held, matching what keychain_get_userdata() returns with no wallet loaded.
    for (size_t i = 0; i < MAX_SEED_SLOTS; ++i) {
        if (keychain_slots[i].in_use && !keychain_slots[i].temporary) {
            return keychain_slots[i].userdata;
        }
    }
    return 0;
}

void keychain_clear(void)
{
    // BBB-AIRGAP: wipes the whole table, not just the slot in use.  Every caller of this means
    // "no wallet may remain in memory" - abort, idle timeout, log out, connection lost - so
    // leaving the other slots loaded would be the exact leak they exist to prevent.  Wiping the
    // table in a single call also keeps the wipe independent of any per-slot bookkeeping, so no
    // flag can ever leave key material behind.
    JADE_WALLY_VERIFY(wally_bzero(keychain_slots, sizeof(keychain_slots)));
    keychain_data = NULL;
    active_slot = 0;

    // Clear any mnemonic entropy we may have been holding
    JADE_WALLY_VERIFY(wally_bzero(mnemonic_entropy, sizeof(mnemonic_entropy)));
    mnemonic_entropy_len = 0;

    // Reload key flags
    key_flags = storage_get_key_flags();
}

const keychain_t* keychain_get(void) { return keychain_data; }

bool keychain_requires_passphrase(void)
{
    // We require a passphrase when we have mnemonic entropy but no key data as yet
    // ie. the final wallet derivation step has yet to occur.
    // (This may be an explicitly user-provided phrase, or may be the default/blank phrase)
    return !keychain_data && mnemonic_entropy_len;
}

void keychain_set_passphrase_frequency(const passphrase_freq_t freq)
{
    switch (freq) {
    case PASSPHRASE_NEVER:
        key_flags |= KEY_FLAGS_AUTO_DEFAULT_PASSPHRASE;
        key_flags &= ~KEY_FLAGS_USER_TO_ENTER_PASSPHRASE;
        break;
    case PASSPHRASE_ALWAYS:
        key_flags &= ~KEY_FLAGS_AUTO_DEFAULT_PASSPHRASE;
        key_flags |= KEY_FLAGS_USER_TO_ENTER_PASSPHRASE;
        break;
    case PASSPHRASE_ONCE:
        // Set both 'auto default' and 'user to set' to imply
        // 'user to enter just this once, but usually auto-default'
        key_flags |= KEY_FLAGS_AUTO_DEFAULT_PASSPHRASE;
        key_flags |= KEY_FLAGS_USER_TO_ENTER_PASSPHRASE;
        break;
    default:
        JADE_LOGE("Unexpected passphrase frequency flag ignored: %u", freq);
    }
}

passphrase_freq_t keychain_get_passphrase_freq(void)
{
    // NOTE: Both flags set implies 'once only'
    return (key_flags & KEY_FLAGS_USER_TO_ENTER_PASSPHRASE)
        ? ((key_flags & KEY_FLAGS_AUTO_DEFAULT_PASSPHRASE) ? PASSPHRASE_ONCE : PASSPHRASE_ALWAYS)
        : PASSPHRASE_NEVER;
}

void keychain_set_passphrase_type(const passphrase_type_t type)
{
    if (type == PASSPHRASE_WORDLIST) {
        key_flags |= KEY_FLAGS_WORDLIST_PASSPHRASE;
    } else {
        key_flags &= ~KEY_FLAGS_WORDLIST_PASSPHRASE;
    }
}

passphrase_type_t keychain_get_passphrase_type(void)
{
    return (key_flags & KEY_FLAGS_WORDLIST_PASSPHRASE) ? PASSPHRASE_WORDLIST : PASSPHRASE_FREETEXT;
}

void keychain_set_confirm_export_blinding_key(const bool confirm_export)
{
    if (confirm_export) {
        key_flags |= KEY_FLAGS_CONFIRM_EXPORT_BLINDING_KEY;
    } else {
        key_flags &= ~KEY_FLAGS_CONFIRM_EXPORT_BLINDING_KEY;
    }
}

bool keychain_get_confirm_export_blinding_key(void) { return (key_flags & KEY_FLAGS_CONFIRM_EXPORT_BLINDING_KEY); }

void keychain_persist_key_flags(void)
{
    // If both the 'auto-default (ie. empty) passphrase' flag and the 'ask user for passphrase'
    // flags are set, then we ask the user for a passphrase *just for the
    // current session/next-login*.
    // ie. We cache the 'user to enter passphrase' flag in memory, but we persist the
    // 'auto-apply empty passphrase' flag into flash nvs.
    // NOTE: the flags use is a bit clumsy because of the way they evolved over time, and we
    // always want to maintain backward-compatibility with the previous meanings of these flags.
    if ((key_flags & KEY_FLAGS_AUTO_DEFAULT_PASSPHRASE) && (key_flags & KEY_FLAGS_USER_TO_ENTER_PASSPHRASE)) {
        storage_set_key_flags(key_flags & ~KEY_FLAGS_USER_TO_ENTER_PASSPHRASE);
    } else {
        storage_set_key_flags(key_flags);
    }
}

// Only for use under specific circumstances during wallet setup, when the initialisation is
// started as standard/pin-protected, but the user then wants to flip to temporary-wallet only.
void keychain_set_temporary(void)
{
    // This combination should only occur when part way through initial setup
    JADE_ASSERT(keychain_data);
    JADE_ASSERT(mnemonic_entropy_len);
    JADE_ASSERT(!keychain_slots[active_slot].temporary);
    JADE_ASSERT(!keychain_has_pin());
    keychain_slots[active_slot].temporary = true;
}

bool keychain_has_temporary(void)
{
    // A slot cannot be flagged temporary while no wallet is loaded: keychain_clear() wipes the
    // flag along with the key data, so the two can only be inconsistent through a bug here.
    JADE_ASSERT(keychain_data || !keychain_slots[active_slot].temporary);
    return keychain_data ? keychain_slots[active_slot].temporary : false;
}

uint8_t keychain_get_userdata(void) { return keychain_data ? keychain_slots[active_slot].userdata : 0; }

// Cache/clear mnemonic entropy (if using passphrase)
void keychain_cache_mnemonic_entropy(const char* mnemonic)
{
    JADE_ASSERT(mnemonic);
    JADE_ASSERT(!keychain_has_temporary());
    JADE_ASSERT(!mnemonic_entropy_len);

    JADE_WALLY_VERIFY(
        bip39_mnemonic_to_bytes(NULL, mnemonic, mnemonic_entropy, sizeof(mnemonic_entropy), &mnemonic_entropy_len));

    // Only 12 or 24 word mnemonics are supported
    JADE_ASSERT(mnemonic_entropy_len == BIP39_ENTROPY_LEN_128 || mnemonic_entropy_len == BIP39_ENTROPY_LEN_256);
}

// Clear the network type restriction
void keychain_clear_network_type_restriction(void)
{
    JADE_LOGI("Clearing network type restriction");
    // If we are not currently working with temporary keys, clear the keys from storage
    if (!keychain_has_temporary()) {
        storage_set_network_type_restriction(NETWORK_TYPE_NONE);
    }
    network_type_restriction = NETWORK_TYPE_NONE;
}

// Set the network type restriction (must currently be 'none', or same as passed).
void keychain_set_network_type_restriction(const network_type_t network_type)
{
    JADE_ASSERT(keychain_is_network_type_consistent(network_type));

    if (network_type_restriction == NETWORK_TYPE_NONE) {
        JADE_LOGI("Restricting to network type: %s", network_type == NETWORK_TYPE_TEST ? "TEST" : "MAIN");

        // If we have a persisted wallet, and we are not currently working with temporary keys
        // then persist the network type to the storage (as it applies to the stored wallet)
        if (keychain_has_pin() && !keychain_has_temporary()) {
            storage_set_network_type_restriction(network_type);
        }

        // If we have keys loaded in memory, set the in-memory value also
        if (keychain_data) {
            network_type_restriction = network_type;
        }
    }
}

// Get the current network type restriction
network_type_t keychain_get_network_type_restriction(void) { return network_type_restriction; }

// Compare pinned/restricted network type and the type of the network passed
bool keychain_is_network_type_consistent(const network_type_t network_type)
{
    return network_type_restriction == NETWORK_TYPE_NONE || network_type == network_type_restriction;
}

bool keychain_is_network_id_consistent(const network_t network_id)
{
    const network_type_t network_type = network_to_type(network_id);
    return keychain_is_network_type_consistent(network_type);
}

const struct ext_key* keychain_cached_service(const struct ext_key* const service, const bool subaccount_root)
{
    JADE_ASSERT(keychain_data);

    // If no service passed, invalidate cache
    if (!service) {
        keychain_data->cached_service = NULL;
        return NULL;
    }

    // Recompute cached values if service mismatch
    if (service != keychain_data->cached_service) {
        JADE_ASSERT(wallet_get_gaservice_root_key(service, false, &keychain_data->cached_gaservice_main_root));
        JADE_ASSERT(wallet_get_gaservice_root_key(service, true, &keychain_data->cached_gaservice_subact_root));
        keychain_data->cached_service = service;
    }

    // Return cached value
    return subaccount_root ? &keychain_data->cached_gaservice_subact_root : &keychain_data->cached_gaservice_main_root;
}

void keychain_get_new_mnemonic(char** mnemonic, const size_t nwords)
{
    JADE_INIT_OUT_PPTR(mnemonic);

    // Support 12-word and 24-word mnemonics only
    JADE_ASSERT(nwords == 12 || nwords == 24);

    // Large enough for 12 and 24 word mnemonic
    uint8_t entropy[BIP39_ENTROPY_LEN_256];
    SENSITIVE_PUSH(entropy, sizeof(entropy));

    const size_t entropy_len = nwords == 12 ? BIP39_ENTROPY_LEN_128 : BIP39_ENTROPY_LEN_256;
    get_random(entropy, entropy_len);
    const int wret = bip39_mnemonic_from_bytes(NULL, entropy, entropy_len, mnemonic);
    SENSITIVE_POP(entropy);
    JADE_WALLY_VERIFY(wret);
    JADE_WALLY_VERIFY(bip39_mnemonic_validate(NULL, *mnemonic));
}

// Derive master key from given seed
void keychain_derive_from_seed(const uint8_t* seed, const size_t seed_len, keychain_t* keydata)
{
    JADE_ASSERT(seed);
    JADE_ASSERT(seed_len);
    JADE_ASSERT(keydata);
    JADE_ASSERT(seed_len <= sizeof(keydata->seed));

    // Cache the seed
    memcpy(keydata->seed, seed, seed_len);
    keydata->seed_len = seed_len;

    // Use mainnet version by default - will be overridden if key serialised for specific network
    // (eg. in get_xpub call).
    JADE_WALLY_VERIFY(bip32_key_from_seed(seed, seed_len, BIP32_VER_MAIN_PRIVATE, 0, &keydata->xpriv));

    // NOTE: 'master_unblinding_key' is stored here as the full output of hmac512, when according to slip-0077
    // the master unblinding key is only the second half of that - ie. 256 bits.
    JADE_WALLY_VERIFY(
        wally_asset_blinding_key_from_seed(seed, seed_len, keydata->master_unblinding_key, HMAC_SHA512_LEN));

    // Compute and cache the path the GA server will use to sign
    JADE_ASSERT(wallet_calculate_gaservice_path(&keydata->xpriv, keydata->gaservice_path, GASERVICE_PATH_LEN));

    // Ensure cached green-multisig service path roots are unset
    keydata->cached_service = NULL;
}

// Derive master key from mnemonic if passed a valid mnemonic
bool keychain_derive_from_mnemonic(const char* mnemonic, const char* passphrase, keychain_t* keydata)
{
    // NOTE: passphrase is optional, but if passed must fit the size limit
    if (!mnemonic || !keydata) {
        return false;
    }
    if (passphrase) {
        const size_t passphrase_len = strnlen(passphrase, PASSPHRASE_MAX_LEN + 1);
        if (passphrase_len > PASSPHRASE_MAX_LEN) {
            JADE_LOGE("Passphrase too long");
            return false;
        }
    }

    // Mnemonic must be valid
    if (bip39_mnemonic_validate(NULL, mnemonic) != WALLY_OK) {
        JADE_LOGE("Invalid mnemonic");
        return false;
    }

    uint8_t seed[BIP32_ENTROPY_LEN_512];
    SENSITIVE_PUSH(seed, sizeof(seed));

    size_t written = 0;
    JADE_WALLY_VERIFY(bip39_mnemonic_to_seed(mnemonic, passphrase, seed, sizeof(seed), &written));
    JADE_ASSERT_MSG(written == sizeof(seed), "Unexpected seed length: %u", written);

    keychain_derive_from_seed(seed, sizeof(seed), keydata);

    SENSITIVE_POP(seed);
    return true;
}

// Derive keys from cached mnemonic and passed passphrase
bool keychain_complete_derivation_with_passphrase(const char* passphrase)
{
    if (!passphrase || !keychain_requires_passphrase()) {
        return false;
    }

    keychain_t keydata = { 0 };
    SENSITIVE_PUSH(&keydata, sizeof(keydata));

    // Convert entropy bytes to mnemonic string
    bool ret = false;
    char* mnemonic = NULL;
    if (bip39_mnemonic_from_bytes(NULL, mnemonic_entropy, mnemonic_entropy_len, &mnemonic) != WALLY_OK) {
        JADE_LOGE("Failed to convert entropy bytes to mnemonic string");
        goto cleanup;
    }
    JADE_ASSERT(mnemonic);

    SENSITIVE_PUSH(mnemonic, strlen(mnemonic));
    ret = keychain_derive_from_mnemonic(mnemonic, passphrase, &keydata);
    SENSITIVE_POP(mnemonic);
    JADE_WALLY_VERIFY(wally_free_string(mnemonic));

    if (ret) {
        keychain_set(&keydata, 0, false);
    }

cleanup:
    SENSITIVE_POP(&keydata);
    return ret;
}

static void serialize(uint8_t* serialized, const size_t serialized_len, const keychain_t* keydata)
{
    JADE_ASSERT(serialized);
    JADE_ASSERT(serialized_len == SERIALIZED_KEY_LEN);
    JADE_ASSERT(keydata);

    // ext-key, ga-path, master-blinding-key
    JADE_WALLY_VERIFY(bip32_key_serialize(&keydata->xpriv, BIP32_FLAG_KEY_PRIVATE, serialized, BIP32_SERIALIZED_LEN));
    const bool ret = wallet_serialize_gaservice_path(
        serialized + BIP32_SERIALIZED_LEN, HMAC_SHA512_LEN, keydata->gaservice_path, GASERVICE_PATH_LEN);
    JADE_ASSERT(ret);
    memcpy(serialized + BIP32_SERIALIZED_LEN + HMAC_SHA512_LEN, keydata->master_unblinding_key, HMAC_SHA512_LEN);
}

static void unserialize(const uint8_t* decrypted, const size_t decrypted_len, keychain_t* keydata)
{
    JADE_ASSERT(decrypted);
    JADE_ASSERT(decrypted_len == SERIALIZED_KEY_LEN);
    JADE_ASSERT(keydata);

    // ext-key, ga-path, master-blinding-key
    JADE_WALLY_VERIFY(bip32_key_unserialize(decrypted, BIP32_SERIALIZED_LEN, &keydata->xpriv));
    const bool ret = wallet_unserialize_gaservice_path(
        decrypted + BIP32_SERIALIZED_LEN, HMAC_SHA512_LEN, keydata->gaservice_path, GASERVICE_PATH_LEN);
    JADE_ASSERT(ret);
    memcpy(keydata->master_unblinding_key, decrypted + BIP32_SERIALIZED_LEN + HMAC_SHA512_LEN, HMAC_SHA512_LEN);
}

// AES encrypt passed bytes with passed key (uses new random iv).  Also appends HMAC of the encrypted bytes.
static bool get_encrypted_blob(const uint8_t* aeskey, const size_t aeslen, const uint8_t* bytes, const size_t bytes_len,
    uint8_t* output, const size_t output_len)
{
    JADE_ASSERT(aeskey);
    JADE_ASSERT(aeslen);
    JADE_ASSERT(bytes);
    JADE_ASSERT(bytes_len);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len == AES_ENCRYPTED_LEN(bytes_len) + HMAC_SHA256_LEN); // hmac appended

    // 1. Encrypt the passed data into the start of the buffer
    if (!aes_encrypt_bytes(aeskey, aeslen, bytes, bytes_len, output, output_len - HMAC_SHA256_LEN)) {
        JADE_LOGW("Failed to encrypt wallet!");
        return false;
    }

    // 2. Write the hmac into the buffer after the encrypted data
    JADE_WALLY_VERIFY(wally_hmac_sha256(
        aeskey, aeslen, output, output_len - HMAC_SHA256_LEN, output + output_len - HMAC_SHA256_LEN, HMAC_SHA256_LEN));

    return true;
}

static bool get_decrypted_payload(const uint8_t* aeskey, const size_t aeslen, const uint8_t* bytes,
    const size_t bytes_len, uint8_t* output, const size_t output_len, size_t* written)
{
    JADE_ASSERT(aeskey);
    JADE_ASSERT(aeslen);
    JADE_ASSERT(bytes);
    JADE_ASSERT(bytes_len > HMAC_SHA256_LEN); // hmac appended
    JADE_ASSERT(output);
    JADE_ASSERT(output_len);
    JADE_INIT_OUT_SIZE(written);

    // 1. Verify HMAC at the tail of the input buffer
    uint8_t hmac_calculated[HMAC_SHA256_LEN];
    JADE_WALLY_VERIFY(wally_hmac_sha256(
        aeskey, aeslen, bytes, bytes_len - HMAC_SHA256_LEN, hmac_calculated, sizeof(hmac_calculated)));
    if (crypto_verify_32(hmac_calculated, bytes + bytes_len - HMAC_SHA256_LEN) != 0) {
        JADE_LOGW("hmac mismatch (bad pin)");
        return false;
    }

    // 2. Decrypt bytes at front of buffer
    if (!aes_decrypt_bytes(aeskey, aeslen, bytes, bytes_len - HMAC_SHA256_LEN, output, output_len, written)) {
        JADE_LOGW("Failed to decrypt wallet!");
        return false;
    }

    return true;
}

static bool keychain_encrypt_and_save_blob(
    const uint8_t* aeskey, const size_t aeslen, const uint8_t* cleartext_blob, const size_t blob_len)
{
    if (!aeskey || aeslen != AES_KEY_LEN_256) {
        return false;
    }
    if (!cleartext_blob || blob_len > SERIALIZED_KEY_LEN) {
        // Invlaid cleartext blob
        return false;
    }

    // This buffer is sized for deserialising the extended key structure
    // If instead we are storing mnemonic entropy, the buffer is of ample size.
    uint8_t encrypted[ENCRYPTED_DATA_LEN(SERIALIZED_KEY_LEN)];

    // 1. Get as encrypted blob
    const size_t encrypted_data_len = ENCRYPTED_DATA_LEN(blob_len);
    if (!get_encrypted_blob(aeskey, aeslen, cleartext_blob, blob_len, encrypted, encrypted_data_len)) {
        JADE_LOGE("Failed to encrypt key data");
        return false;
    }

    // 2. Push into flash storage
    if (!storage_set_encrypted_blob(encrypted, encrypted_data_len)) {
        JADE_LOGE("Failed to store encrypted key data");
        return false;
    }

    return true;
}

static bool keychain_load_and_decrypt_blob(
    const uint8_t* aeskey, const size_t aeslen, uint8_t* cleartext_blob, const size_t blob_len, size_t* written)
{
    if (!aeskey || aeslen != AES_KEY_LEN_256 || !cleartext_blob || blob_len < AES_PADDED_LEN(SERIALIZED_KEY_LEN)
        || !written) {
        return false;
    }
    if (!keychain_has_pin() || !storage_decrement_counter()) {
        // No valid keychain data in storage to load.
        //
        // BBB-AIRGAP: storage_decrement_counter() erases the blob for every counter it rejects -
        // the zero of an exhausted device, and the wallet-erase sentinel written by
        // storage_erase_encrypted_blob() (main/storage.c) - so this early return can be the moment
        // the wallet actually disappears.  Re-derive the cached flag the way a boot would
        // (keychain_init_cache() below uses the same expression), or it stays true over a wallet
        // that is gone and the next unlock trips JADE_ASSERT(pin_attempts_remaining > 0) in
        // main/process/auth_user.c.  storage_get_counter() maps a read failure to zero, so a
        // storage layer that has stopped answering makes this say "no wallet" - that is the answer
        // the next boot would give from the same read, not a new claim made here.
        has_encrypted_blob = keychain_pin_attempts_remaining() > 0;
        return false;
    }

    // This buffer is sized for deserialising the extended key structure
    // If instead we are storing mnemonic entropy, the buffer is of ample size.
    uint8_t encrypted[ENCRYPTED_DATA_LEN(SERIALIZED_KEY_LEN)];

    // 1. Load from flash storage
    size_t encrypted_data_len = 0;
    if (!storage_get_encrypted_blob(encrypted, sizeof(encrypted), &encrypted_data_len)) {
        JADE_LOGE("Failed to load encrypted blob from storage - ensuring fully erased");
        // BBB-AIRGAP: same rule as keychain_erase_encrypted() below - the flag follows what the
        // erase actually did, not what was asked for.
        has_encrypted_blob = !storage_erase_encrypted_blob();
        return false;
    }

    // 2. Get decrypted payload from the encrypted blob
    if (!get_decrypted_payload(aeskey, aeslen, encrypted, encrypted_data_len, cleartext_blob, blob_len, written)) {
        JADE_LOGW("Failed to decrypt key data (bad pin)");
        if (keychain_pin_attempts_remaining() == 0) {
            JADE_LOGW("Multiple failures to decrypt key data - erasing encrypted keys");
            if (!keychain_erase_encrypted()) {
                JADE_LOGE("Failed to erase encrypted keys after exhausting pin attempts");
            }
        }
        return false;
    }

    // 3. Decrypt succeed so pin ok - reset counter
    // (Ignore failure as it can't make things worse)
    storage_restore_counter();

    return true;
}

bool keychain_store(const uint8_t* aeskey, const size_t aeslen)
{
    if (!aeskey || aeslen != AES_KEY_LEN_256) {
        return false;
    }
    if (!keychain_data && !mnemonic_entropy_len) {
        // No keychain data to store
        return false;
    }

    // This buffer is sized for deserialising the extended key structure
    // If instead we are storing mnemonic entropy, the buffer is of ample size.
    uint8_t serialized[SERIALIZED_KEY_LEN];
    SENSITIVE_PUSH(serialized, sizeof(serialized));

    // If we have cached mnemonic entropy, we store that (as the wallet is passphrase-protected)
    // Otherwise we store the master keychain data (classic)
    uint8_t* p_serialized_data;
    size_t serialized_data_len;

    // 1. Get serialised data to encrypt/persist
    if (mnemonic_entropy_len) {
        // Use mnemonic entropy
        // Only 12 or 24 word mnemonics are supported
        JADE_ASSERT(mnemonic_entropy_len == BIP39_ENTROPY_LEN_128 || mnemonic_entropy_len == BIP39_ENTROPY_LEN_256);
        JADE_ASSERT(mnemonic_entropy_len <= sizeof(mnemonic_entropy));
        JADE_ASSERT(mnemonic_entropy_len < sizeof(serialized));
        p_serialized_data = mnemonic_entropy;
        serialized_data_len = mnemonic_entropy_len;
    } else {
        // Use serialised keychain
        serialize(serialized, sizeof(serialized), keychain_data);
        p_serialized_data = serialized;
        serialized_data_len = sizeof(serialized);
    }

    // 2. Get as encrypted blob and save into storage
    if (!keychain_encrypt_and_save_blob(aeskey, aeslen, p_serialized_data, serialized_data_len)) {
        JADE_LOGE("Failed to encrypt and save key data");
        SENSITIVE_POP(serialized);
        return false;
    }
    SENSITIVE_POP(serialized);

    // 3. Clear main/test network restriction and cache that we have encrypted keys
    keychain_clear_network_type_restriction();
    has_encrypted_blob = true;

    return true;
}

bool keychain_load(const uint8_t* aeskey, const size_t aeslen)
{
    if (!aeskey || aeslen != AES_KEY_LEN_256) {
        return false;
    }
    if (keychain_data || mnemonic_entropy_len) {
        // We already have loaded keychain data - do not overwrite
        return false;
    }
    if (!keychain_has_pin()) {
        // No valid keychain data in storage to load
        return false;
    }

    // This buffer is sized for deserialising the extended key structure
    // If instead we are storing mnemonic entropy, the buffer is of ample size.
    size_t serialized_data_len = 0;
    uint8_t serialized[AES_PADDED_LEN(SERIALIZED_KEY_LEN)];
    SENSITIVE_PUSH(serialized, sizeof(serialized));

    // 1. Load from flash storage and decrypt
    if (!keychain_load_and_decrypt_blob(aeskey, aeslen, serialized, sizeof(serialized), &serialized_data_len)) {
        JADE_LOGE("Failed to load and decrypt blob from storage");
        SENSITIVE_POP(serialized);
        return false;
    }

    // 2. Cache mnemonic entropy or deserialise keychain
    if (serialized_data_len == BIP39_ENTROPY_LEN_128 || serialized_data_len == BIP39_ENTROPY_LEN_256) {
        // Write mnemonic entropy - only 12 or 24 word mnemonics are supported
        memcpy(mnemonic_entropy, serialized, serialized_data_len);
        mnemonic_entropy_len = serialized_data_len;
    } else if (serialized_data_len == SERIALIZED_KEY_LEN) {
        // Deserialise keychain
        keychain_t keydata = { 0 };
        SENSITIVE_PUSH(&keydata, sizeof(keydata));
        unserialize(serialized, serialized_data_len, &keydata);
        keychain_set(&keydata, 0, false);
        SENSITIVE_POP(&keydata);
    } else {
        JADE_LOGE("Unexpected length of decrypted serialised data: %d", serialized_data_len);
        SENSITIVE_POP(serialized);
        return false;
    }
    SENSITIVE_POP(serialized);

    return true;
}

bool keychain_reencrypt(
    const uint8_t* curr_aeskey, const size_t curr_aeslen, const uint8_t* new_aeskey, const size_t new_aeslen)
{
    if (!curr_aeskey || curr_aeslen != AES_KEY_LEN_256 || !new_aeskey || new_aeslen != AES_KEY_LEN_256) {
        return false;
    }

    if (!keychain_has_pin()) {
        // No valid keychain data in storage to load
        return false;
    }

    // This buffer is sized for deserialising the extended key structure
    // If instead we are storing mnemonic entropy, the buffer is of ample size.
    uint8_t serialized[AES_PADDED_LEN(SERIALIZED_KEY_LEN)];
    SENSITIVE_PUSH(serialized, sizeof(serialized));
    size_t serialized_data_len = 0;

    // 1. Load from flash storage and decrypt
    if (!keychain_load_and_decrypt_blob(
            curr_aeskey, curr_aeslen, serialized, sizeof(serialized), &serialized_data_len)) {
        JADE_LOGE("Failed to load and decrypt blob from storage");
        SENSITIVE_POP(serialized);
        return false;
    }

    // 3. Re-encrypt blob (new key) and save to flash storage
    // 2. Get as (re-)encrypted blob (new key) and save into storage
    if (!keychain_encrypt_and_save_blob(new_aeskey, new_aeslen, serialized, serialized_data_len)) {
        JADE_LOGE("Failed to encrypt and save key data");
        SENSITIVE_POP(serialized);
        return false;
    }
    SENSITIVE_POP(serialized);

    return true;
}

bool keychain_has_pin(void) { return has_encrypted_blob; }

uint8_t keychain_pin_attempts_remaining(void) { return storage_get_counter(); }

// BBB-AIRGAP: say whether the blob really went.  storage_erase_encrypted_blob() can fail at
// nvs_open(), nvs_erase_key() or nvs_commit() (main/storage.c:74, 138, 149) and callers used to be
// told nothing; the wallet-erase PIN in particular showed its cover message and shut the device
// down with the encrypted seed still on the card.  A blob that was never there is not a failure:
// erase_key() treats ESP_ERR_NVS_NOT_FOUND as success (main/storage.c:138), so !erased means the
// blob is still on flash and the in-memory flag has to keep saying so.
bool keychain_erase_encrypted(void)
{
    const bool erased = storage_erase_encrypted_blob();
    keychain_clear_network_type_restriction();
    has_encrypted_blob = !erased;
    return erased;
}

bool keychain_get_new_privatekey(uint8_t* privatekey, const size_t size)
{
    if (!privatekey || size != EC_PRIVATE_KEY_LEN) {
        return false;
    }

    for (size_t attempts = 0; attempts < 4; ++attempts) {
        get_random(privatekey, size);

        if (wally_ec_private_key_verify(privatekey, size) == WALLY_OK) {
            JADE_LOGD("Created new random private key");
            return true;
        }
    }

    // Exhausted attempts
    JADE_LOGE("Exhausted attempts creating new private key");
    return false;
}

void keychain_init_cache(void)
{
    // Cache whether we are restricted to main/test networks and whether we have an encrypted blob
    network_type_restriction = storage_get_network_type_restriction();
    has_encrypted_blob = keychain_pin_attempts_remaining() > 0;

    // Cache the user key/passphrase preferences
    key_flags = storage_get_key_flags();
}
#endif // AMALGAMATED_BUILD
