#ifndef AMALGAMATED_BUILD
#include "../descriptor.h"
#include "../descriptor_text.h"
#include "../jade_assert.h"
#include "../jade_wally_verify.h"
#include "../keychain.h"
#include "../process.h"
#include "../storage.h"
#include "../ui.h"
#include "../utils/cbor_rpc.h"
#include "../utils/malloc_ext.h"
#include "../utils/network.h"
#include "../wallet.h"

#include "process_utils.h"

#include <ctype.h>
#include <sodium/utils.h>

bool show_descriptor_activity(const char* descriptor_name, const descriptor_data_t* descriptor,
    const char* blinding_key, const signer_t* signer_details, size_t num_signer_details,
    const uint8_t* wallet_fingerprint, size_t wallet_fingerprint_len, network_t network_id, bool initial_confirmation,
    bool overwriting, bool is_valid);

// Function to validate descriptor and persist the record
static int register_descriptor(
    const char* descriptor_name, const network_t network_id, descriptor_data_t* descriptor, const char** errmsg)
{
    JADE_ASSERT(descriptor_name);
    JADE_ASSERT(network_id != NETWORK_NONE);
    JADE_ASSERT(descriptor);
    JADE_INIT_OUT_PPTR(errmsg);

    JADE_ASSERT(descriptor->script_len < sizeof(descriptor->script));
    JADE_ASSERT(descriptor->num_values <= MAX_ALLOWED_SIGNERS);

    // BBB-AIRGAP: a descriptor record does not carry its network (descriptor_data_t,
    // main/descriptor.h), while the address explorer reconstructs one exact bitcoin network from
    // the device setting.  Accepting liquid, localtest, or testnet while an unrestricted debug
    // keychain defaults to mainnet would therefore persist a record that the explorer later opens
    // under a different network.  Refuse every mismatch here, including the CONFIG_DEBUG_MODE
    // liquid exception in descriptor_allow_liquid(), so emulator behaviour remains what ships.
    const network_t supported_network
        = keychain_get_network_type_restriction() == NETWORK_TYPE_TEST ? NETWORK_BITCOIN_TESTNET : NETWORK_BITCOIN;
    if (network_id != supported_network) {
        *errmsg = "Descriptor network does not match device setting";
        return CBOR_RPC_BAD_PARAMETERS;
    }

    // Check name valid
    if (!storage_key_name_valid(descriptor_name)) {
        *errmsg = "Invalid descriptor name";
        return CBOR_RPC_BAD_PARAMETERS;
    }

    int retval = 0;
    const size_t body_len = DESCRIPTOR_BODY_LEN(descriptor);
    const size_t registration_len = DESCRIPTOR_BYTES_LEN(descriptor);
    uint8_t* const body = JADE_MALLOC(body_len);
    uint8_t* const registration = JADE_MALLOC(registration_len);
    signer_t* const signers = JADE_CALLOC(MAX_ALLOWED_SIGNERS, sizeof(signer_t));
    size_t num_signers = 0;
    char* blinding_key = NULL;

    // Get signers - this also yields the type
    descriptor_type_t deduced_type = DESCRIPTOR_TYPE_UNKNOWN;
    if (!descriptor_get_signers(descriptor_name, descriptor, network_id, &deduced_type, signers, MAX_ALLOWED_SIGNERS,
            &num_signers, &blinding_key, errmsg)) {
        JADE_LOGE("Failed to extract signer information from descriptor");
        retval = CBOR_RPC_BAD_PARAMETERS;
        goto cleanup;
    }

    // Do not support pure miniscript expressions atm
    if (deduced_type != DESCRIPTOR_TYPE_MIXED) {
        *errmsg = "Pure miniscript expressions not supported";
        retval = CBOR_RPC_BAD_PARAMETERS;
        goto cleanup;
    }
    descriptor->type = deduced_type;

    // Validate signers - this also yields the type
    size_t total_path_elements = 0; // unused
    const bool allow_string_paths = true; // child paths may be string
    uint8_t fingerprint[BIP32_KEY_FINGERPRINT_LEN];
    wallet_get_fingerprint(fingerprint, sizeof(fingerprint));
    if (!validate_signers(
            signers, num_signers, allow_string_paths, fingerprint, sizeof(fingerprint), &total_path_elements)) {
        *errmsg = "Failed to validate signers";
        retval = CBOR_RPC_BAD_PARAMETERS;
        goto cleanup;
    }

    // Validate descriptor by fetching an external and change address
    char* addr0 = NULL;
    char* addr1 = NULL;
    if (!descriptor_to_address(descriptor_name, descriptor, network_id, 0, 0, NULL, &addr0, errmsg)
        || !descriptor_to_address(descriptor_name, descriptor, network_id, 1, 0, NULL, &addr1, errmsg)) {
        // errmsg populated by prior call
        retval = CBOR_RPC_BAD_PARAMETERS;
        goto cleanup;
    }
    JADE_WALLY_VERIFY(wally_free_string(addr0));
    JADE_WALLY_VERIFY(wally_free_string(addr1));

    if (!descriptor_body_to_bytes(descriptor, body, body_len)
        || !descriptor_seal_body(body, body_len, registration, registration_len)) {
        *errmsg = "Failed to serialise descriptor";
        retval = CBOR_RPC_INTERNAL_ERROR;
        goto cleanup;
    }

    // See if a record for this name exists already
    const bool overwriting = storage_descriptor_name_exists(descriptor_name);

    // If so, see if it is identical to the record we are trying to persist
    // - if so, just return true immediately.
    if (overwriting) {
        // BBB-AIRGAP: same reason as register_multisig(): random IV, so compare opened bodies.
        size_t existing_len = 0;
        size_t existing_body_len = 0;
        uint8_t* const existing = JADE_MALLOC(MAX_DESCRIPTOR_BYTES_LEN);
        uint8_t* const existing_body = JADE_MALLOC(MAX_DESCRIPTOR_BODY_LEN);
        const bool identical
            = storage_get_descriptor_registration(descriptor_name, existing, MAX_DESCRIPTOR_BYTES_LEN, &existing_len)
            && descriptor_open_registration(
                existing, existing_len, existing_body, MAX_DESCRIPTOR_BODY_LEN, &existing_body_len)
            && existing_body_len == body_len && !sodium_memcmp(existing_body, body, body_len);
        JADE_WALLY_VERIFY(wally_bzero(existing_body, MAX_DESCRIPTOR_BODY_LEN));
        free(existing_body);
        free(existing);
        if (identical) {
            // BBB-AIRGAP: upstream returned here without a word
            // (fdb67a3f:main/process/register_descriptor.c:111), which on a QR-only device reads as the scan
            // having been ignored: the screen goes straight back to the
            // dashboard and nothing says whether a second copy was stored (seen on device round 6).
            // The record is genuinely unchanged, so this is a notice and not a question; it names
            // the record so the reader can see WHICH registration was already held.  Shown on the
            // message path too, the way the "Name In Use" question below already is.
            JADE_LOGI("Descriptor %s: identical registration exists, returning immediately", descriptor_name);
            await_titled_message("Already registered", descriptor_name);
            goto cleanup;
        }

        // BBB-AIRGAP: same guard as register_multisig(), for the same reason - the storage key is
        // the name alone while the record is sealed to the wallet that registered it, so a name
        // reused from a second wallet destroys the first wallet's record with no screen naming
        // its owner.  Kept in step with the multisig branch: descriptors now also arrive from the
        // QR path (register_descriptor_text() below), so this guard is reachable on this device.
        // Heap rather than stack because descriptor_data_t is some
        // 3KB and RPC handlers run inline on the dashboard task (dashboard.c:635 calls
        // task_function() directly), so there is no separate stack to spend it on.
        descriptor_data_t* const existing_data = JADE_MALLOC(sizeof(descriptor_data_t));
        const char* load_errmsg = NULL;
        const bool readable = descriptor_load_from_storage(descriptor_name, existing_data, &load_errmsg);
        free(existing_data);
        if (!readable) {
            JADE_LOGW("Descriptor %s: existing record not readable by current wallet: %s", descriptor_name,
                load_errmsg ? load_errmsg : "no detail");
            const char* question[] = { "Existing record is", "not readable by this", "wallet. Replace it?" };
            if (!await_yesno_activity("Name In Use", question, 3, false, NULL)) {
                *errmsg = "User declined to overwrite descriptor";
                retval = CBOR_RPC_USER_CANCELLED;
                goto cleanup;
            }
        }
    } else {
        // Not overwriting an existing record - check storage slot available
        if (storage_get_descriptor_registration_count() >= MAX_DESCRIPTOR_REGISTRATIONS) {
            *errmsg = "Already have maximum number of descriptor wallets";
            retval = CBOR_RPC_BAD_PARAMETERS;
            goto cleanup;
        }
    }

    // Check to see whether user accepted or declined
    const bool is_valid = true;
    const bool initial_confirmation = true;
    if (!show_descriptor_activity(descriptor_name, descriptor, blinding_key, signers, num_signers, fingerprint,
            sizeof(fingerprint), network_id, initial_confirmation, overwriting, is_valid)) {
        JADE_LOGW("User declined to register descriptor");
        *errmsg = "User declined to register descriptor";
        retval = CBOR_RPC_USER_CANCELLED;
        goto cleanup;
    }
    JADE_LOGD("User accepted descriptor");

    // Persist descriptor registration in nvs
    if (!storage_set_descriptor_registration(descriptor_name, registration, registration_len)) {
        *errmsg = "Failed to persist descriptor data";

        await_error("Error saving descriptor");
        retval = CBOR_RPC_INTERNAL_ERROR;
        goto cleanup;
    }

cleanup:
    free(signers);
    if (blinding_key) {
        JADE_WALLY_VERIFY(wally_free_string(blinding_key));
    }
    // The body carries the descriptor keys in plaintext
    JADE_WALLY_VERIFY(wally_bzero(body, body_len));
    free(body);
    free(registration);
    return retval;
}

// BBB-AIRGAP: entry point for a descriptor scanned as a QR (text, Specter JSON export, or the
// text bcur_parse_crypto_output() makes from a crypto-output UR).  Same validation and screens
// as the RPC path: descriptor_text_parse() only rewrites the text into the persisted form.
int register_descriptor_text(const char* text, const size_t text_len, const char** errmsg)
{
    JADE_ASSERT(text);
    JADE_ASSERT(text_len);
    JADE_INIT_OUT_PPTR(errmsg);

    // Same network rule as register_multisig_file() and the address explorer
    const network_t network_id
        = keychain_get_network_type_restriction() == NETWORK_TYPE_TEST ? NETWORK_BITCOIN_TESTNET : NETWORK_BITCOIN;

    // Heap: descriptor_data_t is some 3KB and this runs on the dashboard task
    char descriptor_name[MAX_DESCRIPTOR_NAME_SIZE];
    descriptor_data_t* const descriptor = JADE_MALLOC(sizeof(descriptor_data_t));
    int retval = CBOR_RPC_BAD_PARAMETERS;
    if (descriptor_text_parse(text, text_len, descriptor, descriptor_name, sizeof(descriptor_name), errmsg)) {
        retval = register_descriptor(descriptor_name, network_id, descriptor, errmsg);
    }
    free(descriptor);
    return retval;
}

static bool get_data_values(const char* field, const CborValue* value, descriptor_data_t* descriptor)
{
    JADE_ASSERT(field);
    JADE_ASSERT(value);
    JADE_ASSERT(descriptor);
    JADE_ASSERT(!descriptor->num_values);

    CborValue result;
    if (!rpc_get_map(field, value, &result)) {
        return false;
    }

    size_t num_map_items = 0;
    if (cbor_value_get_map_length(&result, &num_map_items) != CborNoError || !num_map_items
        || num_map_items > MAX_ALLOWED_SIGNERS) {
        return false;
    }

    CborValue keyItem;
    CborError cberr = cbor_value_enter_container(&result, &keyItem);
    if (cberr != CborNoError || !cbor_value_is_valid(&keyItem)) {
        return false;
    }

    for (size_t i = 0; i < num_map_items; ++i) {
        JADE_ASSERT(!cbor_value_at_end(&keyItem));
        string_value_t* const sv = &(descriptor->values[i]);
        CborValue valueItem;

        if (!cbor_value_is_text_string(&keyItem)) {
            return false;
        }

        size_t written = sizeof(sv->key);
        cberr = cbor_value_copy_text_string(&keyItem, sv->key, &written, &valueItem);
        if (cberr != CborNoError || !written || written >= sizeof(sv->key)) {
            return false;
        }
        sv->key_len = (uint8_t)written;

        if (!cbor_value_is_text_string(&valueItem)) {
            return false;
        }

        written = sizeof(sv->value);
        cberr = cbor_value_copy_text_string(&valueItem, sv->value, &written, &keyItem);
        if (cberr != CborNoError || !written || written >= sizeof(sv->value)) {
            return false;
        }
        sv->value_len = (uint16_t)written;
    }

    descriptor->num_values = num_map_items;
    return true;
}

void register_descriptor_process(void* process_ptr)
{
    JADE_LOGI("Starting: %d", xPortGetFreeHeapSize());
    jade_process_t* process = process_ptr;

    char descriptor_name[MAX_DESCRIPTOR_NAME_SIZE];
    const char* errmsg = NULL;

    // We expect a current message to be present
    ASSERT_CURRENT_MESSAGE(process, "register_descriptor");
    ASSERT_KEYCHAIN_UNLOCKED_BY_MESSAGE_SOURCE(process);
    GET_MSG_PARAMS(process);
    CHECK_NETWORK_CONSISTENT(process);

    // Get name of descriptor wallet
    size_t written = 0;
    rpc_get_string("descriptor_name", sizeof(descriptor_name), &params, descriptor_name, &written);
    if (written == 0 || !storage_key_name_valid(descriptor_name)) {
        jade_process_reject_message(process, CBOR_RPC_BAD_PARAMETERS, "Missing or invalid descriptor name parameter");
        goto cleanup;
    }

    // Descriptor script and paramaterised values
    descriptor_data_t descriptor = { .script_len = 0, .num_values = 0, .type = DESCRIPTOR_TYPE_UNKNOWN };

    written = 0;
    rpc_get_string("descriptor", sizeof(descriptor.script), &params, descriptor.script, &written);
    if (!written) {
        jade_process_reject_message(
            process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract valid output descriptor string");
        goto cleanup;
    }
    descriptor.script_len = (uint16_t)written;

    // Signers' keys
    if (!get_data_values("datavalues", &params, &descriptor)) {
        jade_process_reject_message(process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract valid parameter values");
        goto cleanup;
    }

    const int errcode = register_descriptor(descriptor_name, network_id, &descriptor, &errmsg);
    if (errcode) {
        jade_process_reject_message(process, errcode, errmsg);
        goto cleanup;
    }

    // Ok, all verified and persisted
    jade_process_reply_to_message_ok(process);
    JADE_LOGI("Success");

cleanup:
    return;
}
#endif // AMALGAMATED_BUILD
