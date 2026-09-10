#ifndef BUTTON_EVENTS_H_
#define BUTTON_EVENTS_H_
#include "sdkconfig.h"
typedef enum {
    BTN_EVENT_TIMEOUT,

    BTN_BACK,
    BTN_HELP,
    BTN_YES,
    BTN_NO,
    BTN_QR_HELP_EXIT,
    BTN_QR_BRIGHTNESS,

    BTN_CAMERA_HELP,
    BTN_CAMERA_CLICK,
    BTN_CAMERA_EXIT,

    BTN_SIGNTX_ADDRESS,
    BTN_SIGNTX_TICKERAMOUNT,
    BTN_SIGNTX_ASSETINFO,
    BTN_SIGNTX_ASSETINFO_NEXT,
    BTN_SIGNTX_ASSETINFO_DONE,
    BTN_SIGNTX_WARNING,
    BTN_SIGNTX_REJECT,
    BTN_SIGNTX_ACCEPT,

    BTN_TX_SCREEN_NEXT,
    BTN_TX_SCREEN_PREV,
    BTN_TX_SCREEN_EXIT,

    BTN_SIGNMSG_MSG,
    BTN_SIGNMSG_HASH,
    BTN_SIGNMSG_PATH,
    BTN_SIGNMSG_NEXT,
    BTN_SIGNMSG_BACK,
    BTN_SIGNMSG_REJECT,
    BTN_SIGNMSG_ACCEPT,

    BTN_SIGNIDENTITY_REJECT,
    BTN_SIGNIDENTITY_ACCEPT,

    BTN_CANCEL_SIGNATURE,
    BTN_ACCEPT_SIGNATURE,

    BTN_ADDRESS_NEXT,
    BTN_ADDRESS_REJECT,
    BTN_ADDRESS_ACCEPT,

    BTN_OTA_VIEW_CURRENT_VERSION,
    BTN_OTA_VIEW_NEW_VERSION,
    BTN_OTA_VIEW_FW_HASH,
    BTN_OTA_REJECT,
    BTN_OTA_ACCEPT,

    BTN_BLE_CONFIRM,
    BTN_BLE_DENY,

    BTN_MNEMONIC_TYPE,
    BTN_MNEMONIC_ADVANCED,
    BTN_MNEMONIC_METHOD,

    BTN_NEW_MNEMONIC,
    BTN_NEW_MNEMONIC_12,
    BTN_NEW_MNEMONIC_24,
    // BBB-AIRGAP: user-supplied entropy sources
    BTN_NEW_MNEMONIC_SOURCE,
    BTN_NEW_MNEMONIC_DEVICE,
    BTN_NEW_MNEMONIC_DICE,
    BTN_NEW_MNEMONIC_CAMERA,
    BTN_NEW_MNEMONIC_COMBINED,

    BTN_MNEMONIC_PREV,
    BTN_MNEMONIC_NEXT,
    BTN_MNEMONIC_EXIT,
    BTN_MNEMONIC_VERIFY,

    BTN_RESTORE_MNEMONIC,
    BTN_RESTORE_MNEMONIC_12,
    BTN_RESTORE_MNEMONIC_24,
    BTN_RESTORE_MNEMONIC_QR,

    BTN_MNEMONIC_FINAL_WORD_EXISTING,
    BTN_MNEMONIC_FINAL_WORD_CALCULATE,
    BTN_MNEMONIC_FINAL_WORD_HELP,

    BTN_QR_EXPORT_PREV,
    BTN_QR_EXPORT_NEXT,
    BTN_QR_EXPORT_CONTEXT,
    BTN_QR_EXPORT_DONE,

    BTN_PASSPHRASE_ONCE,
    BTN_PASSPHRASE_ALWAYS,
    BTN_PASSPHRASE_NEVER,
    BTN_PASSPHRASE_FREQUENCY,
    BTN_PASSPHRASE_METHOD,
    BTN_PASSPHRASE_HELP,
    BTN_PASSPHRASE_EXIT,

    BTN_BIP85_12_WORDS,
    BTN_BIP85_24_WORDS,
    BTN_BIP85_CONTINUE,
    BTN_BIP85_EXIT,

    BTN_INITIALIZE,
    BTN_SCAN_SEEDQR,
    BTN_SETTINGS,
    BTN_SCAN_QR,
    BTN_SESSION,

    // BBB-AIRGAP: one id per wallet held, so a click says which one was picked. The ids after
    // the first are reserved for the rest of the table; handle_session() asserts the range is
    // wide enough for MAX_SEED_SLOTS rather than letting them run into the ones below.
    BTN_SESSION_SEED_0,
    BTN_SESSION_SEED_LAST = BTN_SESSION_SEED_0 + 7,
    BTN_SESSION_LOGOUT,
    BTN_SESSION_SLEEP,
    BTN_SESSION_EXIT,

    BTN_CONNECT_TO_BACK,

    BTN_CONNECT_SELECT_BACK,
    BTN_CONNECT_VIA_USB,
    BTN_CONNECT_VIA_BLE,
    BTN_CONNECT_VIA_QR,
    BTN_CONNECT_BACK,
    BTN_CONNECT_HELP,

    BTN_CONNECT_QR_BACK,
    BTN_CONNECT_QR_HELP,
    BTN_CONNECT_QR_PIN,
    BTN_CONNECT_QR_SCAN,

    BTN_QR_MODE,
    BTN_CONNECT,

    BTN_SETTINGS_TOGGLE_ORIENTATION,
    BTN_SETTINGS_WALLET,
    BTN_SETTINGS_WALLET_EXIT,
    BTN_SETTINGS_WALLET_FORGET,
    BTN_SETTINGS_WALLET_SCAN_QR,
    // BBB-AIRGAP: lists the wallet's own addresses, so one can be checked against the address a
    // watch-only wallet is showing without scanning it first.  See handle_address_explorer().
    BTN_SETTINGS_WALLET_ADDRESSES,
    // BBB-AIRGAP: scans a message to sign with this wallet.  See handle_sign_message().
    BTN_SETTINGS_WALLET_SIGN_MSG,
    // BBB-AIRGAP: the backup sub-menu under a wallet - words, SeedQR, and the verification quiz
    BTN_SETTINGS_WALLET_BACKUP,
    BTN_WALLET_BACKUP_EXIT,
    BTN_WALLET_BACKUP_VIEW,
    BTN_WALLET_BACKUP_SEEDQR,
    BTN_WALLET_BACKUP_VERIFY,
    // BBB-AIRGAP: load a second wallet into a free slot from the Options list.  Upstream had no
    // such row because only one wallet fitted in memory; the slot table makes it an operation the
    // menu can name rather than a side effect of the generic QR scanner.
    BTN_SETTINGS_ADD_WALLET,

    // BBB-AIRGAP: the PIN and passphrase screens, gathered under one row of the Options list.
    // Upstream split them between an 'Authentication' menu that only existed while unlocked and
    // the preferences list, so the same screen sat at two addresses depending on device state.
    BTN_SETTINGS_SECURITY,
    BTN_SETTINGS_SECURITY_EXIT,
    BTN_SETTINGS_PREFS,
    BTN_SETTINGS_PREFS_EXIT,
    BTN_SETTINGS_DISPLAY,
    BTN_SETTINGS_QR, // BBB-AIRGAP: qr density/speed, reached from the settings menu
    // BBB-AIRGAP: which optional wallet features are offered, reached from the settings menu.
    // See handle_wallet_options() and storage_get_feature_flags().  One id per row rather than a
    // shared toggle id, because the rows are laid out from a table and the handler needs to know
    // which flag was picked; the same shape as BTN_SESSION_SEED_0 above.
    BTN_SETTINGS_FEATURES,
    BTN_SETTINGS_FEATURES_EXIT,
    BTN_FEATURE_ROW_0,
    BTN_FEATURE_ROW_LAST = BTN_FEATURE_ROW_0 + 6,
    BTN_SETTINGS_DISPLAY_EXIT,
    BTN_SETTINGS_DISPLAY_BRIGHTNESS,
    BTN_SETTINGS_DISPLAY_ORIENTATION,
    BTN_SETTINGS_DISPLAY_CAMERA_ROTATION, // BBB-AIRGAP: see HAVE_CAMERA_ROTATION_SETTING
    BTN_SETTINGS_DISPLAY_THEME,
    BTN_SETTINGS_XPUB_EXPORT,
    BTN_SETTINGS_QR_PINSERVER,
    BTN_SETTINGS_TEMPORARY_WALLET_LOGIN,
    BTN_SETTINGS_PINSERVER,
    BTN_SETTINGS_PINSERVER_SHOW,
#ifdef CONFIG_HAS_CAMERA
    BTN_SETTINGS_PINSERVER_SCAN_QR,
#endif
    BTN_SETTINGS_PINSERVER_RESET,
    BTN_SETTINGS_PINSERVER_HELP,
    BTN_SETTINGS_PINSERVER_EXIT,
    BTN_SETTINGS_INFO,
    BTN_SETTINGS_INFO_EXIT,
    BTN_SETTINGS_INFO_FWVERSION,
    BTN_SETTINGS_INFO_FWVERSION_EXIT,
    BTN_SETTINGS_DEVICE_INFO,
    BTN_SETTINGS_DEVICE_INFO_MAC,
#ifdef CONFIG_HAS_BATTERY
    BTN_SETTINGS_DEVICE_INFO_BATTERY,
#endif
    BTN_SETTINGS_DEVICE_INFO_STORAGE,
    BTN_SETTINGS_DEVICE_INFO_STORAGE_EXIT,
    BTN_SETTINGS_DEVICE_INFO_DETAIL_EXIT,
    BTN_SETTINGS_DEVICE_INFO_EXIT,
    BTN_SETTINGS_LEGAL,

    // BBB-AIRGAP: hardware checks reached from the Info menu - see handle_io_test(),
    // main/process/dashboard.c
    BTN_SETTINGS_IO_TEST,
    BTN_IO_TEST_EXIT,
    BTN_IO_TEST_SCREEN,
    BTN_IO_TEST_CAMERA,
    BTN_IO_TEST_BUTTONS,
    BTN_SETTINGS_IDLE_TIMEOUT,
    BTN_SETTINGS_SCREEN_TIMEOUT, // BBB-AIRGAP: screen dimming threshold
    BTN_SETTINGS_REGISTERED_WALLETS,
    BTN_SETTINGS_WALLET_ERASE_PIN,
    BTN_SETTINGS_BIP39_PASSPHRASE,
    BTN_SETTINGS_BIP85,
    BTN_SETTINGS_OTP,
    BTN_SETTINGS_OTP_VIEW,
    // BBB-AIRGAP: the Pi has no RTC, so the clock has to be set once per boot before any
    // time-based code is correct; this row puts that step next to the codes it serves.
    BTN_SETTINGS_OTP_SET_CLOCK,
    BTN_SETTINGS_OTP_NEW,
#ifdef CONFIG_HAS_CAMERA
    BTN_SETTINGS_OTP_NEW_QR,
#endif
    BTN_SETTINGS_OTP_NEW_KB,
    BTN_SETTINGS_OTP_NEW_EXIT,
    BTN_SETTINGS_OTP_HELP,
    BTN_SETTINGS_OTP_EXIT,
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_HAS_BATTERY)
    BTN_SETTINGS_USBSTORAGE,
    BTN_SETTINGS_USBSTORAGE_FW,
    BTN_SETTINGS_USBSTORAGE_BACK,
    BTN_SETTINGS_USBSTORAGE_HELP,
    BTN_SETTINGS_USBSTORAGE_SIGN,
    BTN_SETTINGS_USBSTORAGE_EXPORT_XPUB,
    BTN_SETTINGS_USBSTORAGE_EXPORT_XPUB_OPTIONS,
    BTN_SETTINGS_USBSTORAGE_EXPORT_XPUB_ACTION,
    BTN_SETTINGS_EXPORT_XPUB_BACK,
    BTN_SETTINGS_EXPORT_XPUB_EXIT,
    BTN_SETTINGS_USBSTORAGE_EXIT,
#endif
    BTN_SETTINGS_BLE,
    BTN_SETTINGS_NETWORK_TYPE,
    BTN_SETTINGS_CHANGE_PIN,
#ifdef CONFIG_HAS_CAMERA
    BTN_SETTINGS_CHANGE_PIN_QR,
#endif
    BTN_SETTINGS_RESET,
    BTN_SETTINGS_EXIT,

    BTN_WALLET_ERASE_PIN_SET,
    BTN_WALLET_ERASE_PIN_DISABLE,
    BTN_WALLET_ERASE_PIN_HELP,
    BTN_WALLET_ERASE_PIN_EXIT,

    BTN_BLE_STATUS,
    BTN_BLE_RESET_PAIRING,
    BTN_BLE_HELP,
    BTN_BLE_EXIT,

    BTN_LEGAL_NEXT,
    BTN_LEGAL_PREV,
    BTN_LEGAL_EXIT,

    BTN_PINSERVER_DETAILS_URL_A,
    BTN_PINSERVER_DETAILS_URL_B,
    BTN_PINSERVER_DETAILS_PUBKEY,
    BTN_PINSERVER_DETAILS_CERT_HASH,
    BTN_PINSERVER_DETAILS_RETAIN_CONFIRM,
    BTN_PINSERVER_DETAILS_DISCARD_DELETE,

    BTN_VIEW_WALLET,
    BTN_EXPORT_WALLET,
    BTN_DELETE_WALLET,

    BTN_SIGNER_PREV,
    BTN_SIGNER_NEXT,
    BTN_SIGNER_FINGERPRINT,
    BTN_SIGNER_DERIVATION,
    BTN_SIGNER_XPUB,
    BTN_SIGNER_XPUB_NEXT,
    BTN_SIGNER_PATH,

    BTN_MULTISIG_NAME,
    BTN_MULTISIG_TYPE,
    BTN_MULTISIG_SORTED,
    BTN_MULTISIG_BLINDINGKEY,
    BTN_MULTISIG_RETAIN_CONFIRM,
    BTN_MULTISIG_DISCARD_DELETE,

    BTN_DESCRIPTOR_NAME,
    BTN_DESCRIPTOR_SCRIPT,
    BTN_DESCRIPTOR_SCRIPT_NEXT,
    BTN_DESCRIPTOR_RETAIN_CONFIRM,
    BTN_DESCRIPTOR_DISCARD_DELETE,

    BTN_OTP_NAME,
    BTN_OTP_LABEL,
    BTN_OTP_ISSUER,
    BTN_OTP_TYPE,
    BTN_OTP_DETAILS,
    BTN_OTP_DETAILS_VIEW,
    BTN_OTP_DETAILS_EXPORT,
    BTN_OTP_DETAILS_SECRET,
    BTN_OTP_RETAIN_CONFIRM,
    BTN_OTP_DISCARD_DELETE,

    BTN_XPUB_OPTIONS,
    BTN_XPUB_OPTIONS_HELP,
    BTN_XPUB_OPTIONS_EXIT,
    BTN_XPUB_OPTIONS_SCRIPTTYPE,
    BTN_XPUB_OPTIONS_WALLETTYPE,
    BTN_XPUB_OPTIONS_ACCOUNT,
    BTN_XPUB_OPTIONS_QR, // BBB-AIRGAP: density/speed screen, reachable from the xpub menu too
    BTN_XPUB_EXIT,

    BTN_SCAN_ADDRESS_SKIP_ADDRESSES,
    BTN_SCAN_ADDRESS_EXIT,

    BTN_SCAN_ADDRESS_OPTIONS,
    BTN_SCAN_ADDRESS_OPTIONS_CHANGE,
    BTN_SCAN_ADDRESS_OPTIONS_ACCOUNT,
    // BBB-AIRGAP: only the explorer offers this row.  The verify flow reads the script type off
    // the address being checked, so there is nothing to choose there.
    BTN_SCAN_ADDRESS_OPTIONS_SCRIPTTYPE,
    BTN_SCAN_ADDRESS_OPTIONS_EXIT,

    // BBB-AIRGAP: the address explorer.  One id per address on a page, so a click says which
    // address was picked; the ids after the first are reserved for the rest of the page, and
    // handle_address_explorer() asserts the range is wide enough rather than letting them run
    // into the ones below.
    BTN_ADDR_EXPLORER_ROW_0,
    BTN_ADDR_EXPLORER_ROW_LAST = BTN_ADDR_EXPLORER_ROW_0 + 9,
    BTN_ADDR_EXPLORER_PREV,
    BTN_ADDR_EXPLORER_NEXT,
    BTN_ADDR_EXPLORER_OPTIONS,
    // BBB-AIRGAP: the explorer opens on a menu naming what it is about to list, rather than
    // dropping straight into receive addresses with the change setting buried in the options.
    BTN_ADDR_EXPLORER_RECEIVE,
    BTN_ADDR_EXPLORER_CHANGE,
    BTN_ADDR_EXPLORER_EXIT,

    BTN_QR_OPTIONS,
    BTN_QR_OPTIONS_DENSITY,
    BTN_QR_OPTIONS_FRAMERATE,
    BTN_QR_OPTIONS_HELP,
    BTN_QR_OPTIONS_EXIT,
    BTN_QR_DISPLAY_EXIT,
    BTN_MINING_STOP,
    // BBB-AIRGAP: the mining menu (Options > Mining) and its two rows. Upstream drives
    // mining over the serial link and so has no menu at all; on this port it is the only way in
    // that does not depend on a code turning up in the general scan.
    BTN_SETTINGS_MINING,
    BTN_MINING_START,
    BTN_MINING_REWARD,
    BTN_MINING_EXIT,

    // BBB-AIRGAP: wallet QRs are shown on their own screen, reached from the screen that
    // describes what is being exported. See display_fullscreen_qr() in main/qrmode.c.
    BTN_QR_SHOW_FULLSCREEN,
    BTN_QR_FULLSCREEN_EXIT,

    // BBB-AIRGAP: the four physical rows of a scrolling list. The number of logical items is
    // independent of these: run_list_activity() moves a window over the items and rewrites the
    // row labels, so a list of any length still uses these four ids. See main/ui/dialogs.c.
    BTN_LIST_ROW_0,
    BTN_LIST_ROW_1,
    BTN_LIST_ROW_2,
    BTN_LIST_ROW_3,

    // BBB-AIRGAP: what gui_activity_wait_event() reports to a GUI_BUTTON_EVENT waiter when the
    // user asks to leave the screen with KEY3.  It has to live in this enum rather than be the
    // raw GUI_ALT_EVENT because the two enums both start at zero and overlap: GUI_ALT_EVENT is 6,
    // which is BTN_QR_BRIGHTNESS here, and GUI_FRONT_CLICK_EVENT is 3, which is BTN_YES.  Handing
    // a gui_event_t id straight to a button loop would fire whatever button happens to share the
    // number, up to and including an accidental confirmation.
    BTN_ESCAPE_HOME,

    // NOTE: Always leave these ones last as keyboard buttons use
    // BTN_KEYBOARD_ASCII_OFFSET + <ascii-value>
    BTN_KEYBOARD_BACKSPACE,
    BTN_KEYBOARD_ENTER,
    BTN_KEYBOARD_SHIFT,
    BTN_KEYBOARD_ASCII_OFFSET

} button_event_id;

#endif /* BUTTON_EVENTS_H_ */
