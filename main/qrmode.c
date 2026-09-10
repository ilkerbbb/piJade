#ifndef AMALGAMATED_BUILD
#include "qrmode.h"

#include "bcur.h"
#include "descriptor_text.h"
#include "button_events.h"
#include "descriptor.h"
#include "gui.h"
#include "idletimer.h"
#include "jade_assert.h"
#include "jade_tasks.h"
#include "jade_wally_verify.h"
#include "keychain.h"
#include "miner.h"
#include "multisig.h"
#include "otpauth.h"
#include "process.h"
#include "qrcode.h"
#include "sensitive.h"
#include "storage.h"
#include "ui.h"
#include "utils/address.h"
#include "utils/malloc_ext.h"
#include "utils/network.h"
#include "utils/util.h"
#include "wallet.h"

#include <wally_script.h>

#include <string.h>
#include <time.h>

#ifdef CONFIG_LIBJADE
#include <pthread.h> // BBB-AIRGAP: pthread_cleanup_push around the mining loop, see handle_mining_qr
#endif

#define MAX_QR_V2_DATA_LEN 32
#define MAX_QR_V4_DATA_LEN 78
#define MAX_QR_V6_DATA_LEN 134

#define ACCOUNT_INDEX_MAX 65536
#define ACCOUNT_INDEX_FLAGS_SHIFT 16

#define MAX_OTP_SCREENS 1
#define OTP_TEXTSPLITLEN 4
#define OTP_GRID_TOPPAD 4
#define OTP_GRID_X 4
#define OTP_GRID_Y 6
#define OTP_GRID_SIZE (OTP_GRID_X * OTP_GRID_Y)
// When we are displaying a BCUR QR code we ensure the timeout is at least this value
// as we don't want the unit to shut down because of apparent inactivity.
#define BCUR_QR_DISPLAY_MIN_TIMEOUT_SECS 300

// QR 'version' (ie size) used to display bcur codes
// NOTE: we can scale up more on larger screens - note paradoxically the larger screen
// uses a smaller 'largest' version, as this scales v.nicely to the screen extents.
// (tbh ver12 on the smaller screen is v difficult to scan but is the next scaling-to-fit)
#define QR_VER_LOW 4
#define QR_VER_MID 6
// BBB-AIRGAP: xpub payloads build into a 128-byte cbor buffer, small enough for a version-3 code.
// Upstream's ladder starts at 4 because it is shared with psbts, which are far larger. A version-3
// code is 29 modules against version 4's 33, which on this 240x240 panel is 7 px per module rather
// than 6 - measured to be the difference between a webcam-grade scanner reading the code and
// stalling on it. See pijade/UPSTREAM.md.
#define QR_VER_XPUB_LOW 3
#if CONFIG_DISPLAY_WIDTH >= 320 && CONFIG_DISPLAY_HEIGHT >= 170
#define QR_VER_HIGH 9
#else
#define QR_VER_HIGH 12
#endif

gui_activity_t* make_show_qr_help_activity(const char* url, Icon* qr_icon);
gui_activity_t* make_qr_back_continue_activity(
    const char* message[], size_t message_size, const char* url, Icon* qr_icon, bool default_selection);

gui_activity_t* make_show_xpub_qr_activity(const char* label, const char* pathstr);
gui_activity_t* make_fullscreen_qr_activity(Icon* icons, size_t num_icons, size_t frames_per_qr_icon);
gui_activity_t* make_xpub_qr_options_activity(
    gui_view_node_t** script_textbox, gui_view_node_t** wallet_textbox, gui_view_node_t** density_textbox);

gui_activity_t* make_show_otp_qr_actvity(const char* otp_name, Icon* qr_icon);

gui_activity_t* make_search_verify_address_activity(
    const char* root_label, gui_view_node_t** label_text, progress_bar_t* progress_bar, gui_view_node_t** index_text);
gui_activity_t* make_search_address_options_activity(bool show_script, bool show_account, bool show_change,
    gui_view_node_t** script_textbox, gui_view_node_t** account_textbox, gui_view_node_t** change_textbox);

gui_activity_t* make_show_qr_activity(const char* message[], size_t message_size, bool show_options_button);
gui_activity_t* make_qr_options_activity(gui_view_node_t** density_textbox, gui_view_node_t** framerate_textbox);
gui_activity_t* make_mining_menu_activity(void);

bool import_mnemonic(const uint8_t* bytes, size_t bytes_len, char* buf, size_t buf_len, size_t* written);
int register_otp_string(const char* otp_uri, size_t uri_len, const char** errmsg);
int register_otp_migrate_string(const char* otp_uri, size_t uri_len, const char** errmsg);
int register_multisig_file(const char* multisig_file, size_t multisig_file_len, const char** errmsg);
int register_descriptor_text(const char* text, size_t text_len, const char** errmsg);
int update_pinserver(const CborValue* const params, const char** errmsg);
int params_set_epoch_time(CborValue* params, const char** errmsg);
int sign_message_file(
    const char* str, size_t str_len, uint8_t* sig_output, size_t sig_len, size_t* written, const char** errmsg);
int get_bip85_bip39_entropy_cbor(const CborValue* params, CborEncoder* output, const char** errmsg);

bool show_confirm_address_activity(const char* address, bool default_selection);
gui_activity_t* make_display_address_activities(const char* title, bool show_one_screen_tick, const char* address,
    bool default_selection, gui_activity_t** actaddr2);

bool handle_mnemonic_qr(const char* mnemonic);

bool select_registered_wallet(const char multisig_names[][NVS_KEY_NAME_MAX_SIZE], size_t num_multisigs,
    const bool* owned_multisigs, const char descriptor_names[][NVS_KEY_NAME_MAX_SIZE], size_t num_descriptors,
    const bool* owned_descriptors, const char** wallet_name_out, bool* is_multisig);

// PSBT struct and functions
struct wally_psbt;
network_t network_from_psbt_type(struct wally_psbt* psbt);
int sign_psbt(
    jade_process_t* process, CborValue* params, network_t network_id, struct wally_psbt* psbt, const char** errmsg);
int wally_psbt_free(struct wally_psbt* psbt);

#define EXPORT_XPUB_PATH_LEN 4

#define ADDRESS_SEARCH_BATCH_SIZE(registered_wallet) (registered_wallet ? 10 : 20)
#define NUM_BATCHES_TO_RECONFIRM(registered_wallet) (registered_wallet ? 20 : 25)
#define NUM_INDEXES_TO_RECONFIRM(registered_wallet)                                                                    \
    (NUM_BATCHES_TO_RECONFIRM(registered_wallet) * ADDRESS_SEARCH_BATCH_SIZE(registered_wallet))

// Test whether 'flags' contains the entirety of the 'test_flags'
// (ie. maybe compound/multiple bits set)
static inline bool contains_flags(const uint32_t flags, const uint32_t test_flags)
{
    return (flags & test_flags) == test_flags;
}

// Rotate through: low -> high -> high|low -> low -> high ...
// 'unset' treated as 'high' (ie. the middle value)
static void rotate_flags(uint32_t* flags, const uint32_t high, const uint32_t low)
{
    JADE_ASSERT(flags);

    if (contains_flags(*flags, high | low)) {
        *flags &= ~high;
    } else if (contains_flags(*flags, high)) {
        *flags |= low;
    } else if (contains_flags(*flags, low)) {
        *flags ^= (high | low);
    } else { // ie. currently 0/default/uninitialised - treat as 'high'
        *flags |= (high | low);
    }
}

// Rotate scripttype flags
// Legacy -> wrapped segwit -> segwit (v0) -> taproot (segwit v1) -> legacy ...
// BBB-AIRGAP: 'allow_taproot' is false for a multisig wallet.  Taproot is a singlesig script in
// this screen's vocabulary - xpub_script_variant_from_flags() answers P2TR before it looks at the
// multisig bit - so a multisig wallet has to step over that position rather than land on it.
// Clearing the bit afterwards instead would leave the carousel looking stuck, because the position
// before taproot and taproot-with-the-bit-dropped both read as 'Native Segwit'.
static void rotate_scripttypes(uint32_t* flags, const bool reverse, const bool allow_taproot)
{
    JADE_ASSERT(flags);

    if (reverse) {
        if (contains_flags(*flags, QR_XPUB_LEGACY | QR_XPUB_WITNESS)) {
            *flags &= ~QR_XPUB_WITNESS;
        } else if (contains_flags(*flags, QR_XPUB_LEGACY)) {
            *flags ^= (QR_XPUB_LEGACY | QR_XPUB_TAPROOT);
        } else if (contains_flags(*flags, QR_XPUB_TAPROOT)) {
            *flags ^= (QR_XPUB_WITNESS | QR_XPUB_TAPROOT);
        } else if (contains_flags(*flags, QR_XPUB_WITNESS)) {
            *flags |= QR_XPUB_LEGACY;
        } else { // ie. currently 0/default/uninitialised - treat as 'segwit v0'
            *flags |= (QR_XPUB_LEGACY | QR_XPUB_WITNESS);
        }
    } else {
        if (contains_flags(*flags, QR_XPUB_LEGACY | QR_XPUB_WITNESS)) {
            *flags &= ~QR_XPUB_LEGACY;
        } else if (contains_flags(*flags, QR_XPUB_WITNESS)) {
            *flags ^= (QR_XPUB_WITNESS | QR_XPUB_TAPROOT);
        } else if (contains_flags(*flags, QR_XPUB_TAPROOT)) {
            *flags ^= (QR_XPUB_LEGACY | QR_XPUB_TAPROOT);
        } else if (contains_flags(*flags, QR_XPUB_LEGACY)) {
            *flags |= QR_XPUB_WITNESS;
        } else { // ie. currently 0/default/uninitialised - treat as 'segwit v0'
            *flags |= QR_XPUB_TAPROOT;
        }
    }

    // One more step if this landed on taproot and taproot is not on offer.  The step after taproot
    // is never taproot again in either direction, so this recurses once at most.
    if (!allow_taproot && contains_flags(*flags, QR_XPUB_TAPROOT)) {
        rotate_scripttypes(flags, reverse, true);
    }
}

// BBB-AIRGAP: upstream reads 'neither bit set' as the middle value, so an untouched device gets
// medium density and the medium frame rate. Neither is reliably scannable off this panel, so an
// untouched device is read as Low for both - menu labels included, so what the options screen says
// matches what the panel draws. An explicit Medium choice survives this untouched: rotate_flags()
// encodes Medium as the HIGH bit alone, which is not the empty state.
static uint32_t qr_flags_with_defaults(uint32_t qr_flags)
{
    if (!(qr_flags & (QR_DENSITY_LOW | QR_DENSITY_HIGH))) {
        qr_flags |= QR_DENSITY_LOW;
    }
    if (!(qr_flags & (QR_SPEED_LOW | QR_SPEED_HIGH))) {
        qr_flags |= QR_SPEED_LOW;
    }
    return qr_flags;
}

static uint8_t qr_framerate_from_flags(const uint32_t qr_flags)
{
    // Frame periods around 800ms, 450ms, 270ms  (see GUI_TARGET_FRAMERATE)
    // Frame rates: HIGH|LOW > HIGH > LOW ...
    // unset/default is treated as 'high' (ie. the middle value)
    return contains_flags(qr_flags, QR_SPEED_HIGH | QR_SPEED_LOW) ? 4 : contains_flags(qr_flags, QR_SPEED_LOW) ? 12 : 7;
}
static const char* qr_framerate_desc_from_flags(const uint32_t qr_flags)
{
    // unset/default is treated as 'high' (ie. the middle value)
    return contains_flags(qr_flags, QR_SPEED_HIGH | QR_SPEED_LOW) ? "High"
        : contains_flags(qr_flags, QR_SPEED_LOW)                  ? "Low"
                                                                  : "Medium";
}

static uint8_t qr_version_from_flags(const uint32_t qr_flags)
{
    // QR versions 12, 6 and 4 fit well on the Jade screen with scaling of
    // 2 px-per-cell, 3 px-per-cell, and 4 px-per-cell respectively.
    // Version/Size/Density: HIGH|LOW > HIGH > LOW ... 0 implies unset/default
    // unset/default is treated as 'high' (ie. the middle value)
    return contains_flags(qr_flags, QR_DENSITY_HIGH | QR_DENSITY_LOW) ? QR_VER_HIGH
        : contains_flags(qr_flags, QR_DENSITY_LOW)                    ? QR_VER_LOW
                                                                      : QR_VER_MID;
}
// BBB-AIRGAP: the same three-way density choice, mapped onto the versions an xpub can use:
// Low -> 3 (29 modules), Medium -> 4 (33), High -> 6 (41). Kept beside qr_version_from_flags()
// rather than replacing it, because psbts cannot use version 3 - their payload overruns its
// 77-character capacity, see BCUR_FRAGMENT_SIZE_V3 in main/bcur.c.
static uint8_t xpub_qr_version_from_flags(const uint32_t qr_flags)
{
    return contains_flags(qr_flags, QR_DENSITY_HIGH | QR_DENSITY_LOW) ? QR_VER_MID
        : contains_flags(qr_flags, QR_DENSITY_LOW)                    ? QR_VER_XPUB_LOW
                                                                      : QR_VER_LOW;
}

static const char* qr_density_desc_from_flags(const uint32_t qr_flags)
{
    // unset/default is treated as 'high' (ie. the middle value)
    return contains_flags(qr_flags, QR_DENSITY_HIGH | QR_DENSITY_LOW) ? "High"
        : contains_flags(qr_flags, QR_DENSITY_LOW)                    ? "Low"
                                                                      : "Medium";
}

// We support native segwit and p2sh-wrapped segwit, singlesig and multisig
script_variant_t xpub_script_variant_from_flags(const uint32_t qr_flags)
{
    // unset/default is treated as 'high' (ie. the middle value)
    if (contains_flags(qr_flags, QR_XPUB_TAPROOT)) {
        return P2TR;
    }
    if (contains_flags(qr_flags, QR_XPUB_MULTISIG)) {
        return contains_flags(qr_flags, QR_XPUB_WITNESS | QR_XPUB_LEGACY) ? MULTI_P2WSH_P2SH
            : contains_flags(qr_flags, QR_XPUB_LEGACY)                    ? MULTI_P2SH
                                                                          : MULTI_P2WSH;
    }
    return contains_flags(qr_flags, QR_XPUB_WITNESS | QR_XPUB_LEGACY) ? P2WPKH_P2SH
        : contains_flags(qr_flags, QR_XPUB_LEGACY)                    ? P2PKH
                                                                      : P2WPKH;
}
static inline const char* xpub_scripttype_desc_from_flags(const uint32_t qr_flags)
{
    // unset/default is treated as 'high' (ie. the middle value)
    return contains_flags(qr_flags, QR_XPUB_WITNESS | QR_XPUB_LEGACY) ? "Wrapped Segwit"
        : contains_flags(qr_flags, QR_XPUB_LEGACY)                    ? "Legacy"
        : contains_flags(qr_flags, QR_XPUB_TAPROOT)                   ? "Taproot"
                                                                      : "Native Segwit";
}
static inline const char* xpub_wallettype_desc_from_flags(const uint32_t qr_flags)
{
    // unset/default is treated as singlesig
    return contains_flags(qr_flags, QR_XPUB_MULTISIG) ? "Multisig" : "Singlesig";
}

// BBB-AIRGAP: show the passed icons on a screen of their own and wait for a click, then return to
// 'prev_act'. The activity takes ownership of the icons and frees them when destroyed, so callers
// build the icons afresh each time - which also means they always match the current qr options.
// 'processing_act' is optional - when the caller showed a 'processing' screen while building the
// icons, pass it here so it is freed as the code takes over rather than left on the activity list.
static void display_fullscreen_qr(gui_activity_t* const prev_act, gui_activity_t* const processing_act, Icon* icons,
    const size_t num_icons, const size_t frames_per_qr)
{
    JADE_ASSERT(prev_act);
    JADE_ASSERT(icons);
    JADE_ASSERT(num_icons);

    gui_activity_t* const act = make_fullscreen_qr_activity(icons, num_icons, frames_per_qr);
    if (processing_act) {
        gui_destroy_current_activity(processing_act, act);
    } else {
        gui_set_current_activity(act);
    }

    // The whole screen is a single button, so any click is the exit
    gui_activity_wait_button(act, BTN_QR_FULLSCREEN_EXIT);
#ifdef CONFIG_DEBUG_UNATTENDED_CI
    // BBB-AIRGAP: that wait returns after a millisecond under CI, before the gui task has drawn
    // the code; hold the screen for several frames so an unattended walkthrough captures it
    vTaskDelay(500 / portTICK_PERIOD_MS);
#endif

    gui_destroy_current_activity(act, prev_act);
}

// BBB-AIRGAP: the screens that describe an export wait for a button through this rather than
// gui_activity_wait_button() directly. Under CONFIG_DEBUG_UNATTENDED_CI that call returns the
// default event straight away, so an unattended walkthrough would leave the export without ever
// drawing the code; here it asks for the code on the first pass and exits on the second.
int32_t wait_export_screen_button(gui_activity_t* const act, const int32_t exit_ev_id, bool* const code_shown)
{
    JADE_ASSERT(code_shown);

    const int32_t ev_id = gui_activity_wait_button(act, exit_ev_id);
#ifdef CONFIG_DEBUG_UNATTENDED_CI
    if (!*code_shown) {
        *code_shown = true;
        return BTN_QR_SHOW_FULLSCREEN;
    }
#endif
    return ev_id;
}

// Deduce path based on script type and main/test network restrictions
static size_t xpub_export_path_from_flags(const uint32_t qr_flags, uint32_t* path, const size_t path_size)
{
    JADE_ASSERT(path);

    const script_variant_t script_variant = xpub_script_variant_from_flags(qr_flags);
    const uint16_t account_index = qr_flags >> ACCOUNT_INDEX_FLAGS_SHIFT;
    size_t path_len = 0;
    wallet_get_default_xpub_export_path(script_variant, account_index, path, path_size, &path_len);
    return path_len;
}

// BBB-AIRGAP: the screen that describes the export - the code itself is a screen further in
static gui_activity_t* create_display_xpub_qr_activity(const uint32_t qr_flags)
{
    uint32_t path[EXPORT_XPUB_PATH_LEN]; // 3 or 4 - purpose'/cointype'/account'/[multisig bip48 script type']
    const size_t path_len = xpub_export_path_from_flags(qr_flags, path, EXPORT_XPUB_PATH_LEN);

    char pathstr[MAX_PATH_STR_LEN(EXPORT_XPUB_PATH_LEN)];
    const bool path_only = false;
    const bool ret = wallet_bip32_path_as_str(path, path_len, pathstr, sizeof(pathstr), path_only);
    JADE_ASSERT(ret);

    const char* label = contains_flags(qr_flags, QR_XPUB_MULTISIG) ? "Multisig" : "Singlesig";
    return make_show_xpub_qr_activity(label, pathstr);
}

// BBB-AIRGAP: build the xpub code for the current options and show it full screen
static void display_xpub_fullscreen_qr(gui_activity_t* const prev_act, const uint32_t qr_flags)
{
    const bool use_format_hdkey = false; // qr_flags & QR_XPUB_HDKEY;  - not currently in use
    const char* const xpub_qr_format = use_format_hdkey ? BCUR_TYPE_CRYPTO_HDKEY : BCUR_TYPE_CRYPTO_ACCOUNT;

    uint32_t path[EXPORT_XPUB_PATH_LEN];
    const size_t path_len = xpub_export_path_from_flags(qr_flags, path, EXPORT_XPUB_PATH_LEN);

    // Construct BC-UR CBOR message for 'crypto-account' or 'crypto-hdkey' bcur
    uint8_t cbor[128];
    size_t written = 0;
    if (use_format_hdkey) {
        bcur_build_cbor_crypto_hdkey(path, path_len, cbor, sizeof(cbor), &written);
    } else {
        bcur_build_cbor_crypto_account(
            xpub_script_variant_from_flags(qr_flags), path, path_len, cbor, sizeof(cbor), &written);
    }

    // Map BCUR cbor into a series of QR-code icons
    Icon* icons = NULL;
    size_t num_icons = 0;
    // BBB-AIRGAP: was pinned to low density and the slow frame rate, which meant the density and
    // speed options in the qr menu changed psbt codes but not this one. Both now follow the user's
    // setting; qr_flags_with_defaults() is what keeps an untouched device on the readable end.
    const uint8_t qrcode_version = xpub_qr_version_from_flags(qr_flags);
    bcur_create_qr_icons(cbor, written, xpub_qr_format, qrcode_version, &icons, &num_icons);

    const uint8_t frames_per_qr = qr_framerate_from_flags(qr_flags);
    display_fullscreen_qr(prev_act, NULL, icons, num_icons, frames_per_qr);
}

// BBB-AIRGAP: defined further down, beside the psbt flow it was written for
static bool handle_qr_options(uint32_t* qr_flags, const char* help_url);

// BBB-AIRGAP: narrow a stored wallet type to one this device still offers, so what is exported is
// what the screen says.  Taproot goes with the multisig pin: xpub_script_variant_from_flags()
// answers P2TR before it looks at the multisig bit, so a stored taproot script would otherwise
// survive the pin and be exported as an m/86' singlesig key under a Multisig label.  The two
// feature flags cannot both be off - handle_wallet_options() refuses the press that would do it -
// so one branch always applies when the device is restricted.  Returns whether it changed
// anything, ie. whether the type on screen is the device's doing rather than the user's.
static bool pin_wallettype_to_features(uint32_t* qr_flags, const uint8_t feature_flags)
{
    JADE_ASSERT(qr_flags);

    if (!(feature_flags & FEATURE_FLAGS_MULTISIG)) {
        *qr_flags &= ~QR_XPUB_MULTISIG;
        return true;
    }
    if (!(feature_flags & FEATURE_FLAGS_SINGLESIG)) {
        *qr_flags |= QR_XPUB_MULTISIG;
        *qr_flags &= ~QR_XPUB_TAPROOT;
        return true;
    }
    return false;
}

bool handle_xpub_options(uint32_t* qr_flags, bool for_descriptor)
{
    JADE_ASSERT(qr_flags);

    // BBB-AIRGAP: these two bits cannot stand together - xpub_script_variant_from_flags() answers
    // P2TR before it looks at the multisig bit (main/qrmode.c:251-257), so a stored pair would be
    // labelled Multisig and exported as an m/86' singlesig key.  Every screen that can set either
    // bit already keeps them apart; the assertions below are on what this function persists, which
    // is the one place that assembles flags out of two different visits.
    const uint32_t incompatible_flags = QR_XPUB_MULTISIG | QR_XPUB_TAPROOT;

    // BBB-AIRGAP: what was stored before this screen narrowed anything, and before anything it
    // opens can write.  The QR settings screen reached from here persists whatever flags it is
    // handed (handle_qr_options() below), so by the time this function ends the stored flags can
    // already carry a type the user never picked.  Everything put back below comes from this, not
    // from a re-read.
    const uint32_t stored_on_entry = storage_get_qr_flags();

    if (for_descriptor) {
        *qr_flags &= ~QR_XPUB_MULTISIG; // Disallow multisig
    }

    // BBB-AIRGAP: a device set to offer only one wallet type has nothing to choose between, so the
    // type is pinned and the 'Wallet' row stops responding - the same shape as the descriptor case
    // above, which pins singlesig for its own reason.  The descriptor case is left out of the pin
    // itself, because it has already narrowed the flags and the export that called it asserts the
    // multisig bit is clear (main/usbhmsc/usbmode.c:938); putting that bit back would abort the
    // export.  It still counts as pinned below, because its type is no more the user's choice.
    const uint8_t feature_flags = storage_get_feature_flags();
    const bool wallettype_pinned = for_descriptor || pin_wallettype_to_features(qr_flags, feature_flags);
    const bool offer_wallettype
        = !for_descriptor && (feature_flags & FEATURE_FLAGS_SINGLESIG) && (feature_flags & FEATURE_FLAGS_MULTISIG);

    uint16_t account_index = (*qr_flags) >> ACCOUNT_INDEX_FLAGS_SHIFT;

    char buf[8];
    int rc = snprintf(buf, sizeof(buf), "%u", account_index);
    JADE_ASSERT(rc > 0 && rc < sizeof(buf));

    gui_view_node_t* script_item = NULL;
    gui_view_node_t* wallet_item = NULL;
    gui_view_node_t* account_item = NULL;
    gui_activity_t* const act = make_xpub_qr_options_activity(&script_item, &wallet_item, &account_item);
    update_menu_item(script_item, "Script", xpub_scripttype_desc_from_flags(*qr_flags));
    update_menu_item(wallet_item, "Wallet", xpub_wallettype_desc_from_flags(*qr_flags));
    update_menu_item(account_item, "Account Index", buf);
    gui_set_current_activity(act);

    gui_view_node_t* script_textbox = NULL;
    gui_activity_t* const act_scripttype = make_carousel_activity("Script Type", NULL, &script_textbox);
    gui_update_text(script_textbox, xpub_scripttype_desc_from_flags(*qr_flags));

    gui_view_node_t* wallet_textbox = NULL;
    gui_activity_t* const act_wallettype = make_carousel_activity("Wallet Type", NULL, &wallet_textbox);
    gui_update_text(wallet_textbox, xpub_wallettype_desc_from_flags(*qr_flags));

    digit_entry_t digit_entry = { .entry_type = DIGIT_ENTRY_INDEX, .initial_state = ZERO, .digits_shown = true };
    make_digit_entry_activity(&digit_entry, "Account Index", "Enter index:");
    JADE_ASSERT(digit_entry.activity);

    const uint32_t initial_flags = *qr_flags;
    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen, and what was turned on the wheel but never
        // confirmed goes back with it.  Without the restore the code below still sees changed
        // flags and writes them to storage, so leaving would persist a selection the user did
        // not accept; putting the entry value back routes into the 'nothing changed' branch.
        if (gui_escape_pending()) {
            *qr_flags = initial_flags;
            break;
        }

        // Show, and await button click
        gui_set_current_activity(act);

        int32_t ev_id = gui_activity_wait_button(act, BTN_XPUB_OPTIONS_EXIT);
        if (ev_id == BTN_XPUB_OPTIONS_SCRIPTTYPE) {
            gui_set_current_activity(act_scripttype);
            while (true) {
                // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
                if (gui_escape_pending()) {
                    break;
                }

                gui_update_text(script_textbox, xpub_scripttype_desc_from_flags(*qr_flags));
                if (gui_activity_wait_event(act_scripttype, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
                    const bool allow_taproot = !contains_flags(*qr_flags, QR_XPUB_MULTISIG);
                    if (ev_id == GUI_WHEEL_LEFT_EVENT) {
                        rotate_scripttypes(qr_flags, true, allow_taproot);
                    } else if (ev_id == GUI_WHEEL_RIGHT_EVENT) {
                        rotate_scripttypes(qr_flags, false, allow_taproot);
                    } else if (ev_id == gui_get_click_event()) {
                        // Done
                        break;
                    }
                }
            }
            update_menu_item(script_item, "Script", xpub_scripttype_desc_from_flags(*qr_flags));
        } else if (offer_wallettype && ev_id == BTN_XPUB_OPTIONS_WALLETTYPE) {
            gui_set_current_activity(act_wallettype);
            while (true) {
                // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
                if (gui_escape_pending()) {
                    break;
                }

                gui_update_text(wallet_textbox, xpub_wallettype_desc_from_flags(*qr_flags));
                if (gui_activity_wait_event(act_wallettype, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
                    if (ev_id == GUI_WHEEL_LEFT_EVENT || ev_id == GUI_WHEEL_RIGHT_EVENT) {
                        *qr_flags ^= QR_XPUB_MULTISIG; // toggle
                        if (contains_flags(*qr_flags, QR_XPUB_MULTISIG)) {
                            // The two bits cannot both be set - see rotate_scripttypes().  The
                            // script row is redrawn below because this can have changed it.
                            *qr_flags &= ~QR_XPUB_TAPROOT;
                        }
                    } else if (ev_id == gui_get_click_event()) {
                        // Done
                        break;
                    }
                }
            }
            update_menu_item(script_item, "Script", xpub_scripttype_desc_from_flags(*qr_flags));
            update_menu_item(wallet_item, "Wallet", xpub_wallettype_desc_from_flags(*qr_flags));
        } else if (ev_id == BTN_XPUB_OPTIONS_ACCOUNT) {

            while (true) {
                // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
                if (gui_escape_pending()) {
                    break;
                }
                reset_digit_entry(&digit_entry, NULL);
                gui_set_current_activity(digit_entry.activity);
                if (!run_digit_entry_loop(&digit_entry)) {
                    // User abandoned index entry
                    break;
                }

                // Get entered digits as single numeric value
                const uint32_t new_account_index = get_entry_as_number(&digit_entry);
                if (new_account_index < ACCOUNT_INDEX_MAX) {
                    account_index = new_account_index;

                    // Update the display
                    const int ret = snprintf(buf, sizeof(buf), "%u", account_index);
                    JADE_ASSERT(ret > 0 && ret < sizeof(buf));
                    update_menu_item(account_item, "Account Index", buf);
                    break;
                } else {
                    // Show message and retry
                    const int ret = snprintf(buf, sizeof(buf), "%u", ACCOUNT_INDEX_MAX);
                    JADE_ASSERT(ret > 0 && ret < sizeof(buf));
                    await_error_3("Account index must", "be less than", buf);
                }
            }
        } else if (ev_id == BTN_XPUB_OPTIONS_QR) {
            // BBB-AIRGAP: this screen is shared with the one under Settings, and the usb export
            // path reaches handle_xpub_options() with flags read straight from storage
            // (main/usbhmsc/usbmode.c). Read them through the same defaults the display uses, or a
            // device that has never visited the menu would be labelled Medium here while drawing
            // the Low code.
            uint32_t shared_qr_flags = qr_flags_with_defaults(*qr_flags);
            handle_qr_options(&shared_qr_flags, "blkstrm.com/qrmode");
            *qr_flags = shared_qr_flags;
            gui_set_current_activity(act);
        } else if (ev_id == BTN_XPUB_OPTIONS_HELP) {
            await_qr_help_activity("blkstrm.com/xpub");
        } else if (ev_id == BTN_XPUB_OPTIONS_EXIT) {
            // Done
            break;
        }
    }

    // If updated, persist prefereces
    *qr_flags = (uint16_t)(*qr_flags);
    *qr_flags |= (((uint32_t)account_index) << ACCOUNT_INDEX_FLAGS_SHIFT);
    if (initial_flags == *qr_flags) {
        // Nothing was chosen here, but the QR settings screen may still have written the pinned
        // flags out on its way through.  Undo that, and only that.
        if (wallettype_pinned && storage_get_qr_flags() != stored_on_entry) {
            JADE_ASSERT(!contains_flags(stored_on_entry, incompatible_flags));
            storage_set_qr_flags(stored_on_entry);
        }
        return false;
    }

    // BBB-AIRGAP: the wallet type pinned on the way in is how this screen had to behave, not a
    // choice the user made here, so it is kept out of what gets written back: turning the feature
    // on again must find the type last picked, not the one the pin forced.  The caller still gets
    // the pinned value in *qr_flags, because that is the code it is about to draw.
    uint32_t flags_to_store = *qr_flags;
    if (wallettype_pinned) {
        const uint32_t stored = stored_on_entry;
        const uint32_t script_bits = QR_XPUB_TAPROOT | QR_XPUB_WITNESS | QR_XPUB_LEGACY;
        const bool script_changed = ((*qr_flags ^ initial_flags) & script_bits) != 0;

        // BBB-AIRGAP: taproot is a singlesig script in this screen's vocabulary, so choosing it
        // here is also an explicit wallet-type choice.  Keep that latest choice instead of silently
        // restoring an older multisig preference on top of it.  Other scripts work with either type.
        if (!(script_changed && contains_flags(*qr_flags, QR_XPUB_TAPROOT))) {
            flags_to_store = (flags_to_store & ~QR_XPUB_MULTISIG) | (stored & QR_XPUB_MULTISIG);
        }

        // The pin can also change the script.  If the user did not choose one on this visit, put
        // the stored script back whole; restoring only taproot could combine it with other bits.
        if (!script_changed) {
            flags_to_store = (flags_to_store & ~script_bits) | (stored & script_bits);
        }
    }

    // Return to indicate if any options were updated
    JADE_ASSERT(!contains_flags(flags_to_store, incompatible_flags));
    storage_set_qr_flags(flags_to_store);
    return true;
}

// BBB-AIRGAP: the density and speed settings apply to every wallet code the device builds - xpub
// export, psbt output, wallet output - so they belong in the device settings tree, not only on the
// screens that happen to be showing a code. Upstream opens this screen from the psbt flow alone.
// handle_qr_options() writes the choice out itself, so there is nothing to persist here.
void handle_qr_settings(void)
{
    uint32_t qr_flags = qr_flags_with_defaults(storage_get_qr_flags());
    handle_qr_options(&qr_flags, "blkstrm.com/qrmode");
}

// Display xpub qr code
void display_xpub_qr(void)
{
    uint32_t qr_flags = qr_flags_with_defaults(storage_get_qr_flags());

    // BBB-AIRGAP: same narrowing the options screen does, and for the same reason - what this
    // screen is labelled with has to be what it exports.  It is not enough to do it in
    // handle_xpub_options(): the user reaches the code from here without necessarily opening that.
    const uint8_t feature_flags = storage_get_feature_flags();
    pin_wallettype_to_features(&qr_flags, feature_flags);

    // BBB-AIRGAP: the screen the loop below runs describes what is about to be exported - type,
    // derivation path - and the code itself is a screen further in.  A device set to skip that
    // description goes straight to the code and never builds the description at all: on the way out
    // display_fullscreen_qr() puts back whatever screen it was handed (main/qrmode.c, its closing
    // gui_destroy_current_activity()), so handing it a description screen would flash up the very
    // screen this setting exists to keep off the display - measured, not guessed.  The code returns
    // to the menu it was opened from instead, which is where this function returns to anyway.
    if (!(feature_flags & FEATURE_FLAGS_XPUB_DETAILS)) {
        display_xpub_fullscreen_qr(gui_current_activity(), qr_flags);
        return;
    }

    // Create show xpub activity for those icons
    gui_activity_t* act = create_display_xpub_qr_activity(qr_flags);
    bool code_shown = false;

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            break;
        }
        // Show, and await button click
        gui_set_current_activity(act);

        const int32_t ev_id = wait_export_screen_button(act, BTN_XPUB_EXIT, &code_shown);
        if (ev_id == BTN_QR_SHOW_FULLSCREEN) {
            display_xpub_fullscreen_qr(act, qr_flags);
        } else if (ev_id == BTN_XPUB_OPTIONS) {
            const bool for_descriptor = false;
            if (handle_xpub_options(&qr_flags, for_descriptor)) {
                // Options were updated - re-create xpub screen
                act = create_display_xpub_qr_activity(qr_flags);
            }
        } else if (ev_id == BTN_QR_BRIGHTNESS) {
            gui_next_qrcode_color();
            gui_repaint(act->root_node);
        } else if (ev_id == BTN_XPUB_EXIT) {
            // Done
            break;
        }
    }
}

// Helper to get user to select and load registered wallet record.
// BBB-AIRGAP: 'script_type' is optional - the verify flow knows the script type of the address it
// is checking and passes it to narrow the choice, the address explorer has no address yet and
// passes NULL to offer every record.  'raw_name_out' is optional too: 'name_out' comes back as
// '<name>/0' because the search-root helpers below use it as the screen label and rewrite its last
// character, but descriptor_to_address() wants the record name on its own.
static bool load_registered_wallet(const size_t* script_type, char* name_out, const size_t name_out_len,
    char* raw_name_out, const size_t raw_name_out_len, multisig_data_t** multisig_data,
    descriptor_data_t** descriptor)
{
    JADE_ASSERT(name_out);
    JADE_ASSERT(name_out_len > NVS_KEY_NAME_MAX_SIZE);
    // 'name_out' needs the two extra bytes for the "/0" appended below; 'raw_name_out' takes
    // the record name unchanged, so a buffer of exactly NVS_KEY_NAME_MAX_SIZE holds it.
    JADE_ASSERT(!raw_name_out || raw_name_out_len >= NVS_KEY_NAME_MAX_SIZE);
    JADE_INIT_OUT_PPTR(multisig_data);
    JADE_INIT_OUT_PPTR(descriptor);

    // Could be multisig/descriptor - offer choice of wallet records
    char multisig_names[MAX_MULTISIG_REGISTRATIONS][NVS_KEY_NAME_MAX_SIZE]; // Sufficient
    const size_t num_multisig_names = sizeof(multisig_names) / sizeof(multisig_names[0]);
    size_t num_multisigs = 0;
    multisig_get_valid_record_names(script_type, multisig_names, num_multisig_names, &num_multisigs);

    char descriptor_names[MAX_DESCRIPTOR_REGISTRATIONS][NVS_KEY_NAME_MAX_SIZE]; // Sufficient
    const size_t num_descriptor_names = sizeof(descriptor_names) / sizeof(descriptor_names[0]);
    size_t num_descriptors = 0;
    descriptor_get_valid_record_names(descriptor_names, num_descriptor_names, &num_descriptors);

    if (!num_multisigs && !num_descriptors) {
        // No valid records
        return false;
    }

    bool is_multisig = false;
    const char* wallet_name = NULL;
    // BBB-AIRGAP: no ownership flags - both lists above are already filtered to the records this
    // wallet can read, so there is no row here that could be marked as another wallet's.
    const bool* const all_owned = NULL;
    if (!select_registered_wallet(multisig_names, num_multisigs, all_owned, descriptor_names, num_descriptors,
            all_owned, &wallet_name, &is_multisig)
        || !wallet_name) {
        // No wallet selected
        return false;
    }

    // Copy the selected name (assume external address for now - is updated later)
    const int ret = snprintf(name_out, name_out_len, "%s/0", wallet_name);
    JADE_ASSERT(ret > 0 && ret < name_out_len);

    if (raw_name_out) {
        const int raw_ret = snprintf(raw_name_out, raw_name_out_len, "%s", wallet_name);
        JADE_ASSERT(raw_ret > 0 && raw_ret < raw_name_out_len);
    }

    // Load the selected wallet
    const char* errmsg = NULL;
    if (is_multisig) {
        multisig_data_t* const allocated = JADE_MALLOC(sizeof(multisig_data_t));
        if (!multisig_load_from_storage(wallet_name, allocated, NULL, 0, NULL, &errmsg)) {
            await_error("Failed to load multisig record");
            free(allocated);
            return false;
        }
        *multisig_data = allocated;
    } else {
        descriptor_data_t* const allocated = JADE_MALLOC(sizeof(descriptor_data_t));
        if (!descriptor_load_from_storage(wallet_name, allocated, &errmsg)) {
            await_error("Failed to load descriptor record");
            free(allocated);
            return false;
        }
        *descriptor = allocated;
    }
    return true;
}

// Get search root for singlesig address
static void get_singlesig_search_root(const script_variant_t variant, const uint16_t account_index,
    const bool is_change, char* pathstr, const size_t pathstr_len, struct ext_key* search_roots,
    const size_t search_roots_len)
{
    JADE_ASSERT(pathstr);
    JADE_ASSERT(pathstr_len);
    JADE_ASSERT(search_roots);
    JADE_ASSERT(search_roots_len == 1);

    // Get the path to search
    size_t path_len = 0;
    uint32_t path[EXPORT_XPUB_PATH_LEN];
    wallet_get_default_xpub_export_path(variant, account_index, path, EXPORT_XPUB_PATH_LEN, &path_len);
    JADE_ASSERT(path_len == EXPORT_XPUB_PATH_LEN - 1);
    path[path_len++] = is_change ? 1 : 0; // set change indicator

    // Get as hdkey
    bool ret = wallet_get_hdkey(path, path_len, BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, search_roots);
    JADE_ASSERT(ret);

    // Use the root bip32 path as the label
    const bool path_only = false;
    ret = wallet_bip32_path_as_str(path, path_len, pathstr, pathstr_len, path_only);
    JADE_ASSERT(ret);
}

// Get search root for multisig address
static void get_multisig_search_roots(const multisig_data_t* multisig_data, const bool is_change, char* pathstr,
    const size_t pathstr_len, struct ext_key* search_roots, const size_t search_roots_len)
{
    JADE_ASSERT(multisig_data);
    JADE_ASSERT(pathstr);
    JADE_ASSERT(pathstr_len);
    JADE_ASSERT(search_roots);
    JADE_ASSERT(search_roots_len);
    JADE_ASSERT(search_roots_len == multisig_data->num_xpubs);

    // Derive set of multisig parent keys
    const uint32_t path[] = { is_change ? 1 : 0 }; // set change indicator
    for (int i = 0; i < search_roots_len; ++i) {
        const uint8_t* xpub = multisig_data->xpubs + (i * BIP32_SERIALIZED_LEN);
        const bool ret = wallet_derive_pubkey(
            xpub, BIP32_SERIALIZED_LEN, path, 1, BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, &search_roots[i]);
        JADE_ASSERT(ret);
    }

    // Use the existing name plus the change indicator as the label
    const size_t len = strlen(pathstr);
    JADE_ASSERT(len < pathstr_len);
    JADE_ASSERT(pathstr[len - 1] == '0' || pathstr[len - 1] == '1');
    pathstr[len - 1] = is_change ? '1' : '0';
}

// Get search root for descriptor address
static void get_descriptor_search_roots(const descriptor_data_t* descriptor, const bool is_change, char* pathstr,
    const size_t pathstr_len, struct ext_key* search_roots, const size_t search_roots_len)
{
    JADE_ASSERT(descriptor);
    JADE_ASSERT(pathstr);
    JADE_ASSERT(pathstr_len);
    JADE_ASSERT(!search_roots);
    JADE_ASSERT(!search_roots_len);

    // NOTE: No keys to derive as all included in descriptor data

    // Use the existing name plus the change indicator as the label
    const size_t len = strlen(pathstr);
    JADE_ASSERT(len < pathstr_len);
    JADE_ASSERT(pathstr[len - 1] == '0' || pathstr[len - 1] == '1');
    pathstr[len - 1] = is_change ? '1' : '0';
}

// BBB-AIRGAP: 'script_flags' is optional.  The verify flow reads the script type off the address
// being checked and so passes NULL; the address explorer has no such address and has to be told,
// so it passes the same script-type bits the xpub export uses (see xpub_script_variant_from_flags)
// and gets a row to turn them with.
static bool handle_address_options(
    const bool show_account, uint16_t* account_index, bool* is_change, uint32_t* script_flags)
{
    JADE_ASSERT(account_index || !show_account);
    // BBB-AIRGAP: is_change is optional now, on the same terms as script_flags.  A caller that
    // has already settled the branch - the address explorer's entry menu - passes NULL, and the
    // row is left off rather than shown and then overwritten on the way out.
    // script_flags is optional

    const bool show_script = script_flags;
    const bool show_change = is_change;

    // Create the 'options' screens
    char buf[8];
    gui_view_node_t* script_item = NULL;
    gui_view_node_t* account_item = NULL;
    gui_view_node_t* change_item = NULL;
    gui_activity_t* const act_options
        = make_search_address_options_activity(
            show_script, show_account, show_change, &script_item, &account_item, &change_item);
    JADE_ASSERT(!account_item == !show_account);
    JADE_ASSERT(!script_item == !show_script);
    JADE_ASSERT(!change_item == !show_change);

    if (script_item) {
        update_menu_item(script_item, "Script", xpub_scripttype_desc_from_flags(*script_flags));
    }
    if (account_item) {
        const int ret = snprintf(buf, sizeof(buf), "%u", *account_index);
        JADE_ASSERT(ret > 0 && ret < sizeof(buf));
        update_menu_item(account_item, "Account Index", buf);
    }
    if (change_item) {
        update_menu_item(change_item, "Change", *is_change ? "Yes" : "No");
    }

    // BBB-AIRGAP: one registration held for the whole screen, rather than the one
    // gui_activity_wait_event() would make and throw away on every iteration.  That call builds a
    // fresh wait_event_data and a fresh registration each time (main/gui.c), so a press arriving
    // between two turns of this loop is signalled to the previous semaphore and lost, leaving the
    // screen drawn with nothing waiting on it.  The activity owns the registration and the data,
    // so switching away removes the handler before the data is freed.  Only GUI_BUTTON_EVENT is
    // registered: the menu moves its own selection in response to the wheel (select_action() in
    // main/gui.c) and a press posts exactly one button event here.
    wait_event_data_t* const event_data = gui_activity_make_wait_event_data(act_options);
    JADE_ASSERT(event_data);
    gui_activity_register_event(act_options, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);

    // BBB-AIRGAP: that registration takes GUI_BUTTON_EVENT only, so KEY3 (which posts
    // GUI_EVENT / GUI_ALT_EVENT) never woke this wait and the escape check at the head of the
    // loop below could not be reached while the screen was waiting.  A second registration on
    // the same wait data wakes it; the wait reports the base as well, because the two event id
    // spaces overlap and GUI_ALT_EVENT is also a button id (BTN_QR_BRIGHTNESS).
    gui_activity_register_event(act_options, GUI_EVENT, GUI_ALT_EVENT, sync_wait_event_handler, event_data);

    // The script-type carousel, built up-front so its registration is held for as long as it is
    // used - it is turned repeatedly, which is exactly where a dropped event would show.
    gui_view_node_t* script_textbox = NULL;
    gui_activity_t* act_scripttype = NULL;
    wait_event_data_t* scripttype_event_data = NULL;
    if (show_script) {
        act_scripttype = make_carousel_activity("Script Type", NULL, &script_textbox);
        scripttype_event_data = gui_activity_make_wait_event_data(act_scripttype);
        JADE_ASSERT(scripttype_event_data);
        // The carousel holds no selectable button, so a click posts only its GUI_EVENT and each
        // input produces a single dispatch into this data.
        gui_activity_register_event(
            act_scripttype, GUI_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, scripttype_event_data);
    }

    const bool initial_change = show_change && *is_change;
    const uint16_t initial_account = show_account ? *account_index : 0;
    const uint32_t initial_script_flags = show_script ? *script_flags : 0;
    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            goto cancelled;
        }

        int32_t ev_id;
        // BBB-AIRGAP: make the permanent options handler live before waiting.  After the script
        // carousel or account entry returns, an asynchronous switch leaves the outgoing handler
        // active briefly; a fast next press is delivered there and never reaches this wait.  The
        // options activity listens only for GUI_BUTTON_EVENT, so the paired GUI_EVENT click from
        // the screen that opened it cannot become a stale activation and needs no drain here.
        gui_set_current_activity_sync(act_options, false);
        esp_event_base_t ev_base = NULL;
        if (sync_wait_event(event_data, &ev_base, &ev_id, NULL, 0) == ESP_OK) {
            // BBB-AIRGAP: the escape arrives on the GUI_EVENT base; it is answered before any
            // button id is compared, because the ids overlap.
            if (ev_base == GUI_EVENT && ev_id == GUI_ALT_EVENT) {
                goto cancelled;
            }

            if (ev_id == BTN_SCAN_ADDRESS_OPTIONS_SCRIPTTYPE && show_script) {
                // Switch synchronously, then discard what is left of the press that opened this
                // carousel.  A single press posts the menu's GUI_BUTTON_EVENT via select_action()
                // and then, unconditionally, its own GUI_EVENT click (main/gui.c:2556-2573); the
                // registration above takes any GUI_EVENT, so that second half would be read here
                // as the click that closes the carousel, before the user had turned it.  The
                // asynchronous gui_set_current_activity() cannot be drained against, as it only
                // queues the switch: the handlers go live later, on the gui task.  Same pair of
                // calls, and the same 10ms idle timeout, as run_list_activity()
                // (main/ui/dialogs.c).
                gui_set_current_activity_sync(act_scripttype, false);
                while (sync_wait_event(scripttype_event_data, NULL, NULL, NULL, 10 / portTICK_PERIOD_MS) == ESP_OK) {
                    // discard - see comment above
                }
                while (true) {
                    // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
                    if (gui_escape_pending()) {
                        break;
                    }

                    gui_update_text(script_textbox, xpub_scripttype_desc_from_flags(*script_flags));
                    int32_t carousel_ev_id;
                    if (sync_wait_event(scripttype_event_data, NULL, &carousel_ev_id, NULL, 0) == ESP_OK) {
                        // Singlesig flow: taproot is one of the choices here.
                        if (carousel_ev_id == GUI_WHEEL_LEFT_EVENT) {
                            rotate_scripttypes(script_flags, true, true);
                        } else if (carousel_ev_id == GUI_WHEEL_RIGHT_EVENT) {
                            rotate_scripttypes(script_flags, false, true);
                        } else if (carousel_ev_id == gui_get_click_event()) {
                            // Done
                            break;
                        }
                    }
                }
                update_menu_item(script_item, "Script", xpub_scripttype_desc_from_flags(*script_flags));
            } else if (ev_id == BTN_SCAN_ADDRESS_OPTIONS_ACCOUNT && show_account) {
                digit_entry_t digit_entry
                    = { .entry_type = DIGIT_ENTRY_INDEX, .initial_state = ZERO, .digits_shown = true };
                make_digit_entry_activity(&digit_entry, "Account Index", "Enter index:");
                JADE_ASSERT(digit_entry.activity);

                while (true) {
                    // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
                    if (gui_escape_pending()) {
                        break;
                    }
                    gui_set_current_activity(digit_entry.activity);
                    if (!run_digit_entry_loop(&digit_entry)) {
                        // User abandoned index entry
                        break;
                    }

                    // Get entered digits as single numeric value
                    uint32_t new_account_index = get_entry_as_number(&digit_entry);
                    if (new_account_index < ACCOUNT_INDEX_MAX) {
                        *account_index = new_account_index;

                        // Update the display
                        const int ret = snprintf(buf, sizeof(buf), "%u", *account_index);
                        JADE_ASSERT(ret > 0 && ret < sizeof(buf));
                        update_menu_item(account_item, "Account Index", buf);

                        break;
                    } else {
                        await_message_2("Invalid index", "Please try again");
                    }
                }
            } else if (ev_id == BTN_SCAN_ADDRESS_OPTIONS_CHANGE && show_change) {
                // Simple toggle
                *is_change = !*is_change;
                update_menu_item(change_item, "Change", *is_change ? "Yes" : "No");
            } else if (ev_id == BTN_SCAN_ADDRESS_OPTIONS_EXIT) {
                // Exit options screen
                break;
            }
        }
    }

    // Return value indicates whether options were changed
    return (show_change && *is_change != initial_change) || (show_account && *account_index != initial_account)
        || (show_script && *script_flags != initial_script_flags);

cancelled:
    // BBB-AIRGAP: an unconfirmed script/account preview must not be returned as a change,
    // which makes verify_address() derive new search roots after the user asked to leave.
    if (show_change) {
        *is_change = initial_change;
    }
    if (show_account) {
        *account_index = initial_account;
    }
    if (show_script) {
        *script_flags = initial_script_flags;
    }
    return false;
}
// Verify an address string by brute-forcing
static bool verify_address(const address_data_t* const addr_data)
{
    JADE_ASSERT(addr_data);
    JADE_ASSERT(addr_data->network_id != NETWORK_NONE);
    JADE_ASSERT(addr_data->script_len);

    const bool default_selection = true;
    if (!show_confirm_address_activity(addr_data->address, default_selection)) {
        // Abandon
        return false;
    }

    // check network - eg. testnet address, but this jade is setup for mainnet only
    if (!keychain_is_network_id_consistent(addr_data->network_id)) {
        await_error("Network type inconsistent");
        return false;
    }

    // Get the script type
    size_t script_type = 0;
    if (wally_scriptpubkey_get_type(addr_data->script, addr_data->script_len, &script_type) != WALLY_OK) {
        await_error("Failed to parse scriptpubkey");
        return false;
    }

    char label[MAX_PATH_STR_LEN(EXPORT_XPUB_PATH_LEN)];
    script_variant_t variant = GREEN;
    uint16_t account_index = 0;
    descriptor_data_t* descriptor = NULL;
    multisig_data_t* multisig_data = NULL;
    bool is_change = false;
    struct ext_key* search_roots = NULL;
    size_t search_roots_len = 0;

    // If it is (or might be) multisig, ask the user to select one, and load details
    if (script_type == WALLY_SCRIPT_TYPE_P2SH || script_type == WALLY_SCRIPT_TYPE_P2WSH) {
        // p2sh-wrapped could be multi- or single- sig.  User to select which.
        const char* question[] = { "Are you trying to", "verify a registered", "wallet address?" };
        if (script_type != WALLY_SCRIPT_TYPE_P2SH || await_yesno_activity("Verify Address", question, 3, false, NULL)) {

            // Must have a multisig or descriptor record - user to select
            // NOTE: Use wallet name as ui label
            if (!load_registered_wallet(&script_type, label, sizeof(label), NULL, 0, &multisig_data, &descriptor)) {
                JADE_ASSERT(!multisig_data && !descriptor);
                // BBB-AIRGAP: leaving the picker is not a missing-registration error screen.
                if (gui_escape_pending()) {
                    return false;
                }
                JADE_LOGE("No relevant wallet records found/selected for address");
                await_error_3("Register wallet record", "before attempting to", "verify address");
                return false;
            }
            JADE_ASSERT(!multisig_data != !descriptor); // Must be one or the other

            if (multisig_data) {
                // Calculate the key search roots (ie. up to the final leaf)
                search_roots_len = multisig_data->num_xpubs;
                search_roots = JADE_CALLOC(search_roots_len, sizeof(struct ext_key));
                get_multisig_search_roots(
                    multisig_data, is_change, label, sizeof(label), search_roots, search_roots_len);
            } else {
                // Not actually any search roots - but updates label string
                get_descriptor_search_roots(
                    descriptor, is_change, label, sizeof(label), search_roots, search_roots_len);
            }
        }
    }

    // If not multisig or descriptor, must be singlesig
    const bool registered_wallet = multisig_data || descriptor;
    if (!registered_wallet) {
        JADE_ASSERT(!search_roots);
        JADE_ASSERT(!search_roots_len);

        // BBB-AIRGAP: KEY3 on the registered-wallet question is not 'No, derive singlesig'.
        if (gui_escape_pending()) {
            return false;
        }
        if (!get_singlesig_variant_from_script_type(script_type, &variant) || variant == GREEN) {
            await_error("Address scriptpubkey unsupported");
            return false;
        }

        // Default search root account to the last exported xpub
        const uint32_t qr_flags = storage_get_qr_flags();
        account_index = qr_flags >> ACCOUNT_INDEX_FLAGS_SHIFT;

        // Calculate the key search root (ie. up to the final leaf)
        search_roots_len = 1;
        search_roots = JADE_CALLOC(search_roots_len, sizeof(struct ext_key));
        get_singlesig_search_root(
            variant, account_index, is_change, label, sizeof(label), search_roots, search_roots_len);
    }

    // Create the main search progress screen
    gui_view_node_t* label_text = NULL;
    gui_view_node_t* index_text = NULL;
    progress_bar_t progress_bar = {};
    gui_activity_t* const act = make_search_verify_address_activity(label, &label_text, &progress_bar, &index_text);
    JADE_ASSERT(label_text);
    JADE_ASSERT(index_text);

    // Make an event-data structure to track events - attached to the activity
    wait_event_data_t* const event_data = gui_activity_make_wait_event_data(act);
    JADE_ASSERT(event_data);

    // ... and register against the activity - we will await btn events later
    gui_activity_register_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);

    size_t index = 0;
    size_t confirmed_at_index = index;
    bool verified = false;
    const size_t address_search_batch_size = ADDRESS_SEARCH_BATCH_SIZE(registered_wallet);
    const size_t num_indexes_to_reconfirm = NUM_INDEXES_TO_RECONFIRM(registered_wallet);
    while (!verified) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            break;
        }

        gui_set_current_activity(act);

        // Update the progress bar and text label
        char idx_txt[12];
        const int ret = snprintf(idx_txt, sizeof(idx_txt), "%u", index);
        JADE_ASSERT(ret > 0 && ret < sizeof(idx_txt));
        update_progress_bar(&progress_bar, num_indexes_to_reconfirm, index - confirmed_at_index);
        gui_update_text(index_text, idx_txt);

        // Search a small batch of paths for the address script
        // NOTE: 'index' is updated as we go along
        if (multisig_data) {
            JADE_ASSERT(search_roots);
            JADE_ASSERT(search_roots_len);
            verified = wallet_search_for_multisig_script(multisig_data->variant, multisig_data->sorted,
                multisig_data->threshold, search_roots, search_roots_len, &index, address_search_batch_size,
                addr_data->script, addr_data->script_len);
        } else if (descriptor) {
            JADE_ASSERT(!search_roots);
            JADE_ASSERT(!search_roots_len);
            const uint32_t multi_index = is_change ? 1 : 0;
            verified = wallet_search_for_descriptor_script(addr_data->network_id, label, descriptor, multi_index,
                &index, address_search_batch_size, addr_data->script, addr_data->script_len);
        } else {
            JADE_ASSERT(search_roots);
            JADE_ASSERT(search_roots_len == 1);
            JADE_ASSERT(variant != GREEN);
            verified = wallet_search_for_singlesig_script(addr_data->network_id, variant, &search_roots[0], &index,
                address_search_batch_size, addr_data->script, addr_data->script_len);
        }

        if (verified) {
            // Address script found and matched - verified
            // NOTE: 'index' will hold the relevant value
            JADE_LOGI("Found script at index: %u", index);
            break;
        }

        // Every so often suggest to user that they might want to abandon the search
        if (index >= confirmed_at_index + num_indexes_to_reconfirm) {
            char next_n_addrs[32];
            const int ret
                = snprintf(next_n_addrs, sizeof(next_n_addrs), "next %u addresses?", num_indexes_to_reconfirm);
            JADE_ASSERT(ret > 0 && ret < sizeof(next_n_addrs));

            const char* message[] = { "Failed to verify, check", next_n_addrs };
            if (!await_yesno_activity("Verify Address", message, 2, true, "blkstrm.com/scanaddress")) {
                // Abandon - exit loop
                break;
            }
            confirmed_at_index = index;
        } else {
            // Giver user a chance to exit or to skip this batch of addresses
            int32_t ev_id = 0;
#ifndef CONFIG_DEBUG_UNATTENDED_CI
            const bool ret = sync_wait_event(event_data, NULL, &ev_id, NULL, 10 / portTICK_PERIOD_MS) == ESP_OK;
#else
            sync_wait_event(event_data, NULL, NULL, NULL, CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS / portTICK_PERIOD_MS);
            const bool ret = index > 4 * num_indexes_to_reconfirm; // let it run for a few batches, then exit
            ev_id = BTN_SCAN_ADDRESS_EXIT;
#endif
            if (ret) {
                if (ev_id == BTN_SCAN_ADDRESS_SKIP_ADDRESSES) {
                    // Jump to end of this batch
                    index = confirmed_at_index + num_indexes_to_reconfirm;
                    confirmed_at_index = index;
                } else if (ev_id == BTN_SCAN_ADDRESS_OPTIONS) {
                    if (handle_address_options(!registered_wallet, &account_index, &is_change, NULL)) {
                        // Recreate the search root(s) and update the screen label
                        if (multisig_data) {
                            get_multisig_search_roots(
                                multisig_data, is_change, label, sizeof(label), search_roots, search_roots_len);
                        } else if (descriptor) {
                            get_descriptor_search_roots(
                                descriptor, is_change, label, sizeof(label), search_roots, search_roots_len);
                        } else {
                            get_singlesig_search_root(variant, account_index, is_change, label, sizeof(label),
                                search_roots, search_roots_len);
                        }
                        gui_update_text(label_text, label);

                        // Restart search from index 0
                        confirmed_at_index = 0;
                        index = 0;
                    }
                } else if (ev_id == BTN_SCAN_ADDRESS_EXIT) {
                    // Abandon - exit loop
                    break;
                }
            }
        }
    }

    // BBB-AIRGAP: KEY3 cancels the search, not its verification result; skip the result screen
    // and still free the search roots and registration data below.
    if (gui_escape_pending()) {
        verified = false;
    } else if (verified) {
        char pathstr[48];
        const int ret = snprintf(pathstr, sizeof(pathstr), "%s/%u", label, index);
        JADE_ASSERT(ret > 0 && ret < sizeof(pathstr));
        await_message_2("Address verified:", pathstr);
    } else {
        await_error("Address NOT verified!");
    }

    // Free any allocated data
    free(search_roots);
    free(multisig_data);
    free(descriptor);

    return verified;
}

// BBB-AIRGAP: the address explorer.  SeedSigner lists a wallet's own addresses so one can be read
// off the screen and checked against what a watch-only wallet is showing, without having to scan
// that wallet's address first.  Every derivation below is one verify_address() already runs; the
// difference is that the results are listed rather than compared against a scanned address.
#define ADDR_EXPLORER_PAGE_SIZE 10

// A list row is not wide enough for even the shortest address, so the middle is elided.  What is
// kept is what a user compares by eye: the leading characters that name the network and script
// type, and the trailing ones.
#define ADDR_LABEL_HEAD 6
#define ADDR_LABEL_TAIL 6
#define ADDR_LABEL_LEN 32

static void make_address_row_label(const size_t index, const char* address, char* output, const size_t output_len)
{
    JADE_ASSERT(address);
    JADE_ASSERT(output);

    const size_t addrlen = strlen(address);
    JADE_ASSERT(addrlen > ADDR_LABEL_HEAD + ADDR_LABEL_TAIL);

    const int ret = snprintf(output, output_len, "%u: %.*s..%s", (unsigned)index, ADDR_LABEL_HEAD, address,
        address + addrlen - ADDR_LABEL_TAIL);
    JADE_ASSERT(ret > 0 && ret < output_len);
}

// The address in full, on the screens the verify flow already uses for one: they wrap the string
// over as many lines as it takes and roll onto a second screen when it does not fit, which the
// three fixed lines of make_show_qr_activity() cannot do - a taproot address would need 21
// characters a line and run off this 240px panel.  The tick opens the code.
static void show_address_detail(const size_t index, const char* address)
{
    JADE_ASSERT(address);

    char title[24];
    const int ret = snprintf(title, sizeof(title), "Address %u", (unsigned)index);
    JADE_ASSERT(ret > 0 && ret < sizeof(title));

    const bool show_tick = true;
    const bool default_selection = true;
    gui_activity_t* act2 = NULL;
    gui_activity_t* const act1 = make_display_address_activities(title, show_tick, address, default_selection, &act2);

    gui_activity_t* act = act1;
#ifdef CONFIG_DEBUG_UNATTENDED_CI
    // Declared with its use below, so a build without CI does not carry an unused variable
    bool code_shown = false;
#endif
    while (true) {
        // BBB-AIRGAP: the address QR consumes KEY3; the detail view must carry it to the list.
        if (gui_escape_pending()) {
            return;
        }
        gui_set_current_activity(act);

        int32_t ev_id = gui_activity_wait_button(act, BTN_ADDRESS_REJECT);
#ifdef CONFIG_DEBUG_UNATTENDED_CI
        // BBB-AIRGAP: that wait returns the default straight away under CI, so an unattended
        // walkthrough would leave without the code ever being drawn - and the code is what such a
        // walkthrough can actually read back.  Ask for it on the first pass, leave on the second.
        // Same reasoning as wait_export_screen_button(), which the export screens use.
        if (!code_shown) {
            code_shown = true;
            ev_id = BTN_ADDRESS_ACCEPT;
        }
#endif
        switch (ev_id) {
        case BTN_ADDRESS_NEXT:
            act = act2;
            break;

        case BTN_BACK:
            act = act1;
            break;

        case BTN_ADDRESS_ACCEPT: {
            // The address itself is the payload, so the code scans as an address wherever one is
            // read - there is no jade-specific wrapper here.
            const char* message[] = { "Scan QR", "address" };
            await_single_qr_activity(message, 2, (const uint8_t*)address, strlen(address));
            act = act1;
            break;
        }

        case BTN_ADDRESS_REJECT:
        default:
            return;
        }
    }
}

// BBB-AIRGAP: whether this device holds any registered wallet record at all.  The explorer only
// offers the choice when there is something to choose - load_registered_wallet() would otherwise
// open a carousel with nothing but 'Back' in it.
static bool any_registered_wallet(void)
{
    char multisig_names[MAX_MULTISIG_REGISTRATIONS][NVS_KEY_NAME_MAX_SIZE]; // Sufficient
    size_t num_multisigs = 0;
    multisig_get_valid_record_names(
        NULL, multisig_names, sizeof(multisig_names) / sizeof(multisig_names[0]), &num_multisigs);
    if (num_multisigs) {
        return true;
    }

    char descriptor_names[MAX_DESCRIPTOR_REGISTRATIONS][NVS_KEY_NAME_MAX_SIZE]; // Sufficient
    size_t num_descriptors = 0;
    descriptor_get_valid_record_names(
        descriptor_names, sizeof(descriptor_names) / sizeof(descriptor_names[0]), &num_descriptors);
    return num_descriptors > 0;
}

// BBB-AIRGAP: with 'out_address' set the list becomes a picker: choosing a row copies that address
// out and closes the screen instead of opening the detail view, and the return value says whether a
// row was chosen or the user backed out.  Mining's reward-address setting is the one caller that
// picks (handle_mining_address_settings()); everything else passes NULL and gets the screen as it
// was.  A picker rather than a fresh derivation screen, so the address offered for the block reward
// is the one the wallet already lists, derived by the very same code.
static bool address_explorer(char* out_address, const size_t out_address_len)
{
    JADE_ASSERT(keychain_get());
    JADE_ASSERT(!out_address == !out_address_len);
    bool picked = false;

    // One network for the whole device, as with the qr auth flow - there is no per-wallet setting.
    const network_t network_id
        = keychain_get_network_type_restriction() == NETWORK_TYPE_TEST ? NETWORK_BITCOIN_TESTNET : NETWORK_BITCOIN;

    // Script type and account start from what the xpub export is set to, but are only read: what
    // this screen lists is changed here alone, and never written back to that preference.  The
    // multisig bit is masked off, so what is listed is this wallet's own singlesig addresses.
    const uint32_t qr_flags = storage_get_qr_flags();
    uint32_t script_flags = qr_flags & ~QR_XPUB_MULTISIG;
    uint16_t account_index = qr_flags >> ACCOUNT_INDEX_FLAGS_SHIFT;
    bool is_change = false;

    // The addresses listed are this wallet's own unless the user picks a registered record, which
    // is only offered when one exists.  Either way the derivation below is the one verify_address()
    // runs for the same kind of wallet.
    multisig_data_t* multisig_data = NULL;
    descriptor_data_t* descriptor = NULL;
    char descriptor_name[NVS_KEY_NAME_MAX_SIZE] = { 0 };

    // The path to the leaf keys doubles as the screen title, so what is being listed is on screen
    // without a further trip into the options.  A registered record puts its own name here instead,
    // which is why this is sized for whichever of the two is longer.
    char root_path[MAX_PATH_STR_LEN(EXPORT_XPUB_PATH_LEN) > NVS_KEY_NAME_MAX_SIZE + 2
            ? MAX_PATH_STR_LEN(EXPORT_XPUB_PATH_LEN)
            : NVS_KEY_NAME_MAX_SIZE + 2];

    if (any_registered_wallet()) {
        const char* question[] = { "List addresses of a", "registered wallet?" };
        if (await_yesno_activity("Address Explorer", question, 2, false, NULL)) {
            // No script-type filter: any record can be listed, there is no address to match here
            if (!load_registered_wallet(NULL, root_path, sizeof(root_path), descriptor_name,
                    sizeof(descriptor_name), &multisig_data, &descriptor)) {
                // Nothing selected, or the record would not load - the error is already on screen
                return false;
            }
            JADE_ASSERT(!multisig_data != !descriptor); // Must be one or the other

            // BBB-AIRGAP: the network fixed above also applies to a record loaded here;
            // register_multisig() and register_descriptor() accept only the bitcoin choice that
            // the device setting resolves to, so liquid, localtest, and an unrestricted debug
            // keychain's mismatched testnet request cannot reach storage.  Neither record carries
            // its network (multisig_data_t, main/multisig.h; descriptor_data_t, main/descriptor.h),
            // so if either refusal is lifted this screen has no way to tell and can list an address
            // under a different network; the check belongs at registration, while it is known.
        }
    }

    // A page of addresses and their row labels lives on the heap: this runs on the dashboard task,
    // whose stack the strings would otherwise sit on for as long as the screen is up.
    char (*addresses)[MAX_ADDRESS_LEN] = JADE_CALLOC(ADDR_EXPLORER_PAGE_SIZE, sizeof(addresses[0]));
    char (*labels)[ADDR_LABEL_LEN] = JADE_CALLOC(ADDR_EXPLORER_PAGE_SIZE, sizeof(labels[0]));

    // One search root per signer for multisig, one for singlesig, none for a descriptor - which
    // carries its own keys.  Allocated once: the count cannot change while this screen is up.
    const size_t search_roots_len = multisig_data ? multisig_data->num_xpubs : descriptor ? 0 : 1;
    struct ext_key* const search_roots
        = search_roots_len ? JADE_CALLOC(search_roots_len, sizeof(struct ext_key)) : NULL;

    // Rows, then optionally 'Previous', then 'Next' and 'Options'
    list_item_t items[ADDR_EXPLORER_PAGE_SIZE + 3];
    size_t num_items = 0;

    size_t page = 0;
    size_t selected = 0;
    bool rebuild = true;

    // BBB-AIRGAP: the explorer opens on a menu that names what it is about to list.  Upstream
    // dropped straight into the receive addresses and kept the change branch inside the options
    // screen, where the device round of 2026-09-02 could not tell what turning it on had done:
    // the rows are truncated addresses either way and nothing on screen said which branch they
    // came from.  Naming the branch on the way in, and again in the list's title below, is what
    // was missing.  'Options' stays here as well as in the list, so the script type and account
    // can be set before a page is derived rather than after.
    bool show_entry_menu = true;
    size_t entry_selected = 0;

    // The title carries the branch in front of the path, so it is still on screen while browsing
    char list_title[8 + sizeof(root_path)];

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            goto cleanup;
        }
        if (show_entry_menu) {
            // A record carries its own script type and account, so with the branch picked here
            // there is nothing left for the options screen to offer and the row is dropped -
            // make_menu_activity() asserts on an empty menu (main/ui/dialogs.c:265).
            const bool registered_wallet = multisig_data || descriptor;
            list_item_t entry_items[3];
            size_t num_entry_items = 0;
            entry_items[num_entry_items++] = (list_item_t){ .txt = "Receive", .ev_id = BTN_ADDR_EXPLORER_RECEIVE };
            entry_items[num_entry_items++] = (list_item_t){ .txt = "Change", .ev_id = BTN_ADDR_EXPLORER_CHANGE };
            if (!registered_wallet) {
                entry_items[num_entry_items++] = (list_item_t){ .txt = "Options", .ev_id = BTN_ADDR_EXPLORER_OPTIONS };
            }
            JADE_ASSERT(entry_selected < num_entry_items);
            const int32_t entry_ev = run_list_activity(
                "Address Explorer", BTN_ADDR_EXPLORER_EXIT, entry_items, num_entry_items, &entry_selected);
            if (entry_ev == BTN_ADDR_EXPLORER_RECEIVE || entry_ev == BTN_ADDR_EXPLORER_CHANGE) {
                is_change = entry_ev == BTN_ADDR_EXPLORER_CHANGE;
                show_entry_menu = false;
                page = 0;
                selected = 0;
                rebuild = true;
            } else if (entry_ev == BTN_ADDR_EXPLORER_OPTIONS) {
                JADE_ASSERT(!registered_wallet);
                handle_address_options(true, &account_index, NULL, &script_flags);
            } else {
                goto cleanup;
            }
            continue;
        }

        if (rebuild) {
            // The search root(s), and the label that names them.  Multisig and descriptor rewrite
            // the change digit of the record name they were given; singlesig builds a fresh path.
            script_variant_t variant = P2WPKH;
            if (multisig_data) {
                get_multisig_search_roots(
                    multisig_data, is_change, root_path, sizeof(root_path), search_roots, search_roots_len);
            } else if (descriptor) {
                get_descriptor_search_roots(descriptor, is_change, root_path, sizeof(root_path), NULL, 0);
            } else {
                variant = xpub_script_variant_from_flags(script_flags);
                JADE_ASSERT(is_singlesig(variant));
                get_singlesig_search_root(
                    variant, account_index, is_change, root_path, sizeof(root_path), search_roots, search_roots_len);
            }

            num_items = 0;
            for (size_t i = 0; i < ADDR_EXPLORER_PAGE_SIZE; ++i) {
                const size_t index = (page * ADDR_EXPLORER_PAGE_SIZE) + i;
                JADE_ASSERT(index < BIP32_INITIAL_HARDENED_CHILD);

                if (descriptor) {
                    // The descriptor holds its own keys, so wally derives and encodes in one step
                    const uint32_t multi_index = is_change ? 1 : 0;
                    char* addr = NULL;
                    const char* errmsg = NULL;
                    if (!descriptor_to_address(
                            descriptor_name, descriptor, network_id, multi_index, index, NULL, &addr, &errmsg)) {
                        // Nothing partial is shown: a page with a hole in it would be read as a
                        // gap in the wallet rather than as a failure to derive.
                        await_error(errmsg ? errmsg : "Failed to derive address");
                        goto cleanup;
                    }
                    const int ret = snprintf(addresses[i], MAX_ADDRESS_LEN, "%s", addr);
                    wally_free_string(addr);
                    JADE_ASSERT(ret > 0 && ret < MAX_ADDRESS_LEN);
                } else {
                    uint8_t script[WALLY_SCRIPTPUBKEY_P2WSH_LEN]; // Sufficient
                    size_t script_len = 0;

                    if (multisig_data) {
                        // Derive the leaf key of every signer, then build the script they share -
                        // the same two steps wallet_search_for_multisig_script() takes per index.
                        uint8_t pubkeys[MAX_ALLOWED_SIGNERS * EC_PUBLIC_KEY_LEN]; // Sufficient
                        JADE_ASSERT(search_roots_len <= MAX_ALLOWED_SIGNERS);
                        for (size_t j = 0; j < search_roots_len; ++j) {
                            struct ext_key derived;
                            JADE_WALLY_VERIFY(bip32_key_from_parent(
                                &search_roots[j], index, BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, &derived));
                            memcpy(pubkeys + (j * EC_PUBLIC_KEY_LEN), derived.pub_key, sizeof(derived.pub_key));
                        }
                        const bool ret = wallet_build_multisig_script(multisig_data->variant, multisig_data->sorted,
                            multisig_data->threshold, pubkeys, search_roots_len * EC_PUBLIC_KEY_LEN, script,
                            sizeof(script), &script_len);
                        JADE_ASSERT(ret);
                    } else {
                        struct ext_key derived;
                        JADE_WALLY_VERIFY(bip32_key_from_parent(
                            &search_roots[0], index, BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, &derived));
                        const bool ret = wallet_build_singlesig_script(
                            network_id, variant, &derived, script, sizeof(script), &script_len);
                        JADE_ASSERT(ret);
                    }

                    const bool has_value = false;
                    script_to_address(network_id, script, script_len, has_value, addresses[i], MAX_ADDRESS_LEN);
                }

                make_address_row_label(index, addresses[i], labels[i], ADDR_LABEL_LEN);
                items[num_items++] = (list_item_t){ .txt = labels[i], .ev_id = BTN_ADDR_EXPLORER_ROW_0 + i };
            }

            if (page) {
                items[num_items++] = (list_item_t){ .txt = "Previous 10", .ev_id = BTN_ADDR_EXPLORER_PREV };
            }
            // The last page a wheel can reach is nowhere near the hardened boundary, but the row
            // is dropped rather than deriving past it.
            if ((page + 1) * ADDR_EXPLORER_PAGE_SIZE < BIP32_INITIAL_HARDENED_CHILD) {
                items[num_items++] = (list_item_t){ .txt = "Next 10", .ev_id = BTN_ADDR_EXPLORER_NEXT };
            }
            items[num_items++] = (list_item_t){ .txt = "Options", .ev_id = BTN_ADDR_EXPLORER_OPTIONS };
            JADE_ASSERT(num_items <= sizeof(items) / sizeof(items[0]));
            JADE_ASSERT(selected < num_items);

            // BBB-AIRGAP: the branch word always goes in the title; the path follows it only
            // while both fit.  populate_title_bar() (main/ui/dialogs.c:171) gives the title 70%
            // of the screen and does not shorten what it is handed - measured on 2026-09-02, the
            // pair wrapped onto a second line and crowded the first row.  Nothing here can assume
            // a length either: this path is at its longest on a record, whose name runs to 15
            // characters (NVS_KEY_NAME_MAX_SIZE).  So it is measured rather than counted, and the
            // path is what gives way, because the word is the thing the branch cannot be read
            // from anywhere else on this screen.
            const char* const branch = is_change ? "Change" : "Receive";
            int title_ret = snprintf(list_title, sizeof(list_title), "%s %s", branch, root_path);
            JADE_ASSERT(title_ret > 0 && title_ret < (int)sizeof(list_title));
            if (!gui_text_fits_width(list_title, GUI_TITLE_FONT, (CONFIG_DISPLAY_WIDTH * TITLE_CELL_PCNT) / 100)) {
                title_ret = snprintf(list_title, sizeof(list_title), "%s", branch);
                JADE_ASSERT(title_ret > 0 && title_ret < (int)sizeof(list_title));
            }
            rebuild = false;
        }

        const int32_t ev_id = run_list_activity(list_title, BTN_ADDR_EXPLORER_EXIT, items, num_items, &selected);
        if (ev_id >= BTN_ADDR_EXPLORER_ROW_0 && ev_id <= BTN_ADDR_EXPLORER_ROW_LAST) {
            const size_t row = (size_t)(ev_id - BTN_ADDR_EXPLORER_ROW_0);
            JADE_ASSERT(row < ADDR_EXPLORER_PAGE_SIZE);

            if (out_address) {
                const int ret = snprintf(out_address, out_address_len, "%s", addresses[row]);
                JADE_ASSERT(ret > 0 && (size_t)ret < out_address_len);
                picked = true;
                goto cleanup;
            }

            show_address_detail((page * ADDR_EXPLORER_PAGE_SIZE) + row, addresses[row]);
            continue;
        }

        switch (ev_id) {
        case BTN_ADDR_EXPLORER_NEXT:
            ++page;
            selected = 0;
            rebuild = true;
            break;

        case BTN_ADDR_EXPLORER_PREV:
            JADE_ASSERT(page);
            --page;
            selected = 0;
            rebuild = true;
            break;

        case BTN_ADDR_EXPLORER_OPTIONS: {
            // A registered record carries its own script type and account, so only the change
            // branch is the user's to pick there - the same rows verify_address() offers.
            const bool registered_wallet = multisig_data || descriptor;
            const bool show_account = !registered_wallet;
            if (handle_address_options(
                    show_account, &account_index, &is_change, registered_wallet ? NULL : &script_flags)) {
                // A different derivation is a different set of addresses - start again at its top
                page = 0;
                selected = 0;
                rebuild = true;
            }
            break;
        }

        case BTN_ADDR_EXPLORER_EXIT:
            // Back to the menu this screen was reached from, not out of the explorer: leaving
            // takes a second press there, which is also where the other branch is picked.
            show_entry_menu = true;
            break;

        default:
            goto cleanup;
        }
    }

cleanup:
    free(addresses);
    free(labels);
    free(search_roots);
    free(multisig_data);
    free(descriptor);
    return picked;
}

void handle_address_explorer(void) { address_explorer(NULL, 0); }

// Handle QR Options dialog - ie. QR size and frame-rate
static bool handle_qr_options(uint32_t* qr_flags, const char* help_url)
{
    JADE_ASSERT(qr_flags);

    gui_view_node_t* density_item = NULL;
    gui_view_node_t* framerate_item = NULL;
    gui_activity_t* const act = make_qr_options_activity(&density_item, &framerate_item);
    update_menu_item(density_item, "QR Density", qr_density_desc_from_flags(*qr_flags));
    update_menu_item(framerate_item, "Frame Rate", qr_framerate_desc_from_flags(*qr_flags));

    gui_view_node_t* density_textbox = NULL;
    gui_activity_t* const act_density = make_carousel_activity("QR Density", NULL, &density_textbox);
    gui_update_text(density_textbox, qr_density_desc_from_flags(*qr_flags));

    gui_view_node_t* framerate_textbox = NULL;
    gui_activity_t* const act_framerate = make_carousel_activity("Frame Rate", NULL, &framerate_textbox);
    gui_update_text(framerate_textbox, qr_framerate_desc_from_flags(*qr_flags));

    const uint32_t initial_flags = *qr_flags;
    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen, and what was turned on the wheel but never
        // confirmed goes back with it.  Without the restore the code below still sees changed
        // flags and writes them to storage, so leaving would persist a selection the user did
        // not accept; putting the entry value back routes into the 'nothing changed' branch.
        if (gui_escape_pending()) {
            *qr_flags = initial_flags;
            break;
        }

        // Show, and await button click
        gui_set_current_activity(act);

        int32_t ev_id = gui_activity_wait_button(act, BTN_QR_OPTIONS_EXIT);
        // NOTE: For Density and Speed :- HIGH|LOW > HIGH > LOW
        // Rotate through: LOW -> HIGH -> HIGH|LOW -> LOW -> ...
        // unset/default is treated as HIGH ie. the middle value
        if (ev_id == BTN_QR_OPTIONS_DENSITY) {
            gui_set_current_activity(act_density);
            while (true) {
                // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
                if (gui_escape_pending()) {
                    break;
                }

                gui_update_text(density_textbox, qr_density_desc_from_flags(*qr_flags));
                if (gui_activity_wait_event(act_density, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
                    if (ev_id == GUI_WHEEL_LEFT_EVENT) {
                        rotate_flags(qr_flags, QR_DENSITY_LOW, QR_DENSITY_HIGH); // reverse
                    } else if (ev_id == GUI_WHEEL_RIGHT_EVENT) {
                        rotate_flags(qr_flags, QR_DENSITY_HIGH, QR_DENSITY_LOW);
                    } else if (ev_id == gui_get_click_event()) {
                        // Done
                        break;
                    }
                }
            }
            update_menu_item(density_item, "QR Density", qr_density_desc_from_flags(*qr_flags));
        } else if (ev_id == BTN_QR_OPTIONS_FRAMERATE) {
            gui_set_current_activity(act_framerate);
            while (true) {
                // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
                if (gui_escape_pending()) {
                    break;
                }

                gui_update_text(framerate_textbox, qr_framerate_desc_from_flags(*qr_flags));
                if (gui_activity_wait_event(act_framerate, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
                    if (ev_id == GUI_WHEEL_LEFT_EVENT) {
                        rotate_flags(qr_flags, QR_SPEED_LOW, QR_SPEED_HIGH); // reverse
                    } else if (ev_id == GUI_WHEEL_RIGHT_EVENT) {
                        rotate_flags(qr_flags, QR_SPEED_HIGH, QR_SPEED_LOW);
                    } else if (ev_id == gui_get_click_event()) {
                        // Done
                        break;
                    }
                }
            }
            update_menu_item(framerate_item, "Frame Rate", qr_framerate_desc_from_flags(*qr_flags));
        } else if (ev_id == BTN_QR_OPTIONS_HELP) {
            await_qr_help_activity(help_url);
        } else if (ev_id == BTN_QR_OPTIONS_EXIT) {
            // Done
            break;
        }
    }

    // If nothing was updated, return false
    if (initial_flags == *qr_flags) {
        return false;
    }

    // Persist prefereces and return true to indicate they were changed
    storage_set_qr_flags(*qr_flags);
    return true;
}

static void display_bcur_qr(const char* message[], const size_t message_size, const char* bcur_type,
    const uint8_t* cbor, const size_t cbor_len, const char* help_url)
{
    JADE_ASSERT(message);
    JADE_ASSERT(message_size);
    JADE_ASSERT(bcur_type);
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);
    // help_url is optional

    uint32_t qr_flags = qr_flags_with_defaults(storage_get_qr_flags());

    // When displaying a bcur qr code we set the minimum idle timeout to keep the hw from sleeping too quickly
    // (If the user has set a longer timeout value that is respected)
    idletimer_set_min_timeout_secs(BCUR_QR_DISPLAY_MIN_TIMEOUT_SECS);

    // BBB-AIRGAP: the screen that describes what is being exported - the code is a screen further
    // in, and its icons are built there so they always reflect the current qr options
    const bool show_options_button = true;
    gui_activity_t* const act = make_show_qr_activity(message, message_size, show_options_button);
    bool code_shown = false;

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            break;
        }
        // Show, and await button click
        gui_set_current_activity(act);

        const int32_t ev_id = wait_export_screen_button(act, BTN_QR_DISPLAY_EXIT, &code_shown);
        if (ev_id == BTN_QR_SHOW_FULLSCREEN) {
            // Building the fragments takes a moment for anything psbt-sized
            gui_activity_t* const processing_act = display_processing_message_activity();

            Icon* icons = NULL;
            size_t num_icons = 0;
            bcur_create_qr_icons(cbor, cbor_len, bcur_type, qr_version_from_flags(qr_flags), &icons, &num_icons);
            display_fullscreen_qr(act, processing_act, icons, num_icons, qr_framerate_from_flags(qr_flags));
        } else if (ev_id == BTN_QR_OPTIONS) {
            // The options only matter when the code is built, so nothing to redraw here
            handle_qr_options(&qr_flags, help_url);
        } else if (ev_id == BTN_QR_BRIGHTNESS) {
            gui_next_qrcode_color();
            gui_repaint(act->root_node);
        } else if (ev_id == BTN_QR_DISPLAY_EXIT) {
            // Done
            break;
        }
    }

    // Remove the minimum idle timeout
    idletimer_set_min_timeout_secs(0);
}

// BBB-AIRGAP: split out of handle_qr_bytes() below so the wallet's 'Sign Message' entry can accept
// this one format and refuse everything else (handle_sign_message()).  The general scan still
// reaches the same code by the same test, so the two entries cannot drift apart.
static bool is_sign_message_payload(const char* strbytes, const size_t bytes_len)
{
    JADE_ASSERT(strbytes);
    return bytes_len > sizeof("signmessage") && !strncasecmp(strbytes, "signmessage", sizeof("signmessage") - 1);
}

static bool handle_sign_message_payload(const char* strbytes, const size_t bytes_len)
{
    JADE_ASSERT(strbytes);
    JADE_ASSERT(is_sign_message_payload(strbytes, bytes_len));

    uint8_t sig[EC_SIGNATURE_LEN * 2]; // Sufficient
    size_t written = 0;
    const char* errmsg = NULL;
    const int errcode = sign_message_file(strbytes, bytes_len, sig, sizeof(sig), &written, &errmsg);
    if (errcode) {
        if (errcode != CBOR_RPC_USER_CANCELLED) {
            JADE_LOGE("Processing 'signmessage' QR failed: %d, %s", errcode, errmsg);
            await_error(errmsg);
        }
        return false;
    }
    JADE_ASSERT(written);
    JADE_ASSERT(written < sizeof(sig));
    JADE_ASSERT(sig[written - 1] == '\0');

    const char* message[] = { "Scan QR", "signature" };
    await_single_qr_activity(message, 2, sig, written - 1);
    return true;
}

// Handle undifferentiated byte string friom QR code
// ie. raw data (not BC-UR wrapped), OR the payload of a UR:BYTES message
// BBB-AIRGAP: UR:CRYPTO-OUTPUT (Sparrow 'Export wallet' QR) -> descriptor text -> same path as text
static bool handle_crypto_output_qr(const uint8_t* cbor, const size_t cbor_len)
{
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);

    const size_t text_len = MAX_DESCRIPTOR_SCRIPT_LEN + (MAX_ALLOWED_SIGNERS * sizeof(((string_value_t*)0)->value));
    char* const text = JADE_MALLOC(text_len);
    const char* errmsg = NULL;
    bool ret = false;
    if (!bcur_parse_crypto_output(cbor, cbor_len, text, text_len, &errmsg)) {
        JADE_LOGE("Parsing crypto-output failed: %s", errmsg ? errmsg : "no detail");
        await_error(errmsg ? errmsg : "Invalid descriptor");
    } else {
        const int errcode = register_descriptor_text(text, strlen(text), &errmsg);
        if (errcode && errcode != CBOR_RPC_USER_CANCELLED) {
            JADE_LOGE("Processing crypto-output descriptor failed: %s", errmsg ? errmsg : "no detail");
            await_error(errmsg ? errmsg : "Invalid descriptor");
        }
        ret = errcode == 0;
    }
    free(text);
    return ret;
}

static bool handle_qr_bytes(const uint8_t* bytes, const size_t bytes_len)
{
    JADE_ASSERT(bytes);
    JADE_ASSERT(bytes_len);

    const char* strbytes = (const char*)bytes;

    // Try to handle as 'sign message' (specter format)
    // BBB-AIRGAP: gated where the payload is dispatched rather than inside the format test, so the
    // test keeps meaning 'this is a message to sign' for the menu entry that reports on it
    // (handle_sign_message()).  With the feature turned off the code falls through to the handlers
    // below and is reported as unhandled, the same as any other code this device does not take.
    if ((storage_get_feature_flags() & FEATURE_FLAGS_SIGN_MESSAGE) && is_sign_message_payload(strbytes, bytes_len)) {
        return handle_sign_message_payload(strbytes, bytes_len);
    }

    // Try to handle as otp string
    if (bytes_len > sizeof(OTP_SCHEMA_FULL) && !strncasecmp(strbytes, OTP_SCHEMA_FULL, sizeof(OTP_SCHEMA_FULL) - 1)) {
        // Looks like an OTP URI
        const char* errmsg = NULL;
        const int errcode = register_otp_string(strbytes, bytes_len, &errmsg);
        if (errcode) {
            JADE_LOGE("Processing OTP URI failed: %s", errmsg);
            return false;
        }
        return true;
    }

    // Try to handle as otpauth-migrate string
    if (bytes_len > sizeof(OTP_MIGRATE_SCHEMA_FULL)
        && !strncasecmp(strbytes, OTP_MIGRATE_SCHEMA_FULL, sizeof(OTP_MIGRATE_SCHEMA_FULL) - 1)) {
        // Looks like an OTP MIGRATE URI
        const char* errmsg = NULL;
        const int errcode = register_otp_migrate_string(strbytes, bytes_len, &errmsg);
        if (errcode) {
            JADE_LOGE("Processing OTP MIGRATE URI failed: %s", errmsg);
            return false;
        }
        return true;
    }

    // Try to handle as multisig file
    if (strncasestr(strbytes, "Name", bytes_len) && strncasestr(strbytes, "Format", bytes_len)
        && strncasestr(strbytes, "Policy", bytes_len) && strncasestr(strbytes, "Derivation", bytes_len)) {
        // Looks like a multisig registration file
        const char* errmsg = NULL;
        const int errcode = register_multisig_file(strbytes, bytes_len, &errmsg);
        if (errcode) {
            if (errcode != CBOR_RPC_USER_CANCELLED) {
                JADE_LOGE("Processing multisig file failed: %s", errmsg);
                await_error(errmsg);
            }
            return false;
        }
        return true;
    }

    // BBB-AIRGAP: descriptor text (Sparrow, Specter) or a Specter JSON wallet export
    if (descriptor_text_is_descriptor(strbytes, bytes_len)) {
        const char* errmsg = NULL;
        const int errcode = register_descriptor_text(strbytes, bytes_len, &errmsg);
        if (errcode) {
            if (errcode != CBOR_RPC_USER_CANCELLED) {
                JADE_LOGE("Processing descriptor failed: %s", errmsg ? errmsg : "no detail");
                await_error(errmsg ? errmsg : "Invalid descriptor");
            }
            return false;
        }
        return true;
    }

    // See if it looks like a new wallet phrase
    // NOTE: these must always be nul-terminated (even when a binary compact seed qr)
    if (strbytes[bytes_len] == '\0') {
        char mnemonic[MNEMONIC_BUFLEN];
        SENSITIVE_PUSH(mnemonic, sizeof(mnemonic));
        size_t written = 0;
        if (import_mnemonic(bytes, bytes_len, mnemonic, sizeof(mnemonic), &written) && written < sizeof(mnemonic)) {
            if (!handle_mnemonic_qr(mnemonic)) {
                JADE_LOGE("Handling new scanned mnemonic failed");
                await_error("Failed loading wallet");
                SENSITIVE_POP(mnemonic);
                return false;
            }
            SENSITIVE_POP(mnemonic);
            return true;
        }
        SENSITIVE_POP(mnemonic);
    }

    JADE_LOGW("Unhandled QR (bytes) message");
    await_error("Unhandled QR payload");
    return false;
}

// Unwrap UR:BYTES message and pass payload bytes to above handler
static bool handle_bcur_bytes(const uint8_t* cbor, const size_t cbor_len)
{
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);

    const uint8_t* bytes = NULL;
    size_t bytes_len = 0;
    if (!bcur_parse_bytes(cbor, cbor_len, &bytes, &bytes_len)) {
        await_error("Invalid QR/BYTES format");
        return false;
    }
    return handle_qr_bytes(bytes, bytes_len);
}

// Parse a BC-UR PSBT and attempt to sign and display as BC-UR QR
static bool parse_sign_display_bcur_psbt_qr(const uint8_t* cbor, const size_t cbor_len)
{
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);

    // Parse scanned QR data
    struct wally_psbt* psbt = NULL;
    if (!bcur_parse_psbt(cbor, cbor_len, &psbt)) {
        // Unexpected type/format
        await_error("Unsupported QR/PSBT format");
        return false;
    }

    // Try to sign extracted PSBT
    bool ret = false;
    const char* errmsg = NULL;
    const network_t network_id = network_from_psbt_type(psbt);

    // Note we pass NULL process/params as we don't have any additional info
    const int errcode = sign_psbt(NULL, NULL, network_id, psbt, &errmsg);
    if (errcode) {
        if (errcode != CBOR_RPC_USER_CANCELLED) {
            await_error(errmsg);
        }
        goto cleanup;
    }

    // Build BCUR message holding the signed PSBT
    uint8_t* cbor_signed = NULL;
    size_t cbor_signed_len = 0;
    if (!bcur_build_cbor_crypto_psbt(psbt, &cbor_signed, &cbor_signed_len)) {
        JADE_LOGW("Failed to build bcur/cbor for psbt");
        goto cleanup;
    }

    // Now display bcur QR
    const char* message[] = { "Scan with", "wallet", "app" };
    display_bcur_qr(message, 3, BCUR_TYPE_CRYPTO_PSBT, cbor_signed, cbor_signed_len, "blkstrm.com/psbt");
    ret = true;

cleanup:
    JADE_WALLY_VERIFY(wally_psbt_free(psbt));
    return ret;
}

void show_bip85_bip39_entropy_qr(const uint8_t* cbor, const size_t cbor_len)
{
    JADE_ASSERT(cbor && cbor_len);
    const char* message[] = { "Scan with", "wallet", "app" };
    display_bcur_qr(message, 3, BCUR_TYPE_JADE_BIP8539_REPLY, cbor, cbor_len, "blkstrm.com/bip85");
}

// Returns false if an error occured or the user cancelled the action
static bool handle_bip85_bip39_request_qr(const uint8_t* cbor, const size_t cbor_len)
{
    JADE_ASSERT(cbor && cbor_len);

    // Parse cbor
    CborValue root;
    CborParser parser;
    if (!bcur_parse_jade_message(cbor, cbor_len, &parser, &root, NULL, NULL)) {
        JADE_LOGE("Failed to parse Jade bip85/bip39 entropy request");
        await_error("Error parsing message");
        return false;
    }

    const char* errmsg = NULL;
    uint8_t reply_cbor[176]; // sufficient for encrypted bip85 reply
    SENSITIVE_PUSH(reply_cbor, sizeof(reply_cbor));

    CborEncoder reply_encoder;
    cbor_encoder_init(&reply_encoder, reply_cbor, sizeof(reply_cbor), 0);

    const int errcode = get_bip85_bip39_entropy_cbor(&root, &reply_encoder, &errmsg);
    if (errcode) {
        if (errcode != CBOR_RPC_USER_CANCELLED) {
            JADE_LOGE("Error generating encrypted bip85 entropy: %s", errmsg);
            await_error_2("Error generating entropy", errmsg);
        }
        // An error occurred, or the user cancelled the action
        SENSITIVE_POP(reply_cbor);
        return false;
    }

    const size_t reply_cbor_len = cbor_encoder_get_buffer_size(&reply_encoder, reply_cbor);
    JADE_ASSERT(reply_cbor_len && reply_cbor_len <= sizeof(reply_cbor));

    show_bip85_bip39_entropy_qr(reply_cbor, reply_cbor_len);
    SENSITIVE_POP(reply_cbor);
    return true;
}

// BBB-AIRGAP: mining template via qr code (mining M3). The template is attacker-chosen input;
// every field is bounded here, and the bits/target and coinbase consistency are re-validated
// inside on_new_target() (components/miner/miner.c) when mining starts. Nothing in this path
// touches the keychain.
// Length sanity bounds only: base58 (1.., 3.., m.., n.., 2..) is 26..35 chars, bech32 42..90
// (BIP-173 ceiling). miner.c accepts both forms and validates the address itself in
// on_new_target(); anything that fits is passed through unchanged.
#define MINING_ADDRESS_MIN 26
#define MINING_ADDRESS_MAX 90
#define MINING_SOLUTION_MAX (80 + 1 + 300) // header, tx count byte, rawtx[300] in miner.c

typedef struct {
    uint32_t version;
    uint32_t curtime;
    uint32_t bits;
    uint32_t height;
    const uint8_t* prevhash; // into the scanned cbor
    const uint8_t* target; // into the scanned cbor
    char address[MINING_ADDRESS_MAX + 1];
} mining_template_t;

static bool parse_mining_template(const CborValue* params, mining_template_t* t)
{
    JADE_ASSERT(params);
    JADE_ASSERT(t);

    // uint64_t on purpose: size_t is 32 bits on the ESP32 targets, where '> UINT32_MAX' on it is
    // always false (dead code plus a -Wtype-limits warning); the 64-bit emulator build hides that.
    uint64_t version = 0, curtime = 0, bits = 0, height = 0;
    if (!rpc_get_uint64("version", params, &version) || !rpc_get_uint64("curtime", params, &curtime)
        || !rpc_get_uint64("bits", params, &bits) || !rpc_get_uint64("height", params, &height) || !curtime || !height
        || version > UINT32_MAX || curtime > UINT32_MAX || bits > UINT32_MAX || height > UINT32_MAX) {
        return false;
    }

    size_t len = 0;
    rpc_get_bytes_ptr("previousblockhash", params, &t->prevhash, &len);
    if (!t->prevhash || len != 32) {
        return false;
    }
    len = 0;
    rpc_get_bytes_ptr("target", params, &t->target, &len);
    if (!t->target || len != 32) {
        return false;
    }
    len = 0;
    rpc_get_string("address", sizeof(t->address), params, t->address, &len);
    if (len < MINING_ADDRESS_MIN || len > MINING_ADDRESS_MAX) {
        return false;
    }

    t->version = (uint32_t)version; // all four range-checked above
    t->curtime = (uint32_t)curtime;
    t->bits = (uint32_t)bits;
    t->height = (uint32_t)height;
    return true;
}

typedef struct {
    uint8_t data[MINING_SOLUTION_MAX];
    size_t len;
} mining_solution_t;

// Called from check_solutions() on the owner task, never from a miner task, so no locking here.
static void on_mining_solution(void* ctx, const uint8_t* data, const uint32_t len)
{
    mining_solution_t* const sol = ctx;
    JADE_ASSERT(sol);
    JADE_ASSERT(data);
    if (len == 0 || len > sizeof(sol->data)) {
        JADE_LOGE("Mining solution of %u bytes rejected", (unsigned)len);
        sol->len = 0;
        return;
    }
    memcpy(sol->data, data, len);
    sol->len = len;
}

// BBB-AIRGAP: upstream's mining screen (make_mining_screen(), its main/ui/dashboard.c) puts three
// rows on a 240x135 panel: hash rate, block reward, and the reward address.  Two of them are kept
// here.  The address row is dropped: a bech32 address runs to 90 characters, which upstream left to
// be clipped, and on this port the address has just been shown in full on the confirmation screen
// that opens this one.  What is left gets half the panel each, so both read at arm's length.
static gui_activity_t* make_mining_activity(const char* title, const uint64_t reward, gui_view_node_t** rate_node)
{
    JADE_ASSERT(title);
    JADE_INIT_OUT_PPTR(rate_node);

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_MINING_STOP },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };
    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* const parent = add_title_bar(act, title, hdrbtns, 2, NULL);

    gui_view_node_t* vsplit;
    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 2, 50, 50);
    gui_set_parent(vsplit, parent);

    // BBB-AIRGAP: a fill behind the text, or gui_update_text() has no background to repaint and
    // successive rates pile up (main/ui/mnemonic.c:515 lesson).
    gui_view_node_t* fill;
    gui_make_fill(&fill, TFT_BLACK, FILL_PLAIN, vsplit);

    gui_view_node_t* node;
    gui_make_text_font(&node, "-- h/s", TFT_WHITE, GUI_DEFAULT_FONT);
    gui_set_align(node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(node, fill);
    *rate_node = node;

    // The reward is fixed for as long as this template is being mined, so this row is built once
    // and never updated - no fill behind it.  Shown in whichever unit the device is set to, the
    // way every other amount on this device reads.
    char amount[32];
    const char* const ticker = format_btc_amount(reward, amount, sizeof(amount));
    char rewardstr[48];
    const int ret = snprintf(rewardstr, sizeof(rewardstr), "%s %s", amount, ticker);
    JADE_ASSERT(ret > 0 && ret < sizeof(rewardstr));

    gui_view_node_t* rewardnode;
    gui_make_text_font(&rewardnode, rewardstr, TFT_WHITE, GUI_DEFAULT_FONT);
    gui_set_align(rewardnode, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(rewardnode, vsplit);
    return act;
}

// BBB-AIRGAP: reuse Jade's own address screen (main/ui/confirm_address.c, declared at the top of
// this file and used by the 'Scan address' flow) so the address is laid out the way the device
// already shows addresses. MINING_ADDRESS_MAX (90) < MAX_DISPLAY_ADDRESS_LEN (96): always a single
// screen, act2 stays NULL. Default event BTN_ADDRESS_ACCEPT: on the device the pressed button
// decides (gui.c:2944-2947); the unattended-CI emulator build returns the default after 1 ms
// (gui.c:2948-2951), exactly as await_yesno_activity returns BTN_YES there (dialogs.c:835), so
// the emulator walkthrough reaches the mining screen. The initial highlight is still the reject
// button (default_selection = false), so a real user has to move to the tick on purpose.
static bool confirm_mining_template(const mining_template_t* t)
{
    JADE_ASSERT(t);
    char title[24]; // same size and pattern as the 'Address %u' title, qrmode.c:1206-1208
    const int ret = snprintf(title, sizeof(title), "Mine block %u", (unsigned)t->height);
    JADE_ASSERT(ret > 0 && ret < sizeof(title));

    gui_activity_t* act2 = NULL;
    gui_activity_t* const act = make_display_address_activities(title, true, t->address, false, &act2);
    JADE_ASSERT(!act2);
    gui_set_current_activity(act);
    return gui_activity_wait_button(act, BTN_ADDRESS_ACCEPT) == BTN_ADDRESS_ACCEPT;
}

static void show_mining_solution_qr(const mining_solution_t* sol, const char* id, const size_t id_len)
{
    JADE_ASSERT(sol);
    JADE_ASSERT(sol->len);
    JADE_ASSERT(id);
    JADE_ASSERT(id_len && id_len <= MAXLEN_ID);

    // Same envelope upstream sent over serial ({"id","result":bytes}, main/process.c:508-531),
    // so a later companion parses the qr reply with the code it already has for the serial one.
    // Room for the map header, "id" plus up to MAXLEN_ID chars, "result" plus the byte string.
    uint8_t reply_cbor[MINING_SOLUTION_MAX + 48];
    CborEncoder root;
    CborEncoder map;
    cbor_encoder_init(&root, reply_cbor, sizeof(reply_cbor), 0);
    CborError cberr = cbor_encoder_create_map(&root, &map, 2);
    JADE_ASSERT(cberr == CborNoError);
    rpc_init_cbor(&map, id, id_len); // the request's own id, as the serial reply builder does
    const bytes_info_t r = { .data = sol->data, .size = sol->len };
    cbor_result_bytes_cb(&r, &map);
    cberr = cbor_encoder_close_container(&root, &map);
    JADE_ASSERT(cberr == CborNoError);
    const size_t len = cbor_encoder_get_buffer_size(&root, reply_cbor);
    JADE_LOGI("Mining solution %u bytes, reply cbor %u bytes", (unsigned)sol->len, (unsigned)len);

    const char* message[] = { "Scan with", "mining", "companion" };
    display_bcur_qr(message, 3, BCUR_TYPE_JADE_MINE_REPLY, reply_cbor, len, NULL);
}

// BBB-AIRGAP: the address in a scanned template is the sender's choice, not the user's, so the
// device carries a setting for whose address a mined block pays (Options > Mining >
// Reward). It is applied here rather than at the menu, so a template reached through the general
// scan honours it too. Only the choice is stored; the address is picked now, from the very list the
// address explorer shows, so nothing new derives keys for mining. Returns false when mining should
// not start: the user backed out of the picker, or asked for their own address while no wallet is
// loaded. That second case fails closed rather than quietly paying the template's address: a block
// reward cannot be taken back, and 'reward goes elsewhere' is exactly what the setting exists to
// prevent.
static bool apply_reward_address_preference(mining_template_t* t)
{
    JADE_ASSERT(t);

    if (!(storage_get_qr_flags() & QR_MINING_ADDRESS_WALLET)) {
        return true;
    }

    if (!keychain_get()) {
        JADE_LOGW("Mining reward address is set to this wallet, but no wallet is loaded");
        await_error("Unlock wallet to mine");
        return false;
    }

    if (!address_explorer(t->address, sizeof(t->address))) {
        JADE_LOGI("Mining cancelled at reward address selection");
        return false;
    }

    JADE_LOGI("Mining reward address taken from this wallet");
    return true;
}

// Accept a mining template via qr code
static bool handle_mining_qr(const uint8_t* cbor, const size_t cbor_len)
{
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);

    // Parse cbor
    CborValue root;
    CborValue params;
    CborParser parser;
    if (!bcur_parse_jade_message(cbor, cbor_len, &parser, &root, "mine", &params)) {
        JADE_LOGE("Failed to parse mining template");
        await_error_2("Error parsing", "mining template");
        return false;
    }

    mining_template_t t = { 0 };
    if (!parse_mining_template(&params, &t)) {
        JADE_LOGE("Mining template rejected at parse");
        await_error("Invalid mining template");
        return false;
    }

    JADE_LOGI("Mining template accepted: height %u", (unsigned)t.height);
    // BBB-AIRGAP: the reply carries the request's id, as the serial reply builder does
    // (main/process.c); bcur_parse_jade_message() does not check the id, so a template without a
    // usable one (1..MAXLEN_ID chars) is refused here rather than answered under a made-up id.
    char id[MAXLEN_ID + 1];
    size_t id_len = 0;
    rpc_get_id(&root, id, sizeof(id), &id_len);
    if (!id_len) {
        JADE_LOGE("Mining template rejected: missing id");
        await_error("Invalid mining template");
        return false;
    }

    if (!apply_reward_address_preference(&t)) {
        return false;
    }

    if (!confirm_mining_template(&t)) {
        return false;
    }

    mining_solution_t sol = { .len = 0 };
    void* mctx = NULL;
    start_miners(&mctx, on_mining_solution, &sol);
    if (!mctx) {
        await_error("Could not start miners");
        return false;
    }
    const uint64_t reward
        = on_new_target(mctx, t.version, t.prevhash, t.target, t.curtime, t.bits, t.height, t.address);
    if (!reward) {
        stop_miners(mctx);
        JADE_LOGE("Mining template rejected by the miner");
        await_error("Invalid mining template");
        return false;
    }

    char title[24];
    const int ret = snprintf(title, sizeof(title), "Mining %u", (unsigned)t.height);
    JADE_ASSERT(ret > 0 && ret < sizeof(title));
    gui_view_node_t* rate_node = NULL;
    gui_activity_t* const act = make_mining_activity(title, reward, &rate_node);
    // BBB-AIRGAP: one registration held for the whole screen. gui_activity_wait_event() would
    // allocate a fresh wait_event_data and registration on every turn (main/gui.c) and free none
    // of them until the activity changes; over hours of mining that is an unbounded leak, and a
    // press landing between two turns would be signalled to a stale semaphore and lost. Same
    // pattern as the search-address options screen above.
    wait_event_data_t* const event_data = gui_activity_make_wait_event_data(act);
    JADE_ASSERT(event_data);
    gui_activity_register_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);
    gui_set_current_activity(act);

    bool stopped = false;
#ifdef CONFIG_LIBJADE
    // BBB-AIRGAP: under libjade, sync_wait_event() ends this thread with pthread_exit() once
    // libjade_stop() is requested (main/utils/event.c), so the stop_miners() after the loop would
    // never run and both miner tasks would keep hashing until the process exits. The cleanup
    // handler keeps the teardown ordered on that path. The ESP32 task is never ended here.
    pthread_cleanup_push(stop_miners, mctx);
#endif
    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            stopped = true;
            break;
        }

        int32_t ev_id = 0;
        if (sync_wait_event(event_data, NULL, &ev_id, NULL, 2000 / portTICK_PERIOD_MS) == ESP_OK
            && ev_id == BTN_MINING_STOP) {
            stopped = true;
            break;
        }
        // BBB-AIRGAP: K1 decision. Mining runs for hours; without this the idle timer reboots the
        // device with the wallet cleared (main/idletimer.c:176-215). Registering activity leaves
        // nothing to restore on exit, unlike the min-timeout override pattern of display_bcur_qr().
        idletimer_register_activity(false);
        if (check_solutions(mctx)) {
            break; // the callback either filled sol, or dropped an oversized one (sol.len == 0)
        }
        uint32_t speed = 0;
        check_speed(mctx, &speed);
        char ratestr[24];
        snprintf(ratestr, sizeof(ratestr), "%lu h/s", (unsigned long)speed);
        gui_update_text(rate_node, ratestr);
    }
#ifdef CONFIG_LIBJADE
    pthread_cleanup_pop(0);
#endif
    stop_miners(mctx);
    if (stopped) {
        return false;
    }
    if (!sol.len) {
        // A10: the callback dropped an oversized solution (logged there); say so on screen rather
        // than returning to the menu silently.
        await_error("Mining solution rejected");
        return false;
    }
    JADE_LOGI("Mining solution found: %u bytes", (unsigned)sol.len);
    show_mining_solution_qr(&sol, id, id_len);
    return true;
}

// BBB-AIRGAP: the way in to mining from the menu. The general scan still takes a mining template
// when one turns up, the way it takes any other code; this entry takes that format alone, the same
// shape handle_sign_message() uses, so a mis-scan is reported here rather than being handled as
// whatever the general scan would have made of it.
static void handle_mining_start(void)
{
    char* type = NULL;
    uint8_t* data = NULL;
    size_t data_len = 0;
    if (!bcur_scan_qr("Mining QR", &type, &data, &data_len, 0, NULL) || !data) {
        // Scan aborted
        JADE_ASSERT(!type);
        JADE_ASSERT(!data);
        return;
    }

    if (!type || strcasecmp(type, BCUR_TYPE_JADE_MINE)) {
        JADE_LOGW("Unexpected BC-UR type scanned for mining: %s", type ? type : "(none)");
        await_error("Not a mining template");
    } else if (!handle_mining_qr(data, data_len)) {
        JADE_LOGE("Processing BC-UR as mining template failed");
    }

    free(type);
    free(data);
}

// BBB-AIRGAP: whose address a mined block pays. Only the choice is kept; the address itself is
// picked when mining starts (apply_reward_address_preference()), so nothing has to be stored and
// no address goes stale behind the setting. 'This wallet' is refused while the device is locked,
// because the address cannot be derived and a setting that silently means something else is worse
// than one that says no.
static void handle_mining_reward_setting(void)
{
    const uint32_t qr_flags = storage_get_qr_flags();
    const bool was_wallet = qr_flags & QR_MINING_ADDRESS_WALLET;

    // Chosen on a screen of its own, as Jade sets its QR options: the address in use between the
    // arrows, the click keeps the one shown.
    static const char* const LABELS[] = { "Template", "This Wallet" };
    const bool use_wallet = await_carousel_activity("Reward Address", LABELS, 2, was_wallet ? 1 : 0) == 1;
    if (use_wallet == was_wallet) {
        return;
    }

    if (use_wallet && !keychain_get()) {
        await_error("Unlock wallet first");
        return;
    }

    if (!storage_set_qr_flags(
            use_wallet ? (qr_flags | QR_MINING_ADDRESS_WALLET) : (qr_flags & ~QR_MINING_ADDRESS_WALLET))) {
        JADE_LOGE("Failed to store mining reward address preference");
        await_error("Failed to save setting");
    }
}

// BBB-AIRGAP: mining's own menu, opened from Options. Built again each time round, as the
// dashboard does with gui_set_current_activity_ex(act, true): free previous managed activities,
// including the reward carousel, while keeping the new act alive for gui_activity_wait_button().
// Screens opened here can free this menu; Options is rebuilt on return and the dashboard is unmanaged.
void handle_mining_settings(void)
{
    while (true) {
        // BBB-AIRGAP: mining, scanning and reward settings consume the escape themselves.
        // Carry it home instead of rebuilding a menu that waits for another press.
        if (gui_escape_pending()) {
            return;
        }
        gui_activity_t* const act = make_mining_menu_activity();
        gui_set_current_activity_ex(act, true);

        const int32_t ev_id = gui_activity_wait_button(act, BTN_MINING_EXIT);
        if (ev_id == BTN_MINING_START) {
            handle_mining_start();
        } else if (ev_id == BTN_MINING_REWARD) {
            handle_mining_reward_setting();
        } else {
            return;
        }
    }
}

// BBB-AIRGAP: localtime_r() returns NULL for a value outside the calendar range it can render,
// and an epoch QR carries an arbitrary uint64_t.  Handing an uninitialised buffer on as a string
// would overread the stack on the very screen that exists to let the user judge the value
// (Codex review, 2026-09-03).
//
// The layout is ISO rather than ctime's because ctime's 24 characters do not fit: measured on the
// 240px display, the confirmation screen renders 22 of them and cuts the string mid-year, so the
// user was asked to approve a clock change without being able to see which year it moved to - the
// one field an attacker would move furthest.  Nineteen characters fit with room to spare.  The
// success screen shares this helper, so it was cropping the same way and is fixed with it.
//
// The length is checked exactly rather than for success, because %Y is not fixed width: an epoch
// naming year 10000 or beyond formats to a longer string that localtime_r accepts and the buffer
// holds, putting the clipping straight back on screen.  ctime_r refused those values as a side
// effect of its own fixed layout; requiring nineteen characters keeps that refusal deliberate
// (Codex review, 2026-09-03).
static bool format_epoch(const uint64_t epoch, char* const buf, const size_t buf_len)
{
    JADE_ASSERT(buf);
    JADE_ASSERT(buf_len >= 20); // "YYYY-MM-DD HH:MM:SS" and its terminator

    if (epoch > (uint64_t)INT64_MAX) {
        return false; // would wrap into a negative time_t
    }
    const time_t when = (time_t)epoch;
    struct tm parts;
    if (!localtime_r(&when, &parts)) {
        return false;
    }
    return strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", &parts) == 19;
}

// Accept an epoch (time) message via qr code
static bool handle_epoch_qr(const uint8_t* cbor, const size_t cbor_len)
{
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);

    // Parse cbor
    CborValue root;
    CborValue params;
    CborParser parser;
    if (!bcur_parse_jade_message(cbor, cbor_len, &parser, &root, "set_epoch", &params)) {
        JADE_LOGE("Failed to parse Jade epoch message");
        await_error_2("Error parsing", "epoch data");
        return false;
    }

    // BBB-AIRGAP: the clock is not cosmetic - TOTP codes are derived from it (main/otpauth.c) - so
    // a QR that moves it must not pass unseen.  It was a yes/no confirmation until 2026-09-08; the
    // page that draws the QR refreshes it every second, and answering the question cost more
    // seconds than that, so the clock always landed behind the time the user had just read
    // (measured on the device, compared against a phone authenticator).  The notice moved after
    // the fact instead of being removed: the value still reaches the screen in the same
    // human-readable form, now on the result screen, which waits on 'Continue' rather than timing
    // out (await_message_activity(), main/ui/dialogs.c).  What this gives up is the chance to
    // decline: an epoch QR arriving through the generic scanner now moves the clock and reports
    // it, where before it asked first.  Deliberate for this fork.
    //
    // The value is still parsed and rendered here rather than trusted, because an epoch that
    // cannot be rendered as a date is rejected before the clock is touched.
    uint64_t proposed = 0;
    if (!rpc_get_uint64("epoch", &params, &proposed)) {
        JADE_LOGE("Failed to extract epoch value");
        await_error_2("Error parsing", "epoch data");
        return false;
    }
    char proposed_str[32];
    if (!format_epoch(proposed, proposed_str, sizeof(proposed_str))) {
        JADE_LOGE("Epoch value cannot be rendered as a date");
        await_error("Invalid time in QR code");
        return false;
    }

    const char* errmsg = NULL;
    const int errcode = params_set_epoch_time(&params, &errmsg);
    if (errcode) {
        if (errcode != CBOR_RPC_USER_CANCELLED) {
            JADE_LOGE("Error setting epoch time: %s", errmsg);
            await_error_2("Error setting epoch time", errmsg);
        }
        return false;
    }

    char timestr[32];
    const uint64_t epoch_value = time(NULL);
    if (format_epoch(epoch_value, timestr, sizeof(timestr))) {
        await_message_2("Time set successfully", timestr);
    } else {
        await_message("Time set successfully");
    }

    return true;
}

// Accept an update-pinserver message via qr code
bool handle_update_pinserver_qr(const uint8_t* cbor, const size_t cbor_len)
{
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);

    // Parse cbor
    CborValue root;
    CborValue params;
    CborParser parser;
    if (!bcur_parse_jade_message(cbor, cbor_len, &parser, &root, "update_pinserver", &params)) {
        JADE_LOGE("Failed to parse Jade pinserver message");
        await_error("Error parsing Oracle data");
        return false;
    }

    const char* errmsg = NULL;
    const int errcode = update_pinserver(&params, &errmsg);
    if (errcode) {
        if (errcode != CBOR_RPC_USER_CANCELLED) {
            JADE_LOGE("Error updating pinserver details: %s", errmsg);
            await_error_2("Error updating Oracle", errmsg);
        }
        return false;
    }
    return true;
}

static bool handle_bip39_qr(const uint8_t* cbor, const size_t cbor_len)
{
    char mnemonic[MNEMONIC_BUFLEN];
    SENSITIVE_PUSH(mnemonic, sizeof(mnemonic));
    size_t written = 0;
    bool ret = true;
    if (!bcur_parse_bip39(cbor, cbor_len, mnemonic, sizeof(mnemonic), &written) || written >= sizeof(mnemonic)
        || !handle_mnemonic_qr(mnemonic)) {
        JADE_LOGE("Processing scanned mnemonic data failed");
        await_error("Failed loading wallet");
        ret = false;
    }
    SENSITIVE_POP(mnemonic);
    return ret;
}

// Handle scanning a QR - supports addresses and PSBTs
void handle_scan_qr(const char* title, const char* help_url)
{
    JADE_ASSERT(title);
    JADE_ASSERT(help_url);

    // Scan QR - potentially a BC-UR/multi-frame QR
    char* type = NULL;
    uint8_t* data = NULL;
    size_t data_len = 0;
    if (!bcur_scan_qr(title, &type, &data, &data_len, 0, help_url) || !data) {
        // Scan aborted
        JADE_ASSERT(!type);
        JADE_ASSERT(!data);
        return;
    }

    if (type) {
        // BC-UR scanned - check type string
        if (!strcasecmp(type, BCUR_TYPE_CRYPTO_PSBT)) {
            // PSBT
            if (!parse_sign_display_bcur_psbt_qr(data, data_len)) {
                JADE_LOGE("Processing BC-UR as PSBT failed");
            }
        } else if (!strcasecmp(type, BCUR_TYPE_JADE_BIP8539_REQUEST)) {
            // BIP85/BIP39 entropy request
            if (!handle_bip85_bip39_request_qr(data, data_len)) {
                JADE_LOGE("Processing BC-UR as bip85/bip39 entropy request failed");
            }
        } else if (!strcasecmp(type, BCUR_TYPE_JADE_EPOCH)) {
            // Epoch value
            if (!handle_epoch_qr(data, data_len)) {
                JADE_LOGE("Processing BC-UR as epoch failed");
            }
        } else if (!strcasecmp(type, BCUR_TYPE_JADE_UPDPS)) {
            // Pinserver details
            if (!handle_update_pinserver_qr(data, data_len)) {
                JADE_LOGE("Processing BC-UR as pinserver details failed");
            }
        } else if (!strcasecmp(type, BCUR_TYPE_JADE_MINE)) {
            // Mining template
            if (!handle_mining_qr(data, data_len)) {
                JADE_LOGE("Processing BC-UR as mining template failed");
            }
        } else if (!strcasecmp(type, BCUR_TYPE_CRYPTO_BIP39)) {
            // BIP39 phrase
            if (!handle_bip39_qr(data, data_len)) {
                JADE_LOGE("Processing BC-UR as bip39 phrase failed");
            }
        } else if (!strcasecmp(type, BCUR_TYPE_CRYPTO_OUTPUT)) {
            // Output descriptor (Sparrow wallet export)
            if (!handle_crypto_output_qr(data, data_len)) {
                JADE_LOGE("Processing BC-UR as crypto-output failed");
            }
        } else if (!strcasecmp(type, BCUR_TYPE_BYTES)) {
            // Opaque bytes
            if (!handle_bcur_bytes(data, data_len)) {
                JADE_LOGE("Processing BC-UR BYTES failed");
            }
        } else {
            // Other - unhandled
            JADE_LOGW("Unhandled BC-UR type: %s", type);
            await_error("Unhandled UR message");
        }
    } else {
        // Non-BC-UR (single frame) undifferentiated bytes
        JADE_ASSERT(data[data_len] == '\0');

        // Try address first, otherwise pass to undifferentiated bytes handler
        address_data_t addr_data;
        if (parse_address((const char*)data, &addr_data)) {
            if (!verify_address(&addr_data)) {
                // BBB-AIRGAP: the scanned payload is user data and is not logged.
                JADE_LOGW("Verifying address failed");
            }
        } else if (!handle_qr_bytes(data, data_len)) {
            JADE_LOGW("Unhandled QR (as bytes) message");
        }
    }

    // In either case we need to free the scanned data
    free(type);
    free(data);
}

// BBB-AIRGAP: message signing was written and works, but nothing on this device opened it - it ran
// only when a 'signmessage' code happened to be scanned by the general scan above, or over a host
// connection this image does not build (pijade/host/pijade_host.c).  This is that entry, reached
// from a wallet, and it takes that format alone: a code of any other kind is reported here rather
// than being handled as whatever the general scan would have made of it.
void handle_sign_message(void)
{
    char* type = NULL;
    uint8_t* data = NULL;
    size_t data_len = 0;
    // BBB-AIRGAP: the help address is ours, not blkstrm.com/jadescan.  That page lists the kinds of
    // code a Jade camera reads and sends the reader on to "supported companion apps" for the rest
    // (measured 2026-09-08); it never says how to make one, and this port builds none of them.
    // This screen takes a single format - "signmessage <path> ascii:<text>",
    // main/process/sign_message.c:76 - so the address points at a page of ours that draws exactly
    // that (PIJADE_HELP_SIGN_URL, main/qrmode.h; source docs/sign/index.html).  Measured
    // rather than assumed: the page's own code was run and the code it painted was fed to this
    // camera, which reached the confirm screen and produced a signature
    // (pijade/tools/t4148_help.sh, 2026-09-08).
    if (!bcur_scan_qr("Message QR", &type, &data, &data_len, 0, PIJADE_HELP_SIGN_URL) || !data) {
        // Scan aborted
        JADE_ASSERT(!type);
        JADE_ASSERT(!data);
        return;
    }

    // A single-frame code arrives as the payload itself; a BC-UR one has to be the BYTES type and
    // is unwrapped the same way handle_bcur_bytes() does it.
    const uint8_t* payload = data;
    size_t payload_len = data_len;
    if (type && (strcasecmp(type, BCUR_TYPE_BYTES) || !bcur_parse_bytes(data, data_len, &payload, &payload_len))) {
        JADE_LOGW("Unexpected BC-UR type scanned for message signing: %s", type);
        await_error("Not a message to sign");
    } else if (!is_sign_message_payload((const char*)payload, payload_len)) {
        // BBB-AIRGAP: the scanned payload is user data and is not logged.
        JADE_LOGW("Scanned QR is not a 'signmessage' payload");
        await_error("Not a message to sign");
    } else {
        handle_sign_message_payload((const char*)payload, payload_len);
    }

    free(type);
    free(data);
}

// Populate an Icon with a QR code of text
// Handles up to v6 codes - ie. text up to 134 bytes
// Caller takes ownership of Icon data and must free
static void bytes_to_qr_icon(
    const uint8_t* bytes, const size_t bytes_len, const bool fullscreen, Icon* const qr_icon)
{
    JADE_ASSERT(bytes);
    JADE_ASSERT(bytes_len);
    JADE_ASSERT(bytes_len < MAX_QR_V6_DATA_LEN); // v6, binary
    JADE_ASSERT(qr_icon);

    // Create icon for url
    // For sizes, see: https://www.qrcode.com/en/about/version.html - 'Binary'
    const uint8_t qr_version = bytes_len < MAX_QR_V2_DATA_LEN ? 2 : bytes_len < MAX_QR_V4_DATA_LEN ? 4 : 6;
#if CONFIG_DISPLAY_WIDTH >= 480 && CONFIG_DISPLAY_HEIGHT >= 220
    const uint8_t split_scale_factor = (qr_version == 2 ? 7 : qr_version == 4 ? 6 : 5);
#elif CONFIG_DISPLAY_WIDTH >= 320 && CONFIG_DISPLAY_HEIGHT >= 170
    const uint8_t split_scale_factor = (qr_version == 2 ? 5 : qr_version == 4 ? 4 : 4);
#else
    const uint8_t split_scale_factor = (qr_version == 2 ? 4 : qr_version == 4 ? 3 : 3);
#endif
    // BBB-AIRGAP: codes shown on a screen of their own are scaled to the panel; the ones that share
    // a screen with text keep the scale upstream tuned for that column.
    const uint8_t scale_factor = fullscreen ? qr_fullscreen_scale_factor(qr_version) : split_scale_factor;

    // Convert url to qr code, then to Icon
    QRCode qrcode;
    uint8_t qrbuffer[256]; // opaque work area
    JADE_ASSERT(qrcode_getBufferSize(qr_version) <= sizeof(qrbuffer));
    const int qret = qrcode_initBytes(&qrcode, qrbuffer, qr_version, ECC_LOW, (uint8_t*)bytes, bytes_len);
    JADE_ASSERT(qret == 0);

    qrcode_toIcon(&qrcode, qr_icon, scale_factor);
}

// Display a BC-UR bytes message
bool display_bcur_bytes_qr(
    const char* message[], const size_t message_size, const uint8_t* data, const size_t data_len, const char* help_url)
{
    JADE_ASSERT(message);
    JADE_ASSERT(message_size);
    JADE_ASSERT(data);
    JADE_ASSERT(data_len);

    // Build BCUR message holding the bytes
    uint8_t* cbor = NULL;
    size_t cbor_len = 0;
    if (!bcur_build_cbor_bytes(data, data_len, &cbor, &cbor_len)) {
        JADE_LOGW("Failed to build cbor bytes message");
        return false;
    }

    // Now display bcur QR
    display_bcur_qr(message, message_size, BCUR_TYPE_BYTES, cbor, cbor_len, help_url);

    free(cbor);
    return true;
}

void await_single_qr_activity(
    const char* message[], const size_t message_size, const uint8_t* data, const size_t data_len)
{
    JADE_ASSERT(message);
    JADE_ASSERT(message_size);
    JADE_ASSERT(data);
    JADE_ASSERT(data_len);

    const bool show_options_button = false;
    gui_activity_t* const act = make_show_qr_activity(message, message_size, show_options_button);
    bool code_shown = false;

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            break;
        }
        gui_set_current_activity(act);

        const int32_t ev_id = wait_export_screen_button(act, BTN_QR_DISPLAY_EXIT, &code_shown);
        if (ev_id == BTN_QR_SHOW_FULLSCREEN) {
            // BBB-AIRGAP: note the activity takes ownership of the icon
            Icon* const qr_icon = JADE_MALLOC(sizeof(Icon));
            const bool fullscreen = true;
            bytes_to_qr_icon(data, data_len, fullscreen, qr_icon);
            display_fullscreen_qr(act, NULL, qr_icon, 1, 0);
        } else if (ev_id == BTN_QR_DISPLAY_EXIT) {
            // Done
            break;
        } else if (ev_id == BTN_QR_BRIGHTNESS) {
            gui_next_qrcode_color();
            gui_repaint(act->root_node);
        }
    }
}

// Put an explicit \n before the last part of the url
static void add_cr_after_last_slash(const char* url, char* output, const size_t output_len)
{
    JADE_ASSERT(url);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len);

    const size_t url_len = strlen(url);
    JADE_ASSERT(output_len >= url_len + 2); // url + new \n +_ trailing \0

    // Find last '/' character
    const char* last_slash = url;
    while (true) {
        const char* next_slash = strchr(last_slash + 1, '/');
        if (!next_slash) {
            break;
        } else {
            last_slash = next_slash;
        }
    }
    JADE_ASSERT(last_slash);

    const size_t index = last_slash - url;
    JADE_ASSERT(index < url_len);

    strncpy(output, url, index + 1); // up to and including the '/'
    output[index + 1] = '\n'; // explict line-break
    strcpy(output + index + 2, url + index + 1);
}

bool show_otp_secret_text_activity(const otpauth_ctx_t* otp_ctx)
{
    JADE_ASSERT(otp_ctx);

    size_t num_words = 0;
    size_t words_len = 0;
    char secret_display[256];
    SENSITIVE_PUSH(secret_display, sizeof(secret_display));

    split_text(otp_ctx->secret, otp_ctx->secret_len, OTP_TEXTSPLITLEN, secret_display, sizeof(secret_display),
        &num_words, &words_len);
    JADE_ASSERT(num_words <= MAX_OTP_SCREENS * OTP_GRID_SIZE);
    JADE_ASSERT(words_len <= sizeof(secret_display));

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_BACK },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    const char* remaining_words = NULL;
    gui_activity_t* const act = make_text_grid_activity("Secret Key", hdrbtns, 2, OTP_GRID_TOPPAD, OTP_GRID_X,
        OTP_GRID_Y, secret_display, num_words, GUI_DEFAULT_FONT, &remaining_words);
    JADE_ASSERT(remaining_words == secret_display + words_len);
    gui_set_current_activity(act);

    int32_t ev_id;
    while (gui_activity_wait_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            break;
        }

        if (ev_id == BTN_BACK) {
            break;
        }
    }

    SENSITIVE_POP(secret_display);
    return true;
}

bool show_otp_uri_qr_activity(const otpauth_ctx_t* otp_ctx)
{
    JADE_ASSERT(otp_ctx);
    JADE_ASSERT(otp_ctx->name);

    bool ok = false;
    char uri[OTP_MAX_URI_LEN];
    Icon* qr_icon = NULL;
    size_t written = 0;

    SENSITIVE_PUSH(uri, sizeof(uri));

    if (!otp_load_uri(otp_ctx->name, uri, sizeof(uri), &written) || !written) {
        await_error_2("Failed to load", "OTP URI");
        goto cleanup;
    }

    if (written >= MAX_QR_V6_DATA_LEN) {
        await_error_2("URI too long", "for QR");
        goto cleanup;
    }

    qr_icon = JADE_MALLOC(sizeof(Icon));
    bytes_to_qr_icon((const uint8_t*)uri, written, false, qr_icon);
    JADE_ASSERT(qr_icon->data && qr_icon->width && qr_icon->height);
    SENSITIVE_PUSH(qr_icon->data, qrcode_get_icon_data_size(qr_icon->width, qr_icon->height));

    gui_activity_t* const act = make_show_otp_qr_actvity(otp_ctx->name, qr_icon);
    int32_t ev_id;

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            ok = true;
            goto cleanup;
        }

        gui_set_current_activity(act);
        if (gui_activity_wait_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
            if (ev_id == BTN_BACK) {
                ok = true;
                goto cleanup;
            } else if (ev_id == BTN_OTP_DETAILS_SECRET) {
                show_otp_secret_text_activity(otp_ctx);
            } else if (ev_id == BTN_QR_BRIGHTNESS) {
                gui_next_qrcode_color();
                gui_repaint(act->root_node);
            }
        }
    }
cleanup:
    if (qr_icon) {
        // The GUI node owns the icon: make sure we clear its
        // data here before the activity is freed.
        SENSITIVE_POP(qr_icon->data);
    }
    SENSITIVE_POP(uri);
    return ok;
}

// Display screen with help url and qr code
// Handles up to v4 codes - ie. text up to 78 bytes
void await_qr_help_activity(const char* url)
{
    JADE_ASSERT(url);

    const size_t url_len = strlen(url);
    JADE_ASSERT(url_len < MAX_QR_V4_DATA_LEN); // v4, binary

    Icon* const qr_icon = JADE_MALLOC(sizeof(Icon));
    bytes_to_qr_icon((const uint8_t*)url, url_len, false, qr_icon);

    // Put an explicit \n before the last part of the url
    char url_with_crlf[MAX_QR_V4_DATA_LEN + 2]; // new \n and trailing \0
    add_cr_after_last_slash(url, url_with_crlf, sizeof(url_with_crlf));

    gui_activity_t* const prev_act = gui_current_activity(); // Save current activity

    // Show, and await button click - note gui takes ownership of icon
    gui_activity_t* const act = make_show_qr_help_activity(url_with_crlf, qr_icon);
    gui_set_current_activity(act);

    // Show, and await button click
    while (true) {
        const int32_t ev_id = gui_activity_wait_button(act, BTN_QR_HELP_EXIT);
        if (ev_id == BTN_QR_BRIGHTNESS) {
            gui_next_qrcode_color();
            gui_repaint(act->root_node);
        } else if (ev_id == BTN_QR_HELP_EXIT || ev_id == BTN_ESCAPE_HOME) {
            // Done.  BBB-AIRGAP: KEY3 leaves through the same exit, so the activity below is
            // still destroyed and the previous screen restored before the escape carries on.
            break;
        }
    }
    gui_destroy_current_activity(act, prev_act); // restore previous activity
}

// Display screen with help url and qr code
bool await_qr_back_continue_activity(
    const char* message[], const size_t message_size, const char* url, const bool default_selection)
{
    JADE_ASSERT(message);
    JADE_ASSERT(message_size);
    JADE_ASSERT(url);

    const size_t url_len = strlen(url);
    JADE_ASSERT(url_len < MAX_QR_V4_DATA_LEN); // v4, binary

    Icon* const qr_icon = JADE_MALLOC(sizeof(Icon));
    bytes_to_qr_icon((const uint8_t*)url, url_len, false, qr_icon);

    // Show, and await button click
    gui_activity_t* const act = make_qr_back_continue_activity(message, message_size, url, qr_icon, default_selection);
    gui_set_current_activity(act);

    // Show, and await button click
    while (true) {
        const int32_t ev_id = gui_activity_wait_button(act, BTN_YES);
        if (ev_id == BTN_QR_BRIGHTNESS) {
            gui_next_qrcode_color();
            gui_repaint(act->root_node);
        } else if (ev_id == BTN_YES || ev_id == BTN_NO || ev_id == BTN_ESCAPE_HOME) {
            // Done: return whether 'Continue' was cicked.
            // BBB-AIRGAP: KEY3 leaves through 'back', never through 'Continue'.
            return ev_id == BTN_YES;
        }
    }
}

// QR-Mode PinServer interaction

// Create and post a 'cancel' message
static bool post_cancel_message(const jade_msg_source_t source)
{
    uint8_t cbor_buf[32 + 1];
    CborEncoder root_encoder;
    cbor_encoder_init(&root_encoder, cbor_buf + 1, sizeof(cbor_buf) - 1, 0);

    CborEncoder root_map_encoder; // id, method
    CborError cberr = cbor_encoder_create_map(&root_encoder, &root_map_encoder, 2);
    JADE_ASSERT(cberr == CborNoError);
    add_string_to_map(&root_map_encoder, "id", "qrcancel");
    add_string_to_map(&root_map_encoder, "method", "cancel");
    cberr = cbor_encoder_close_container(&root_encoder, &root_map_encoder);
    JADE_ASSERT(cberr == CborNoError);

    const size_t cbor_len = cbor_encoder_get_buffer_size(&root_encoder, cbor_buf + 1);
    cbor_buf[0] = source;
    return jade_process_push_in_message(cbor_buf, cbor_len + 1);
}

// Locally create and post an 'auth_user' request
static bool post_auth_msg_request(const jade_msg_source_t source, const bool suppress_pin_change_confirmation)
{
    uint8_t cbor_buf[96 + 1];
    CborEncoder root_encoder;
    cbor_encoder_init(&root_encoder, cbor_buf + 1, sizeof(cbor_buf) - 1, 0);

    CborEncoder root_map_encoder; // id, method, params
    CborError cberr = cbor_encoder_create_map(&root_encoder, &root_map_encoder, 3);
    JADE_ASSERT(cberr == CborNoError);
    add_string_to_map(&root_map_encoder, "id", "qrauth");
    add_string_to_map(&root_map_encoder, "method", "auth_user");

    // Add parameters (ie. network)
    cberr = cbor_encode_text_stringz(&root_map_encoder, "params");
    JADE_ASSERT(cberr == CborNoError);

    CborEncoder params_encoder; // network
    cberr = cbor_encoder_create_map(&root_map_encoder, &params_encoder, 2);
    JADE_ASSERT(cberr == CborNoError);
    const network_type_t restriction = keychain_get_network_type_restriction();
    network_t network_id;
    if (restriction == NETWORK_TYPE_TEST) {
        network_id = NETWORK_BITCOIN_TESTNET;
    } else {
        network_id = NETWORK_BITCOIN;
    }
    add_string_to_map(&params_encoder, "network", network_to_name(network_id));
    add_boolean_to_map(&params_encoder, "suppress_pin_change_confirmation", suppress_pin_change_confirmation);
    cberr = cbor_encoder_close_container(&root_map_encoder, &params_encoder);
    JADE_ASSERT(cberr == CborNoError);

    cberr = cbor_encoder_close_container(&root_encoder, &root_map_encoder);
    JADE_ASSERT(cberr == CborNoError);

    const size_t cbor_len = cbor_encoder_get_buffer_size(&root_encoder, cbor_buf + 1);
    cbor_buf[0] = source;
    return jade_process_push_in_message(cbor_buf, cbor_len + 1);
}

// Scan a bcur QR code, and post it into Jade with SOURCE_INTERNAL
static bool scan_qr_post_in_message(const char* label, const char* expected_type)
{
    JADE_ASSERT(label);
    JADE_ASSERT(expected_type);

    char* type = NULL;
    uint8_t* data = NULL;
    size_t data_len = 0;
    bool ret = false;

    // NOTE: we take ownership of 'type' and 'data'
    const uint32_t offset = 1; // Allow for a prefix message source byte
    if (!bcur_scan_qr(label, &type, &data, &data_len, offset, "blkstrm.com/qrpin")) {
        JADE_LOGI("QR scanning failed or abandoned");
        return false;
    }

    // Check if a non-bc-ur code frame was scanned
    if (!type) {
        JADE_LOGW("Scanning encountered a non-BC-UR QR code, when expecting BC-UR type %s", expected_type);
        await_error("Unexpected QR payload");
        goto cleanup;
    }

    // Check the type is as expected
    if (strcasecmp(expected_type, type)) {
        JADE_LOGW("Scanning returned unexpected type %s when expecting %s", type, expected_type);
        await_error("Unexpected QR payload type");
        goto cleanup;
    }

    // Post as message into Jade with source-qr prefix
    data[0] = SOURCE_INTERNAL;
    ret = jade_process_push_in_message(data, data_len);

cleanup:
    free(data);
    free(type);
    return ret;
}

// NOTE: this 'writer' callback must return true to indicate that it has taken the message
// (whether valid/expected or not), and it does not want to wait to be presented with another message.
// ie. the return indicates processing has finished, not that processing was necessarily successful.
// (That information is returned in the context object.)
// NOTE: the presence of a label messages indicate we want to display the payload as a QR
static bool handle_jade_reply_http_request_show_qr(const char* message[], const size_t message_size, const uint8_t* msg,
    const size_t len, void* ctx, const char* help_url)
{
    // message and message_size are optional, but must be consistent
    JADE_ASSERT(!message_size || message);
    JADE_ASSERT(msg);
    JADE_ASSERT(len);
    JADE_ASSERT(ctx);
    // help_url is optional

    bool* const ok = (bool*)ctx;
    *ok = false;

    // Parse the received message
    CborParser parser;
    CborValue root;
    const CborError cberr = cbor_parser_init(msg, len, CborValidateBasic, &parser, &root);
    if (cberr != CborNoError || !rpc_message_valid(&root)) {
        JADE_LOGE("Invalid cbor message");
        goto cleanup;
    }

    // Ultimate response is boolean
    bool bool_result = false;
    if (rpc_get_bool("result", &root, &bool_result)) {
        JADE_LOGI("Boolean result: %u", bool_result);
        goto cleanup;
    }

    CborValue result;
    CborValue http_request;
    if (!rpc_get_map("result", &root, &result) || !rpc_get_map("http_request", &result, &http_request)) {
        JADE_LOGE("Unexpected cbor message - no 'http_request' result payload");
        goto cleanup;
    }

    // Display message as bcur qr if a screen label was passed
    if (message_size) {
        display_bcur_qr(message, message_size, BCUR_TYPE_JADE_PIN, msg, len, help_url);
    }

    // Message received and QR displayed successfully
    *ok = true;

cleanup:
    // We return true in all cases to indicate that a message was received
    // and we should stop waiting - whether the message was processed 'successfully'
    // is indicated by the 'ok' flag in the passed context object.
    return true;
}

static bool handle_jade_reply_pinserver_request(const uint8_t* msg, const size_t len, void* ctx)
{
    const char* message[] = { "Step 1/2", "Scan Jade", "QR" };
    return handle_jade_reply_http_request_show_qr(message, 3, msg, len, ctx, "blkstrm.com/qrpin");
}

static bool handle_outbound_reply(outbound_message_writer_fn_t handler)
{
    JADE_ASSERT(handler);

    // Await message from Jade to pinserver
    bool ok = false;
    while (!jade_process_get_out_message(handler, SOURCE_INTERNAL, &ok)) {
        // Await outbound message
    }
    return ok;
}

// This task is run to act as a client to Jade's normal 'auth-user' processing
static void auth_qr_client_task(void* ctx)
{
    JADE_LOGI("Starting Auth QR client task: %d with ctx %p", xPortGetFreeHeapSize(), ctx);

    // NOTE: we just use ctx being NULL or not to convey boolean
    const bool suppress_pin_change_confirmation = (ctx != NULL);

    // Only needed/expected for 'full' inititialistion with pinserver
    JADE_ASSERT(!keychain_has_temporary());

    // Drain any old messages sitting on the QR queue
    while (jade_process_get_out_message(NULL, SOURCE_INTERNAL, NULL)) {
        JADE_LOGW("Discarded stale message from QR queue");
    }

    // Post in a synthesized 'auth_user' message
    JADE_LOGI("Posting initial auth_user message");
    if (!post_auth_msg_request(SOURCE_INTERNAL, suppress_pin_change_confirmation)) {
        JADE_LOGW("Failed to post initial auth_user message");
        goto cleanup;
    }

    // Wait for message (from synthesized auth_user/pinclient processing)
    // and display the message payload as bcur QR code on screen.
    JADE_LOGI("Awaiting auth_user reply data to display as qr");
    while (handle_outbound_reply(handle_jade_reply_pinserver_request)) {
        // Scan qr code and post back to auth_user/pinclient task
        // 'pin'
        JADE_LOGI("Scanning/posting 'pin' data");
        if (!scan_qr_post_in_message("Web QR 2/2", BCUR_TYPE_JADE_PIN)) {
            JADE_LOGW("Failed to scan pin message");
            goto cleanup;
        }
    }

    JADE_LOGI("Complete");

cleanup:
    // Post a cancel message which should ensure the main dashboard task returns
    // (it will be ignored if not required)
    post_cancel_message(SOURCE_INTERNAL);

    // Log the task stack HWM so we can estimate ideal stack size
    JADE_LOGI("Auth QR client task complete - task stack HWM: %u free", uxTaskGetStackHighWaterMark(NULL));

    // Delete this task
    vTaskDelete(NULL);
}

void handle_qr_auth(const bool suppress_pin_change_confirmation)
{
    // Only needed/expected for 'full' inititialistion with pinserver
    JADE_ASSERT(!keychain_has_temporary());

    // Start a task to run the qr client side
    TaskHandle_t auth_qr_client_task_handle;
    const BaseType_t retval = xTaskCreatePinnedToCore(&auth_qr_client_task, "auth_qr_client_task", 4 * 1024,
        (void*)suppress_pin_change_confirmation, JADE_TASK_PRIO_GUI, &auth_qr_client_task_handle, JADE_CORE_SECONDARY);
    JADE_ASSERT_MSG(
        retval == pdPASS, "Failed to create auth_qr_client_task, xTaskCreatePinnedToCore() returned %d", retval);

    // Then we return to the dispatcher to handle messages as sent by the task we have just started
}
#endif // AMALGAMATED_BUILD
