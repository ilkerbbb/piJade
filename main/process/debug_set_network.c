#ifndef AMALGAMATED_BUILD
#include "../jade_assert.h"
#include "../keychain.h"
#include "../process.h"
#include "../utils/cbor_rpc.h"
#include "../utils/network.h"

#include <string.h>

#include "process_utils.h"

#ifdef CONFIG_DEBUG_MODE
// BBB-AIRGAP: this fork refuses every registration whose network does not match the device
// setting (register_multisig.c, register_descriptor.c), and on a debug build nothing can move
// that setting off 'none': auth_user.c only restricts on a release build, so the sole remaining
// writer is the Settings > Network screen, which needs a button press.  The test suite has to
// register testnet and mainnet records in one run, so it needs the same choice the user makes
// from that screen.  This handler is that screen and nothing more - the two calls below are
// exactly what handle_network_type() runs (dashboard.c), so the emulator's behaviour stays what
// ships.  It is a debug handler like debug_set_mnemonic: the production image is built with
// -DDEBUG_MODE=0 (pijade/images/build-armv6.sh), which leaves CONFIG_DEBUG_MODE undefined and
// compiles none of this in.
void debug_set_network_process(void* process_ptr)
{
    JADE_LOGI("Starting: %d", xPortGetFreeHeapSize());
    jade_process_t* process = process_ptr;

    // We expect a current message to be present
    ASSERT_CURRENT_MESSAGE(process, "debug_set_network");

    GET_MSG_PARAMS(process);

    char network[MAX_NETWORK_NAME_LEN];
    size_t network_len = 0;
    rpc_get_string("network", sizeof(network), &params, network, &network_len);
    if (!network_len) {
        jade_process_reject_message(process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract network from parameters");
        goto cleanup;
    }

    // The settings screen asserts a wallet is loaded before it offers the choice, because the
    // in-memory value is only written when there are keys to restrict (keychain.c).  Without
    // that check the caller would get an ok reply and no change at all.
    if (!keychain_get()) {
        jade_process_reject_message(
            process, CBOR_RPC_HW_LOCKED, "Setting the network restriction requires a loaded wallet");
        goto cleanup;
    }

    // 'none' restores the unrestricted state a debug build starts in, so a caller can put the
    // device back the way it found it.  Resolve the name before touching the restriction, so a
    // rejected message leaves the device untouched.
    network_type_t network_type = NETWORK_TYPE_NONE;
    if (strcmp(network, "none")) {
        const network_t network_id = network_from_name(network);
        if (network_id == NETWORK_NONE) {
            jade_process_reject_message(
                process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract valid network from parameters");
            goto cleanup;
        }
        network_type = network_to_type(network_id);
    }

    keychain_clear_network_type_restriction();
    if (network_type != NETWORK_TYPE_NONE) {
        keychain_set_network_type_restriction(network_type);
    }

    jade_process_reply_to_message_ok(process);
    JADE_LOGI("Success");

cleanup:
    return;
}
#endif // CONFIG_DEBUG_MODE
#endif // AMALGAMATED_BUILD
