#ifndef AMALGAMATED_BUILD
#include "multisig.h"
#include "jade_assert.h"
#include "jade_wally_verify.h"
#include "storage.h"
#include "utils/malloc_ext.h"

#include <sodium/utils.h>
#include <wally_script.h>

// 0 - 0.1.30 - variant, threshold, signers, hmac
// 1 - 0.1.31 - include the 'sorted' flag
// 2 - 0.1.34 - include any liquid master blinding key
// 3 - 1.0.22 - persist all metadata so can recreate original input
// BBB-AIRGAP: v4 seals the v3 body (main/registration_seal.c); the version byte moved out of the
// body and the HMAC now covers the ciphertext.  Nothing older than v4 is read (spec B3).
static const uint8_t CURRENT_MULTISIG_RECORD_VERSION = 4;

_Static_assert(MAX_MULTISIG_BODY_LEN == 3217, "multisig body size drifted; update spec B4 and pijade_settings.c");
_Static_assert(MAX_MULTISIG_BYTES_LEN == 3281, "multisig record size drifted; update pijade_settings.c max_len");

// The smallest valid multisig record body, for sanity checking
// variant, sorted, threshold, keylen, num_signers, then one signer with no paths
#define MIN_MULTISIG_BODY_LEN (5 + BIP32_KEY_FINGERPRINT_LEN + 1 + BIP32_SERIALIZED_LEN + 1)

bool multisig_seal_body(const uint8_t* body, const size_t body_len, uint8_t* output, const size_t output_len)
{
    return registration_seal(CURRENT_MULTISIG_RECORD_VERSION, body, body_len, output, output_len);
}

bool multisig_open_registration(
    const uint8_t* bytes, const size_t bytes_len, uint8_t* body, const size_t body_len, size_t* written)
{
    return registration_open(CURRENT_MULTISIG_RECORD_VERSION, bytes, bytes_len, body, body_len, written);
}

bool multisig_body_to_bytes(const script_variant_t variant, const bool sorted, const uint8_t threshold,
    const uint8_t* master_blinding_key, const size_t master_blinding_key_len, const signer_t* signers,
    const size_t num_signers, const size_t total_num_path_elements, uint8_t* body, const size_t body_len)
{
    JADE_ASSERT(threshold > 0);
    JADE_ASSERT(IS_VALID_BLINDING_KEY(master_blinding_key, master_blinding_key_len));
    JADE_ASSERT(signers);
    JADE_ASSERT(num_signers >= threshold);
    JADE_ASSERT(num_signers <= MAX_ALLOWED_SIGNERS);
    JADE_ASSERT(total_num_path_elements <= num_signers * 2 * MAX_PATH_LEN);
    JADE_ASSERT(body);
    JADE_ASSERT(body_len == MULTISIG_BODY_LEN(master_blinding_key_len, num_signers, total_num_path_elements));

    // Script variant
    uint8_t* write_ptr = body;
    const uint8_t variant_byte = (uint8_t)variant;
    memcpy(write_ptr, &variant_byte, sizeof(variant_byte));
    write_ptr += sizeof(variant_byte);

    // 'sorted' flag
    const uint8_t sorted_byte = (uint8_t)sorted;
    memcpy(write_ptr, &sorted_byte, sizeof(sorted_byte));
    write_ptr += sizeof(sorted_byte);

    // Threshold
    memcpy(write_ptr, &threshold, sizeof(threshold));
    write_ptr += sizeof(threshold);

    // Blinding key len, and data
    const uint8_t keylen = (uint8_t)master_blinding_key_len;
    memcpy(write_ptr, &keylen, sizeof(keylen));
    write_ptr += sizeof(keylen);

    if (master_blinding_key_len) {
        memcpy(write_ptr, master_blinding_key, master_blinding_key_len);
        write_ptr += master_blinding_key_len;
    }

    // Num signers
    JADE_ASSERT(num_signers <= MAX_ALLOWED_SIGNERS);
    const uint8_t num_signers_byte = num_signers;
    memcpy(write_ptr, &num_signers_byte, sizeof(num_signers_byte));
    write_ptr += sizeof(num_signers_byte);

    // All signers
    size_t counted_path_elements = 0;
    for (size_t i = 0; i < num_signers; ++i) {
        const signer_t* const signer = signers + i;

        // Check total number of path elements persisted
        counted_path_elements += signer->derivation_len;
        counted_path_elements += signer->path_len;
        JADE_ASSERT(counted_path_elements <= total_num_path_elements);

        // Key origin information
        memcpy(write_ptr, signer->fingerprint, sizeof(signer->fingerprint));
        write_ptr += sizeof(signer->fingerprint);

        JADE_ASSERT(signer->derivation_len <= MAX_PATH_LEN);
        const uint8_t derivation_len = (uint8_t)signer->derivation_len;
        memcpy(write_ptr, &derivation_len, sizeof(derivation_len));
        write_ptr += sizeof(derivation_len);

        const size_t derivation_bytes_len = signer->derivation_len * sizeof(signer->derivation[0]);
        memcpy(write_ptr, signer->derivation, derivation_bytes_len);
        write_ptr += derivation_bytes_len;

        // Xpub as passed (not the derived immediate parent xpub)
        // BBB-AIRGAP: decoded via a scratch buffer - wally needs room for the 4 checksum bytes it
        // strips, and the last signer of a body has exactly 78 + 1 bytes left (the trailing HMAC
        // that used to absorb this is now outside the body).
        uint8_t xpub_bytes[BIP32_SERIALIZED_LEN + BASE58_CHECKSUM_LEN];
        size_t written = 0;
        if (wally_base58_to_bytes(signer->xpub, BASE58_FLAG_CHECKSUM, xpub_bytes, sizeof(xpub_bytes), &written)
                != WALLY_OK
            || written != BIP32_SERIALIZED_LEN) {
            JADE_LOGE("Failed to parse/write signer %u xpub: '%s'", i, signer->xpub);
            return false;
        }
        memcpy(write_ptr, xpub_bytes, BIP32_SERIALIZED_LEN);
        write_ptr += BIP32_SERIALIZED_LEN;

        // Additional path

        // We do not support persisting path strings
        if (signer->path_is_string) {
            JADE_LOGE("Cannot persist multisig signer with string path");
            return false;
        }

        JADE_ASSERT(signer->path_len <= MAX_PATH_LEN);
        const uint8_t path_len = (uint8_t)signer->path_len;
        memcpy(write_ptr, &path_len, sizeof(path_len));
        write_ptr += sizeof(path_len);

        const size_t path_bytes_len = path_len * sizeof(signer->path[0]);
        memcpy(write_ptr, signer->path, path_bytes_len);
        write_ptr += path_bytes_len;
    }
    JADE_ASSERT(counted_path_elements == total_num_path_elements);
    JADE_ASSERT(write_ptr == body + body_len);
    return true;
}

// Since v3 signer data contains all the metadata from the original registration, such that
// the original registration could be recreated if required (eg. to export from the device).
// Can pass 'signer_t' structs to fetch that data now (in addition to the data needed for address generation)
static bool read_complete_signers(const uint8_t* const signer_bytes, const size_t signer_bytes_len,
    multisig_data_t* output, signer_t* signer_details, const size_t signer_details_len, size_t* written)
{
    JADE_ASSERT(signer_bytes);
    JADE_ASSERT(signer_bytes_len);

    // signer_details (incl written) is optional (passed for more detailed data)
    JADE_ASSERT(signer_details || !signer_details_len);
    JADE_ASSERT(written || !signer_details);
    if (written) {
        *written = 0;
    }

    const uint8_t* read_ptr = signer_bytes;

    // Num signers
    const uint8_t num_signers = *read_ptr;
    if (!num_signers || num_signers > MAX_ALLOWED_SIGNERS) {
        JADE_LOGE("Bad number of signers read from registered multisig data");
        return false;
    }
    output->num_xpubs = num_signers;
    read_ptr += sizeof(num_signers);

    for (size_t i = 0; i < num_signers; ++i) {
        // Do we want to return signer details
        signer_t* const signer = (signer_details && i < signer_details_len) ? signer_details + i : NULL;

        if (signer) {
            // Key origin information
            memcpy(signer->fingerprint, read_ptr, sizeof(signer->fingerprint));
        }
        read_ptr += sizeof(signer->fingerprint);

        uint8_t derivation_len = 0;
        memcpy(&derivation_len, read_ptr, sizeof(derivation_len));
        const size_t derivation_bytes_len = derivation_len * sizeof(signer->derivation[0]);
        if (derivation_len > MAX_PATH_LEN || derivation_bytes_len > sizeof(signer->derivation)) {
            JADE_LOGE("Overlong derivation path length %u for signer %u", derivation_len, i);
            return false;
        }
        read_ptr += sizeof(derivation_len);

        if (signer) {
            signer->derivation_len = derivation_len;
            memcpy(signer->derivation, read_ptr, derivation_bytes_len);
        }
        read_ptr += derivation_bytes_len;

        // Xpub as passed (not the derived immediate parent xpub)
        // Copy it into the output position now - it may get overwritten later if there is additional path
        uint8_t* const xpub = output->xpubs + (i * BIP32_SERIALIZED_LEN);
        memcpy(xpub, read_ptr, BIP32_SERIALIZED_LEN);
        read_ptr += BIP32_SERIALIZED_LEN;

        if (signer) {
            // Write xpub as string into signer details
            char* pstr = NULL;
            if (wally_base58_from_bytes(xpub, BIP32_SERIALIZED_LEN, BASE58_FLAG_CHECKSUM, &pstr) != WALLY_OK || !pstr) {
                JADE_LOGE("Failed to dump signer %u xpub as string", i);
                return false;
            }

            const size_t len = strlen(pstr);
            if (len >= sizeof(signer->xpub)) {
                JADE_LOGE("Signer %u xpub string too long %u: %s", i, len, pstr);
                JADE_WALLY_VERIFY(wally_free_string(pstr));
                return false;
            }

            strcpy(signer->xpub, pstr);
            signer->xpub_len = len;
            JADE_WALLY_VERIFY(wally_free_string(pstr));
        }

        // Additional path
        // NOTE: path strings not supported - explicit numeric array only
        uint8_t path_len = 0;
        memcpy(&path_len, read_ptr, sizeof(path_len));
        const size_t path_bytes_len = path_len * sizeof(signer->path[0]);
        if (path_len > MAX_PATH_LEN || path_bytes_len > sizeof(signer->path)) {
            JADE_LOGE("Overlong additional path length %u for signer %u", path_len, i);
            return false;
        }
        read_ptr += sizeof(path_len);

        if (path_len) {
            // Need to overwrite the output xpub with a further derived one
            struct ext_key hdkey;
            uint32_t path[MAX_PATH_LEN];
            memcpy(path, read_ptr, path_bytes_len);
            if (!wallet_derive_pubkey(
                    xpub, BIP32_SERIALIZED_LEN, path, path_len, BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, &hdkey)) {
                JADE_LOGE("Failed to derive immediate parent pubkey for signer %u", i);
                return false;
            }
            if (bip32_key_serialize(&hdkey, BIP32_FLAG_KEY_PUBLIC, xpub, BIP32_SERIALIZED_LEN) != WALLY_OK) {
                JADE_LOGE("Failed to serialise derived parent xpub for signer %u", i);
                return false;
            }
        }

        if (signer) {
            signer->path_is_string = false;
            signer->path_len = path_len;
            memcpy(signer->path, read_ptr, path_bytes_len);
        }
        read_ptr += path_bytes_len;
    }

    // Return the total number of signers
    if (written) {
        *written = num_signers;
    }

    if (read_ptr != signer_bytes + signer_bytes_len) {
        JADE_LOGE("Unexpected multisig data length for %d signers", num_signers);
        return false;
    }

    return true;
}

static bool multisig_body_from_bytes(const uint8_t* body, const size_t body_len, multisig_data_t* output,
    signer_t* signer_details, const size_t signer_details_len, size_t* written)
{
    JADE_ASSERT(body);
    JADE_ASSERT(output);

    if (body_len < MIN_MULTISIG_BODY_LEN) {
        JADE_LOGE("Multisig record body too short: %u", body_len);
        return false;
    }

    const uint8_t* read_ptr = body;
    const uint8_t* const end = body + body_len;

    // Script variant
    output->variant = (script_variant_t)*read_ptr;
    read_ptr += sizeof(uint8_t);

    // 'sorted' flag
    output->sorted = (bool)*read_ptr;
    read_ptr += sizeof(uint8_t);

    // Threshold
    output->threshold = *read_ptr;
    read_ptr += sizeof(uint8_t);

    // Blinding key len, and data
    output->master_blinding_key_len = 0;
    const uint8_t keylen = *read_ptr;
    read_ptr += sizeof(keylen);
    if (keylen) {
        if (keylen != sizeof(output->master_blinding_key) || read_ptr + keylen > end) {
            JADE_LOGE("Unexpected blinding key length %d", keylen);
            return false;
        }
        output->master_blinding_key_len = keylen;
        memcpy(output->master_blinding_key, read_ptr, keylen);
        read_ptr += keylen;
    }

    // All bytes remaining are the signer data (num_signers byte, then signers)
    return read_complete_signers(read_ptr, end - read_ptr, output, signer_details, signer_details_len, written);
}

bool multisig_data_from_bytes(const uint8_t* bytes, const size_t bytes_len, multisig_data_t* output,
    signer_t* signer_details, const size_t signer_details_len, size_t* written)
{
    JADE_ASSERT(bytes);
    JADE_ASSERT(output);

    // signer_details (incl written) is optional (passed for more detailed data)
    JADE_ASSERT(signer_details || !signer_details_len);
    JADE_ASSERT(written || !signer_details);
    if (written) {
        *written = 0;
    }

    // BBB-AIRGAP: open the sealed record onto the heap (up to 3217 bytes) and parse the plaintext
    // body; the body is wiped before the buffer is released.
    size_t body_len = 0;
    uint8_t* const body = JADE_MALLOC(MAX_MULTISIG_BODY_LEN);
    const bool ret = multisig_open_registration(bytes, bytes_len, body, MAX_MULTISIG_BODY_LEN, &body_len)
        && multisig_body_from_bytes(body, body_len, output, signer_details, signer_details_len, written);
    JADE_WALLY_VERIFY(wally_bzero(body, MAX_MULTISIG_BODY_LEN));
    free(body);
    return ret;
}

bool multisig_load_from_storage(const char* multisig_name, multisig_data_t* output, signer_t* signer_details,
    const size_t signer_details_len, size_t* written, const char** errmsg)
{
    JADE_ASSERT(multisig_name);
    JADE_ASSERT(output);
    JADE_INIT_OUT_PPTR(errmsg);

    // signer_details (incl written) is optional (passed for more detailed data)
    JADE_ASSERT(signer_details || !signer_details_len);
    JADE_ASSERT(written || !signer_details);
    if (written) {
        *written = 0;
    }

    size_t registration_len = 0;
    uint8_t* const registration = JADE_MALLOC(MAX_MULTISIG_BYTES_LEN); // Sufficient
    if (!storage_get_multisig_registration(multisig_name, registration, MAX_MULTISIG_BYTES_LEN, &registration_len)) {
        *errmsg = "Cannot find named multisig wallet";
        free(registration);
        return false;
    }

    if (!multisig_data_from_bytes(
            registration, registration_len, output, signer_details, signer_details_len, written)) {
        *errmsg = "Cannot de-serialise multisig wallet data";
        free(registration);
        return false;
    }

    // Sanity check data we are have loaded
    if (!is_multisig(output->variant) || output->threshold == 0 || output->threshold > output->num_xpubs
        || !output->num_xpubs || output->num_xpubs > MAX_ALLOWED_SIGNERS
        || (output->master_blinding_key_len && output->master_blinding_key_len != MULTISIG_MASTER_BLINDING_KEY_SIZE)) {
        *errmsg = "Multisig wallet data invalid";
        free(registration);
        return false;
    }

    free(registration);
    return true;
}

bool multisig_validate_paths(
    const bool is_change, CborValue* all_signer_paths, bool* all_paths_as_expected, bool* final_elements_consistent)
{
    JADE_ASSERT(all_signer_paths);
    JADE_ASSERT(all_paths_as_expected);

    bool seen_unusual_path = false;
    bool seen_final_element_mismatch = false;

    size_t num_array_items = 0;
    if (cbor_value_get_array_length(all_signer_paths, &num_array_items) != CborNoError || num_array_items == 0) {
        return false;
    }

    uint32_t expected_final_path_element;
    uint32_t path[MAX_PATH_LEN];
    const size_t max_path_len = sizeof(path) / sizeof(path[0]);

    CborValue arrayItem;
    CborError cberr = cbor_value_enter_container(all_signer_paths, &arrayItem);
    JADE_ASSERT(cberr == CborNoError);
    for (size_t i = 0; i < num_array_items; ++i) {
        JADE_ASSERT(!cbor_value_at_end(&arrayItem));

        size_t path_len = 0;
        if (!rpc_get_bip32_path_from_value(&arrayItem, path, max_path_len, &path_len) || path_len == 0) {
            return false;
        }

        if (!wallet_is_expected_multisig_path(i, is_change, path, path_len)) {
            // Path is valid, but does not fit an expected pattern/format
            seen_unusual_path = true;
        }

        if (i == 0) {
            expected_final_path_element = path[path_len - 1];
        } else if (path[path_len - 1] != expected_final_path_element) {
            // Final path element varies across signers
            seen_final_element_mismatch = true;
        }
    }

    *all_paths_as_expected = !seen_unusual_path;
    *final_elements_consistent = !seen_final_element_mismatch;
    return true;
}

bool multisig_get_pubkeys(const uint8_t* xpubs, const size_t num_xpubs, CborValue* all_signer_paths, uint8_t* pubkeys,
    const size_t pubkeys_len, size_t* written)
{
    JADE_ASSERT(xpubs);
    JADE_ASSERT(num_xpubs >= 1);
    JADE_ASSERT(all_signer_paths);
    JADE_ASSERT(pubkeys);
    JADE_ASSERT(pubkeys_len >= num_xpubs * EC_PUBLIC_KEY_LEN);
    JADE_INIT_OUT_SIZE(written);

    // Check the number of signers
    if (cbor_value_get_array_length(all_signer_paths, written) != CborNoError || *written != num_xpubs) {
        return false;
    }

    uint32_t path[MAX_PATH_LEN];
    const size_t max_path_len = sizeof(path) / sizeof(path[0]);

    CborValue arrayItem;
    CborError cberr = cbor_value_enter_container(all_signer_paths, &arrayItem);
    JADE_ASSERT(cberr == CborNoError);
    for (size_t i = 0; i < num_xpubs; ++i) {
        JADE_ASSERT(!cbor_value_at_end(&arrayItem));

        size_t path_len = 0;
        if (!rpc_get_bip32_path_from_value(&arrayItem, path, max_path_len, &path_len) || path_len == 0) {
            return false;
        }
        for (size_t j = 0; j < path_len; ++j) {
            if (path[j] & BIP32_INITIAL_HARDENED_CHILD) {
                return false;
            }
        }

        struct ext_key hdkey;
        const uint8_t* xpub = xpubs + (i * BIP32_SERIALIZED_LEN);
        if (!wallet_derive_pubkey(xpub, BIP32_SERIALIZED_LEN, path, path_len, BIP32_FLAG_SKIP_HASH, &hdkey)) {
            return false;
        }

        uint8_t* dest = pubkeys + (i * EC_PUBLIC_KEY_LEN);
        memcpy(dest, hdkey.pub_key, EC_PUBLIC_KEY_LEN);
    }
    *written = num_xpubs * EC_PUBLIC_KEY_LEN;

    return true;
}

bool multisig_get_master_blinding_key(const multisig_data_t* multisig_data, uint8_t* master_blinding_key,
    const size_t master_blinding_key_len, const char** errmsg)
{
    JADE_ASSERT(multisig_data);
    JADE_ASSERT(master_blinding_key);
    JADE_ASSERT(master_blinding_key_len == HMAC_SHA512_LEN);
    JADE_INIT_OUT_PPTR(errmsg);

    if (multisig_data->master_blinding_key_len != sizeof(multisig_data->master_blinding_key)) {
        *errmsg = "No blinding key for multisig record";
        return false;
    }

    // Need full SHA512 for low-level calls - pad front with 0's
    memset(master_blinding_key, 0, master_blinding_key_len - multisig_data->master_blinding_key_len);
    memcpy(master_blinding_key + master_blinding_key_len - multisig_data->master_blinding_key_len,
        multisig_data->master_blinding_key, multisig_data->master_blinding_key_len);

    return true;
}

// Are the Jade script variant and the wally script type consistent
static inline bool variant_matches_script_type(const script_variant_t variant, const size_t* script_type)
{
    return !script_type || (*script_type == WALLY_SCRIPT_TYPE_P2WSH && variant == MULTI_P2WSH)
        || (*script_type == WALLY_SCRIPT_TYPE_P2SH && (variant == MULTI_P2SH || variant == MULTI_P2WSH_P2SH));
}

// Get the registered multisig record names
// Filtered to those valid for this signer, and optionally for the given script type
void multisig_get_valid_record_names(
    const size_t* script_type, char names[][MAX_MULTISIG_NAME_SIZE], const size_t num_names, size_t* num_written)
{
    // script_type filter is optional
    JADE_ASSERT(names);
    JADE_ASSERT(num_names);
    JADE_INIT_OUT_SIZE(num_written);

    // Get registered multisig names
    size_t num_multisigs = 0;
    if (!storage_get_all_multisig_registration_names(names, num_names, &num_multisigs) || !num_multisigs) {
        // No registered multisig records
        return;
    }

    // Load description of each - remove ones that are not valid for this wallet or passed script type
    size_t written = 0;
    for (size_t i = 0; i < num_multisigs; ++i) {
        const char* errmsg = NULL;
        multisig_data_t multisig_data;
        if (multisig_load_from_storage(names[i], &multisig_data, NULL, 0, NULL, &errmsg)
            && variant_matches_script_type(multisig_data.variant, script_type)) {
            // If any previous records were not valid, move subsequent valid record names down
            if (written != i) {
                strcpy(names[written], names[i]);
            }
            ++written;
        }
    }
    *num_written = written;
}

// Export a multisig in the common (coldcard) file format
static bool write_text(const char* text, const bool add_eol, char* output, const size_t output_len, size_t* written)
{
    JADE_ASSERT(text);
    JADE_ASSERT(output);
    JADE_INIT_OUT_SIZE(written);

    const size_t len = strlen(text);
    if (len >= output_len) {
        JADE_LOGE("Output buffer filled, failed to write line: %s", text);
        return false;
    }

    strcpy(output, text);

    if (add_eol) {
        output[len] = '\n';
        output[len + 1] = '\0';
    }

    *written = add_eol ? len + 1 : len;
    return true;
}

#define WRITE_TEXT(text, add_eol)                                                                                      \
    do {                                                                                                               \
        size_t written = 0;                                                                                            \
        if (!write_text(text, add_eol, write_ptr, end - write_ptr, &written)) {                                        \
            return false;                                                                                              \
        }                                                                                                              \
        write_ptr += written;                                                                                          \
    } while (false)

#define WRITE_KEY_VALUE_LINE(key, value)                                                                               \
    do {                                                                                                               \
        WRITE_TEXT(key, false);                                                                                        \
        WRITE_TEXT(": ", false);                                                                                       \
        WRITE_TEXT(value, true);                                                                                       \
    } while (false)

bool multisig_create_export_file(const char* multisig_name, const multisig_data_t* multisig_data,
    const signer_t* signer_details, const size_t num_signer_details, char* output, const size_t output_len,
    size_t* written)
{
    JADE_ASSERT(multisig_name);
    JADE_ASSERT(multisig_data);
    JADE_ASSERT(signer_details);
    JADE_ASSERT(num_signer_details == multisig_data->num_xpubs);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len >= MULTISIG_FILE_MAX_LEN(num_signer_details));
    JADE_INIT_OUT_SIZE(written);

    char* write_ptr = output;
    char* const end = output + output_len;
    char buf[MAX_PATH_STR_LEN(MAX_PATH_LEN)];

    // Comment and name
    WRITE_TEXT("# Exported by Blockstream Jade", true);
    WRITE_KEY_VALUE_LINE(MSIG_FILE_NAME, multisig_name);

    // Policy
    const int ret = snprintf(buf, sizeof(buf), "%u of %u", multisig_data->threshold, multisig_data->num_xpubs);
    JADE_ASSERT(ret && ret < sizeof(buf));
    WRITE_KEY_VALUE_LINE(MSIG_FILE_POLICY, buf);

    // Format (script type)
    const char* format = NULL;
    if (multisig_data->variant == MULTI_P2WSH) {
        format = "P2WSH";
    } else if (multisig_data->variant == MULTI_P2WSH_P2SH) {
        format = "P2SH-P2WSH";
    } else if (multisig_data->variant == MULTI_P2SH) {
        format = "P2SH";
    } else {
        JADE_LOGE("Unhandled multisig variant: %u", multisig_data->variant);
        return false;
    }
    WRITE_KEY_VALUE_LINE(MSIG_FILE_FORMAT, format);

    // Only output sorted flag if the multisig is not sorted
    // (as sorted is the default, and the 'sorted' flag is not standard)
    if (!multisig_data->sorted) {
        WRITE_KEY_VALUE_LINE(MSIG_FILE_SORTED, "False");
    }

    // Only output blinding key if it's set (as the 'blindingkey' flag is not standard)
    if (multisig_data->master_blinding_key_len) {
        char* blinding_key_hex = NULL;
        JADE_WALLY_VERIFY(wally_hex_from_bytes(
            multisig_data->master_blinding_key, multisig_data->master_blinding_key_len, &blinding_key_hex));
        WRITE_KEY_VALUE_LINE(MSIG_FILE_BLINDING_KEY, blinding_key_hex);
        JADE_WALLY_VERIFY(wally_free_string(blinding_key_hex));
    }

    // Signers
    for (size_t i = 0; i < num_signer_details; ++i) {
        const signer_t* const signer = signer_details + i;
        JADE_ASSERT(signer->xpub[signer->xpub_len] == '\0');

        if (signer->path_len || signer->path_is_string) {
            JADE_LOGW("Multisig signers with additional path cannot be exported");
            return false;
        }

        // Derivation
        const bool path_only = false;
        if (!wallet_bip32_path_as_str(signer->derivation, signer->derivation_len, buf, sizeof(buf), path_only)) {
            JADE_LOGE("Multisig signer derivation path error");
            return false;
        }
        WRITE_KEY_VALUE_LINE(MSIG_FILE_DERIVATION, buf);

        // Fingerprint: xpub
        char* fingerprint_hex = NULL;
        JADE_WALLY_VERIFY(wally_hex_from_bytes(signer->fingerprint, sizeof(signer->fingerprint), &fingerprint_hex));
        WRITE_KEY_VALUE_LINE(fingerprint_hex, signer->xpub);
        JADE_WALLY_VERIFY(wally_free_string(fingerprint_hex));
    }

    JADE_ASSERT(write_ptr < end);
    JADE_ASSERT(*write_ptr == '\0');

    *written = write_ptr - output;
    return true;
}
#endif // AMALGAMATED_BUILD
