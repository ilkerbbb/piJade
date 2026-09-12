/*
 * BBB-AIRGAP: the bounded NVS store that survives a power cut, and its on-disk format.
 *
 * This format is deliberately separate from libjade_save_nvs()/libjade_load_nvs(). It persists an
 * allowlist from the default namespace and applies per-namespace bounds to registrations, with a
 * digest and validation of the complete file before anything is stored. Keeping the format here
 * also means an upstream change to the daemon RPC's whole-store format cannot change the card file
 * under us.
 */
#ifndef PIJADE_SETTINGS_H
#define PIJADE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * BBB-AIRGAP: the two sealed-record ceilings this format enforces, which are main/multisig.h's
 * MAX_MULTISIG_BYTES_LEN and main/descriptor.h's MAX_DESCRIPTOR_BYTES_LEN.
 *
 * They are literals here rather than an include of those headers because pijade/tools/settings_test.c
 * includes this header and is built on its own, without main/ on its include path (the chain reaches
 * <cbor.h> through signer.h). pijade_settings.c does have that path and asserts both macros against
 * the owning headers at compile time, so a change in main/ that is not mirrored here fails the build
 * rather than leaving a second copy of the number that quietly disagrees.
 */
#define PIJADE_SETTINGS_MAX_MULTISIG_LEN 3281
#define PIJADE_SETTINGS_MAX_DESCRIPTOR_LEN 3281

struct wally_map;

/*
 * Collects the allowlisted fields of `prefs` and all four registration namespaces into a freshly
 * malloc'd blob. The caller owns `*output` and frees it. Returns false and leaves the outputs
 * zeroed on failure. An empty store still produces a valid, near-empty blob.
 */
bool pijade_settings_serialize(const struct wally_map* prefs, uint8_t** output, size_t* output_len);

/*
 * Applies a blob produced by pijade_settings_serialize() to `prefs` and the other four namespaces.
 *
 * Rejects the blob whole if its framing, digest, namespace, key, or value is invalid rather than
 * applying the part it understood, so a corrupt or edited file leaves Jade on its defaults instead
 * of in a state that is half one thing and half another.
 */
bool pijade_settings_deserialize(struct wally_map* prefs, const uint8_t* bytes, size_t bytes_len);

/*
 * Indexed access to nvs_flash.c's five private namespace maps, in the order encoded on disk.
 * Returns NULL for an index outside 0 through 4.
 */
struct wally_map* pijade_settings_storage(size_t namespace_index);

/*
 * Called from nvs_commit() after any persisted namespace changes. Implemented in libjade.c, which
 * serialises the whole store and hands the result to the host's handler. Returns true if the change
 * was persisted, false otherwise; see libjade_settings_fn in libjade.h for caller semantics.
 */
bool libjade_settings_changed(void);

/* Called after nvs_flash_erase() clears every namespace; asks the host to remove every copy. */
bool libjade_settings_erased(void);

#endif /* PIJADE_SETTINGS_H */
