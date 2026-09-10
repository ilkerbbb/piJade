#ifndef AMALGAMATED_BUILD
#include "../bcur.h"
#include "../button_events.h"
#include "../descriptor.h"
#include "../display.h"
#include "../idletimer.h"
#include "../input.h"
#include "../jade_assert.h"
#include "../jade_wally_verify.h"
#include "../keychain.h"
#include "../multisig.h"
#include "../otpauth.h"
#include "../power.h"
#include "../process.h"
#include "../qrmode.h"
#include "../random.h"
#include "../sensitive.h"
#include "../serial.h"
#include "../storage.h"
#include "../ui.h"
#include "../utils/cbor_rpc.h"
#include "../utils/event.h"
#include "../utils/malloc_ext.h"
#include "../utils/network.h"
#include "../utils/util.h"
#include "../utils/wally_ext.h"
#include "../wallet.h"

#include <string.h>

#ifdef CONFIG_HAS_CAMERA
#include "../camera.h"
#endif
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "usbhmsc/usbhmsc.h"
#include "usbhmsc/usbmode.h"
#endif
#include <esp_app_desc.h>
#include <esp_system.h>

// A genuine production v2 Jade may be awaiting mandatory attestation data
#if defined(CONFIG_BOARD_TYPE_JADE_V2_ANY) && defined(CONFIG_SECURE_BOOT)                                              \
    && defined(CONFIG_SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS)
#include "attestation/attestation.h"
static inline bool awaiting_attestation_data(void) { return !attestation_initialised(); }
#else
// Jade v1.x and diy devices are never awaiting mandatory attestation data
static inline bool awaiting_attestation_data(void) { return false; }
#endif

#include "../ble/ble.h"
#include "process/ota_defines.h"
#include "process_utils.h"

#include <esp_ota_ops.h>

#include <ctype.h>
#include <sodium/utils.h>
#include <time.h>

// Whether during initialisation we select USB, BLE QR etc.
static jade_msg_source_t initialisation_source = SOURCE_NONE;
static jade_msg_source_t internal_relogin_source = SOURCE_NONE;
static bool tolerate_usb_disconnection = false;
static bool show_connect_screen = false;

// The dynamic home screen menu
#define HOME_SCREEN_TYPE_UNINIT 0
#define HOME_SCREEN_TYPE_LOCKED 1
#define HOME_SCREEN_TYPE_ACTIVE 2
#define HOME_SCREEN_TYPE_NUM_STATES 3

static uint8_t home_screen_type = 0;
static uint8_t home_screen_menu_item = 0;

home_menu_entry_t home_screen_selected_entry;
home_menu_entry_t home_screen_next_entry;

typedef struct {
    const char* symbol;
    const char* text;
    const uint32_t btn_id;
} home_menu_item_t;

// Menus for the HOME_SCREEN_TYPE_XXX values above
#ifdef CONFIG_HAS_CAMERA
#define NUM_HOME_SCREEN_MENU_ENTRIES 3
#else
#define NUM_HOME_SCREEN_MENU_ENTRIES 2
#endif

static const home_menu_item_t home_menu_items[HOME_SCREEN_TYPE_NUM_STATES][NUM_HOME_SCREEN_MENU_ENTRIES] = {
    // Uninitialised
    { { .symbol = "1", .text = "Set Up Jade", .btn_id = BTN_INITIALIZE },
#ifdef CONFIG_HAS_CAMERA
        { .symbol = "2", .text = "Scan SeedQR", .btn_id = BTN_SCAN_SEEDQR },
#endif
        { .symbol = "3", .text = "Options", .btn_id = BTN_SETTINGS } },

    // Initialised/Locked
    { { .symbol = "5", .text = "Ready", .btn_id = BTN_CONNECT },
#ifdef CONFIG_HAS_CAMERA
        { .symbol = "2", .text = "QR Mode", .btn_id = BTN_QR_MODE },
#endif
        { .symbol = "3", .text = "Options", .btn_id = BTN_SETTINGS } },

    // Active/Unlocked/Ready
    { { .symbol = "4", .text = "Session", .btn_id = BTN_SESSION },
#ifdef CONFIG_HAS_CAMERA
        { .symbol = "2", .text = "Scan QR", .btn_id = BTN_SCAN_QR },
#endif
        { .symbol = "3", .text = "Options", .btn_id = BTN_SETTINGS } }
};

// The device name and running firmware info, loaded at startup
static const char* device_name;

// The mac-id and running firmware-info, loaded at startup in main.c
extern esp_app_desc_t running_app_info;
extern uint8_t macid[6];

// Flag set when main thread is busy processing a message or awaiting user menu navigation
#define MAIN_THREAD_ACTIVITY_NONE 0
#define MAIN_THREAD_ACTIVITY_MESSAGE 1
#define MAIN_THREAD_ACTIVITY_UI_MENU 2
uint32_t main_thread_action = MAIN_THREAD_ACTIVITY_NONE;

// Functional actions
void register_attestation_process(void* process_ptr);
void sign_attestation_process(void* process_ptr);
void register_otp_process(void* process_ptr);
void get_otp_code_process(void* process_ptr);
void get_xpubs_process(void* process_ptr);
void get_registered_multisigs_process(void* process_ptr);
void get_registered_multisig_process(void* process_ptr);
void register_multisig_process(void* process_ptr);
void get_registered_descriptors_process(void* process_ptr);
void get_registered_descriptor_process(void* process_ptr);
void register_descriptor_process(void* process_ptr);
void get_receive_address_process(void* process_ptr);
void get_identity_pubkey_process(void* process_ptr);
void get_identity_shared_key_process(void* process_ptr);
void sign_identity_process(void* process_ptr);
void sign_message_process(void* process_ptr);
void sign_psbt_process(void* process_ptr);
void sign_tx_process(void* process_ptr);
void get_master_blinding_key_process(void* process_ptr);
void get_blinding_key_process(void* process_ptr);
void get_shared_nonce_process(void* process_ptr);
void get_commitments_process(void* process_ptr);
void get_blinding_factor_process(void* process_ptr);
void sign_liquid_tx_process(void* process_ptr);
void get_bip85_pubkey_process(void* process_ptr);
void sign_bip85_digests_process(void* process_ptr);
void show_bip85_bip39_entropy_process(void* process_ptr);
#ifdef CONFIG_DEBUG_MODE
void get_bip85_bip39_entropy_process(void* process_ptr);
void get_bip85_rsa_entropy_process(void* process_ptr);
void debug_capture_image_data_process(void* process_ptr);
void debug_scan_qr_process(void* process_ptr);
void debug_set_mnemonic_process(void* process_ptr);
void debug_clean_reset_process(void* process_ptr);
void debug_handshake(void* process_ptr);
bool debug_selfcheck(jade_process_t* process);
#endif
void ota_process(void* process_ptr);
void ota_delta_process(void* process_ptr);
void update_pinserver_process(void* process_ptr);
void auth_user_process(void* process_ptr);

// Home screen
gui_activity_t* make_home_screen_activity(const char* device_name, const char* firmware_version,
    home_menu_entry_t* selected_entry, home_menu_entry_t* next_entry, gui_view_node_t** status_light,
    gui_view_node_t** status_text, gui_view_node_t** label);

// Temporary screens while connecting
gui_activity_t* make_connect_activity(void);
gui_activity_t* make_connect_to_activity(const char* device_name, jade_msg_source_t initialisation_source);

// GUI screens
gui_activity_t* make_select_connection_activity_if_required(bool temporary_restore);
gui_activity_t* make_connect_qrmode_activity(const char* device_name);
gui_activity_t* make_confirm_qrmode_activity(void);

gui_activity_t* make_startup_options_activity(void);
gui_activity_t* make_usbstorage_settings_activity(bool wallet_loaded, bool firmware_upgrade_allowed);
gui_activity_t* make_display_settings_activity(void);
gui_activity_t* make_info_activity(const char* fw_version);
gui_activity_t* make_io_test_activity(void);
gui_activity_t* make_io_test_screen_activity(gui_view_node_t** colour_fill);
gui_activity_t* make_io_test_buttons_activity(gui_view_node_t** marks);
gui_activity_t* make_device_info_activity(bool show_ble);

#ifdef CONFIG_HAS_CAMERA
// BBB-AIRGAP: main/process/mnemonic.c - scans a recovery phrase and rejects any other qr.
WARN_UNUSED_RESULT bool mnemonic_qr(char* mnemonic, size_t mnemonic_len);
#endif

#ifdef CONFIG_BOARD_TYPE_JADE_ANY
gui_activity_t* make_legal_certifications_activity(void);
#endif
gui_activity_t* make_storage_stats_activity(size_t entries_used, size_t entries_free);

gui_activity_t* make_wallet_erase_pin_info_activity(void);
gui_activity_t* make_wallet_erase_pin_options_activity(void);

gui_activity_t* make_bip39_passphrase_prefs_activity(
    gui_view_node_t** frequency_textbox, gui_view_node_t** method_textbox);

gui_activity_t* make_otp_activity(void);
gui_activity_t* make_new_otp_activity(void);

gui_activity_t* make_view_export_otp_activity(const char* name, bool is_valid);

bool show_otp_details_activity(
    const otpauth_ctx_t* ctx, bool initial_confirmation, bool is_valid, bool show_delete_btn);
gui_activity_t* make_show_hotp_code_activity(const char* name, const char* codestr, bool confirm_only);
gui_activity_t* make_show_totp_code_activity(const char* name, const char* timestamp, const char* codestr,
    bool confirm_only, progress_bar_t* progress_bar, gui_view_node_t** txt_ts, gui_view_node_t** txt_code);

gui_activity_t* make_pinserver_activity(void);

bool select_registered_wallet(const char multisig_names[][NVS_KEY_NAME_MAX_SIZE], size_t num_multisigs,
    const bool* owned_multisigs, const char descriptor_names[][NVS_KEY_NAME_MAX_SIZE], size_t num_descriptors,
    const bool* owned_descriptors, const char** wallet_name_out, bool* is_multisig);
gui_activity_t* make_view_delete_wallet_activity(const char* wallet_name, bool allow_export);
bool show_multisig_activity(const char* multisig_name, bool is_sorted, size_t threshold, size_t num_signers,
    const signer_t* signer_details, size_t num_signer_details, const char* master_blinding_key_hex,
    const uint8_t* wallet_fingerprint, size_t wallet_fingerprint_len, bool initial_confirmation, bool overwriting,
    bool is_valid);
bool show_descriptor_activity(const char* descriptor_name, const descriptor_data_t* descriptor,
    const char* blinding_key, const signer_t* signer_details, size_t num_signer_details,
    const uint8_t* wallet_fingerprint, size_t wallet_fingerprint_len, network_t network_id, bool initial_confirmation,
    bool overwriting, bool is_valid);

gui_activity_t* make_ble_activity(gui_view_node_t** ble_status_item);

// Wallet initialisation functions
derive_keychain_result_t derive_keychain(bool temporary_restore, const char* mnemonic, bool into_free_slot);
void initialise_with_mnemonic(bool temporary_restore, bool force_qr_scan, bool* offer_qr_temporary);

// Register a new otp code
bool register_otp_qr(void);
bool register_otp_kb_entry(void);

// Updating Pinserver settings
void show_pinserver_details(void);
bool handle_update_pinserver_qr(const uint8_t* cbor, const size_t cbor_len);
bool reset_pinserver(void);

// Bip85
void handle_bip85_mnemonic();
#ifdef CONFIG_HAS_CAMERA
// BBB-AIRGAP: the backup screens of the wallet in use - see main/process/mnemonic.c
void handle_wallet_backup(void);
#endif

// Version info reply
void build_version_info_reply(const void* ctx, CborEncoder* container);

// Set flag to change PIN on next successful unlock
void set_request_change_pin(bool change_pin);

static void process_get_version_info_request(jade_process_t* process)
{
    ASSERT_CURRENT_MESSAGE(process, "get_version_info");

    uint8_t buf[1024];
    jade_process_reply_to_message_result(
        &process->ctx, buf, sizeof(buf), &process->ctx.source, build_version_info_reply);
}

// If the user has successfully authenticated over a given connection interface,
// we stop/close the 'other' interface for security and performance/stability reasons.
// If the user is not authenticated or tied to a given interface, enable all.
static void enable_connection_interfaces(const jade_msg_source_t source)
{
    JADE_LOGD("enable_connection_interfaces(%u)", source);

    if (source == SOURCE_SERIAL || (source == SOURCE_NONE && internal_relogin_source == SOURCE_SERIAL)) {
        // serial_start();
        ble_stop();
    } else if (source == SOURCE_BLE || (source == SOURCE_NONE && internal_relogin_source == SOURCE_BLE)) {
        ble_start();
#ifdef CONFIG_LOG_DEFAULT_LEVEL_NONE // Leave serial up if used for logging
        // serial_stop();
#endif
    } else if (source == SOURCE_INTERNAL || (source == SOURCE_NONE && internal_relogin_source == SOURCE_INTERNAL)) {
        ble_stop();
#ifdef CONFIG_LOG_DEFAULT_LEVEL_NONE // Leave serial up if used for logging
        // serial_stop();
#endif
    } else { // ie. SOURCE_NONE
        JADE_ASSERT(internal_relogin_source == SOURCE_NONE);

        // Ensure serial running, and start BLE if configured but not running
        // serial_start();

#ifdef CONFIG_BT_ENABLED
        if (!ble_enabled()) {
#ifndef CONFIG_DEBUG_UNATTENDED_CI
            const uint8_t ble_flags = storage_get_ble_flags();
#else
            const uint8_t ble_flags = BLE_ENABLED;
#endif
            if (ble_flags & BLE_ENABLED) {
                ble_start();
            }
        }
#endif
    }
}

// Home screen/menu update
static void update_home_screen(gui_view_node_t* status_light, gui_view_node_t* status_text, gui_view_node_t* label)
{
    if (home_screen_type == HOME_SCREEN_TYPE_ACTIVE) {
        gui_set_color(status_light, gui_get_highlight_color());
        gui_update_text(status_light, keychain_has_temporary() ? "N" : "J"); // Clock or Filled circle
        gui_update_text(status_text, "Active");

        // Wallet fingerprint in uppercase hex
        char* fphex = NULL;
        uint8_t fingerprint[BIP32_KEY_FINGERPRINT_LEN];
        wallet_get_fingerprint(fingerprint, sizeof(fingerprint));
        JADE_WALLY_VERIFY(wally_hex_from_bytes(fingerprint, sizeof(fingerprint), &fphex));
        map_string(fphex, toupper);
        gui_update_text(label, fphex);
        JADE_WALLY_VERIFY(wally_free_string(fphex));
    } else if (home_screen_type == HOME_SCREEN_TYPE_LOCKED) {
        gui_set_color(status_light, TFT_LIGHTGREY);
        gui_update_text(status_light, "J"); // Filled circle
        gui_update_text(status_text, "Initialized");
        gui_update_text(label, running_app_info.version);
    } else if (home_screen_type == HOME_SCREEN_TYPE_UNINIT) {
        gui_set_color(status_light, GUI_BLOCKSTREAM_BUTTONBORDER_GREY);
        gui_update_text(status_light, "J"); // Filled circle
        gui_update_text(status_text, "Uninitialized");
        gui_update_text(label, running_app_info.version);
    } else {
        JADE_ASSERT_MSG(false, "Unexpected home screen type: %u", home_screen_type);
    }
}

static const home_menu_item_t* get_selected_home_screen_menu_item(const home_menu_item_t** next_item)
{
    // next_item is optional
    const size_t nbtns = sizeof(home_menu_items[0]) / sizeof(home_menu_items[0][0]);
    JADE_ASSERT(home_screen_type < sizeof(home_menu_items) / sizeof(home_menu_items[0]));
    JADE_ASSERT(home_screen_menu_item < nbtns);

    const home_menu_item_t* const menu_item = &home_menu_items[home_screen_type][home_screen_menu_item];
    if (next_item) {
        const uint8_t next_index = (home_screen_menu_item + 1) % nbtns;
        *next_item = &home_menu_items[home_screen_type][next_index];
    }
    return menu_item;
}

static void update_home_screen_menu_entry(home_menu_entry_t* entry, const home_menu_item_t* item)
{
    JADE_ASSERT(entry);
    JADE_ASSERT(item);

    gui_update_text(entry->symbol, item->symbol);
    gui_update_text(entry->text, item->text);
}

static void update_home_screen_menu(void)
{
    const home_menu_item_t* next_item = NULL;
    const home_menu_item_t* selected_item = get_selected_home_screen_menu_item(&next_item);
    update_home_screen_menu_entry(&home_screen_selected_entry, selected_item);
    update_home_screen_menu_entry(&home_screen_next_entry, next_item);
}

// Function to print a pin into a char buffer.
// Assumes each pin component value is a single digit.
// NOTE: the passed buffer must be large enough.
// (In normal circumstances that should be DIGIT_ENTRY_SIZE digits)
static void format_pin(char* buf, const uint8_t buf_len, const uint8_t* pin, const size_t pin_len)
{
    JADE_ASSERT(pin_len == DIGIT_ENTRY_SIZE);
    JADE_ASSERT(buf_len > pin_len);

    for (int i = 0; i < pin_len; ++i) {
        JADE_ASSERT(pin[i] < 10);
        const int ret = snprintf(buf++, buf_len - i, "%d", pin[i]);
        JADE_ASSERT(ret == 1);
    }
}

// Unpack entropy bytes from message and add to random generator
static void process_add_entropy_request(jade_process_t* process)
{
    const uint8_t* entropy = NULL;
    size_t written = 0;

    ASSERT_CURRENT_MESSAGE(process, "add_entropy");
    GET_MSG_PARAMS(process);

    rpc_get_bytes_ptr("entropy", &params, &entropy, &written);

    if (!written) {
        jade_process_reject_message(
            process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract valid entropy bytes from parameters");
        goto cleanup;
    }

    // Feed received entropy into the random gnerator
    refeed_entropy(entropy, written);
    jade_process_reply_to_message_ok(process);

cleanup:
    return;
}

// Set the current time epoch value
static void process_set_epoch_request(jade_process_t* process)
{
    ASSERT_CURRENT_MESSAGE(process, "set_epoch");
    GET_MSG_PARAMS(process);

    const char* errmsg = NULL;
    const int errcode = params_set_epoch_time(&params, &errmsg);
    if (errcode) {
        jade_process_reject_message(process, errcode, errmsg);
        goto cleanup;
    }

    jade_process_reply_to_message_ok(process);

cleanup:
    return;
}

// Logout of jade hww, clear all key material
static void process_logout_request(jade_process_t* process)
{
    ASSERT_CURRENT_MESSAGE(process, "logout");
    keychain_clear();
    jade_process_reply_to_message_ok(process);
}

// OTA is allowed if either:
// a) There is no PIN set (ie. no encrypted keys set, eg. new device)
// or
// b) User has passed PIN screen and unlocked Jade (ie. not a temporary signer) and:
//   - OTA is over same network interface
//  or
//   - OTA is from the 'INTERNAL' source (eg. is coming via QR codes or connected usb mass storage)
static bool ota_allowed(const jade_msg_source_t ota_source)
{
    return !keychain_has_pin()
        || (keychain_get() && !keychain_has_temporary()
            && (ota_source == (jade_msg_source_t)keychain_get_userdata() || ota_source == SOURCE_INTERNAL));
}

// method_name should be a string literal - or at least non-null and nul terminated
#define IS_METHOD(method_name) (!strncmp(method, method_name, method_len) && strlen(method_name) == method_len)

// Message dispatcher - expects valid cbor messages, routed by 'method'
static void dispatch_message(jade_process_t* process)
{
    ASSERT_HAS_CURRENT_MESSAGE(process);
    JADE_ASSERT(process->ctx.cbor);
    JADE_ASSERT(process->ctx.cbor_len);

    size_t method_len = 0;
    const char* method = NULL;
    rpc_get_method(&process->ctx.value, &method, &method_len);
    JADE_ASSERT(method_len != 0);

    TaskFunction_t task_function = NULL;

    JADE_LOGD("dashboard dispatching message method='%.*s'", method_len, method);

    // Methods available before user is authorised
    if (IS_METHOD("get_version_info")) {
        JADE_LOGD("Received request for version");
        process_get_version_info_request(process);
    } else if (IS_METHOD("add_entropy")) {
        JADE_LOGD("Received external entropy message");
        process_add_entropy_request(process);
    } else if (IS_METHOD("set_epoch")) {
        JADE_LOGD("Received set-epoch message");
        process_set_epoch_request(process);
    } else if (IS_METHOD("logout")) {
        JADE_LOGD("Received logout message");
        process_logout_request(process);
    } else if (IS_METHOD("register_attestation")) {
        JADE_LOGD("Received register_attestation message");
        task_function = register_attestation_process;
    } else if (IS_METHOD("sign_attestation")) {
        JADE_LOGD("Received sign_attestation message");
        task_function = sign_attestation_process;
    } else if (IS_METHOD("update_pinserver")) {
        JADE_LOGD("Received update to pinserver details");
        task_function = update_pinserver_process;
    } else if (IS_METHOD("auth_user")) {
        JADE_LOGD("Received auth-user request");
        task_function = auth_user_process;
    } else if (IS_METHOD("cancel")) {
        // 'cancel' is completely ignored (as nothing is 'in-progress' to cancel)
        JADE_LOGD("Received 'cancel' request - no-op");
    } else if (IS_METHOD("ota")) {
        if (ota_allowed(process->ctx.source)) {
            // If we are about to start an OTA we stop the other/unused external connection
            // interface for performance, stabililty and security reasons.
            enable_connection_interfaces(process->ctx.source);
            task_function = ota_process;
        } else {
            // Reject the message as hw locked
            jade_process_reject_message(process, CBOR_RPC_HW_LOCKED, "OTA is only allowed on new or logged-in device.");
        }
    } else if (IS_METHOD("ota_delta")) {
        if (ota_allowed(process->ctx.source)) {
            // If we are about to start an OTA we stop the other/unused external connection
            // interface for performance, stabililty and security reasons.
            enable_connection_interfaces(process->ctx.source);
            task_function = ota_delta_process;
        } else {
            // Reject the message as hw locked
            jade_process_reject_message(
                process, CBOR_RPC_HW_LOCKED, "OTA delta is only allowed on new or logged-in device.");
        }
#ifdef CONFIG_DEBUG_MODE
    } else if (IS_METHOD("debug_selfcheck")) {
        // Time test run and return to caller
        const TickType_t start_time = xTaskGetTickCount();
        if (debug_selfcheck(process)) {
            const TickType_t end_time = xTaskGetTickCount();
            const uint64_t elapsed_time_ms = (end_time - start_time) * portTICK_PERIOD_MS;

            uint8_t buf[64];
            jade_process_reply_to_message_result(
                &process->ctx, buf, sizeof(buf), &elapsed_time_ms, cbor_result_uint64_cb);
        } else {
            jade_process_reject_message(process, CBOR_RPC_INTERNAL_ERROR, "ERROR");
        }
    } else if (IS_METHOD("debug_clean_reset")) {
        task_function = debug_clean_reset_process;
    } else if (IS_METHOD("debug_set_mnemonic")) {
        task_function = debug_set_mnemonic_process;
    } else if (IS_METHOD("debug_handshake")) {
        task_function = debug_handshake;
    } else if (IS_METHOD("debug_scan_qr")) {
        task_function = debug_scan_qr_process;
    } else if (IS_METHOD("get_bip85_bip39_entropy")) {
        // ATM only exposed for testing purposes
        task_function = get_bip85_bip39_entropy_process;
    } else if (IS_METHOD("get_bip85_rsa_entropy")) {
        // ATM only exposed for testing purposes
        task_function = get_bip85_rsa_entropy_process;
#ifdef CONFIG_RETURN_CAMERA_IMAGES
    } else if (IS_METHOD("debug_capture_image_data")) {
        task_function = debug_capture_image_data_process;
#endif // CONFIG_RETURN_CAMERA_IMAGES
#endif // CONFIG_DEBUG_MODE
    } else {
        // Methods only available after user authorised
        if (!KEYCHAIN_UNLOCKED_BY_MESSAGE_SOURCE(process)) {
            // Reject the message as hw locked
            jade_process_reject_message(
                process, CBOR_RPC_HW_LOCKED, "Cannot process message - hardware locked or uninitialized");
        } else if (IS_METHOD("register_otp")) {
            task_function = register_otp_process;
        } else if (IS_METHOD("get_otp_code")) {
            task_function = get_otp_code_process;
        } else if (IS_METHOD("get_xpub")) {
            task_function = get_xpubs_process;
        } else if (IS_METHOD("get_registered_multisigs")) {
            task_function = get_registered_multisigs_process;
        } else if (IS_METHOD("get_registered_multisig")) {
            task_function = get_registered_multisig_process;
        } else if (IS_METHOD("register_multisig")) {
            task_function = register_multisig_process;
        } else if (IS_METHOD("get_registered_descriptors")) {
            task_function = get_registered_descriptors_process;
        } else if (IS_METHOD("get_registered_descriptor")) {
            task_function = get_registered_descriptor_process;
        } else if (IS_METHOD("register_descriptor")) {
            task_function = register_descriptor_process;
        } else if (IS_METHOD("get_receive_address")) {
            task_function = get_receive_address_process;
        } else if (IS_METHOD("get_identity_pubkey")) {
            task_function = get_identity_pubkey_process;
        } else if (IS_METHOD("get_identity_shared_key")) {
            task_function = get_identity_shared_key_process;
        } else if (IS_METHOD("sign_identity")) {
            task_function = sign_identity_process;
        } else if (IS_METHOD("sign_message")) {
            task_function = sign_message_process;
        } else if (IS_METHOD("sign_psbt")) {
            task_function = sign_psbt_process;
        } else if (IS_METHOD("sign_tx")) {
            task_function = sign_tx_process;
        } else if (IS_METHOD("sign_liquid_tx")) {
            task_function = sign_liquid_tx_process;
        } else if (IS_METHOD("get_commitments")) {
            task_function = get_commitments_process;
        } else if (IS_METHOD("get_blinding_factor")) {
            task_function = get_blinding_factor_process;
        } else if (IS_METHOD("get_master_blinding_key")) {
            task_function = get_master_blinding_key_process;
        } else if (IS_METHOD("get_blinding_key")) {
            task_function = get_blinding_key_process;
        } else if (IS_METHOD("get_shared_nonce")) {
            task_function = get_shared_nonce_process;
        } else if (IS_METHOD("get_bip85_pubkey")) {
            task_function = get_bip85_pubkey_process;
        } else if (IS_METHOD("sign_bip85_digests")) {
            task_function = sign_bip85_digests_process;
        } else if (IS_METHOD("show_bip85_bip39_entropy")) {
            task_function = show_bip85_bip39_entropy_process;
        } else if (IS_METHOD("ota_data") || IS_METHOD("ota_complete") || IS_METHOD("tx_input")
            || IS_METHOD("get_extended_data") || IS_METHOD("get_signature") || IS_METHOD("pin")) {
            // Method we only expect as part of a multi-message protocol
            jade_process_reject_message(process, CBOR_RPC_PROTOCOL_ERROR, "Unexpected method");
        } else {
            // Reject the message as unknown, and free message
            jade_process_reject_message(process, CBOR_RPC_UNKNOWN_METHOD, "Unknown method");
        }
    }

    if (task_function) {
        // Make new process object for the message
        jade_process_t task_process;
        init_jade_process(&task_process);
        jade_process_transfer_current_message(process, &task_process);

        // re-randomize secp256k1 ctx for this task
        jade_wally_randomize_secp_ctx();

        // Call the function
        task_function(&task_process);

        // Then clean up after the process has finished
        cleanup_jade_process(&task_process);

        // When the authentication process exits clear any initialisation-source.
        // Also set the 'connect screen' flag if it looks like the auth failed,
        // and handle any relogin data that may have been cached.
        if (task_function == auth_user_process) {
            show_connect_screen = (keychain_get() && keychain_get_userdata() == SOURCE_NONE);
            initialisation_source = SOURCE_NONE;

            if (internal_relogin_source != SOURCE_NONE) {
                if (keychain_has_temporary() || !keychain_has_pin() || keychain_get_userdata() != SOURCE_INTERNAL) {
                    // If pin-wallet wiped (eg bad pins) or a temporary login or non-internal
                    // login made, wipe the internal-relogin data as it no longer applies.
                    internal_relogin_source = SOURCE_NONE;
                } else if (keychain_get()) {
                    // On successful login, re-instate original login source
                    keychain_set(keychain_get(), internal_relogin_source, false);
                    internal_relogin_source = SOURCE_NONE;
                }
                // else bad-pin (no wallet loaded) but still have tries remaining.
                // Retain re-login data until successful login or ultimate failure
                // when encrypted pin-protected wallet is wiped completely.
            }
        }
    }

    // Reset this flag after processing a single message, so that we will automatically lock the
    // device should an in-use serial connection be physically disconnected/unplugged.
    // (We may have tolerated it briefly to handle an action involving a usb mass storage device.)
    tolerate_usb_disconnection = false;
}

// Function to get user confirmation, then erase all flash memory.
static void offer_jade_reset(void)
{
    // Run 'Reset Jade?'  confirmation screen and wait for yes/no response
    const char* question[] = { "Reset Jade and erase all", "PIN and wallet data?", "This cannot be undone!" };
    if (!await_yesno_activity("Factory Reset", question, 3, false, "blkstrm.com/reset")) {
        // User decided against it
        return;
    }

    // Force user to confirm a random number
    uint8_t num[DIGIT_ENTRY_SIZE];
    for (int i = 0; i < DIGIT_ENTRY_SIZE; ++i) {
        num[i] = get_uniform_random_byte(10);
    }
    char pinstr[sizeof(num) + 1];
    format_pin(pinstr, sizeof(pinstr), num, sizeof(num));

    // BBB-AIRGAP: the confirmation code is shown on screen only; logging it would weaken the gate.
    JADE_LOGI("Awaiting reset confirmation code entry");

    char confirm_msg[64];
    const int ret = snprintf(confirm_msg, sizeof(confirm_msg), "Confirm reset: %s", pinstr);
    JADE_ASSERT(ret > 0 && ret < sizeof(confirm_msg));

    digit_entry_t digit_entry = { .entry_type = DIGIT_ENTRY_PIN, .initial_state = RANDOM, .digits_shown = true };
    make_digit_entry_activity(&digit_entry, "Reset Jade", confirm_msg);
    JADE_ASSERT(digit_entry.activity);
    JADE_STATIC_ASSERT(sizeof(num) == sizeof(digit_entry.digit));

    gui_set_current_activity(digit_entry.activity);
    if (!run_digit_entry_loop(&digit_entry)) {
        // User abandoned pin entry - continue to boot screen
        JADE_LOGI("User confirmation abandoned, not wiping data.");
        return;
    }

    // BBB-AIRGAP: upstream formatted the entered digits into pinstr only to log them; the value
    // equals the on-screen confirmation code when correct, so neither the formatting nor the log
    // remains. The two branches below already record the outcome.
    JADE_LOGI("Reset confirmation code entered");

    if (!sodium_memcmp(num, digit_entry.digit, sizeof(num))) {
        // Correct - erase all jade non-volatile storage
        JADE_LOGI("User confirmed - erasing Jade data");
        if (storage_erase()) {
            // BBB-AIRGAP: the wallets in memory go with it.  This menu is reachable with a wallet
            // loaded (the 'Factory Reset' row of the Options list), and on this port a
            // restart does not clear DRAM by itself - so without this, "erase all Jade data"
            // would leave the wallet, now including the entropy its words can be rebuilt from,
            // sitting in memory.  Storage first, memory second, for the reason the same order is
            // used elsewhere: an interruption between the two must not leave the durable copy.
            keychain_clear();

            // Erase succeeded, better reboot to re-initialise
            esp_restart();
        } else {
            // Erase failed ?    What can we do other than alert the user ?
            JADE_LOGE("Factory reset failed!");
            await_error_2("Unable to completely", "reset Jade.");
        }
    } else {
        // Incorrect - continue to boot screen
        JADE_LOGI("User confirmation number incorrect, not wiping data.");
        await_error_2("Confirmation number", "incorrect!");
    }
}

// Offer to communicate with pinserver via QRs
static bool auth_qr_mode_ex(const bool suppress_pin_change_confirmation)
{
    // Temporary login via QR - just set the message source
    if (keychain_has_temporary()) {
        keychain_set(keychain_get(), SOURCE_INTERNAL, true);
        initialisation_source = SOURCE_INTERNAL;
        show_connect_screen = false;
        return true;
    }

    // Otherwise user to confirm pinserver-via-QRs
    char buf[16];
    const int ret = snprintf(buf, sizeof(buf), "pn to %s", keychain_has_pin() ? "unlock" : "secure");
    JADE_ASSERT(ret > 0 && ret < sizeof(buf));
    const char* message[] = { "Visit", "blkstrm.com/", buf };
    if (!await_qr_back_continue_activity(message, 3, "blkstrm.com/pn", true)) {
        // User decided against it
        return false;
    }

    // Start pinserver/qr handshake process
    initialisation_source = SOURCE_INTERNAL;
    show_connect_screen = true;
    handle_qr_auth(suppress_pin_change_confirmation);
    return true;
}

// Offer to communicate with pinserver via QRs
static bool auth_qr_mode(void)
{
    // Standard/normal authentication using QR codes
    const bool suppress_pin_change_confirmation = false;
    return auth_qr_mode_ex(suppress_pin_change_confirmation);
}

// Unlock jade using qr-codes to effect communication with the pinserver
static bool offer_pinserver_qr_unlock(void)
{
    JADE_ASSERT(keychain_has_pin());
    JADE_ASSERT(!keychain_has_temporary());
    return auth_qr_mode();
}

// Screen to select whether the initial connection is via USB, BLE or QR
static void select_initial_connection(const bool offer_qr_temporary)
{
    // Don't offer temporary (qr-mode) if already a temporary wallet
    JADE_ASSERT(!offer_qr_temporary || !keychain_has_temporary());

    // If there are connection options, the user must choose one
    // Otherwise this call returns null and we default to USB
    gui_activity_t* const act_select = make_select_connection_activity_if_required(keychain_has_temporary());
    gui_activity_t* act = act_select;

    // In advanced-setup, when choosing QRs double check re: temporary-restore/'QR Mode'
    gui_activity_t* const act_confirm_qr_mode
        = (act_select && offer_qr_temporary) ? make_confirm_qrmode_activity() : NULL;

    // If no BLE and no camera/QR-scan (ie. no selection screen created) then assume USB
    initialisation_source = act_select ? SOURCE_NONE : SOURCE_SERIAL;
    show_connect_screen = initialisation_source != SOURCE_NONE;
    bool cancelled = false;

    while (initialisation_source == SOURCE_NONE && !cancelled) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            // BBB-AIRGAP: use the same cleanup as BTN_CONNECT_SELECT_BACK. A newly derived
            // SOURCE_NONE wallet left here violates the dashboard's authenticated-wallet assertion.
            if (keychain_get() && keychain_get_userdata() == SOURCE_NONE) {
                keychain_clear();
            }
            cancelled = true;
            break;
        }

        gui_set_current_activity(act);

        const int32_t ev_id = gui_activity_wait_button(act, BTN_CONNECT_VIA_USB);
        if (ev_id == BTN_CONNECT_VIA_USB) {
            // Set USB/SERIAL source
            initialisation_source = SOURCE_SERIAL;
            show_connect_screen = true;
        } else if (ev_id == BTN_CONNECT_VIA_BLE) {
            // Set BLE source and ensure ble enabled now and by default
            initialisation_source = SOURCE_BLE;
            show_connect_screen = true;
            if (!ble_enabled()) {
                const uint8_t ble_flags = storage_get_ble_flags() | BLE_ENABLED;
                storage_set_ble_flags(ble_flags);
                ble_start();
            }
        } else if (ev_id == BTN_CONNECT_VIA_QR) {
            // Offer pinserver via qr with urls etc
            if (act_confirm_qr_mode) {
                // Double check re: temporary-restore/'QR Mode'
                act = act_confirm_qr_mode;
            } else if (auth_qr_mode()) {
                JADE_ASSERT(initialisation_source == SOURCE_INTERNAL);
                JADE_ASSERT(show_connect_screen == !keychain_has_temporary());
            }
        } else if (ev_id == BTN_CONNECT_QR_PIN) {
            // Offer pinserver via qr with urls etc
            if (auth_qr_mode()) {
                JADE_ASSERT(initialisation_source == SOURCE_INTERNAL);
                JADE_ASSERT(show_connect_screen == !keychain_has_temporary());
            }
        } else if (ev_id == BTN_CONNECT_QR_SCAN) {
            const char* message[] = { "This wallet will be", "temporary and", "forgotten on reboot" };
            if (await_continueback_activity(NULL, message, 3, true, "blkstrm.com/qrmode")) {
                // 'QR-Mode' temporary login only
                keychain_set_temporary();
                if (auth_qr_mode()) {
                    JADE_ASSERT(initialisation_source == SOURCE_INTERNAL);
                    JADE_ASSERT(show_connect_screen == !keychain_has_temporary());
                }
            }
        } else if (ev_id == BTN_CONNECT_QR_BACK) {
            act = act_select;
        } else if (ev_id == BTN_CONNECT_QR_HELP) {
            await_qr_help_activity("blkstrm.com/qrmode");
        } else if (ev_id == BTN_CONNECT_SELECT_BACK) {
            // BBB-AIRGAP: Upstream has no exit here.  initialise_wallet() always holds a new
            // SOURCE_NONE wallet, while the Connect-To caller can hold either a sourced wallet or
            // a newly derived SOURCE_NONE wallet.  Forgetting only the latter avoids the dashboard
            // assertion, using the same keychain_clear() operation as BTN_SESSION_LOGOUT.  Both
            // callers hold exactly one wallet by the time they reach this screen, so clearing the
            // table takes nothing else with it.
            if (keychain_get() && keychain_get_userdata() == SOURCE_NONE) {
                keychain_clear();
            }
            cancelled = true;
        }
    }
}

// Called when the generic QR-scanner sees a valid mnemonic QR
bool handle_mnemonic_qr(const char* mnemonic)
{
    JADE_ASSERT(mnemonic);

    // BBB-AIRGAP: upstream logs the current wallet out and switches to the scanned one, because
    // only one wallet fits in memory.  With the slot table the scanned wallet is loaded next to
    // the ones already there, so nothing is logged out.  A full table is refused inside
    // derive_keychain(), which is the first place that knows whether a slot is needed: the words
    // scanned may be a wallet already held, and switching to that one needs no slot.
    // Load the new wallet alongside the ones already held, and switch to it
    // BBB-AIRGAP: the confirmation used to be asked here, before the words had been worked
    // through, and so could only ask about "this wallet".  derive_keychain() asks it once the
    // fingerprint is known, which also lets it recognise a wallet already held; both of those
    // endings leave nothing loaded and are not errors, hence the three-way result.
    const uint8_t prev_userdata = keychain_get_userdata();
    const bool assume_qr_mode = (prev_userdata == SOURCE_INTERNAL);
    JADE_LOGI("Loading wallet into free slot - qrmode: %u", assume_qr_mode);

    const bool temporary_restore = true;
    const bool into_free_slot = true;
    const derive_keychain_result_t derived = derive_keychain(temporary_restore, mnemonic, into_free_slot);
    if (derived == DERIVE_KEYCHAIN_FAILED) {
        JADE_LOGE("Failed to derive new wallet to load");
        return false;
    }
    if (derived == DERIVE_KEYCHAIN_ABORTED) {
        // Nothing was loaded, and nothing went wrong: the user declined, or the wallet was one
        // already held and has been made the wallet in use.  Either way the carrier handover
        // below would be wrong - it belongs to a wallet that has just arrived.
        // Return true - the qr was handled, this is not a processing error
        return true;
    }

    // If the original wallet was in qrmode, remain in qr-mode.  Otherwise inherit the carrier used
    // by the already-connected session; no new connection selection is needed.
    if (assume_qr_mode) {
        auth_qr_mode();
    } else {
        // BBB-AIRGAP: the scanned wallet is loaded alongside an active wallet, so associate it with
        // that wallet's carrier without entering the blocking connection-selection screen.
        keychain_set(keychain_get(), prev_userdata, true);
    }

    return true;
}

// Helper to initialise with mnemonic, and (if successful) request whether the
// initial connection will be over USB or BLE.
static void initialise_wallet(const bool temporary_restore)
{
    const bool force_qr_scan = false;
    bool offer_qr_temporary = false;
    initialise_with_mnemonic(temporary_restore, force_qr_scan, &offer_qr_temporary);
    if (keychain_get()) {
        select_initial_connection(offer_qr_temporary);
    }
}

static bool offer_temporary_wallet_login(void)
{
    const char* message[] = { "Do you want to", "temporarily log in with", "a recovery phrase?" };
    if (!await_continueback_activity(NULL, message, 3, true, "blkstrm.com/temporary")) {
        // User decided against it
        return false;
    }

    // Initialise 'temporary' wallet
    const bool temporary_restore = true;
    initialise_wallet(temporary_restore);
    return true;
}

#ifdef CONFIG_BOARD_TYPE_JADE_ANY
static void handle_legal(void)
{
    gui_activity_t* const first_activity = make_legal_certifications_activity();
    gui_set_current_activity(first_activity);

    while (sync_await_single_event(GUI_BUTTON_EVENT, BTN_LEGAL_EXIT, NULL, NULL, NULL, 0) != ESP_OK) {
        // Wait until we get this event
    }
}
#endif

#ifdef CONFIG_BT_ENABLED
// Reset BLE pairing data
static void handle_ble_reset(void)
{
    const char* question[] = { "Delete Bluetooth", "pairings for all", "bonded devices?" };
    if (!await_yesno_activity(device_name, question, 3, false, NULL)) {
        return;
    }

    if (ble_remove_all_devices()) {
        await_message_2("Bluetooth pairings", "deleted");
    } else {
        await_error_2("Failed to remove all", "Bluetooth pairings!");
    }
}

static inline void update_ble_status_item(gui_view_node_t* ble_status_item, const bool enabled)
{
    gui_update_text(ble_status_item, enabled ? "Status: Enabled" : "Status: Disabled");
}

static inline void update_ble_carousel_label(gui_view_node_t* status_textbox, const bool enabled)
{
    gui_update_text(status_textbox, enabled ? "Enabled" : "Disabled");
}

// BLE properties screen
static void handle_ble(void)
{
    uint8_t ble_flags = storage_get_ble_flags();
    bool enabled = (ble_flags & BLE_ENABLED);

    gui_view_node_t* ble_status_item = NULL;
    gui_activity_t* const act = make_ble_activity(&ble_status_item);
    update_ble_status_item(ble_status_item, enabled);
    gui_set_current_activity(act);

    gui_view_node_t* status_textbox = NULL;
    gui_activity_t* const act_status = make_carousel_activity("Bluetooth Status", NULL, &status_textbox);
    update_ble_carousel_label(status_textbox, enabled);

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return;
        }

        // Show, and await button click
        gui_set_current_activity(act);

        int32_t ev_id = gui_activity_wait_button(act, BTN_BLE_EXIT);
        if (ev_id == BTN_BLE_STATUS) {
            gui_set_current_activity(act_status);
            while (true) {
                // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
                if (gui_escape_pending()) {
                    return;
                }

                update_ble_carousel_label(status_textbox, enabled);
                if (gui_activity_wait_event(act_status, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
                    if (ev_id == GUI_WHEEL_LEFT_EVENT || ev_id == GUI_WHEEL_RIGHT_EVENT) {
                        enabled = !enabled; // Just toggle label at this point
                    } else if (ev_id == gui_get_click_event()) {
                        // Done - apply ble change
                        break;
                    }
                }
            }

            // Start/stop BLE and persist pref/flags
            if (enabled) {
                if (!ble_enabled()) {
                    // Only start BLE immediately if not using some other interface
                    if (keychain_get_userdata() == SOURCE_NONE) {
                        ble_start();
                    } else {
                        await_message_3("Bluetooth will be", "started on logout", "or disconnection");
                    }
                }
                ble_flags |= BLE_ENABLED;
                storage_set_ble_flags(ble_flags);
            } else {
                if (ble_enabled()) {
                    ble_stop();
                }
                ble_flags &= ~BLE_ENABLED;
                storage_set_ble_flags(ble_flags);
            }
            update_ble_status_item(ble_status_item, enabled);
        } else if (ev_id == BTN_BLE_RESET_PAIRING) {
            handle_ble_reset();
        } else if (ev_id == BTN_BLE_HELP) {
            await_qr_help_activity("blkstrm.com/bluetooth");
        } else if (ev_id == BTN_BLE_EXIT) {
            // Done
            break;
        }
    }
}
#else
static void handle_ble(void) { await_message_2("BLE disabled in", "this firmware"); }

#endif // CONFIG_BT_ENABLED

static void handle_change_pin(void)
{
    // Set flag to change pin on next successful auth/unlock
    const char* message[] = { "Change your PIN", "after unlocking Jade?" };
    const bool change_pin = await_yesno_activity("Change PIN", message, 2, true, NULL);

    // BBB-AIRGAP: 'No' and a KEY3 escape both arrive as false, and 'No' is an answer: it clears
    // a change already requested.  Leaving is not an answer, so the request is left as it was.
    if (gui_escape_pending()) {
        return;
    }
    set_request_change_pin(change_pin);
}

#ifdef CONFIG_HAS_CAMERA
static bool handle_change_pin_qr(void)
{
    // Request/start a qr unlock
    // NOTE: we suppress the pin-change confirmation mid-handling,
    // as we ask up-front before the process is initiated.
    const bool suppress_pin_change_confirmation = true;
    const char* message[] = { "Do you want to", "change your PIN now", "using QR codes?" };
    if (!await_yesno_activity("Change PIN", message, 3, true, NULL)
        || !auth_qr_mode_ex(suppress_pin_change_confirmation)) {
        return false;
    }

    // Cache the existing 'source' userdata so it can be re-instated after a
    // successful pin-change and re-login (otherwise we'd be left in QR mode)
    internal_relogin_source = keychain_get_userdata();

    // Set flag to change pin on next successful auth/unlock and log out of
    // the current session (note the qr unlock has already been initiated).
    // Return to main loop to handle the qr unlock
    set_request_change_pin(true);
    keychain_clear();
    return true;
}
#endif // CONFIG_HAS_CAMERA

// Helper to delete a wallet registration record after user confirms
static bool offer_delete_registered_wallet(const char* name, const bool is_multisig)
{
    // BBB-AIRGAP: this is offered when the viewer screen comes back with 'back', and a KEY3
    // escape returns the same 'back'.  Opening a Delete question there would stop the escape at
    // a screen the user did not ask for, so the pending flag ends the wallet loop instead; the
    // caller's own check then carries the escape home.  A real BTN_DELETE_WALLET press cannot
    // reach here with the flag set, because every non-KEY3 input clears it.
    if (gui_escape_pending()) {
        return true;
    }

    JADE_ASSERT(name);

    if (!await_yesno_activity("Delete Wallet", &name, 1, false, "blkstrm.com/wallets")) {
        return false;
    }

    const bool erased
        = is_multisig ? storage_erase_multisig_registration(name) : storage_erase_descriptor_registration(name);
    if (!erased) {
        await_error_2("Failed to delete", "registered wallet!");
        return false;
    }

    await_message_2("Registered Wallet", "Deleted");
    return true;
}

// BBB-AIRGAP: membership test for the valid-record-name lists gathered below.  Registration names
// are NUL-terminated and bounded by the storage key size, which is what makes both lists the same
// shape.
static bool name_in_list(const char* name, const char names[][NVS_KEY_NAME_MAX_SIZE], const size_t num_names)
{
    JADE_ASSERT(name);
    JADE_ASSERT(names || !num_names);

    for (size_t i = 0; i < num_names; ++i) {
        if (!strcmp(name, names[i])) {
            return true;
        }
    }
    return false;
}

static void handle_registered_wallets(void)
{
    char multisig_names[MAX_MULTISIG_REGISTRATIONS][NVS_KEY_NAME_MAX_SIZE]; // Sufficient
    const size_t num_multisig_names = sizeof(multisig_names) / sizeof(multisig_names[0]);
    size_t num_multisigs = 0;
    bool done = storage_get_all_multisig_registration_names(multisig_names, num_multisig_names, &num_multisigs);
    JADE_ASSERT(done);

    char descriptor_names[MAX_DESCRIPTOR_REGISTRATIONS][NVS_KEY_NAME_MAX_SIZE]; // Sufficient
    const size_t num_descriptor_names = sizeof(descriptor_names) / sizeof(descriptor_names[0]);
    size_t num_descriptors = 0;
    done = storage_get_all_descriptor_registration_names(descriptor_names, num_descriptor_names, &num_descriptors);
    JADE_ASSERT(done);

    const size_t num_registered_wallets = num_multisigs + num_descriptors;
    if (!num_registered_wallets) {
        await_message_2("No additional wallets", "registered");
        return;
    }

    // BBB-AIRGAP: which of these names the wallet in use can actually read.  Upstream leaves that
    // to be discovered by opening a record, which then says "Not valid for current wallet"
    // (main/ui/multisig.c:47-56, main/ui/descriptor.c:50-60); on a device holding several wallets
    // at once that is one press too late to be useful while choosing.  Ownership is the HMAC these
    // two helpers already check (main/wallet.c:1340-1352), so the answer costs one pass over the
    // records the names came from.  The lists are deliberately not narrowed to the owned ones: a
    // record left behind by a wallet that is not loaded can only be deleted through this carousel.
    JADE_STATIC_ASSERT(MAX_MULTISIG_NAME_SIZE == NVS_KEY_NAME_MAX_SIZE);
    JADE_STATIC_ASSERT(MAX_DESCRIPTOR_NAME_SIZE == NVS_KEY_NAME_MAX_SIZE);
    JADE_STATIC_ASSERT(MAX_DESCRIPTOR_REGISTRATIONS <= MAX_MULTISIG_REGISTRATIONS);

    // Scratch for the two helpers, used once each and large enough for either.  This adds 288
    // bytes to the frame (256 of names plus two 16-byte flag arrays) on top of the 512 bytes of
    // names already held above, so this frame holds 800 bytes.  The peak is not set here though:
    // both helpers keep a whole record in their own frame on the same stack - multisig_data_t is
    // about 1.2KB (multisig.c:551) and descriptor_data_t about 3.2KB (descriptor.c:755) - and the
    // deeper of those two calls is what has to fit.
    char owned_names[MAX_MULTISIG_REGISTRATIONS][NVS_KEY_NAME_MAX_SIZE];
    size_t num_owned = 0;

    bool multisig_owned[MAX_MULTISIG_REGISTRATIONS] = { false };
    const size_t* const any_script_type = NULL;
    multisig_get_valid_record_names(any_script_type, owned_names, num_multisig_names, &num_owned);
    for (size_t i = 0; i < num_multisigs; ++i) {
        multisig_owned[i] = name_in_list(multisig_names[i], owned_names, num_owned);
    }

    bool descriptor_owned[MAX_DESCRIPTOR_REGISTRATIONS] = { false };
    num_owned = 0;
    descriptor_get_valid_record_names(owned_names, num_descriptor_names, &num_owned);
    for (size_t i = 0; i < num_descriptors; ++i) {
        descriptor_owned[i] = name_in_list(descriptor_names[i], owned_names, num_owned);
    }

    bool is_multisig = false;
    const char* wallet_name = NULL;
    if (!select_registered_wallet(multisig_names, num_multisigs, multisig_owned, descriptor_names, num_descriptors,
            descriptor_owned, &wallet_name, &is_multisig)
        || !wallet_name) {
        // No wallet selected
        return;
    }

    uint8_t fingerprint[BIP32_KEY_FINGERPRINT_LEN];
    wallet_get_fingerprint(fingerprint, sizeof(fingerprint));
    signer_t* const signer_details = JADE_CALLOC(MAX_ALLOWED_SIGNERS, sizeof(signer_t));
    char* blinding_key = NULL;

    done = false;
    while (!done) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            done = true;
            continue;
        }

        // View/export/delete wallet
        int32_t ev_id;
        gui_activity_t* const act_wallet = make_view_delete_wallet_activity(wallet_name, is_multisig);
        gui_set_current_activity_ex(act_wallet, true);
        if (gui_activity_wait_event(act_wallet, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
            if (ev_id == BTN_BACK) {
                done = true;
                continue;
            }

            if (ev_id == BTN_DELETE_WALLET) {
                done = offer_delete_registered_wallet(wallet_name, is_multisig);
                continue;
            }

            if (ev_id != BTN_EXPORT_WALLET && ev_id != BTN_VIEW_WALLET) {
                // Unexpected event - ignore
                continue;
            }

            // Display or export
            if (is_multisig) {
                // Load selected multisig record from storage given the name
                // Note we pass signer_t structs here to retrieve full signer details
                const char* errmsg = NULL;
                multisig_data_t multisig_data;
                size_t num_signer_details = 0;
                const bool is_valid = multisig_load_from_storage(
                    wallet_name, &multisig_data, signer_details, MAX_ALLOWED_SIGNERS, &num_signer_details, &errmsg);
                JADE_ASSERT(num_signer_details <= MAX_ALLOWED_SIGNERS);

                // We will display the names of invalid entries, just log any message
                if (errmsg) {
                    JADE_LOGW("%s", errmsg);
                }

                if (ev_id == BTN_EXPORT_WALLET) {
                    // Export as QR
                    if (!is_valid || num_signer_details != multisig_data.num_xpubs) {
                        JADE_LOGW("Unable to export multisig details - invalid or incomplete");
                        await_error_2("Unable to export", "wallet details");
                        continue;
                    }

                    // Warning for unsorted multisig, as this is not strictly handled by the origial
                    // common file format and may not be supported by the imprting wallet.
                    if (!multisig_data.sorted) {
                        await_message_4(
                            "Exporting unsorted", "multisig - ensure the", "wallet app supports", "this configuration");
                    }
                    display_processing_message_activity();

                    // Create output file
                    const size_t output_len = MULTISIG_FILE_MAX_LEN(num_signer_details);
                    char* const output = JADE_MALLOC(output_len);
                    size_t written = 0;
                    if (!multisig_create_export_file(wallet_name, &multisig_data, signer_details, num_signer_details,
                            output, output_len, &written)) {
                        JADE_LOGE("Failed to export multisig details");
                        await_error_2("Unable to export", "wallet details");
                        free(output);
                        continue;
                    }

                    // Get as cbor bytes
                    const char* message[] = { "Export", "Multisig", "wallet" };
                    if (!display_bcur_bytes_qr(message, 3, (const uint8_t*)output, written, "blkstrm.com/wallets")) {
                        JADE_LOGE("Failed to create multisig export details QR code");
                        await_error_2("Unable to export", "wallet details");
                        free(output);
                        continue;
                    }

                    // Done
                    free(output);
                } else {
                    // Display details on screen
                    JADE_ASSERT(ev_id == BTN_VIEW_WALLET);

                    char* master_blinding_key_hex = NULL;
                    if (is_valid && multisig_data.master_blinding_key_len) {
                        JADE_WALLY_VERIFY(wally_hex_from_bytes(multisig_data.master_blinding_key,
                            multisig_data.master_blinding_key_len, &master_blinding_key_hex));
                    }

                    // We are not confirming or writing-to-storage
                    const bool initial_confirmation = false;
                    const bool overwriting = false;
                    if (!show_multisig_activity(wallet_name, multisig_data.sorted, multisig_data.threshold,
                            multisig_data.num_xpubs, signer_details, num_signer_details, master_blinding_key_hex,
                            fingerprint, sizeof(fingerprint), initial_confirmation, overwriting, is_valid)) {
                        // Delete record ?
                        done = offer_delete_registered_wallet(wallet_name, is_multisig);
                    }

                    if (master_blinding_key_hex) {
                        JADE_WALLY_VERIFY(wally_free_string(master_blinding_key_hex));
                    }
                }
            } else {
                // Load selected descriptor record from storage given the name
                const char* errmsg = NULL;
                descriptor_data_t descriptor;
                const bool is_valid = descriptor_load_from_storage(wallet_name, &descriptor, &errmsg);

                // We will display the names of invalid entries, just log any message
                if (errmsg) {
                    JADE_LOGW("%s", errmsg);
                }

                // No option to export (atm)
                JADE_ASSERT(ev_id == BTN_VIEW_WALLET);

                // Free any blinding_key from a previous pass through the loop
                if (blinding_key) {
                    JADE_WALLY_VERIFY(wally_free_string(blinding_key));
                    blinding_key = NULL;
                }

                // Get signer info from descriptor
                size_t num_signer_details = 0;
                if (!descriptor_get_signers(wallet_name, &descriptor, NETWORK_NONE, NULL, signer_details,
                        MAX_ALLOWED_SIGNERS, &num_signer_details, &blinding_key, &errmsg)) {
                    JADE_LOGE("Failed to load signer information from descriptor data");
                    await_error_2("Unable to load", "signer details");
                    continue;
                }

                // We are not confirming or writing-to-storage
                const bool initial_confirmation = false;
                const bool overwriting = false;
                if (!show_descriptor_activity(wallet_name, &descriptor, blinding_key, signer_details,
                        num_signer_details, fingerprint, sizeof(fingerprint), NETWORK_NONE, initial_confirmation,
                        overwriting, is_valid)) {
                    // Delete record ?
                    done = offer_delete_registered_wallet(wallet_name, is_multisig);
                }
            }
        }
    }

    // Free any signer / blinding key details
    free(signer_details);
    if (blinding_key) {
        JADE_WALLY_VERIFY(wally_free_string(blinding_key));
    }
}

static void set_wallet_erase_pin(void)
{
    JADE_LOGI("Requesting wallet-erase PIN");

    // Ask user to enter a wallet-erase pin
    digit_entry_t digit_entry = { .entry_type = DIGIT_ENTRY_PIN, .initial_state = RANDOM, .digits_shown = false };
    make_digit_entry_activity(&digit_entry, "Wallet-Erase PIN", "Different from main PIN");
    JADE_ASSERT(digit_entry.activity);

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            break;
        }
        reset_digit_entry(&digit_entry, "Wallet-Erase PIN");
        gui_set_current_activity(digit_entry.activity);

        if (!run_digit_entry_loop(&digit_entry)) {
            // User abandoned pin entry
            JADE_LOGI("User abandoned setting wallet erase PIN");
            break;
        }

        // This is the first pin, copy it and clear screen fields
        uint8_t pin[sizeof(digit_entry.digit)];
        memcpy(pin, digit_entry.digit, sizeof(pin));
        reset_digit_entry(&digit_entry, "Confirm Erase PIN");

        // Ask user to re-enter PIN
        if (!run_digit_entry_loop(&digit_entry)) {
            // User abandoned second input - back to first ...
            continue;
        }

        // Check that the two pins are the same
        JADE_LOGD("Checking pins match");
        if (!sodium_memcmp(pin, digit_entry.digit, sizeof(pin))) {
            JADE_LOGI("Setting Wallet-Erase PIN");
            storage_set_wallet_erase_pin(digit_entry.digit, sizeof(digit_entry.digit));
            break;
        } else {
            // Pins mismatch - try again
            const char* message[] = { "Pin mismatch,", "please try again." };
            if (!await_continueback_activity(NULL, message, 2, true, NULL)) {
                // Abandon
                break;
            }
        }
    }
}

static void handle_wallet_erase_pin(void)
{
    gui_activity_t* act_info = make_wallet_erase_pin_info_activity();

    gui_activity_t* act_options = make_wallet_erase_pin_options_activity();

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            break;
        }

        // BBB-AIRGAP: this screen used to print the stored PIN, which is why the PIN had to be
        // readable back off the card.  It is a salted verifier now (main/storage.c), so the screen
        // says whether one is set and nothing more; Change and Disable work exactly as before.
        gui_activity_t* act = storage_wallet_erase_pin_exists() ? act_options : act_info;
        gui_set_current_activity(act);

        int32_t ev_id;
        if (gui_activity_wait_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
            if (ev_id == BTN_WALLET_ERASE_PIN_SET) {
                // User opted to set a new wallet-erasing PIN
                set_wallet_erase_pin();
            } else if (ev_id == BTN_WALLET_ERASE_PIN_DISABLE) {
                // User opted to disable/erase wallet-erasing PIN
                JADE_LOGI("Erasing Wallet-Erase PIN");
                storage_erase_wallet_erase_pin();

                await_message_2("Wallet-Erase PIN", "deleted");
            } else if (ev_id == BTN_WALLET_ERASE_PIN_HELP) {
                await_qr_help_activity("blkstrm.com/duress");
            } else if (ev_id == BTN_WALLET_ERASE_PIN_EXIT) {
                // Done
                break;
            }
        }
    }
}

// Handle passphrase preferences
static inline const char* passphrase_frequency_desc_from_flags(const passphrase_freq_t freq, const bool shortname)
{
    return freq == PASSPHRASE_ALWAYS ? "Always Ask"
        : freq == PASSPHRASE_ONCE    ? (shortname ? "Next Login" : "Next Login Only")
                                     : "Disabled";
}
static inline const char* passphrase_method_desc_from_flags(const passphrase_type_t type)
{
    return type == PASSPHRASE_WORDLIST ? "WordList" : "Manual";
}

static void handle_passphrase_prefs()
{
    passphrase_freq_t freq = keychain_get_passphrase_freq();
    passphrase_type_t type = keychain_get_passphrase_type();

    // In some cases may need to use shorter names on smaller displays
    const bool menu_freq_shortname = CONFIG_DISPLAY_WIDTH < 320;
    const bool carousel_freq_shortname = false;

    gui_view_node_t* frequency_item = NULL;
    gui_view_node_t* method_item = NULL;
    gui_activity_t* const act = make_bip39_passphrase_prefs_activity(&frequency_item, &method_item);
    update_menu_item(frequency_item, "Frequency", passphrase_frequency_desc_from_flags(freq, menu_freq_shortname));
    update_menu_item(method_item, "Method", passphrase_method_desc_from_flags(type));
    gui_set_current_activity(act);

    gui_view_node_t* frequency_textbox = NULL;
    gui_activity_t* const act_freq = make_carousel_activity("Frequency", NULL, &frequency_textbox);
    gui_update_text(frequency_textbox, passphrase_frequency_desc_from_flags(freq, carousel_freq_shortname));

    gui_view_node_t* method_textbox = NULL;
    gui_activity_t* const act_method = make_carousel_activity("Method", NULL, &method_textbox);
    gui_update_text(method_textbox, passphrase_method_desc_from_flags(type));

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return;
        }

        // Show, and await button click
        gui_set_current_activity(act);

        int32_t ev_id = gui_activity_wait_button(act, BTN_PASSPHRASE_EXIT);
        if (ev_id == BTN_PASSPHRASE_FREQUENCY) {
            // Never -> Once -> Always -> Once ...
            gui_set_current_activity(act_freq);
            while (true) {
                // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
                if (gui_escape_pending()) {
                    return;
                }

                gui_update_text(frequency_textbox, passphrase_frequency_desc_from_flags(freq, carousel_freq_shortname));
                if (gui_activity_wait_event(act_freq, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
                    if (ev_id == GUI_WHEEL_LEFT_EVENT) {
                        freq = (freq == PASSPHRASE_NEVER  ? PASSPHRASE_ALWAYS
                                : freq == PASSPHRASE_ONCE ? PASSPHRASE_NEVER
                                                          : PASSPHRASE_ONCE);
                    } else if (ev_id == GUI_WHEEL_RIGHT_EVENT) {
                        freq = (freq == PASSPHRASE_NEVER  ? PASSPHRASE_ONCE
                                : freq == PASSPHRASE_ONCE ? PASSPHRASE_ALWAYS
                                                          : PASSPHRASE_NEVER);
                    } else if (ev_id == gui_get_click_event()) {
                        // Done
                        break;
                    }
                }
            }
            update_menu_item(
                frequency_item, "Frequency", passphrase_frequency_desc_from_flags(freq, menu_freq_shortname));
        } else if (ev_id == BTN_PASSPHRASE_METHOD) {
            gui_set_current_activity(act_method);
            while (true) {
                // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
                if (gui_escape_pending()) {
                    return;
                }

                gui_update_text(method_textbox, passphrase_method_desc_from_flags(type));
                if (gui_activity_wait_event(act_method, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
                    if (ev_id == GUI_WHEEL_LEFT_EVENT || ev_id == GUI_WHEEL_RIGHT_EVENT) {
                        type = (type == PASSPHRASE_FREETEXT ? PASSPHRASE_WORDLIST : PASSPHRASE_FREETEXT);
                    } else if (ev_id == gui_get_click_event()) {
                        // Done
                        break;
                    }
                }
            }
            update_menu_item(method_item, "Method", passphrase_method_desc_from_flags(type));
        } else if (ev_id == BTN_PASSPHRASE_HELP) {
            await_qr_help_activity("blkstrm.com/passphrase");
        } else if (ev_id == BTN_PASSPHRASE_EXIT) {
            // Done
            break;
        }
    }

    // If user updated the passphrase settings, save the new settings
    if (freq != keychain_get_passphrase_freq() || type != keychain_get_passphrase_type()) {
        keychain_set_passphrase_frequency(freq);
        keychain_set_passphrase_type(type);
        keychain_persist_key_flags();
    }
}

// Helper to delete an otp record after user confirms
static bool delete_otp_record(const char* otpname)
{
    JADE_ASSERT(otpname);

    if (!await_yesno_activity("Delete OTP Record", &otpname, 1, false, "blkstrm.com/otp")) {
        return false;
    }

    if (!storage_erase_otp(otpname)) {
        await_error_2("Failed to delete", "OTP record!");
        return false;
    }

    await_message("OTP Record Deleted");
    return true;
}

static bool show_otp_detail_options_activity(
    const otpauth_ctx_t* otp_ctx, const bool initial_confirmation, const bool is_valid, const bool show_delete_btn)
{
    JADE_ASSERT(otp_ctx);
    JADE_ASSERT(otp_ctx->name);

    gui_activity_t* const act = make_view_export_otp_activity(otp_ctx->name, is_valid);
    int32_t ev_id;

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return true;
        }

        gui_set_current_activity(act);

        if (gui_activity_wait_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
            if (ev_id == BTN_BACK) {
                return true;
            } else if (ev_id == BTN_OTP_DETAILS_VIEW) {
                // BBB-AIRGAP: the details screen returns false when the user asks to delete the
                // record, and that answer was being dropped here, which made the 'X' on that
                // screen a button that does nothing: the discard branch in handle_view_otps
                // could never run.  The intermediate screen arrived upstream in 8af72c79 and the
                // return value was lost in the move.  Callers that pass show_delete_btn = false
                // are unaffected - without the button the screen cannot produce that answer.
                if (!show_otp_details_activity(otp_ctx, initial_confirmation, is_valid, show_delete_btn)) {
                    return false;
                }
            } else if (ev_id == BTN_OTP_DETAILS_EXPORT) {
                show_otp_uri_qr_activity(otp_ctx);
            }
        }
    }
    return true;
}
// HOTP token-code fixed
static bool display_hotp_screen(const otpauth_ctx_t* otp_ctx, const char* token, const bool confirm_only)
{
    JADE_ASSERT(otp_is_valid(otp_ctx));
    JADE_ASSERT(token);

    gui_activity_t* const act = make_show_hotp_code_activity(otp_ctx->name, token, confirm_only);

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return false;
        }

        gui_set_current_activity(act);

        const int32_t ev_id = gui_activity_wait_button(act, BTN_OTP_RETAIN_CONFIRM);
        if (ev_id == BTN_OTP_DETAILS) {
            const bool is_valid = true; // asserted above
            const bool initial_confirmation = false;
            const bool show_delete_btn = false;
            const bool retain
                = show_otp_detail_options_activity(otp_ctx, initial_confirmation, is_valid, show_delete_btn);
            JADE_ASSERT(retain); // should be no 'discard' option
        } else if (ev_id == BTN_OTP_DISCARD_DELETE) {
            if (confirm_only || delete_otp_record(otp_ctx->name))
                return false;
        } else if (ev_id == BTN_OTP_RETAIN_CONFIRM) {
            return true;
        }
    }
}

// TOTP token-code display updates with passage of time (unless flagged not to)
static bool display_totp_screen(otpauth_ctx_t* otp_ctx, uint64_t epoch_value, char* token, const size_t token_len,
    const bool confirm_only, const bool auto_update)
{
    JADE_ASSERT(otp_is_valid(otp_ctx));
    JADE_ASSERT(otp_ctx->otp_type == OTPTYPE_TOTP);
    JADE_ASSERT(token);

    char timestr[32];
    ctime_r((time_t*)&epoch_value, timestr);

    gui_view_node_t* txt_ts = NULL;
    gui_view_node_t* txt_code = NULL;
    progress_bar_t time_left = {};
    gui_activity_t* const act
        = make_show_totp_code_activity(otp_ctx->name, timestr, token, confirm_only, &time_left, &txt_ts, &txt_code);
    JADE_ASSERT(txt_ts);
    JADE_ASSERT(txt_code);
    gui_set_current_activity(act);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Make an event-data structure to track events - attached to the activity
    wait_event_data_t* const event_data = gui_activity_make_wait_event_data(act);
    JADE_ASSERT(event_data);

    // Register for button events
    gui_activity_register_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);
    int32_t ev_id;

    // Token code updates with time (unless explicitly specified otherwise - eg. test fixed value)
    uint8_t count = epoch_value % otp_ctx->period;
    uint8_t last_count = count;
    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return false;
        }

        gui_set_current_activity(act);

        // Update values
        if (auto_update) {
            switch (otp_set_default_value(otp_ctx, &epoch_value)) {
            case OTP_ERR_TOTP_TIME: {
                await_error_3("Clock not set.", "Scan a time QR", "to set it.");
                return false;
            }
            case OTP_ERR_HOTP_COUNTER: {
                await_error_2("Failed to fetch", "counter!");
                return false;
            }
            case OTP_ERR_OK:
                break;
            }
            ctime_r((time_t*)&epoch_value, timestr);
            gui_update_text(txt_ts, timestr);

            count = epoch_value % otp_ctx->period;
            if (count < last_count) {
                // Wrapped - token code should have changed
                if (!otp_get_auth_code(otp_ctx, token, token_len)) {
                    await_error_2("Failed to calculate", "OTP!");
                    return false;
                }
                gui_update_text(txt_code, token);
            }
            last_count = count;
        }

        // NOTE: this is outside the 'auto-update' check as the
        // progress-bar needs to be updated at least once.
        update_progress_bar(&time_left, otp_ctx->period, count);

        // In a debug unattended ci build, assume 'accept' button pressed after a short delay
#ifndef CONFIG_DEBUG_UNATTENDED_CI
        // update every 1s
        const bool ret = sync_wait_event(event_data, NULL, &ev_id, NULL, 1000 / portTICK_PERIOD_MS) == ESP_OK;
#else
        sync_wait_event(event_data, NULL, &ev_id, NULL, CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS / portTICK_PERIOD_MS);
        const bool ret = true;
        ev_id = BTN_OTP_RETAIN_CONFIRM;
#endif

        if (ret) {
            if (ev_id == BTN_OTP_DETAILS) {
                const bool is_valid = true; // asserted above
                const bool initial_confirmation = false;
                const bool show_delete_btn = false;
                const bool retain
                    = show_otp_detail_options_activity(otp_ctx, initial_confirmation, is_valid, show_delete_btn);
                JADE_ASSERT(retain); // should be no 'discard' option
            } else if (ev_id == BTN_OTP_DISCARD_DELETE) {
                if (confirm_only || delete_otp_record(otp_ctx->name))
                    return false;
            } else if (ev_id == BTN_OTP_RETAIN_CONFIRM) {
                return true;
            }
        }
    }
}

bool display_otp_screen(otpauth_ctx_t* otp_ctx, const uint64_t value, char* token, const size_t token_len,
    const bool confirm_only, const bool auto_update)
{
    JADE_ASSERT(otp_is_valid(otp_ctx));
    JADE_ASSERT(token);

    if (otp_ctx->otp_type == OTPTYPE_TOTP) {
        // Token code updates with time (unless explicitly specified otherwise - eg. test fixed value)
        return display_totp_screen(otp_ctx, value, token, token_len, confirm_only, auto_update);
    } else {
        // NOTE: the 'auto_update' flag is ignored as the hotp counter does not change without
        // the caller making an entirely new 'get token code' request - so the value is fixed.
        // Also, ignore counter value as not displayed.
        return display_hotp_screen(otp_ctx, token, confirm_only);
    }
}

static bool show_otp_code(otpauth_ctx_t* otp_ctx)
{
    JADE_ASSERT(otp_is_valid(otp_ctx));

    // Update context with current default 'moving' element
    uint64_t value = 0;
    switch (otp_set_default_value(otp_ctx, &value)) {
    case OTP_ERR_TOTP_TIME: {
        // BBB-AIRGAP: upstream sends the user to the Blockstream companion app over USB or
        // Bluetooth.  Neither exists here - the radio is physically cut and the port is QR only -
        // so the message named a route this device does not have.  The route it does have is the
        // epoch message over a scanned QR (main/qrmode.c:2341, ur:jade-epoch; the host side is
        // pijade/tools/epoch_qr.py).
        await_error_3("Clock not set.", "Scan a time QR", "to set it.");
        return false;
    }
    case OTP_ERR_HOTP_COUNTER: {
        await_error_2("Failed to fetch", "counter!");
        return false;
    }
    case OTP_ERR_OK:
        break;
    }

    // Calculate token
    char token[OTP_MAX_TOKEN_LEN];
    if (!otp_get_auth_code(otp_ctx, token, sizeof(token))) {
        await_error_2("Failed to calculate", "OTP!");
        return false;
    }

    // totp token/code updates with time
    const bool auto_update = true;
    const bool confirm_only = false;
    display_otp_screen(otp_ctx, value, token, sizeof(token), confirm_only, auto_update);
    return true;
}

static void handle_view_otps(void)
{
    char names[OTP_MAX_RECORDS][NVS_KEY_NAME_MAX_SIZE]; // Sufficient
    const size_t num_names = sizeof(names) / sizeof(names[0]);
    size_t num_otp_records = 0;
    bool done = storage_get_all_otp_names(names, num_names, &num_otp_records);
    JADE_ASSERT(done);

    if (num_otp_records == 0) {
        await_message_2("No OTP records", "registered");
        return;
    }

    size_t selected = 0;
    gui_view_node_t* otpname = NULL;

    // BBB-AIRGAP: the title names the wallet that is loaded, because that is what decides which of
    // these records can be opened at all.  The record name is a storage key in one device-wide
    // namespace (main/storage.c) while the uri is encrypted under a key derived from the loaded
    // wallet's seed (get_otp_encryption_key, main/otpauth.c), so the same list looks identical
    // under every wallet while most of it may be unreadable.  Showing the fingerprint is what
    // turns "cannot be read" from a fault into an answer.
    //
    // The row that reaches here is offered only with a wallet loaded that carries a seed
    // (run_options_list), and wallet_get_fingerprint() asserts the same thing itself.
    uint8_t fingerprint[BIP32_KEY_FINGERPRINT_LEN];
    wallet_get_fingerprint(fingerprint, sizeof(fingerprint));
    char* fphex = NULL;
    JADE_WALLY_VERIFY(wally_hex_from_bytes(fingerprint, sizeof(fingerprint), &fphex));
    map_string(fphex, toupper);
    char otp_title[16];
    const int title_ret = snprintf(otp_title, sizeof(otp_title), "OTP %s", fphex);
    JADE_ASSERT(title_ret > 0 && title_ret < sizeof(otp_title));
    JADE_WALLY_VERIFY(wally_free_string(fphex));

    gui_activity_t* const act = make_carousel_activity(otp_title, NULL, &otpname);
    gui_update_text(otpname, names[selected]);
    gui_set_current_activity(act);
    int32_t ev_id;

    const size_t limit = num_otp_records + 1;
    done = false;
    while (!done) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            // BBB-AIRGAP: ending only the selection loop still decrypts the highlighted OTP
            // below, and an unset clock opens an error screen. Nothing was selected on escape.
            return;
        }

        JADE_ASSERT(selected <= limit);
        gui_update_text(otpname, selected < num_otp_records ? names[selected] : "[Cancel]");

        if (gui_activity_wait_event(act, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
            switch (ev_id) {
            case GUI_WHEEL_LEFT_EVENT:
                selected = (selected + limit - 1) % limit;
                break;

            case GUI_WHEEL_RIGHT_EVENT:
                selected = (selected + 1) % limit;
                break;

            default:
                if (ev_id == gui_get_click_event()) {
                    done = true;
                    break;
                }
            }
        }
    }
    if (selected >= num_otp_records) {
        // Back/exit
        return;
    }

    // Load selected OTP record from storage given the name
    JADE_ASSERT(selected < num_otp_records);
    char otp_uri[OTP_MAX_URI_LEN];
    SENSITIVE_PUSH(otp_uri, sizeof(otp_uri));
    otpauth_ctx_t otp_ctx = { .name = names[selected] };

    size_t written = 0;
    const bool is_valid = otp_load_uri(names[selected], otp_uri, sizeof(otp_uri), &written) && written
        && otp_uri_to_ctx(otp_uri, written, &otp_ctx) && otp_is_valid(&otp_ctx);

    // BBB-AIRGAP: an honest store.  These are two different failures and they were being
    // handled as one - both landed here and both were offered deletion.
    //
    // A record that will not decrypt was most likely written by another wallet: the name is the
    // storage key in one device-wide namespace (main/storage.c) while the uri is encrypted under
    // a key derived from the seed of whichever wallet is loaded (get_otp_encryption_key,
    // main/otpauth.c).  Loading a different wallet therefore makes every record unreadable
    // without anything being wrong with it.  Corruption looks exactly the same from here, because
    // the ciphertext carries no authentication tag, so the device cannot tell the two apart.
    // Deleting on that guess destroys another wallet's secret, so deletion is not offered.  The
    // accepted cost: a genuinely corrupt record now goes only with a Factory Reset.
    //
    // A record that reads fine but whose code cannot be produced (the clock is unset - see
    // show_otp_code) says nothing about the record at all.  The error has been shown already; the
    // record is not brought up for deletion over a device-state problem.
    if (!is_valid) {
        JADE_LOGW("OTP record cannot be read with the loaded wallet: %s", names[selected]);
        const bool initial_confirmation = false;
        const bool show_delete_btn = false;
        // The return value says whether the user asked to delete the record, and without the
        // button there is no way for the screen to say yes, so there is nothing to act on here.
        show_otp_detail_options_activity(&otp_ctx, initial_confirmation, is_valid, show_delete_btn);
    } else if (!show_otp_code(&otp_ctx)) {
        JADE_LOGW("Could not display code for otp record: %s", names[selected]);
    }
    SENSITIVE_POP(otp_uri);
}

// NOTE: Only boards listed here have brightness controls
// BBB-AIRGAP: HAVE_DISPLAY_BRIGHTNESS_SETTING added - piJade drives the backlight from the host
// rather than a PMU, see main/gui.h for why the board type itself is not defined.
#if defined(CONFIG_BOARD_TYPE_JADE_V1_1) || defined(CONFIG_BOARD_TYPE_JADE_V2_ANY)                                     \
    || defined(CONFIG_BOARD_TYPE_WS_TOUCH_LCD2) || defined(CONFIG_BOARD_TYPE_TTGO_TDISPLAY)                            \
    || defined(CONFIG_BOARD_TYPE_M5_STICKC_PLUS_2) || defined(HAVE_DISPLAY_BRIGHTNESS_SETTING)
static void handle_screen_brightness(void)
{
    static const char* LABELS[] = { "Min(1)", "Low(2)", "Medium(3)", "High(4)", "Max(5)" };

    const uint8_t initial_brightness = storage_get_brightness();
    uint8_t new_brightness = initial_brightness;
    if (new_brightness < BACKLIGHT_MIN) {
        new_brightness = BACKLIGHT_MIN;
    }
    if (new_brightness > BACKLIGHT_MAX) {
        new_brightness = BACKLIGHT_MAX;
    }

    gui_view_node_t* item_text = NULL;
    gui_activity_t* const act = make_carousel_activity("Brightness", NULL, &item_text);
    JADE_ASSERT(item_text);
    gui_update_text(item_text, LABELS[new_brightness - 1]);
    gui_set_current_activity(act);

    int32_t ev_id;
    bool done = false;
    while (!done) {
        // BBB-AIRGAP: KEY3 leaves this screen.  The wheel has already dimmed or brightened the
        // panel as a preview, so leaving without saving puts the stored level back; storage was
        // not written, and the screens drawn on the way home would otherwise keep the preview.
        if (gui_escape_pending()) {
            power_backlight_on(storage_get_brightness());
            return;
        }

        // wait for a GUI event
        gui_activity_wait_event(act, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);

#if defined(CONFIG_BOARD_TYPE_TTGO_TDISPLAY)
        // Match TTGO value-selection direction to the rest of its numeric entry UI.
        if (ev_id == GUI_WHEEL_LEFT_EVENT) {
            ev_id = GUI_WHEEL_RIGHT_EVENT;
        } else if (ev_id == GUI_WHEEL_RIGHT_EVENT) {
            ev_id = GUI_WHEEL_LEFT_EVENT;
        }
#endif

        switch (ev_id) {
        case GUI_WHEEL_LEFT_EVENT:
            if (new_brightness > BACKLIGHT_MIN) {
                power_backlight_on(--new_brightness);
                gui_update_text(item_text, LABELS[new_brightness - 1]);
            }
            break;

        case GUI_WHEEL_RIGHT_EVENT:
            if (new_brightness < BACKLIGHT_MAX) {
                power_backlight_on(++new_brightness);
                gui_update_text(item_text, LABELS[new_brightness - 1]);
            }
            break;

        default:
            done = (ev_id == gui_get_click_event());
        }
    }

    // Persist updated preferences
    if (new_brightness != initial_brightness) {
        storage_set_brightness(new_brightness);
    }
}
#endif

static void update_timeout_text(gui_view_node_t* timeout_text, const uint16_t timeout)
{
    JADE_ASSERT(timeout_text);
    char txt[16];

    // Prefer to display in minutes
    if (timeout == UINT16_MAX) {
        const int ret = snprintf(txt, sizeof(txt), "Disabled");
        JADE_ASSERT(ret > 0 && ret < sizeof(txt));
    } else if (timeout == 60) {
        const int ret = snprintf(txt, sizeof(txt), "1 minute");
        JADE_ASSERT(ret > 0 && ret < sizeof(txt));
    } else if (timeout % 60 == 0) {
        const int ret = snprintf(txt, sizeof(txt), "%u minutes", timeout / 60);
        JADE_ASSERT(ret > 0 && ret < sizeof(txt));
    } else {
        const int ret = snprintf(txt, sizeof(txt), "%u seconds", timeout);
        JADE_ASSERT(ret > 0 && ret < sizeof(txt));
    }
    gui_update_text(timeout_text, txt);
}

static void handle_idle_timeout(void)
{
    static const uint16_t VALUES[] = {
        60, 120, 180, 300, 600, 900, 1200, 1800, 3600, UINT16_MAX // UINT16_MAX == OFF
    };
    static const uint16_t num_values = sizeof(VALUES) / sizeof(VALUES[0]);

    // Get/track the idle timeout
    const uint16_t initial_timeout = storage_get_idle_timeout();
    uint16_t new_timeout = initial_timeout;

    // Find the position in the list of allowed values
    // (NOTE: UINT16_MAX as final value prevents off-the-end)
    uint8_t pos = 0;
    while (VALUES[pos] < new_timeout) {
        ++pos;
    }

    gui_view_node_t* item_text = NULL;
    gui_activity_t* const act = make_carousel_activity("Idle Timeout", NULL, &item_text);
    JADE_ASSERT(item_text);
    update_timeout_text(item_text, new_timeout);
    gui_set_current_activity(act);

    int32_t ev_id;
    bool done = false;
    while (!done) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return;
        }

        // wait for a GUI event
        gui_activity_wait_event(act, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);

        switch (ev_id) {
        case GUI_WHEEL_LEFT_EVENT:
            pos = (pos + num_values - 1) % num_values;
            new_timeout = VALUES[pos];
            update_timeout_text(item_text, new_timeout);
            break;

        case GUI_WHEEL_RIGHT_EVENT:
            pos = (pos + 1) % num_values;
            new_timeout = VALUES[pos];
            update_timeout_text(item_text, new_timeout);
            break;

        default:
            done = (ev_id == gui_get_click_event());
        }
    }

    // Persist updated preferences
    if (new_timeout != initial_timeout) {
        storage_set_idle_timeout(new_timeout);
        // BBB-AIRGAP: the idle task may be part way through a sleep sized for the old value, and
        // would not read the new one until that sleep ended.
        idletimer_recheck();
    }
}

// BBB-AIRGAP: the dimming threshold used to be a compile-time constant.  It is a separate setting
// from the idle timeout above: this one only blanks the screen, that one locks or powers off the
// device.  A dimming value longer than the idle timeout is harmless - the device simply locks
// first and the dimming is never reached.
static void handle_screen_timeout(void)
{
    static const uint16_t VALUES[] = {
        30, 60, 120, 180, 300, 600, UINT16_MAX // UINT16_MAX == OFF
    };
    static const uint16_t num_values = sizeof(VALUES) / sizeof(VALUES[0]);

    // Get/track the screen timeout
    const uint16_t initial_timeout = storage_get_screen_timeout();
    uint16_t new_timeout = initial_timeout;

    // Find the position in the list of allowed values
    // (NOTE: UINT16_MAX as final value prevents off-the-end)
    uint8_t pos = 0;
    while (VALUES[pos] < new_timeout) {
        ++pos;
    }

    gui_view_node_t* item_text = NULL;
    gui_activity_t* const act = make_carousel_activity("Screen Timeout", NULL, &item_text);
    JADE_ASSERT(item_text);
    update_timeout_text(item_text, new_timeout);
    gui_set_current_activity(act);

    int32_t ev_id;
    bool done = false;
    while (!done) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return;
        }

        // wait for a GUI event
        gui_activity_wait_event(act, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);

        switch (ev_id) {
        case GUI_WHEEL_LEFT_EVENT:
            pos = (pos + num_values - 1) % num_values;
            new_timeout = VALUES[pos];
            update_timeout_text(item_text, new_timeout);
            break;

        case GUI_WHEEL_RIGHT_EVENT:
            pos = (pos + 1) % num_values;
            new_timeout = VALUES[pos];
            update_timeout_text(item_text, new_timeout);
            break;

        default:
            done = (ev_id == gui_get_click_event());
        }
    }

    // Persist updated preferences
    if (new_timeout != initial_timeout) {
        storage_set_screen_timeout(new_timeout);
        // BBB-AIRGAP: same as the idle timeout above - wake the task so a shortened threshold
        // takes effect from this click rather than from the end of the current sleep.
        idletimer_recheck();
    }
}

static void update_home_screen_item_highlight_color(gui_view_node_t* item)
{
    JADE_ASSERT(item);
    JADE_ASSERT(item->parent);
    JADE_ASSERT(item->parent->kind == FILL);
    gui_set_color(item->parent, gui_get_highlight_color());
}

static void handle_display_theme(void)
{
    static const char* THEME_NAMES[GUI_NUM_DISPLAY_THEMES]
        = { "Jade Green", "Bitcoin Orange", "Liquid Blue", "Cypherpunk Black", "Open-Source Opal" };
    JADE_ASSERT(GUI_NUM_DISPLAY_THEMES < GUI_FLAGS_THEMES_MASK);

    const uint8_t initial_gui_flags = storage_get_gui_flags();
    const uint8_t initial_theme = initial_gui_flags & GUI_FLAGS_THEMES_MASK;

    uint8_t new_theme = initial_theme < GUI_NUM_DISPLAY_THEMES ? initial_theme : 0;

    gui_view_node_t* item_text = NULL;
    gui_activity_t* const act = make_carousel_activity("Theme", NULL, &item_text);
    JADE_ASSERT(item_text);
    gui_update_text(item_text, THEME_NAMES[new_theme]);
    gui_set_current_activity(act);

    int32_t ev_id;
    bool done = false;
    const uint8_t entry_theme = new_theme;
    while (!done) {
        // BBB-AIRGAP: KEY3 leaves this screen.  The wheel changes the highlight colour live, so
        // leaving without saving puts it back; storage still holds the old theme, and every
        // screen drawn on the way home would otherwise use a colour that was never chosen.
        if (gui_escape_pending()) {
            gui_set_highlight_color(entry_theme);
            return;
        }

        // wait for a GUI event
        gui_activity_wait_event(act, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);

        switch (ev_id) {
        case GUI_WHEEL_LEFT_EVENT:
            new_theme = (new_theme + GUI_NUM_DISPLAY_THEMES - 1) % GUI_NUM_DISPLAY_THEMES;
            gui_set_highlight_color(new_theme);
            update_carousel_highlight_color(item_text, gui_get_highlight_color(), true);
            gui_update_text(item_text, THEME_NAMES[new_theme]);
            break;

        case GUI_WHEEL_RIGHT_EVENT:
            new_theme = (new_theme + 1) % GUI_NUM_DISPLAY_THEMES;
            gui_set_highlight_color(new_theme);
            update_carousel_highlight_color(item_text, gui_get_highlight_color(), true);
            gui_update_text(item_text, THEME_NAMES[new_theme]);
            break;

        default:
            done = (ev_id == gui_get_click_event());
        }
    }

    // Persist updated preferences
    if (new_theme != initial_theme) {
        JADE_ASSERT(new_theme < GUI_FLAGS_THEMES_MASK);
        const uint8_t new_gui_flags
            = (new_theme & GUI_FLAGS_THEMES_MASK) | (initial_gui_flags & ~GUI_FLAGS_THEMES_MASK);
        storage_set_gui_flags(new_gui_flags);

        // Also update top-level (long-lived) home screen with new colors
        update_home_screen_item_highlight_color(home_screen_selected_entry.symbol);
        update_home_screen_item_highlight_color(home_screen_selected_entry.text);
    }
}

#ifdef HAVE_CAMERA_ROTATION_SETTING
// BBB-AIRGAP: camera mounting angle, as degrees clockwise for the owner rather than the quarter
// turns stored.
static const char* const CAMERA_ROTATION_LABELS[CAMERA_ROTATION_NUM_VALUES] = { "0", "90", "180", "270" };

// The angle is chosen on a screen of its own, as Jade sets its brightness and theme.
static void handle_camera_rotation(void)
{
    const uint8_t initial = gui_get_camera_rotation();
    const size_t chosen
        = await_carousel_activity("Camera Rotation", CAMERA_ROTATION_LABELS, CAMERA_ROTATION_NUM_VALUES, initial);
    if (chosen == initial) {
        return;
    }
    gui_set_camera_rotation((uint8_t)chosen);

    const uint8_t initial_gui_flags = storage_get_gui_flags();
    const uint8_t new_gui_flags = gui_camera_rotation_to_flags(initial_gui_flags, (uint8_t)chosen);
    if (new_gui_flags != initial_gui_flags) {
        storage_set_gui_flags(new_gui_flags);
    }
    // Nothing to repaint: the setting is read when the camera next opens.
}
#endif // HAVE_CAMERA_ROTATION_SETTING

static void handle_flip_orientation(void)
{
    const uint8_t initial_gui_flags = storage_get_gui_flags();
    const bool initial_flipped_orientation = initial_gui_flags & GUI_FLAGS_FLIP_ORIENTATION;
    const bool new_flipped_orientation = gui_set_flipped_orientation(!initial_flipped_orientation); // toggle

    if (new_flipped_orientation != initial_flipped_orientation) {
        const uint8_t new_gui_flags = new_flipped_orientation ? initial_gui_flags | GUI_FLAGS_FLIP_ORIENTATION
                                                              : initial_gui_flags & ~GUI_FLAGS_FLIP_ORIENTATION;
        storage_set_gui_flags(new_gui_flags);
    }

    // Repaint the screen (as now inverted/rotated)
    const gui_activity_t* const act = gui_current_activity();
    if (act) {
        gui_repaint(act->root_node);
    }
}

#ifdef CONFIG_HAS_CAMERA
static void handle_pinserver_scan(void)
{
    if (keychain_has_pin()) {
        // Not allowed if wallet initialised
        await_error_3("Set Oracle not", "permitted once", "wallet initialized");
        return;
    }

    char* type;
    uint8_t* data = NULL;
    size_t data_len = 0;
    if (!bcur_scan_qr("Oracle QR", &type, &data, &data_len, 0, "blkstrm.com/oracle")) {
        // Scan aborted
        JADE_ASSERT(!type);
        JADE_ASSERT(!data);
        return;
    }

    if (!type || strcasecmp(type, BCUR_TYPE_JADE_UPDPS) || !data || !data_len) {
        await_error("Failed to parse Oracle data");
        goto cleanup;
    }

    if (!handle_update_pinserver_qr(data, data_len)) {
        JADE_LOGD("Failed to persist Oracle details");
        goto cleanup;
    }

    await_message("Oracle details updated");

cleanup:
    free(type);
    free(data);
}
#endif // CONFIG_HAS_CAMERA

static void handle_pinserver_reset(void)
{
    if (keychain_has_pin()) {
        // Not allowed if wallet initialised
        await_error_3("Reset Oracle not", "permitted once", "wallet initialized");
        return;
    }

    const char* question[] = { "Reset Oracle details", "and certificate?" };
    if (await_yesno_activity("Reset Oracle", question, 2, false, NULL)) {
        if (!reset_pinserver()) {
            await_error("Error resetting Oracle");
        }
    }
}

// Device info
static void handle_storage(void)
{
    size_t entries_used, entries_free;
    if (!storage_get_stats(&entries_used, &entries_free)) {
        await_error("Error accessing storage!");
        return;
    }

    gui_activity_t* const act = make_storage_stats_activity(entries_used, entries_free);
    gui_set_current_activity(act);
    while (
        !gui_activity_wait_event(act, GUI_BUTTON_EVENT, BTN_SETTINGS_DEVICE_INFO_STORAGE_EXIT, NULL, NULL, NULL, 0)) {
        // await button press
    }
}

static void handle_info_detail_screen(const char* title, const char* detail)
{
    JADE_ASSERT(title);
    JADE_ASSERT(detail);

    const bool show_help_btn = false;
    gui_activity_t* const act = make_show_single_value_activity(title, detail, show_help_btn);
    gui_set_current_activity(act);
    while (!gui_activity_wait_event(act, GUI_BUTTON_EVENT, BTN_BACK, NULL, NULL, NULL, 0)) {
        // await button press
    }
}

static void handle_display_fwversion(void) { handle_info_detail_screen("Firmware Version", running_app_info.version); }

static void handle_display_mac_address(void)
{
    char mac[18] = "NO BLE";
#ifdef CONFIG_BT_ENABLED
    char* hexout = NULL;
    JADE_WALLY_VERIFY(wally_hex_from_bytes((uint8_t*)macid, 6, &hexout));

    mac[0] = toupper((int)hexout[0]);
    mac[1] = toupper((int)hexout[1]);
    mac[2] = ':';
    mac[3] = toupper((int)hexout[2]);
    mac[4] = toupper((int)hexout[3]);
    mac[5] = ':';
    mac[6] = toupper((int)hexout[4]);
    mac[7] = toupper((int)hexout[5]);
    mac[8] = ':';
    mac[9] = toupper((int)hexout[6]);
    mac[10] = toupper((int)hexout[7]);
    mac[11] = ':';
    mac[12] = toupper((int)hexout[8]);
    mac[13] = toupper((int)hexout[9]);
    mac[14] = ':';
    mac[15] = toupper((int)hexout[10]);
    mac[16] = toupper((int)hexout[11]);
    mac[17] = '\0';

    JADE_WALLY_VERIFY(wally_free_string(hexout));
#endif

    handle_info_detail_screen("MAC Address", mac);
}

#ifdef CONFIG_HAS_BATTERY
static void handle_display_battery_volts(void)
{
    char power_status[32] = "NO BAT";
#ifdef CONFIG_HAS_AXP
    const int ret = snprintf(power_status, sizeof(power_status), "%umv", power_get_vbat());
    JADE_ASSERT(ret > 0 && ret < sizeof(power_status));
#elif defined(CONFIG_BOARD_TYPE_M5_STICKC_PLUS_2)
    const uint16_t vbat = power_get_vbat();
    if (vbat > 0) {
        const int ret = snprintf(power_status, sizeof(power_status), "%umv", vbat);
        JADE_ASSERT(ret > 0 && ret < sizeof(power_status));
    }
#elif defined(CONFIG_BOARD_TYPE_WS_TOUCH_LCD2)
    const uint16_t vbat = power_get_vbat() * 3; // applying voltage divider
    if (vbat > 0) {
        const int ret = snprintf(power_status, sizeof(power_status), "%umv", vbat);
        JADE_ASSERT(ret > 0 && ret < sizeof(power_status));
    }
#elif defined(CONFIG_HAS_IP5306)
    const float approx_voltage = power_get_vbat() / 1000.0;
    const int ret = snprintf(power_status, sizeof(power_status), "Approx %.1fv", approx_voltage);
    JADE_ASSERT(ret > 0 && ret < sizeof(power_status));
#endif

    handle_info_detail_screen("Battery Volts", power_status);
}
#endif // CONFIG_HAS_BATTERY

static void update_network_carousel_item(gui_view_node_t* network_type_item, const network_type_t type)
{
    JADE_ASSERT(network_type_item);
    const char* label = type == NETWORK_TYPE_TEST ? "Testnet" : "Mainnet";
    gui_update_text(network_type_item, label);
}

static void handle_network_type(void)
{
    // Only expected for QR Mode atm
    JADE_ASSERT(keychain_get() && keychain_get_userdata() == SOURCE_INTERNAL);

    network_type_t type = keychain_get_network_type_restriction();

    gui_view_node_t* network_textbox = NULL;
    gui_activity_t* const act_network = make_carousel_activity("Network Type", NULL, &network_textbox);
    update_network_carousel_item(network_textbox, type);

    // Show, and await button click
    gui_set_current_activity(act_network);

    int32_t ev_id;
    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return;
        }

        if (gui_activity_wait_event(act_network, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
            if (ev_id == GUI_WHEEL_LEFT_EVENT || ev_id == GUI_WHEEL_RIGHT_EVENT) {
                type = type == NETWORK_TYPE_TEST ? NETWORK_TYPE_MAIN
                                                 : NETWORK_TYPE_TEST; // Just toggle label at this point
                update_network_carousel_item(network_textbox, type);
            } else if (ev_id == gui_get_click_event()) {
                // Done - apply network change
                break;
            }
        }
    }

    keychain_clear_network_type_restriction();
    keychain_set_network_type_restriction(type);
}

// BBB-AIRGAP: the optional features, in the order the screen lists them.  A row names the feature
// and nothing more, as Jade's own settings menus do (Display, Preferences); the value in use shows
// on the feature's own screen.  A row carries both value labels rather than an on/off pair, because
// a row like the denomination has two states with no 'off' reading ('BTC' is not 'units turned
// off').  See storage.h for the flags.
typedef struct {
    const char* name;
    const char* on;
    const char* off;
    uint8_t flag;
} feature_row_t;

static const feature_row_t FEATURE_ROWS[] = {
    { .name = "BIP85", .on = "On", .off = "Off", .flag = FEATURE_FLAGS_BIP85 },
    { .name = "Sign Msg", .on = "On", .off = "Off", .flag = FEATURE_FLAGS_SIGN_MESSAGE },
    { .name = "Warnings", .on = "On", .off = "Off", .flag = FEATURE_FLAGS_HARSH_WARNINGS },
    { .name = "Xpub Info", .on = "On", .off = "Off", .flag = FEATURE_FLAGS_XPUB_DETAILS },
    { .name = "Singlesig", .on = "On", .off = "Off", .flag = FEATURE_FLAGS_SINGLESIG },
    { .name = "Multisig", .on = "On", .off = "Off", .flag = FEATURE_FLAGS_MULTISIG },
    { .name = "Units", .on = "sats", .off = "BTC", .flag = FEATURE_FLAGS_DENOMINATION_SATS },
};

#define NUM_FEATURE_ROWS (sizeof(FEATURE_ROWS) / sizeof(FEATURE_ROWS[0]))

// BBB-AIRGAP: which optional features this device offers.  Every flag here only removes a way in -
// a menu row, an extra screen - so a device that has them all off signs exactly what one with them
// all on signs.  A row opens the feature's own screen, where the value in use sits between the
// arrows and the click keeps the one shown; the flag changes only then.
static void handle_wallet_options(void)
{
    uint8_t flags = storage_get_feature_flags();
    size_t selected = 0;

    list_item_t items[NUM_FEATURE_ROWS];
    for (size_t i = 0; i < NUM_FEATURE_ROWS; ++i) {
        items[i] = (list_item_t){ .txt = FEATURE_ROWS[i].name, .ev_id = BTN_FEATURE_ROW_0 + i };
    }

    while (true) {
        // BBB-AIRGAP: an escape started on a screen this menu opened has to keep going; the list
        // itself only sees KEY3 while it is the one waiting.
        if (gui_escape_pending()) {
            return;
        }
        const int32_t ev_id
            = run_list_activity("Features", BTN_SETTINGS_FEATURES_EXIT, items, NUM_FEATURE_ROWS, &selected);
        if (ev_id == BTN_SETTINGS_FEATURES_EXIT) {
            return;
        }

        const size_t row = (size_t)(ev_id - BTN_FEATURE_ROW_0);
        JADE_ASSERT(row < NUM_FEATURE_ROWS);
        const feature_row_t* const feature = &FEATURE_ROWS[row];

        const char* const labels[] = { feature->on, feature->off };
        const bool was_on = flags & feature->flag;
        const bool on = await_carousel_activity(feature->name, labels, 2, was_on ? 0 : 1) == 0;
        if (on == was_on) {
            continue;
        }

        // The xpub and address screens offer a wallet type from these two, so turning the last one
        // off would leave them with nothing to list.  Refused here, where the user can see what
        // was refused, rather than second-guessed at the point that reads the flags.
        if (!on && (feature->flag == FEATURE_FLAGS_SINGLESIG || feature->flag == FEATURE_FLAGS_MULTISIG)) {
            const uint8_t other
                = feature->flag == FEATURE_FLAGS_SINGLESIG ? FEATURE_FLAGS_MULTISIG : FEATURE_FLAGS_SINGLESIG;
            if (!(flags & other)) {
                await_error_2("One wallet type", "must stay on");
                continue;
            }
        }

        const uint8_t updated = on ? (flags | feature->flag) : (flags & ~feature->flag);
        if (!storage_set_feature_flags(updated)) {
            await_error_2("Failed to save", "wallet options");
            continue;
        }
        flags = updated;
    }
}

// BBB-AIRGAP: paints the panel one flat colour at a time.  A pixel that is stuck shows as a dot
// that does not follow the field; a colour channel that is not reaching the display shows as a
// frame that comes up wrong or black.  Neither is visible against a normal screen, which is why
// this is a screen of its own rather than something read off the menus.
static void handle_io_test_screen(void)
{
    const color_t colours[] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE, TFT_BLACK };

    gui_view_node_t* colour_fill = NULL;
    gui_activity_t* const act = make_io_test_screen_activity(&colour_fill);
    gui_set_color(colour_fill, colours[0]);

    wait_event_data_t* const event_data = gui_activity_make_wait_event_data(act);
    JADE_ASSERT(event_data);
    gui_activity_register_event(act, GUI_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);

    gui_set_current_activity_sync(act, false);

    // BBB-AIRGAP: discard the menu click's trailing raw event before it can skip red; this is the
    // screen-specific instance of the switch-then-drain race documented in main/ui/dialogs.c:565.
    while (sync_wait_event(event_data, NULL, NULL, NULL, 10 / portTICK_PERIOD_MS) == ESP_OK) {
        // discard; see comment above
    }

    for (size_t i = 0; i < sizeof(colours) / sizeof(colours[0]); ++i) {
        if (i) {
            gui_set_color(colour_fill, colours[i]);
            gui_repaint(colour_fill);
        }

        // Both clicks advance.  The button header on this port was wired by hand, so a check that
        // listened to only one of them would leave the user unable to tell a screen that is not
        // painting from a button that is not reaching the firmware.
        int32_t ev_id = 0;
        bool advance = false;
        while (!advance) {
            // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
            if (gui_escape_pending()) {
                return;
            }

            if (sync_wait_event(event_data, NULL, &ev_id, NULL, 0) == ESP_OK) {
                advance = (ev_id == GUI_WHEEL_CLICK_EVENT || ev_id == GUI_FRONT_CLICK_EVENT);
            }
        }
    }
}

// BBB-AIRGAP: a mark stays green once it is lit, so the screen ends up showing the whole set
// rather than only the last press.
static void mark_io_test_button(gui_view_node_t* const mark)
{
    JADE_ASSERT(mark);
    gui_set_color(mark, TFT_GREEN);
    gui_repaint(mark);
}

// BBB-AIRGAP: the buttons check.  Every mark starts grey and turns green when the event naming
// its input arrives; KEY3 ends the screen instead of marking, so leaving is its test.  The echo
// is what makes the vertical joystick pair and KEY1 name themselves here (gui_set_input_echo(),
// main/gui.h), and it is turned off on the way out whichever way the screen ends.
static void handle_io_test_buttons(void)
{
    gui_view_node_t* marks[IO_TEST_NUM_MARKS] = {};
    gui_activity_t* const act = make_io_test_buttons_activity(marks);

    wait_event_data_t* const event_data = gui_activity_make_wait_event_data(act);
    JADE_ASSERT(event_data);
    gui_activity_register_event(act, GUI_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);

    gui_set_current_activity_sync(act, false);

    // Drop the menu click's trailing event before it can mark the centre press for free; the
    // screen check above documents the same switch-then-drain race.
    while (sync_wait_event(event_data, NULL, NULL, NULL, 10 / portTICK_PERIOD_MS) == ESP_OK) {
        // discard; see comment above
    }

    gui_set_input_echo(true);

    bool done = false;
    while (!done) {
        int32_t ev_id = 0;
        if (sync_wait_event(event_data, NULL, &ev_id, NULL, 0) != ESP_OK) {
            continue;
        }

        switch (ev_id) {
        case GUI_WHEEL_UP_EVENT:
            mark_io_test_button(marks[IO_TEST_MARK_UP]);
            break;
        case GUI_WHEEL_DOWN_EVENT:
            mark_io_test_button(marks[IO_TEST_MARK_DOWN]);
            break;
        case GUI_WHEEL_LEFT_EVENT:
            mark_io_test_button(marks[IO_TEST_MARK_LEFT]);
            break;
        case GUI_WHEEL_RIGHT_EVENT:
            mark_io_test_button(marks[IO_TEST_MARK_RIGHT]);
            break;
        case GUI_SELECT_FIRST_EVENT:
            mark_io_test_button(marks[IO_TEST_MARK_KEY1]);
            break;
        // Only the front click: libjade_input() maps LIBJADE_INPUT_CLICK to gui_front_click() and
        // nothing on this board reaches gui_wheel_click(), so the wheel event cannot arrive here.
        case GUI_FRONT_CLICK_EVENT:
            // One input, two keys: both marks light and the screen says which two they are.
            mark_io_test_button(marks[IO_TEST_MARK_CLICK]);
            mark_io_test_button(marks[IO_TEST_MARK_KEY2]);
            break;
        case GUI_ALT_EVENT:
            done = true;
            break;
        }
    }

    gui_set_input_echo(false);
}

#ifdef CONFIG_HAS_CAMERA
// BBB-AIRGAP: a camera that opens but delivers nothing and a camera that is delivering look the
// same on a preview you are watching for the first time, so the frames are counted.  A frame with
// no dimensions or no data is not evidence the sensor is alive and is not counted.  The callback
// never claims to have consumed an image, so the camera screen stays up until the user leaves it.
static bool io_test_camera_cb(
    const size_t width, const size_t height, const uint8_t* data, const size_t len, void* ctx_data)
{
    JADE_ASSERT(ctx_data);
    size_t* const frames = (size_t*)ctx_data;

    if (width && height && data && len) {
        ++*frames;
    }
    return false;
}

static void handle_io_test_camera(void)
{
    size_t frames = 0;
    jade_camera_process_images(
        io_test_camera_cb, &frames, true, "Camera test", false, QR_GUIDE_HIDE, NULL, NULL, NULL, NULL);

    // BBB-AIRGAP: a camera escape must reach the menu's head check without a result-screen wait.
    if (gui_escape_pending()) {
        return;
    }

    char count[16];
    const int ret = snprintf(count, sizeof(count), "%u", (unsigned)frames);
    JADE_ASSERT(ret > 0 && ret < sizeof(count));
    await_message_2("Frames seen:", count);
}
#endif // CONFIG_HAS_CAMERA

// BBB-AIRGAP: reached from Info.  Runs its own loop, like the other option screens, and leaves the
// rebuilding of the menu it came from to the caller.
static void handle_io_test(void)
{
    gui_activity_t* const act = make_io_test_activity();

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return;
        }

        // BBB-AIRGAP: the checks below build their own activities, and coming back to the menu is
        // where those are let go - the same menu pattern handle_settings() uses, and what the
        // comment at the call site promises.  The menu is the new current activity here, so it is
        // the one thing retained (main/gui.c gui_set_current_activity_impl()).
        gui_set_current_activity_ex(act, true);

        int32_t ev_id = 0;
        if (!gui_activity_wait_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
            continue;
        }

        switch (ev_id) {
        case BTN_IO_TEST_SCREEN:
            handle_io_test_screen();
            break;

        case BTN_IO_TEST_BUTTONS:
            handle_io_test_buttons();
            break;

#ifdef CONFIG_HAS_CAMERA
        case BTN_IO_TEST_CAMERA:
            handle_io_test_camera();
            break;
#endif

        case BTN_IO_TEST_EXIT:
            return;
        }
    }
}

// Create the appropriate 'Settings' menu
// BBB-AIRGAP: one Options screen for every device state, replacing the three menu activities
// upstream chose between (uninitialised / locked / unlocked).  Those three disagreed about where a
// screen lived: 'Settings' sat at Options > Settings on an uninitialised device and at
// Options > Device > Settings on every other, and the exit branch had to pick between the two to
// know where to go back to.  Here a row is laid out only where it can act, so every screen keeps
// one address whatever the device is doing.  It is a list rather than a menu because
// make_menu_activity() asserts on a fifth row (main/ui/dialogs.c:265) and this holds up to ten;
// see pijade/ROADMAP.md for the state-by-state measurements behind each condition.
static int32_t run_options_list(size_t* selected)
{
    JADE_ASSERT(selected);

    const bool wallet_loaded = keychain_get();

    // OTP records are encrypted under a key derived from the seed of the wallet in use
    // (get_otp_encryption_key(), main/otpauth.c), and a wallet read back from the blob as a
    // serialised xpriv carries no seed - so with one of those loaded every record would fail to
    // decrypt and be offered for deletion.  The row follows what the screen can actually do.
    const bool otp_usable = wallet_loaded && keychain_get()->seed_len;

    list_item_t items[10];
    size_t num_items = 0;
    // BBB-AIRGAP: rows in the order they are reached, the way the wallet menu under Session is
    // laid out: what a session does first, most wanted at the top, then the groups that are set
    // once, and last the one row that cannot be undone.  Upstream opened the wallet-less menu
    // with Temporary Signer as well (make_uninitialised_settings_activity).  Which rows appear
    // did not change with the order; each condition is still the one described where it stands.
#ifdef CONFIG_HAS_CAMERA
    // BBB-AIRGAP: with the slot table a scanned wallet is loaded beside the ones already held
    // rather than replacing them, but until now the only way to reach that was to point the
    // generic scanner at a SeedQR and hope.  This row names the operation and takes the scanner
    // that accepts nothing else (mnemonic_qr(), main/process/mnemonic.c).
    //
    // It is offered only with a wallet already open, because with none there is nothing to add to:
    // that is the Temporary Signer row in its place, which runs the full first-wallet flow and
    // settles the message source the dashboard then works from.  handle_mnemonic_qr() instead
    // inherits the source of the wallet in use, so it needs one to inherit from.
    if (wallet_loaded) {
        items[num_items++] = (list_item_t){ .txt = "Add Wallet", .ev_id = BTN_SETTINGS_ADD_WALLET };
    }
#endif
    if (!wallet_loaded) {
        items[num_items++] = (list_item_t){ .txt = "Temporary Signer", .ev_id = BTN_SETTINGS_TEMPORARY_WALLET_LOGIN };
    }
    if (otp_usable) {
        items[num_items++] = (list_item_t){ .txt = "OTP", .ev_id = BTN_SETTINGS_OTP };
    }
#ifdef CONFIG_HAS_CAMERA
    // Mining needs a camera to scan its template, but no wallet: the reward address comes from the
    // template unless the user asked for their own (apply_reward_address_preference(),
    // main/qrmode.c).  Upstream kept it in the device menu, which an uninitialised device does not
    // have, so it was unreachable in exactly the state that needs no wallet.
    items[num_items++] = (list_item_t){ .txt = "Mining", .ev_id = BTN_SETTINGS_MINING };
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_HAS_BATTERY)
    // BBB-AIRGAP: a locked device has neither an eligible firmware upgrade nor a wallet to sign or
    // export from, so the entire screen would be a dead end.  A temporary wallet over a PIN blob is
    // different: it can sign and export its own xpub, while the screen builder omits only Firmware
    // Upgrade because ota_allowed() correctly requires the PIN wallet itself to be open.
    if (wallet_loaded || !keychain_has_pin()) {
        items[num_items++] = (list_item_t){ .txt = "USB Storage", .ev_id = BTN_SETTINGS_USBSTORAGE };
    }
#endif
    items[num_items++] = (list_item_t){ .txt = "Preferences", .ev_id = BTN_SETTINGS_PREFS };
    items[num_items++] = (list_item_t){ .txt = "Features", .ev_id = BTN_SETTINGS_FEATURES };
    items[num_items++] = (list_item_t){ .txt = "Display", .ev_id = BTN_SETTINGS_DISPLAY };
    items[num_items++] = (list_item_t){ .txt = "Security", .ev_id = BTN_SETTINGS_SECURITY };
    items[num_items++] = (list_item_t){ .txt = "Info", .ev_id = BTN_SETTINGS_INFO };
    // Last, and after a gap of ordinary rows, because it is the one entry here that cannot be
    // undone.  Upstream had it between 'Settings' and 'Info' in the device menu.
    //
    // Offered in every state on purpose, which is also what upstream did: the locked device
    // reached it through Options > Device (make_locked_settings_activity ->
    // make_device_settings_activity).  Putting it behind the PIN would protect nothing - three
    // wrong PINs already erase the wallet blob (main/process/auth_user.c) - while taking away the
    // only way someone who has forgotten their PIN can clear the card, since this port never
    // reaches the Boot Menu that upstream offers instead (pijade-host starts its input loop after
    // libjade_start(), and the whole click window lives inside that call).  A wipe triggered by
    // wrong PINs also leaves the duress record, the OTP records and the preferences behind; this
    // row is what clears them.  offer_jade_reset() gates the action behind a yes/no screen and a
    // random confirmation code.
    items[num_items++] = (list_item_t){ .txt = "Factory Reset", .ev_id = BTN_SETTINGS_RESET };
    JADE_ASSERT(num_items <= sizeof(items) / sizeof(items[0]));

    return run_list_activity("Options", BTN_SETTINGS_EXIT, items, num_items, selected);
}

// BBB-AIRGAP: the device preferences.  Laid out on every pass, not once, because the network row
// is labelled with the network it is set to.
static int32_t run_preferences_list(size_t* selected)
{
    JADE_ASSERT(selected);

    // hw initialised with internal message source (ie. QR-mode)
    const bool hw_qr_mode = keychain_get() && keychain_get_userdata() == SOURCE_INTERNAL;

    list_item_t items[4];
    size_t num_items = 0;
    // BBB-AIRGAP: upstream offered this only outside QR mode, swapping it for the network row.  On
    // this port every unlock path sets SOURCE_INTERNAL (auth_qr_mode_ex() above), so that swap hid
    // it from the moment a wallet was loaded - the timeout that dims the screen could not be
    // changed in the only mode the device runs in.  handle_idle_timeout() reads and writes storage
    // and nothing else, so it is offered throughout and the network row is added beside it.
    items[num_items++] = (list_item_t){ .txt = "Idle Timeout", .ev_id = BTN_SETTINGS_IDLE_TIMEOUT };
    // The dimming threshold sits next to the timeout it is most easily confused with.
    items[num_items++] = (list_item_t){ .txt = "Screen Timeout", .ev_id = BTN_SETTINGS_SCREEN_TIMEOUT };
    if (hw_qr_mode) {
        // handle_network_type() asserts a wallet in QR mode, so the row carries that condition.
        items[num_items++] = (list_item_t){ .txt
            = keychain_get_network_type_restriction() == NETWORK_TYPE_TEST ? "Network: Testnet" : "Network: Mainnet",
            .ev_id = BTN_SETTINGS_NETWORK_TYPE };
    }
    // qr density and speed drive every wallet code the device shows, so they are a device
    // preference rather than something reached only from inside the flows that display a code.
    items[num_items++] = (list_item_t){ .txt = "QR Settings", .ev_id = BTN_SETTINGS_QR };
    JADE_ASSERT(num_items <= sizeof(items) / sizeof(items[0]));

    return run_list_activity("Preferences", BTN_SETTINGS_PREFS_EXIT, items, num_items, selected);
}

// BBB-AIRGAP: the PIN and passphrase screens.  Upstream split these between an 'Authentication'
// menu that existed only while unlocked and the preferences list, which put 'Change PIN' and
// 'Change PIN (QR)' in different places for what the user sees as the same job.
static int32_t run_security_list(size_t* selected)
{
    JADE_ASSERT(selected);

    const bool hw_locked_initialised = !keychain_get() && keychain_has_pin();
    const bool hw_pin_unlocked = keychain_get() && keychain_has_pin() && !keychain_has_temporary();

    list_item_t items[4];
    size_t num_items = 0;
    if (hw_locked_initialised) {
        items[num_items++] = (list_item_t){ .txt = "Change PIN", .ev_id = BTN_SETTINGS_CHANGE_PIN };
    }
#ifdef CONFIG_HAS_CAMERA
    if (hw_pin_unlocked) {
        items[num_items++] = (list_item_t){ .txt = "Change PIN (QR)", .ev_id = BTN_SETTINGS_CHANGE_PIN_QR };
    }
#endif
    // The duress record is read at one place only, while a PIN is being entered
    // (main/process/auth_user.c), so a device with no PIN could store the setting but never fire
    // it.  The stronger condition is the one that matters though: handle_wallet_erase_pin() offers
    // to change or delete the duress PIN, and whoever can do that can disarm the protection the
    // stored wallet relies on, so it must sit behind the PIN it protects.  A locked device and a
    // temporary wallet loaded over the PIN wallet both fail that test, which is why 'has a PIN'
    // is not enough.  (Until phase 3 the screen also printed the PIN in clear; that reason is
    // gone, this one is not.)
    if (hw_pin_unlocked) {
        items[num_items++] = (list_item_t){ .txt = "Duress PIN", .ev_id = BTN_SETTINGS_WALLET_ERASE_PIN };
    }
    // A preference for wallets loaded later, so it holds in every state, including with one
    // already loaded - upstream showed it only before a wallet was there.
    items[num_items++] = (list_item_t){ .txt = "BIP39 Passphrase", .ev_id = BTN_SETTINGS_BIP39_PASSPHRASE };
    JADE_ASSERT(num_items <= sizeof(items) / sizeof(items[0]));

    return run_list_activity("Security", BTN_SETTINGS_SECURITY_EXIT, items, num_items, selected);
}

// BBB-AIRGAP: which scrolling list is on screen, or NONE while a menu activity is.  Upstream had a
// single bool for the one list it had; three screens are lists now, so the flag names which.  Every
// branch below either names another list, or clears the flag AND rebuilds 'act' - never one without
// the other, because run_list_activity() takes the screen over and frees what was there.
typedef enum {
    SETTINGS_LIST_NONE,
    SETTINGS_LIST_OPTIONS,
    SETTINGS_LIST_PREFERENCES,
    SETTINGS_LIST_SECURITY,
} settings_list_t;

static void handle_settings(const bool startup_menu)
{
    // The startup menu is a menu activity of its own; every other entry opens the Options list.
    // (On this port the startup menu is unreachable - the splash-screen click window closes inside
    // libjade_start() - but it is left intact for hardware that does reach it.)
    gui_activity_t* act = startup_menu ? make_startup_options_activity() : NULL;
    settings_list_t open_list = startup_menu ? SETTINGS_LIST_NONE : SETTINGS_LIST_OPTIONS;


    // Selection is remembered per list, so coming back from a sub-screen lands where it was left.
    size_t options_selected = 0;
    size_t prefs_selected = 0;
    size_t security_selected = 0;

    // NOTE: menu navigation frees prior screens, as the navigation is
    // potentially unbound with all the back and forward buttons.
    bool done = false;
    while (!done) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            done = true;
            continue;
        }

        int32_t ev_id;
        switch (open_list) {
        case SETTINGS_LIST_OPTIONS:
            ev_id = run_options_list(&options_selected);
            break;

        case SETTINGS_LIST_PREFERENCES:
            ev_id = run_preferences_list(&prefs_selected);
            break;

        case SETTINGS_LIST_SECURITY:
            ev_id = run_security_list(&security_selected);
            break;

        default:
            JADE_ASSERT(act);
            gui_set_current_activity_ex(act, true);
            gui_activity_wait_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);
            break;
        }

        switch (ev_id) {

        case BTN_SETTINGS_EXIT:
            done = true;
            break;

        case BTN_SETTINGS_PINSERVER_EXIT:
            // BBB-AIRGAP: the Blind Oracle screen hangs off the Boot Menu and nothing else
            // (BTN_SETTINGS_PINSERVER appears only in make_startup_options_activity()), so its
            // exit goes back there.  Sending it to the Options list would open the ordinary
            // settings tree in the middle of the boot flow, which never asked for it.
            JADE_ASSERT(startup_menu);
            open_list = SETTINGS_LIST_NONE;
            act = make_startup_options_activity();
            break;

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_HAS_BATTERY)
        case BTN_SETTINGS_USBSTORAGE_EXIT:
#endif
        case BTN_SETTINGS_INFO_EXIT:
        case BTN_SETTINGS_PREFS_EXIT:
        case BTN_SETTINGS_SECURITY_EXIT:
            // Back to the Options list, from wherever this was
            open_list = SETTINGS_LIST_OPTIONS;
            break;

        case BTN_SETTINGS_INFO:
        case BTN_SETTINGS_DEVICE_INFO_EXIT:
            // Change to 'Info' menu
            open_list = SETTINGS_LIST_NONE;
            act = make_info_activity(running_app_info.version);
            break;

        case BTN_SETTINGS_DEVICE_INFO:
            // BBB-AIRGAP: a loaded wallet is not sufficient authentication because a temporary
            // SeedQR wallet can sit over a locked PIN blob.  On radio builds the row can persist the
            // Bluetooth state and delete pairings, so expose it only with no device PIN or with the
            // PIN wallet itself open.
            act = make_device_info_activity(!keychain_has_pin() || (keychain_get() && !keychain_has_temporary()));
            break;

#ifdef CONFIG_HAS_CAMERA
        case BTN_SETTINGS_ADD_WALLET: {
            // BBB-AIRGAP: the free-slot load, reached by name rather than by pointing the generic
            // scanner at a SeedQR.  handle_mnemonic_qr() owns everything that follows the scan -
            // the confirmation, derive_keychain() into a free slot, and inheriting the carrier of
            // the wallet in use - so this only supplies the phrase.  A full table is not checked
            // before the camera opens: the wallet scanned may be one already held, which this row
            // then switches to, and refusing that scan for want of a slot it does not need was the
            // bug.  The refusal lives where the wallet is known, in derive_keychain().
            char mnemonic[MNEMONIC_BUFLEN];
            SENSITIVE_PUSH(mnemonic, sizeof(mnemonic));
            if (mnemonic_qr(mnemonic, sizeof(mnemonic))) {
                handle_mnemonic_qr(mnemonic);
            }
            SENSITIVE_POP(mnemonic);
            break;
        }

        case BTN_SETTINGS_MINING:
            // BBB-AIRGAP: mining runs its own menu and screens and returns here when it is done,
            // the way the qr settings entry below does. It neither loads nor drops a wallet, so
            // there is nothing to rebuild; the loop is still in its Options branch and draws that
            // list afresh.
            handle_mining_settings();
            break;
#endif

        // BBB-AIRGAP: the checks run their own screens and free the managed activities behind them
        // on the way through, so the Info menu this returns to is built again rather than the
        // pointer being reused.
        case BTN_SETTINGS_IO_TEST:
            handle_io_test();
            act = make_info_activity(running_app_info.version);
            break;

        case BTN_SETTINGS_PREFS:
            // Change to the 'Preferences' list
            open_list = SETTINGS_LIST_PREFERENCES;
            break;

        case BTN_SETTINGS_SECURITY:
            // Change to the 'Security' list
            open_list = SETTINGS_LIST_SECURITY;
            break;

        case BTN_SETTINGS_DISPLAY_EXIT:
            // 'Display' is entered from the Options list, so that is where its back button goes
            open_list = SETTINGS_LIST_OPTIONS;
            break;

        case BTN_SETTINGS_QR:
            // BBB-AIRGAP: this screen runs its own loop and writes the choice out when it exits,
            // so when it returns there is nothing to save and nothing to rebuild - the list it was
            // entered from is drawn afresh by the loop above, which is still in that branch.
            handle_qr_settings();
            break;

        // BBB-AIRGAP: which optional features this device offers.  Runs its own loop, like the
        // screen above, and comes back to the same list.
        case BTN_SETTINGS_FEATURES:
            handle_wallet_options();
            break;

        case BTN_SETTINGS_DISPLAY:
            // Change to the 'Display' menu
            open_list = SETTINGS_LIST_NONE;
            act = make_display_settings_activity();
            break;

        case BTN_SETTINGS_OTP_EXIT:
            // 'OTP' is entered from the Options list, so that is where its back button goes
            open_list = SETTINGS_LIST_OPTIONS;
            break;

        case BTN_SETTINGS_OTP:
        case BTN_SETTINGS_OTP_NEW_EXIT:
            // Change to 'OTP' menu
            open_list = SETTINGS_LIST_NONE;
            act = make_otp_activity();
            break;

        case BTN_SETTINGS_OTP_NEW:
            // Change to 'New OTP' menu
            act = make_new_otp_activity();
            break;

        case BTN_SETTINGS_PINSERVER:
            // Change to 'PinServer' menu
            act = make_pinserver_activity();
            break;

        // Screen handling
        case BTN_SETTINGS_INFO_FWVERSION:
            handle_display_fwversion();
            break;

        case BTN_SETTINGS_DEVICE_INFO_MAC:
            handle_display_mac_address();
            break;

#ifdef CONFIG_HAS_BATTERY
        case BTN_SETTINGS_DEVICE_INFO_BATTERY:
            handle_display_battery_volts();
            break;
#endif

        case BTN_SETTINGS_DEVICE_INFO_STORAGE:
            handle_storage();
            break;

        case BTN_SETTINGS_IDLE_TIMEOUT:
            handle_idle_timeout();
            break;

        case BTN_SETTINGS_SCREEN_TIMEOUT:
            handle_screen_timeout();
            break;

        case BTN_SETTINGS_NETWORK_TYPE:
            handle_network_type();
            break;

        case BTN_SETTINGS_BLE:
            handle_ble();
            break;

        case BTN_SETTINGS_CHANGE_PIN:
            handle_change_pin();
            break;

#ifdef CONFIG_HAS_CAMERA
        case BTN_SETTINGS_CHANGE_PIN_QR:
            done = handle_change_pin_qr();
            break;
#endif

// NOTE: Only boards listed here have brightness controls
// BBB-AIRGAP: HAVE_DISPLAY_BRIGHTNESS_SETTING added - piJade drives the backlight from the host
// rather than a PMU, see main/gui.h for why the board type itself is not defined.
#if defined(CONFIG_BOARD_TYPE_JADE_V1_1) || defined(CONFIG_BOARD_TYPE_JADE_V2_ANY)                                     \
    || defined(CONFIG_BOARD_TYPE_WS_TOUCH_LCD2) || defined(CONFIG_BOARD_TYPE_TTGO_TDISPLAY)                            \
    || defined(CONFIG_BOARD_TYPE_M5_STICKC_PLUS_2) || defined(HAVE_DISPLAY_BRIGHTNESS_SETTING)
        case BTN_SETTINGS_DISPLAY_BRIGHTNESS:
            handle_screen_brightness();
            break;
#endif

#ifdef HAVE_CAMERA_ROTATION_SETTING
        case BTN_SETTINGS_DISPLAY_CAMERA_ROTATION:
            handle_camera_rotation();
            break;
#endif

        case BTN_SETTINGS_DISPLAY_ORIENTATION:
            handle_flip_orientation();
            break;

        case BTN_SETTINGS_DISPLAY_THEME:
            handle_display_theme();
            // remake parent screen to update colours
            act = make_display_settings_activity();
            break;

        case BTN_SETTINGS_BIP39_PASSPHRASE:
            // persist settings in storage
            handle_passphrase_prefs();
            break;

        case BTN_SETTINGS_WALLET_ERASE_PIN:
            handle_wallet_erase_pin();
            break;

        case BTN_SETTINGS_RESET:
            offer_jade_reset();
            break;

        case BTN_SETTINGS_QR_PINSERVER:
            // If the user starts the process of interacting with the pinserver via QR codes we must break out
            // here and not go back to the menu, as a) the 'auth_user' and pinserver messages need to be handled
            // asap, and b) the process may have invalidated the menu screens/activities we are using here.
            // ofc if the user declines starting the process, staying in the loop is fine/correct.
            done = offer_pinserver_qr_unlock();
            break;

        case BTN_SETTINGS_TEMPORARY_WALLET_LOGIN:
            // If the user starts the process of creating a temporary wallet, we must break out here and not
            // go back to the menu, as a) the 'auth_user' message probably needs to be handled asap, and b) the
            // setup process may have invalidated the menu screens/activities we are using in this loop.
            // ofc if the user declines starting the process, staying in the loop is fine/correct.
            done = offer_temporary_wallet_login();
            break;

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_HAS_BATTERY)
        case BTN_SETTINGS_USBSTORAGE:
            open_list = SETTINGS_LIST_NONE;
            act = make_usbstorage_settings_activity(keychain_get(), ota_allowed(SOURCE_INTERNAL)); // create menu
            break;

        case BTN_SETTINGS_USBSTORAGE_FW:
            // If the ota is initiated, we need to return to the main dispatcher loop
            // to handle the OTA messages - ie. set 'done' flag to exit this loop.
            if (ota_allowed(SOURCE_INTERNAL)) {
                // Set flag that allows usb to be disconnected temporarily
                // (reset after the next message is processed).
                tolerate_usb_disconnection = true;
                done = usbstorage_firmware_ota(NULL);
            } else {
                await_error_2("Unlock with PIN before", "initiating firmware update");
            }
            break;

        case BTN_SETTINGS_USBSTORAGE_SIGN:
            JADE_ASSERT(keychain_get());
            usbstorage_sign_psbt(NULL);
            act = make_usbstorage_settings_activity(keychain_get(), ota_allowed(SOURCE_INTERNAL)); // re-create menu
            break;

        case BTN_SETTINGS_USBSTORAGE_EXPORT_XPUB:
            JADE_ASSERT(keychain_get());
            usbstorage_export_xpub(NULL);
            act = make_usbstorage_settings_activity(keychain_get(), ota_allowed(SOURCE_INTERNAL)); // re-create menu
            break;
#endif
        case BTN_SETTINGS_OTP_VIEW:
            handle_view_otps();
            break;

#ifdef CONFIG_HAS_CAMERA
        // BBB-AIRGAP: reuses the existing scan flow rather than adding a second dispatcher; an
        // epoch QR lands in handle_epoch_qr() (main/qrmode.c:2549) which reports the time it set.
        // Sets 'done' for the same reason the pinserver QR case above does: the scan is generic, so
        // a psbt or a wallet QR can also arrive here, and those screens free the managed activities
        // this loop is holding in 'act' - coming back to the OTP menu would use freed memory.  The
        // home screen rebuilds whatever is needed, and the time it set is reported before we leave.
        case BTN_SETTINGS_OTP_SET_CLOCK: {
            // BBB-AIRGAP: the address comes first, before the camera.  Every other help screen on
            // the device explains a flow the user could still complete without it; this one is the
            // flow - there is no time QR to scan until the page that draws it is open on a phone,
            // and the device said only "scan a time QR" without saying where from (Ilker, device
            // round 6).  The page is ours rather than blkstrm.com because Blockstream has no page
            // that draws a ur:jade-epoch code; source in docs/saat/index.html, served by this
            // repository's own Pages site.  The device itself never reaches it: the QR is for
            // the phone, which is the only side of this that touches a network.
            //
            // The screen is the back/continue one rather than the help one (Ilker, 2026-09-08):
            // the help screen's label reads "Learn more:", which sounds optional, and its only
            // button was a back arrow that opened the camera anyway - the arrow promised the menu
            // and delivered the scanner.  Here 'Continue' opens the camera and the arrow really
            // goes back, leaving 'done' false so the loop redraws the menu.  Nothing new is drawn
            // for this: the same screen already carries the pinserver-unlock address (line 754).
            // All three rows are the address; a label row was tried and measured, and it clipped
            // ("Open on phone:" came out as "Open on pho"), so the rows carry the address alone.
            const char* message[]
                = { PIJADE_HELP_HOST_1, PIJADE_HELP_HOST_2, PIJADE_HELP_CLOCK_PATH };
            if (!await_qr_back_continue_activity(message, 3, PIJADE_HELP_CLOCK_URL, true)) {
                // Declined before the camera opened, so the menu activities are still valid
                break;
            }
            handle_scan_qr("Clock QR", PIJADE_HELP_CLOCK_URL);
            done = true;
            break;
        }

        case BTN_SETTINGS_OTP_NEW_QR:
            register_otp_qr();
            break;
#endif

        case BTN_SETTINGS_OTP_NEW_KB:
            register_otp_kb_entry();
            break;

        case BTN_SETTINGS_PINSERVER_SHOW:
            show_pinserver_details();
            break;

#ifdef CONFIG_HAS_CAMERA
        case BTN_SETTINGS_PINSERVER_SCAN_QR:
            handle_pinserver_scan();
            break;
#endif

        case BTN_SETTINGS_PINSERVER_RESET:
            handle_pinserver_reset();
            break;

#ifdef CONFIG_BOARD_TYPE_JADE_ANY
        case BTN_SETTINGS_LEGAL:
            handle_legal();
            break;
#endif

        // Help screens
        case BTN_SETTINGS_PINSERVER_HELP:
            await_qr_help_activity("blkstrm.com/oracle");
            break;

        case BTN_SETTINGS_OTP_HELP:
            await_qr_help_activity("blkstrm.com/otp");
            break;

        default:
            // Unexpected event, just ignore
            break;
        }
    }
}

void offer_startup_options(void)
{
    const bool is_startup_menu = true;
    handle_settings(is_startup_menu);
}

// Session menu: the loaded wallet under its fingerprint, logout, or sleep/power-off
static void handle_session(void)
{
    // Only reachable with a wallet loaded: 'Session' is the Active/Unlocked home tile
    // (home_menu_items above), so the fingerprint can always be read here.
    JADE_ASSERT(keychain_get());

    // BBB-AIRGAP: one row per wallet held, not just the one in use. Every label is worked out up
    // front, so switching wallets does not leave the screen showing a stale fingerprint and this
    // loop does not have to be restarted. The list cannot go out of date while it is open: the two
    // paths that change what is held both leave immediately - Log Out returns, and Forget returns
    // rather than drawing a list that no longer describes the wallets there are.
    const size_t num_slots = keychain_slot_count();
    JADE_ASSERT(num_slots);
    JADE_ASSERT(num_slots <= MAX_SEED_SLOTS);
    // The reserved id range has to cover every wallet the table can hold
    JADE_ASSERT(MAX_SEED_SLOTS <= BTN_SESSION_SEED_LAST - BTN_SESSION_SEED_0 + 1);

    // Fingerprints in uppercase hex, the same form the home screen shows, so the user can match
    // the two screens. Copied into local buffers because the label text is copied by the builder
    // anyway (main/gui.c:1170).
    char slot_labels[MAX_SEED_SLOTS][2 * BIP32_KEY_FINGERPRINT_LEN + 1];
    list_item_t session_items[MAX_SEED_SLOTS + 2];
    size_t num_session_items = 0;

    for (size_t i = 0; i < num_slots; ++i) {
        uint8_t fingerprint[BIP32_KEY_FINGERPRINT_LEN];
        keychain_slot_fingerprint(i, fingerprint, sizeof(fingerprint));
        char* fphex = NULL;
        JADE_WALLY_VERIFY(wally_hex_from_bytes(fingerprint, sizeof(fingerprint), &fphex));
        map_string(fphex, toupper);
        const int ret = snprintf(slot_labels[i], sizeof(slot_labels[i]), "%s", fphex);
        JADE_ASSERT(ret > 0 && ret < sizeof(slot_labels[i]));
        JADE_WALLY_VERIFY(wally_free_string(fphex));

        // A filled circle for the wallet that survives Log Out, a hollow one for the wallets that
        // do not - see the symbols font (main/fonts/jade_symbols_16x16.c)
        session_items[num_session_items].txt = slot_labels[i];
        session_items[num_session_items].symbol = keychain_slot_is_temporary(i) ? "M" : "J";
        session_items[num_session_items].ev_id = BTN_SESSION_SEED_0 + i;
        ++num_session_items;
    }

    session_items[num_session_items++] = (list_item_t){ .txt = "Log Out", .ev_id = BTN_SESSION_LOGOUT };
#ifndef CONFIG_ETH_USE_OPENETH
    session_items[num_session_items++] = (list_item_t){ .txt = "Sleep", .ev_id = BTN_SESSION_SLEEP };
#endif

    // Two menus share this loop: the session list, and the wallet list that opens under a
    // fingerprint. Both are drawn by run_list_activity(), which rebuilds its screen on every call
    // and scrolls when a list outgrows the four rows the display fits.
    // The wallet menu is titled with the wallet it belongs to, which is the one just activated.
    const char* wallet_title = NULL;
    // BBB-AIRGAP: four of these rows are offered per wallet rather than always - two are optional
    // features (BIP85, Sign Message), 'Backup' needs a wallet that still has the entropy it was
    // built from, and 'Forget' a temporary one - so the rows are laid out when a wallet is picked
    // rather than trimmed from a fixed list.  See there for every condition.  Four rows always,
    // four conditional, which is what the size says.  A device built without a camera drops three
    // of the eight at compile time, so the size is the upper bound rather than the count.
    list_item_t wallet_items[4 + 4];
    size_t num_wallet_items = 0;

    // Selection is remembered per menu, so coming back from a sub-screen lands where it was left.
    bool in_wallet_menu = false;
    size_t session_selected = 0;
    size_t wallet_selected = 0;

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return;
        }
        if (in_wallet_menu && !keychain_get()) {
            return; // no wallet left to draw a menu for
        }

        const int32_t ev_id = in_wallet_menu
            ? run_list_activity(
                  wallet_title, BTN_SETTINGS_WALLET_EXIT, wallet_items, num_wallet_items, &wallet_selected)
            : run_list_activity("Session", BTN_SESSION_EXIT, session_items, num_session_items, &session_selected);

        if (ev_id >= BTN_SESSION_SEED_0 && ev_id < (int32_t)(BTN_SESSION_SEED_0 + num_slots)) {
            // Make that wallet the one in use and open its menu. Only which wallet keychain_get()
            // returns changes here; nothing is loaded or dropped. The dashboard loop this was
            // called from tests keychain_get() and so redraws itself once this returns.
            const size_t position = (size_t)(ev_id - BTN_SESSION_SEED_0);
            keychain_slot_activate(position);
            wallet_title = slot_labels[position];
            num_wallet_items = 0;

            // BBB-AIRGAP: two of the rows below are optional features the device can be set to
            // leave out (Options > Features).  Read here, where the rows
            // are laid out, rather than once per session: that screen can have changed them since
            // this menu was last opened.  Leaving a row out only removes a way in - the wallet
            // itself is untouched, and a device with both turned off signs what one with both
            // turned on signs.
            const uint8_t feature_flags = storage_get_feature_flags();

            // BBB-AIRGAP: the order is the one asked for on 2026-09-03: the two rows reached most
            // often first, then the rest by how often they are wanted, with the two that change
            // what is held (Backup, Forget) last.  Nothing about when a row appears changed with
            // the order; each condition is still the one described where it stands.
#ifdef CONFIG_HAS_CAMERA
            // BBB-AIRGAP: the same scan the home screen offers, reached from a wallet so the one
            // being scanned for is the one already in use.  sign_psbt() still checks ownership and
            // offers to switch wallets, so entering here is a convenience, not the guard.
            wallet_items[num_wallet_items++] = (list_item_t){ .txt = "Scan QR", .ev_id = BTN_SETTINGS_WALLET_SCAN_QR };
#endif
            // BBB-AIRGAP: the wallet's own addresses, derived rather than scanned, so this row
            // needs neither a camera nor the entropy the backup row below requires.
            wallet_items[num_wallet_items++]
                = (list_item_t){ .txt = "Address Explorer", .ev_id = BTN_SETTINGS_WALLET_ADDRESSES };
            wallet_items[num_wallet_items++] = (list_item_t){ .txt = "Export Xpub", .ev_id = BTN_SETTINGS_XPUB_EXPORT };
#ifdef CONFIG_HAS_CAMERA
            // BBB-AIRGAP: the same camera as the Scan QR row, narrowed to one format.  That row
            // accepts anything and decides what it was; this one signs a message with the wallet
            // whose menu it is and says so when the code is something else (handle_sign_message()).
            if (feature_flags & FEATURE_FLAGS_SIGN_MESSAGE) {
                wallet_items[num_wallet_items++]
                    = (list_item_t){ .txt = "Sign Message", .ev_id = BTN_SETTINGS_WALLET_SIGN_MSG };
            }
#endif
            wallet_items[num_wallet_items++]
                = (list_item_t){ .txt = "Registered Wallets", .ev_id = BTN_SETTINGS_REGISTERED_WALLETS };
            if (feature_flags & FEATURE_FLAGS_BIP85) {
                wallet_items[num_wallet_items++] = (list_item_t){ .txt = "BIP85", .ev_id = BTN_SETTINGS_BIP85 };
            }
#ifdef CONFIG_HAS_CAMERA
            // Every backup screen shows the words, and only a wallet whose words were presented in
            // this session can have them drawn again.  A PIN-unlocked wallet is read back from the
            // blob as a serialised key, so its words are not recoverable and the row is not offered
            // rather than failing when pressed.
            if (keychain_slot_has_entropy(position)) {
                wallet_items[num_wallet_items++]
                    = (list_item_t){ .txt = "Backup", .ev_id = BTN_SETTINGS_WALLET_BACKUP };
            }
#endif
            // Forget is offered for a temporary wallet only. Dropping the persisted one cannot be
            // undone while other wallets are held - keychain_load() refuses to read the blob back
            // while any wallet is in memory - so the way to drop that one is Log Out, which drops
            // them all together.
            if (keychain_slot_is_temporary(position)) {
                wallet_items[num_wallet_items++]
                    = (list_item_t){ .txt = "Forget", .ev_id = BTN_SETTINGS_WALLET_FORGET };
            }
            JADE_ASSERT(num_wallet_items <= sizeof(wallet_items) / sizeof(list_item_t));
            wallet_selected = 0;
            in_wallet_menu = true;
            continue;
        }

        switch (ev_id) {

        case BTN_SETTINGS_WALLET_EXIT:
            // Back to the session menu
            in_wallet_menu = false;
            break;

        case BTN_SETTINGS_XPUB_EXPORT:
            display_xpub_qr();
            break;

        case BTN_SETTINGS_REGISTERED_WALLETS:
            handle_registered_wallets();
            break;

        case BTN_SETTINGS_BIP85:
            handle_bip85_mnemonic();
            break;

        case BTN_SETTINGS_WALLET_ADDRESSES:
            // Listing addresses neither loads nor drops a wallet, so the two lists laid out at the
            // top of this function stay valid and the menu is simply redrawn on return.
            handle_address_explorer();
            break;

#ifdef CONFIG_HAS_CAMERA
        case BTN_SETTINGS_WALLET_BACKUP:
            handle_wallet_backup();
            break;

        case BTN_SETTINGS_WALLET_SIGN_MSG:
            // Signing neither loads nor drops a wallet, so unlike 'Scan QR' below the two lists
            // laid out at the top of this function stay valid and the menu is simply redrawn.
            handle_sign_message();
            break;

        case BTN_SETTINGS_WALLET_SCAN_QR:
            // BBB-AIRGAP: a scan can load a wallet into a free slot or, when the psbt names
            // another wallet, switch the one in use (main/process/sign_psbt.c).  Either invalidates
            // the two lists laid out at the top of this function - the session list would be
            // missing a wallet, and the wallet rows would name one wallet while 'Forget' and
            // 'Backup' acted on another.  Neither list can be patched up from here, so
            // return and let the home screen rebuild them, the way 'Forget' already does.
            handle_scan_qr("Scan QR", "blkstrm.com/jadescan");
            return;
#endif

        case BTN_SETTINGS_WALLET_FORGET: {
            // The wallet is named on the screen that asks, and that screen opens on 'No': within
            // this session the wallet cannot be brought back, it would have to be scanned again.
            const char* question[] = { "Forget wallet", wallet_title };
            if (!await_yesno_activity("Forget Wallet", question, 2, false, NULL)) {
                break;
            }
            keychain_slot_forget_active();
            // The list built at the top of this function no longer says what is held, so it is not
            // drawn again - the home screen redraws itself because the wallet in use changed.
            return;
        }

        case BTN_SESSION_LOGOUT:
            // Logout of current wallet, delete keychain
            keychain_clear();
            return;

        case BTN_SESSION_SLEEP:
            // BBB-AIRGAP: drop the wallets before powering down, the way the idle timer already
            // does (main/idletimer.c:267).  Upstream leaves it to the hardware: an ESP32 that
            // cuts its own supply loses SRAM, so a wipe would be belt and braces.  This port has
            // no PMU - poweroff halts the SoC but the board stays powered (see the note below) -
            // so DRAM keeps whatever was in it, and since a slot now holds the entropy the words
            // can be rebuilt from, not just the derived keys, "the device is off" would otherwise
            // be a weaker statement than it looks.
            keychain_clear();
#ifdef CONFIG_LIBJADE
            // BBB-AIRGAP: Jade hardware cuts its own power, so a dark screen is the device
            // switching off. A Pi Zero has no PMU: poweroff halts the SoC but cannot drop the
            // board's supply, and the only outward sign is the backlight going out when
            // pijade-host exits and its GPIO line is released. Without a message the user
            // cannot tell "off" from "frozen" - the confusion reported on 2026-08-28.
            //
            // Drawn through gui_destroy_current_activity() rather than
            // display_message_activity(): the latter posts the switch to the gui task and
            // returns, while power_shutdown() does not return on this port (_power_request is
            // noreturn and the host _exit()s), so the frame could die with the process. This
            // is the only public entry point that waits for the gui task, and the job signals
            // its semaphore after render_activity() (main/gui.c:2417-2439); the flush reaches
            // the panel synchronously from that same task (main/display.c:1018 ->
            // display_hw_flush() -> libjade_display_flushed()). Nothing is destroyed
            // here (first argument NULL): the menu screen belongs to run_list_activity(), and
            // nothing runs after the shutdown anyway, so only the synchronous switch is
            // wanted. Guarded because on real hardware the device powers itself off and the
            // message would be wrong.
            //
            // The message deliberately does not say when the power may be cut. A dark screen
            // is not a completed halt: on_power_request() queues poweroff.target with
            // --no-block and _exit()s as soon as systemctl accepts the job
            // (pijade/host/pijade_host.c:346-367), so the backlight goes out while systemd is
            // still stopping services and unmounting the card; the failure path aborts and
            // darkens the screen without any shutdown at all. Telling the user to cut power at
            // that point would invite a corrupted card. Producing an honest "power can be cut"
            // signal needs a unit that paints the panel after umount.target, which is a
            // separate piece of work (ROADMAP T3.9).
            //
            // A failed request does not leave this notice standing as a false "it is off":
            // on_power_request() returns, _power_request() aborts (libjade/libjade.c:196), and
            // jade_abort() paints "Internal error" over this screen and holds it for five
            // seconds before the real abort (main/jade_abort.c:20-34). Measured in the
            // emulator, where no power handler is registered: the abort screen replaced this
            // one, which is why capturing the notice needed a wait-added twin build.
            {
                const char* message[] = { "Shutting down", "", "The screen goes dark", "before shutdown ends" };
                gui_activity_t* const shutdown_act = make_show_message_activity(message, 4, NULL, NULL, 0, NULL, 0);
                gui_destroy_current_activity(NULL, shutdown_act);

                // Held on screen before the shutdown is asked for, because the request takes
                // the frame away almost at once: on_power_request() runs systemctl --no-block
                // and _exit()s as soon as the job is queued, and the backlight goes out with
                // it. Without this pause the notice would only last as long as systemctl takes
                // to start, which is not a readable interval and is not bounded by anything we
                // control. Same purpose and the same mechanism as jade_abort()'s own wait
                // before it aborts (main/jade_abort.c:31-33), shorter because nothing has gone
                // wrong here.
                vTaskDelay(3000 / portTICK_PERIOD_MS);
            }
#endif
            // Shutdown Jade
            power_shutdown();
            return;

        case BTN_SESSION_EXIT:
            return;

        default:
            break;
        }
    }
}

// Scan seedqr and log in for qr (only) mode
static bool qr_mode_scan_seedqr(void)
{
    const bool temporary_restore = true;
    const bool force_qr_scan = true;
    bool offer_qr_temporary = false; // unused - already flagged as temporary wallet
    initialise_with_mnemonic(temporary_restore, force_qr_scan, &offer_qr_temporary);
    if (!keychain_get()) {
        return false;
    }

    JADE_ASSERT(keychain_has_temporary());
    return auth_qr_mode();
}

static void handle_qr_mode(void)
{
    gui_activity_t* const act = make_connect_qrmode_activity(device_name);
    int32_t ev_id;

    bool done = false;
    while (!done) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            done = true;
            continue;
        }

        gui_set_current_activity(act);
        if (gui_activity_wait_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
            switch (ev_id) {
            case BTN_CONNECT_QR_PIN:
                done = offer_pinserver_qr_unlock();
                break;

            case BTN_CONNECT_QR_SCAN:
                done = qr_mode_scan_seedqr();
                break;

            case BTN_CONNECT_QR_HELP:
                await_qr_help_activity("blkstrm.com/qrmode");
                break;

            case BTN_CONNECT_QR_BACK:
                done = true;
                break;
            }
        }
    }
}

// Process buttons on the dashboard screen
static void handle_btn(const int32_t btn)
{
    switch (btn) {
    case BTN_INITIALIZE:
        initialise_wallet(false);
        break;

    case BTN_SCAN_SEEDQR:
        qr_mode_scan_seedqr();
        break;

    case BTN_CONNECT_TO_BACK:
        select_initial_connection(false);
        break;

    case BTN_QR_MODE:
        handle_qr_mode();
        break;

    case BTN_SESSION:
        handle_session();
        break;

    case BTN_SETTINGS:
        handle_settings(false);
        break;

    case BTN_SCAN_QR:
        handle_scan_qr("Scan QR", "blkstrm.com/jadescan");
        break;

    // The 'connect' screen
    case BTN_CONNECT:
        show_connect_screen = true;
        break;

    case BTN_CONNECT_BACK:
        show_connect_screen = false;
        break;

    case BTN_CONNECT_HELP:
        await_qr_help_activity("blkstrm.com/jadewallets");
        break;

    default:
        break;
    }
}

// Display the passed dashboard screen
static void display_screen(jade_process_t* process, gui_activity_t* act)
{
    JADE_ASSERT(process);
    JADE_ASSERT(act);

    // Print the main stack usage (high water mark), and the DRAM usage
    JADE_LOGI("Main task stack HWM: %u free", uxTaskGetStackHighWaterMark(NULL));
    JADE_LOGI("DRAM block / free: %u / %u", heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL));

    // Switch to passed screen, and at that point free all other managed activities
    // Should be no-op if we didn't switch away from this screen
    gui_set_current_activity_ex(act, true);

    // Refeed sensor entropy every time we return to dashboard screen
    const TickType_t tick_count = xTaskGetTickCount();
    refeed_entropy(&tick_count, sizeof(tick_count));

    // Ensure the correct/expected connection interface(s) are enabled
    // depending on whether the user is authenticated and on which interface.
    enable_connection_interfaces(keychain_get_userdata());
}

// Display the dashboard ready or welcome screen.  Await messages or user GUI input.
static void do_dashboard(jade_process_t* process, const keychain_t* const initial_keychain, const bool initial_has_pin,
    gui_activity_t* act_dashboard, wait_event_data_t* event_data)
{
    JADE_ASSERT(process);
    JADE_ASSERT(act_dashboard);
    JADE_ASSERT(event_data);

    // Loop all the time the keychain is unchanged, awaiting either a message
    // from companion app or a GUI interaction from the user
    bool acted = true;
    const uint8_t initial_userdata = keychain_get_userdata();
    const jade_msg_source_t initial_connection_selection = initialisation_source;
    const bool initial_show_connect_screen = show_connect_screen;

    while (keychain_get() == initial_keychain && keychain_has_pin() == initial_has_pin
        && keychain_get_userdata() == initial_userdata && initial_show_connect_screen == show_connect_screen
        && initialisation_source == initial_connection_selection) {
        // If the last loop did something, ensure the current dashboard screen
        // is displayed. (Doing this too eagerly can either cause unnecessary
        // screen flicker or can cause the dashboard to overwrite other screens
        // eg. BLE pairing/bonding confirm screen.)
        if (acted) {
            display_screen(process, act_dashboard);
        }

        // Fresh iteration
        acted = false;

        // BBB-AIRGAP: the escape has arrived where it was going, so it stops here.  Cleared every
        // time round rather than once, because the loop is re-entered from every screen the
        // dashboard opens and the flag has to be down before the next one is shown.
        gui_escape_clear();

        // 1. Process any message if available (do not block if no message available)
        jade_process_load_in_message(process, false);
        if (process->ctx.cbor) {
            main_thread_action = MAIN_THREAD_ACTIVITY_MESSAGE;
            dispatch_message(process);
            acted = true;
        }

        // 2. Process any outstanding GUI event if we didn't process a message (again, don't block)
        const char* ev_base;
        int32_t ev_id;
        if (!acted) {
            if (sync_wait_event(event_data, &ev_base, &ev_id, NULL, 100 / portTICK_PERIOD_MS) == ESP_OK) {
                if (show_connect_screen && ev_base == GUI_BUTTON_EVENT) {
                    // Normal button press from some other home-like screen
                    // (eg. connect/connect-to screens etc)
                    main_thread_action = MAIN_THREAD_ACTIVITY_UI_MENU;
                    handle_btn(ev_id);
                    acted = true;
                } else if (ev_base == GUI_EVENT) {
                    // Low-level gui event from the generic home screen
                    const size_t nbtns = sizeof(home_menu_items[0]) / sizeof(home_menu_items[0][0]);
                    const home_menu_item_t* menu_item = NULL;
                    if (ev_id == GUI_WHEEL_LEFT_EVENT) {
                        // Back, but skip over any unused menu-item entries (null text)
                        do {
                            home_screen_menu_item = (home_screen_menu_item + nbtns - 1) % nbtns;
                            menu_item = get_selected_home_screen_menu_item(NULL);
                        } while (!menu_item->text);
                        update_home_screen_menu();
                    } else if (ev_id == GUI_WHEEL_RIGHT_EVENT) {
                        // Next, but skip over any unused menu-item entries (null text)
                        do {
                            home_screen_menu_item = (home_screen_menu_item + 1) % nbtns;
                            menu_item = get_selected_home_screen_menu_item(NULL);
                        } while (!menu_item->text);
                        update_home_screen_menu();
                    } else if (ev_id == gui_get_click_event()) {
                        // Click - handle the current button's event
                        main_thread_action = MAIN_THREAD_ACTIVITY_UI_MENU;
                        menu_item = get_selected_home_screen_menu_item(NULL);
                        handle_btn(menu_item->btn_id);
                        acted = true;
                    }
                }
            }
        }

        if (acted) {
            // Cleanup anything attached to the dashboard process
            cleanup_jade_process(process);

            // Assert all sensitive memory was zero'd
            sensitive_assert_empty();

            // Set activity flag back to idle
            main_thread_action = MAIN_THREAD_ACTIVITY_NONE;
        }

        // Ensure to clear any decrypted keychain if in-use ble- or usb- connection lost.
        // Allow serial to be plugged even if we're not unlocked over serial for eg. charging.
        // NOTE: if this clears a populated keychain then this loop will complete
        // and cause this function to return.
        // NOTE: only applies to a *peristed* keychain - ie if we have a pin set, and *NOT*
        // if this is a temporary/emergency-restore wallet.
        // BBB-AIRGAP: upstream puts this question to the wallet in use, because only one wallet
        // could be held.  With the slot table a temporary wallet loaded on top would answer in
        // place of the persisted one and this guard would stop firing, leaving a PIN-unlocked seed
        // in memory after the connection that unlocked it went away.  The question is about the
        // table, so it is put to the table.  The action stays keychain_clear(): once the session
        // the PIN opened is over, no wallet may remain.
        const uint8_t persisted_userdata = keychain_get_persisted_userdata();
        if (initial_has_pin) {
            if ((persisted_userdata == SOURCE_SERIAL && !tolerate_usb_disconnection && !usb_is_powered())
                || (persisted_userdata == SOURCE_BLE && !ble_connected())) {
                JADE_LOGI("Connection lost - clearing keychain");
                keychain_clear();
            }
        }
    }
}

#define UPDATE_HOME_SCREEN(screen_type)                                                                                \
    do {                                                                                                               \
        home_screen_type = screen_type;                                                                                \
        home_screen_menu_item = 0;                                                                                     \
        update_home_screen(status_light, status_text, label);                                                          \
        update_home_screen_menu();                                                                                     \
    } while (false)

// Main/default screen/process when ready for user interaction
void dashboard_process(void* process_ptr)
{
    JADE_LOGI("Starting: %d", xPortGetFreeHeapSize());

    jade_process_t* process = process_ptr;
    ASSERT_NO_CURRENT_MESSAGE(process);

    // At startup we expect no keychain
    JADE_ASSERT(!keychain_get());

    // Populate the static fields about the unit/fw
    device_name = get_jade_id();
    JADE_ASSERT(device_name);

    // NOTE: Create 'Ready' screen for when Jade is unlocked and ready to use early, so that
    // it does not fragment the RAM (since it is long-lived).
    // NOTE: The main home screen is created as an 'unmanaged' activity, so it is not placed
    // in the list of activities to be freed by 'set_current_activity_ex()' calls.
    // This is desirable as this screen is never freed and lives as long as the application.

    // NOTE: the menu nodes are static, so we can update the menu displayed when the user scrolls
    gui_view_node_t* status_light = NULL;
    gui_view_node_t* status_text = NULL;
    gui_view_node_t* label = NULL;
    gui_activity_t* const act_home = make_home_screen_activity(device_name, running_app_info.version,
        &home_screen_selected_entry, &home_screen_next_entry, &status_light, &status_text, &label);
    JADE_ASSERT(home_screen_selected_entry.symbol);
    JADE_ASSERT(home_screen_selected_entry.text);
    JADE_ASSERT(home_screen_next_entry.symbol);
    JADE_ASSERT(home_screen_next_entry.text);
    JADE_ASSERT(status_light);
    JADE_ASSERT(status_text);
    JADE_ASSERT(label);

    // We may as well associate the long-lived event data with this activity also
    wait_event_data_t* const event_data = gui_activity_make_wait_event_data(act_home);
    JADE_ASSERT(event_data);

    // Register for all events on the home screen
    gui_activity_register_event(act_home, GUI_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);

    while (true) {
        // Create/set current 'dashboard' screen, then process all events until that
        // dashboard is no longer appropriate - ie. until the keychain is set (or unset).
        // We have six cases:
        // 1. Ready - has keys already associated with a message source
        //    - ready screen  (created early and persistent, see above)
        // 2. Awaiting QR intialisation - this is a special case of either 3. or 4. below
        //    - just show 'Processing...' screen while we await QR client task
        // 3. Unused keys - has keys in memory, but not yet connected to an app
        //    - connect-to screen
        // 4. Locked - has persisted/encrypted keys, but no keys in memory
        //    - welcome-back screen
        // 5. Connect - as above, but user has clicked into the explanatory 'connect' screen
        //    - connect screen
        // 6. Uninitialised - has no persisted/encrypted keys and no keys in memory
        //    - setup screen
        gui_activity_t* act_dashboard = NULL;
        const bool has_pin = keychain_has_pin();
        const keychain_t* initial_keychain = keychain_get();

        if (awaiting_attestation_data()) {
            // Blank screen while awaiting attestation data upload
            act_dashboard = gui_make_activity();
        } else if (show_connect_screen) {
            // Some sort of connection is in progress
            if (initialisation_source == SOURCE_INTERNAL) {
                JADE_LOGI("Awaiting QR initialisation");
                act_dashboard = display_processing_message_activity();
            } else if (initial_keychain) {
                JADE_LOGI("Wallet/keys initialised but not yet saved/authed - showing Connect-To screen");
                act_dashboard = make_connect_to_activity(device_name, initialisation_source);
                gui_activity_register_event(
                    act_dashboard, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);
            } else {
                JADE_LOGI("User navigated to 'connect' screen");
                act_dashboard = make_connect_activity();
                gui_activity_register_event(
                    act_dashboard, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);
            }
        } else {
            // Show home screen
            initialisation_source = SOURCE_NONE; // Not mid-initialisation
            if (initial_keychain) {
                JADE_ASSERT(keychain_get_userdata() != SOURCE_NONE);
                JADE_LOGI("Connected and have wallet/keys - showing home screen/Active");
                UPDATE_HOME_SCREEN(HOME_SCREEN_TYPE_ACTIVE);
            } else if (has_pin) {
                JADE_LOGI("Wallet/keys pin set but not yet loaded - showing home screen/Initialised");
                UPDATE_HOME_SCREEN(HOME_SCREEN_TYPE_LOCKED);
            } else {
                JADE_LOGI("No wallet/keys and no pin set - showing home screen/Uninitialised");
                UPDATE_HOME_SCREEN(HOME_SCREEN_TYPE_UNINIT);
            }
            act_dashboard = act_home;
        }

        // This call loops/blocks all the time the user keychain (and related details)
        // remains unchanged.  When it changes we go back round this loop setting
        // a new 'dashboard' screen and re-running the dashboard processing loop.
        // NOTE: connecting or disconnecting serial or ble will cause any keys to
        // be cleared (and bzero'd).
        do_dashboard(process, initial_keychain, has_pin, act_dashboard, event_data);
    }
}
#endif // AMALGAMATED_BUILD
