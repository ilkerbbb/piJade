#ifndef AMALGAMATED_BUILD
#include "../button_events.h"
#include "../display.h"
#include "../jade_assert.h"
#include "../ui.h"
#include "process.h"
#include "utils/malloc_ext.h"

#ifdef CONFIG_BOARD_TYPE_JADE_ANY
extern const uint8_t fccstart[] asm("_binary_fcc_bin_gz_start");
extern const uint8_t fccend[] asm("_binary_fcc_bin_gz_end");
extern const uint8_t cestart[] asm("_binary_ce_bin_gz_start");
extern const uint8_t ceend[] asm("_binary_ce_bin_gz_end");
extern const uint8_t weeestart[] asm("_binary_weee_bin_gz_start");
extern const uint8_t weeeend[] asm("_binary_weee_bin_gz_end");
#if defined(CONFIG_BOARD_TYPE_JADE_V1_1) || defined(CONFIG_BOARD_TYPE_JADE_V2_ANY)
extern const uint8_t telecstart[] asm("_binary_telec_bin_gz_start");
extern const uint8_t telecend[] asm("_binary_telec_bin_gz_end");
#endif
#endif

static gui_view_node_t* make_home_screen_panel_item(const color_t color, home_menu_entry_t* entry)
{
    JADE_ASSERT(entry);

    gui_view_node_t* item;
    gui_view_node_t* fill;

    // The items symbol, text and any description will be updated, so we add a
    // background that will be repainted every time to wipe the previous string.
    gui_make_vsplit(&item, GUI_SPLIT_RELATIVE, 2, HOME_SCREEN_DEEP_STATUS_BAR ? 55 : 65, GUI_SPLIT_FILL_REMAINING);

    // Top row, the symbol and label text
    gui_make_fill(&fill, color, FILL_PLAIN, item);
    gui_make_text_font(&entry->symbol, "", TFT_WHITE, JADE_SYMBOLS_24x24_FONT);
    gui_set_padding(entry->symbol, GUI_MARGIN_ALL_DIFFERENT, 0, 0, 0, 8);
    gui_set_align(entry->symbol, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(entry->symbol, fill);

    // Second row, label text
    gui_make_fill(&fill, color, FILL_PLAIN, item);
    gui_make_text_font(&entry->text, "", TFT_WHITE, HOME_SCREEN_DEEP_STATUS_BAR ? DEJAVU24_FONT : GUI_DEFAULT_FONT);
    gui_set_padding(entry->text, GUI_MARGIN_ALL_DIFFERENT, 0, 0, 0, 8);
    gui_set_align(entry->text, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(entry->text, fill);

    return item;
}

gui_activity_t* make_home_screen_activity(const char* device_name, const char* firmware_version,
    home_menu_entry_t* selected_entry, home_menu_entry_t* next_entry, gui_view_node_t** status_light,
    gui_view_node_t** status_text, gui_view_node_t** label)
{
    JADE_ASSERT(device_name);
    JADE_ASSERT(firmware_version);
    JADE_ASSERT(selected_entry);
    JADE_ASSERT(next_entry);
    JADE_INIT_OUT_PPTR(status_light);
    JADE_INIT_OUT_PPTR(status_text);
    JADE_INIT_OUT_PPTR(label);

    // NOTE: The home screen is created as an 'unmanaged' activity as
    // its lifetime is same as that of the entire application
    gui_activity_t* act = NULL;
    gui_make_activity_ex(&act, true, device_name, false);
    JADE_ASSERT(act);

    gui_view_node_t* node;
    gui_view_node_t* hsplit;

    gui_view_node_t* vsplit;

    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 2, 75, 25);
    gui_set_parent(vsplit, act->root_node);

    // Main area, scrolling horizontal menu, in two sections - this/next
    gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 2, HOME_SCREEN_SELECTED_TILE_PCT, 100 - HOME_SCREEN_SELECTED_TILE_PCT);
    const size_t toppad = (CONFIG_DISPLAY_HEIGHT > 200) ? (CONFIG_DISPLAY_HEIGHT - 180) / 2 : 8;
    gui_set_padding(hsplit, GUI_MARGIN_ALL_DIFFERENT, toppad, 0, 8, 0);
    gui_set_parent(hsplit, vsplit);

    // Selected item
    node = make_home_screen_panel_item(gui_get_highlight_color(), selected_entry);
    gui_set_borders(node, TFT_BLACK, 4, GUI_BORDER_RIGHT);
    gui_set_parent(node, hsplit);

    // Next item
    node = make_home_screen_panel_item(GUI_BLOCKSTREAM_UNHIGHTLIGHTED_DEFAULT, next_entry);
    gui_set_borders(node, TFT_BLACK, 6, GUI_BORDER_LEFT);
    gui_set_parent(node, hsplit);

    // Footer, three labels - status light + status, fw-version/wallet-id label
    gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 3, 9, 44, 47);
    gui_set_parent(hsplit, vsplit);

    gui_make_fill(&node, TFT_BLACK, FILL_PLAIN, hsplit);
    gui_make_text_font(status_light, "M", TFT_DARKGREY, JADE_SYMBOLS_16x16_FONT);
    gui_set_align(*status_light, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_padding(*status_light, GUI_MARGIN_ALL_DIFFERENT, 0, 0, 0, 2);
    gui_set_parent(*status_light, node);

    gui_make_fill(&node, TFT_BLACK, FILL_PLAIN, hsplit);
    gui_make_text_font(status_text, "", TFT_WHITE, GUI_TITLE_FONT);
    gui_set_align(*status_text, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(*status_text, node);

    gui_make_fill(&node, TFT_BLACK, FILL_PLAIN, hsplit);
    gui_make_text_font(label, firmware_version, TFT_WHITE, GUI_TITLE_FONT);
    gui_set_align(*label, GUI_ALIGN_RIGHT, GUI_ALIGN_MIDDLE);
    gui_set_padding(*label, GUI_MARGIN_ALL_DIFFERENT, 0, 2, 0, 0);
    gui_set_parent(*label, node);

    return act;
}

gui_activity_t* make_connect_activity(void)
{
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_CONNECT_BACK },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_CONNECT_HELP } };

    const char* message[]
        = { "Open your wallet app.", "If needed, the app", "will prompt you to", "connect via USB/BLE." };

    return make_show_message_activity(message, 4, "Ready to Pair", hdrbtns, 2, NULL, 0);
}

gui_activity_t* make_connect_to_activity(const char* device_name, const jade_msg_source_t initialisation_source)
{
    JADE_ASSERT(device_name);
    JADE_ASSERT(initialisation_source != SOURCE_INTERNAL);

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_CONNECT_TO_BACK },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_CONNECT_HELP } };

    const char* message[] = { NULL, NULL, NULL };
    if (initialisation_source == SOURCE_BLE) {
        char select_device[32];
        const int ret = snprintf(select_device, sizeof(select_device), "Select %s on", device_name);
        JADE_ASSERT(ret > 0 && ret < sizeof(select_device));

        message[0] = select_device;
        message[1] = "the companion app to";
        message[2] = "pair it";
    } else {
        char connect_device[32];
        const int ret = snprintf(connect_device, sizeof(connect_device), "Connect %s", device_name);
        JADE_ASSERT(ret > 0 && ret < sizeof(connect_device));

        message[0] = connect_device;
        message[1] = "to a compatible wallet";
        message[2] = "app";
    }

    return make_show_message_activity(message, 3, device_name, hdrbtns, 2, NULL, 0);
}

gui_activity_t* make_connect_qrmode_activity(const char* device_name)
{
    JADE_ASSERT(device_name);

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_CONNECT_QR_BACK },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_CONNECT_QR_HELP } };

    btn_data_t menubtns[] = { { .txt = "QR PIN Unlock", .font = GUI_DEFAULT_FONT, .ev_id = BTN_CONNECT_QR_PIN },
        { .txt = "Scan SeedQR", .font = GUI_DEFAULT_FONT, .ev_id = BTN_CONNECT_QR_SCAN } };

    return make_menu_activity("QR Mode", hdrbtns, 2, menubtns, 2);
}

gui_activity_t* make_select_connection_activity_if_required(const bool temporary_restore)
{
    // Two or three buttons, depending on whether QR and/or Bluetooth are available in the build
    // NOTE: if neither QR or BLE are an available, then the only option is USB, and this call returns null.
    // Also, a 'recovery phrase login' puts 'QR'(mode) first, whereas a standard initialisation puts QR last!

    // Initially placeholders
    btn_data_t menubtns[] = { { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_CONNECT_SELECT_BACK },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };
    size_t ibtn = 0;

    // Temporary restore has QR first (Camera-Enabled hw only)
#ifdef CONFIG_HAS_CAMERA
    if (temporary_restore) {
        menubtns[ibtn].txt = "QR";
        menubtns[ibtn].ev_id = BTN_CONNECT_VIA_QR;
        ++ibtn;
    }
#endif

    // USB is always available
    menubtns[ibtn].txt = "USB";
    menubtns[ibtn].ev_id = BTN_CONNECT_VIA_USB;
    ++ibtn;

    // BLE if enabled in fw
#ifdef CONFIG_BT_ENABLED
    menubtns[ibtn].txt = "Bluetooth";
    menubtns[ibtn].ev_id = BTN_CONNECT_VIA_BLE;
    ++ibtn;
#endif

    // If not temporary restore, QR is last (Camera-Enabled hw only)
#ifdef CONFIG_HAS_CAMERA
    if (!temporary_restore) {
        menubtns[ibtn].txt = "QR";
        menubtns[ibtn].ev_id = BTN_CONNECT_VIA_QR;
        ++ibtn;
    }
#endif

    // For non-jade hw without BLE enabled, USB might be the only option,
    // in which case we don't really need this screen at all!
    // In this case return null here.
    if (ibtn < 2) {
        return NULL;
    }

    // Otherwise make a menu and return that
    gui_activity_t* const act = make_menu_activity("Select Connection", hdrbtns, 2, menubtns, ibtn);
    // BBB-AIRGAP: The new title button must not steal the screen's existing first menu selection.
    gui_set_activity_initial_selection(menubtns[0].btn);
    return act;
}

gui_activity_t* make_confirm_qrmode_activity(void)
{
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_CONNECT_QR_BACK },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_CONNECT_QR_HELP } };

    const char* message[] = { "Save and encrypt wallet", "with PIN or scan a", "SeedQR every session?" };

    btn_data_t ftrbtns[] = {
        { .txt = "PIN", .font = GUI_DEFAULT_FONT, .ev_id = BTN_CONNECT_QR_PIN, .borders = GUI_BORDER_TOPRIGHT },
        { .txt = "SeedQR", .font = GUI_DEFAULT_FONT, .ev_id = BTN_CONNECT_QR_SCAN, .borders = GUI_BORDER_TOPLEFT }
    };

    return make_show_message_activity(message, 3, NULL, hdrbtns, 2, ftrbtns, 2);
}

gui_activity_t* make_bip39_passphrase_prefs_activity(
    gui_view_node_t** frequency_textbox, gui_view_node_t** method_textbox)
{
    JADE_INIT_OUT_PPTR(frequency_textbox);
    JADE_INIT_OUT_PPTR(method_textbox);

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_PASSPHRASE_EXIT },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_PASSPHRASE_HELP } };

    // menu buttons with bespoke content
    gui_make_text(frequency_textbox, "Frequency", TFT_WHITE);
    gui_set_align(*frequency_textbox, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);

    gui_make_text(method_textbox, "Method", TFT_WHITE);
    gui_set_align(*method_textbox, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);

    btn_data_t menubtns[] = { { .content = *frequency_textbox, .ev_id = BTN_PASSPHRASE_FREQUENCY },
        { .content = *method_textbox, .ev_id = BTN_PASSPHRASE_METHOD } };

    return make_menu_activity("BIP39 Passphrase", hdrbtns, 2, menubtns, 2);
}

gui_activity_t* make_startup_options_activity(void)
{
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_SETTINGS_EXIT },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    btn_data_t menubtns[] = { { .txt = "Factory Reset", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_RESET },
        { .txt = "Blind Oracle", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_PINSERVER }
#ifdef CONFIG_BOARD_TYPE_JADE_ANY
        // Legal screens only apply to official Jade hw
        ,
        { .txt = "Legal", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_LEGAL }
#endif
    };

    return make_menu_activity("Boot Menu", hdrbtns, 2, menubtns, sizeof(menubtns) / sizeof(btn_data_t));
}

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_HAS_BATTERY)
gui_activity_t* make_usbstorage_settings_activity(const bool wallet_loaded, const bool firmware_upgrade_allowed)
{
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_SETTINGS_USBSTORAGE_EXIT },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    // BBB-AIRGAP: a temporary wallet can sit over a locked PIN blob.  Its own signing and xpub
    // export remain valid, but OTA is not authenticated, so build those two rows without the
    // otherwise dead Firmware Upgrade row.
    btn_data_t menubtns[3];
    size_t num_menubtns = 0;
    if (firmware_upgrade_allowed) {
        menubtns[num_menubtns++]
            = (btn_data_t){ .txt = "Firmware Upgrade", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_USBSTORAGE_FW };
    }
    if (wallet_loaded) {
        menubtns[num_menubtns++]
            = (btn_data_t){ .txt = "Sign", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_USBSTORAGE_SIGN };
        menubtns[num_menubtns++] = (btn_data_t){
            .txt = "Export Xpub", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_USBSTORAGE_EXPORT_XPUB
        };
    }
    // Never zero: the row that opens this screen is itself offered only with a wallet loaded or
    // with no PIN on the device (run_options_list(), main/process/dashboard.c), and ota_allowed()
    // returns true whenever there is no PIN - so the two conditions cannot both be false here.
    JADE_ASSERT(num_menubtns > 0 && num_menubtns < 5);

    return make_menu_activity("USB Storage", hdrbtns, 2, menubtns, num_menubtns);
}

#endif

gui_activity_t* make_display_settings_activity(void)
{
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_SETTINGS_DISPLAY_EXIT },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    // NOTE: Only boards listed here have brightness controls
    // NOTE: Jade v1.1's do not support Flip Orientation because of issues with screen offsets
#if defined(CONFIG_BOARD_TYPE_JADE_V2_ANY) || defined(CONFIG_BOARD_TYPE_WS_TOUCH_LCD2)                                 \
    || defined(CONFIG_BOARD_TYPE_TTGO_TDISPLAY) || defined(CONFIG_BOARD_TYPE_M5_STICKC_PLUS_2)
    btn_data_t menubtns[]
        = { { .txt = "Display Brightness", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DISPLAY_BRIGHTNESS },
              { .txt = "Flip Orientation", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DISPLAY_ORIENTATION },
              { .txt = "Theme", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DISPLAY_THEME } };
#elif defined(CONFIG_BOARD_TYPE_JADE_V1_1)
    btn_data_t menubtns[]
        = { { .txt = "Display Brightness", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DISPLAY_BRIGHTNESS },
              { .txt = "Theme", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DISPLAY_THEME } };
#elif defined(CONFIG_BOARD_TYPE_JADE)
    btn_data_t menubtns[] = { { .txt = "Theme", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DISPLAY_THEME } };
#else // DIY units
    btn_data_t menubtns[] = {
#ifdef HAVE_DISPLAY_BRIGHTNESS_SETTING
        // BBB-AIRGAP: kept first, as it is on the boards upstream lists, so the menu reads the same.
        { .txt = "Display Brightness", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DISPLAY_BRIGHTNESS },
#endif
        { .txt = "Flip Orientation", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DISPLAY_ORIENTATION },
        { .txt = "Theme", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DISPLAY_THEME },
#ifdef HAVE_CAMERA_ROTATION_SETTING
        // BBB-AIRGAP: a hand-assembled unit can have the camera mounted at any angle, so the
        // angle upstream fixes per board is chosen here instead - on a screen of its own,
        // as brightness and theme are (main/process/dashboard.c handle_camera_rotation), so
        // the row reads like the three above it.
        { .txt = "Camera Rotation", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DISPLAY_CAMERA_ROTATION },
#endif
    };
#endif

    return make_menu_activity("Display", hdrbtns, 2, menubtns, sizeof(menubtns) / sizeof(btn_data_t));
}

gui_activity_t* make_otp_activity(void)
{
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_SETTINGS_OTP_EXIT },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_SETTINGS_OTP_HELP } };

    // BBB-AIRGAP: 'Set Clock' scans an epoch QR (ur:jade-epoch).  Time-based codes are wrong
    // until the clock is set, and this port loses the clock on every power cut (no RTC), so the
    // step belongs in the menu that leads to those codes rather than buried in a generic scan.
    btn_data_t menubtns[] = { { .txt = "View OTP", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_OTP_VIEW },
#ifdef CONFIG_HAS_CAMERA
        { .txt = "Set Clock", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_OTP_SET_CLOCK },
#endif
        { .txt = "New OTP Record", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_OTP_NEW } };

    return make_menu_activity("OTP", hdrbtns, 2, menubtns, sizeof(menubtns) / sizeof(btn_data_t));
}

gui_activity_t* make_new_otp_activity(void)
{
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_SETTINGS_OTP_NEW_EXIT },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_SETTINGS_OTP_HELP } };

    btn_data_t menubtns[] = {
#ifdef CONFIG_HAS_CAMERA
        { .txt = "Scan QR", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_OTP_NEW_QR },
#endif
        { .txt = "Enter URI", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_OTP_NEW_KB }
    };

    return make_menu_activity("New OTP", hdrbtns, 2, menubtns, sizeof(menubtns) / sizeof(btn_data_t));
}

gui_activity_t* make_pinserver_activity(void)
{
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_SETTINGS_PINSERVER_EXIT },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_SETTINGS_PINSERVER_HELP } };

    btn_data_t menubtns[] = { { .txt = "Settings", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_PINSERVER_SHOW },
#ifdef CONFIG_HAS_CAMERA
        { .txt = "Scan Oracle QR", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_PINSERVER_SCAN_QR },
#endif
        { .txt = "Reset Oracle", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_PINSERVER_RESET } };

    return make_menu_activity("Blind Oracle", hdrbtns, 2, menubtns, sizeof(menubtns) / sizeof(btn_data_t));
}

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_HAS_BATTERY)
gui_activity_t* make_usb_connect_activity(const char* title)
{
    JADE_ASSERT(title);

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_SETTINGS_USBSTORAGE_BACK },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_SETTINGS_USBSTORAGE_HELP } };

    const char* message[] = { "Please connect a USB", "storage device" };
    return make_show_message_activity(message, 2, title, hdrbtns, 2, NULL, 0);
}
#endif

gui_activity_t* make_wallet_erase_pin_info_activity(void)
{
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_WALLET_ERASE_PIN_EXIT },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_WALLET_ERASE_PIN_HELP } };

    const char* message[] = { "A duress PIN will delete", "the wallet stored on", "Jade if entered" };

    btn_data_t ftrbtn
        = { .txt = "Continue", .font = GUI_DEFAULT_FONT, .ev_id = BTN_WALLET_ERASE_PIN_SET, .borders = GUI_BORDER_TOP };

    gui_activity_t* const act = make_show_message_activity(message, 3, "Wallet-Erase PIN", hdrbtns, 2, &ftrbtn, 1);

    // Set the intially selected item to the 'Continue' button
    gui_set_activity_initial_selection(ftrbtn.btn);

    return act;
}

gui_activity_t* make_wallet_erase_pin_options_activity(void)
{
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_WALLET_ERASE_PIN_EXIT },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_WALLET_ERASE_PIN_HELP } };

    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* const parent = add_title_bar(act, "Wallet-Erase PIN", hdrbtns, 2, NULL);

    gui_view_node_t* vsplit;
    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 3, 35, 35, 30);
    gui_set_padding(vsplit, GUI_MARGIN_ALL_DIFFERENT, 8, 0, 8, 0);
    gui_set_parent(vsplit, parent);

    gui_view_node_t* label;
    gui_make_text(&label, "Wallet-erase PIN:", TFT_WHITE);
    gui_set_align(label, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(label, vsplit);

    // BBB-AIRGAP: this row used to hold the stored PIN itself.  The PIN is kept as a salted
    // verifier now (main/storage.c) and cannot be read back, so the row states that one is set.
    // The screen is only ever reached when that is true (main/process/dashboard.c).
    gui_view_node_t* value;
    gui_make_text(&value, "Enabled", TFT_WHITE);
    gui_set_align(value, GUI_ALIGN_CENTER, GUI_ALIGN_TOP);
    gui_set_parent(value, vsplit);

    // BBB-AIRGAP: without explicit borders add_button() draws them in black (dialogs.c:64), so the
    // two controls read as plain text until one is selected; the standard two-button footer is
    // TOPRIGHT/TOPLEFT plus an initial selection (dialogs.c:960-965).  'Change' is selected first
    // because 'Disable' removes the duress wallet's protection - the destructive option is not the
    // one that should sit under the click by default.  Presentation only: the duress PIN's unlock
    // and wipe behaviour is untouched.
    btn_data_t ftrbtns[] = { { .txt = "Change",
                                 .font = GUI_DEFAULT_FONT,
                                 .ev_id = BTN_WALLET_ERASE_PIN_SET,
                                 .borders = GUI_BORDER_TOPRIGHT },
        { .txt = "Disable",
            .font = GUI_DEFAULT_FONT,
            .ev_id = BTN_WALLET_ERASE_PIN_DISABLE,
            .borders = GUI_BORDER_TOPLEFT } };
    add_buttons(vsplit, UI_ROW, ftrbtns, 2);
    gui_set_activity_initial_selection(ftrbtns[0].btn);

    return act;
}

// BBB-AIRGAP: the loaded wallet appears here under its fingerprint, and its operations hang off
// that entry. 'seed_label' is only read while the menu is built - the builder keeps its own copy
// of the text (main/gui.c:1170) - so a caller's stack buffer is enough.
gui_activity_t* make_ble_activity(gui_view_node_t** ble_status_item)
{
    JADE_INIT_OUT_PPTR(ble_status_item);

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_BLE_EXIT },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_BLE_HELP } };

    // menu button with bespoke content
    gui_make_text(ble_status_item, "Status:", TFT_WHITE);
    gui_set_align(*ble_status_item, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);

    btn_data_t menubtns[] = { { .content = *ble_status_item, .ev_id = BTN_BLE_STATUS },
        { .txt = "Reset Pairings", .font = GUI_DEFAULT_FONT, .ev_id = BTN_BLE_RESET_PAIRING } };

    return make_menu_activity("Bluetooth", hdrbtns, 2, menubtns, 2);
}

gui_activity_t* make_view_delete_wallet_activity(const char* wallet_name, const bool allow_export)
{
    JADE_ASSERT(wallet_name);

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_BACK },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    btn_data_t menubtns[] = { { .txt = "Details", .font = GUI_DEFAULT_FONT, .ev_id = BTN_VIEW_WALLET },
        { .txt = "Delete", .font = GUI_DEFAULT_FONT, .ev_id = BTN_DELETE_WALLET },
        { .txt = "Delete", .font = GUI_DEFAULT_FONT, .ev_id = BTN_DELETE_WALLET } };

    if (allow_export) {
        menubtns[1].txt = "Export";
        menubtns[1].ev_id = BTN_EXPORT_WALLET;
    }

    return make_menu_activity(wallet_name, hdrbtns, 2, menubtns, allow_export ? 3 : 2);
}

gui_activity_t* make_info_activity(const char* fw_version)
{
    JADE_ASSERT(fw_version);

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_SETTINGS_INFO_EXIT },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    // menu buttons with bespoke content
    const size_t fwlen = strlen(fw_version);
    const uint32_t fwsplitsize = fwlen > 8 ? 44 : 60;
    gui_view_node_t* splitfw;
    gui_make_hsplit(&splitfw, GUI_SPLIT_RELATIVE, 2, fwsplitsize, 100 - fwsplitsize);

    gui_view_node_t* fwver;
    gui_make_text(&fwver, "Firmware:", TFT_WHITE);
    gui_set_align(fwver, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(fwver, splitfw);

    gui_make_text(&fwver, fw_version, TFT_WHITE);
    gui_set_align(fwver, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(fwver, splitfw);

    btn_data_t menubtns[] = { { .content = splitfw, .ev_id = BTN_SETTINGS_INFO_FWVERSION },
        { .txt = "Device Info", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DEVICE_INFO },
        // BBB-AIRGAP: this port runs on hardware that was rewired by hand, so the display and the
        // camera are the two parts most likely to be the fault and the least likely to be
        // diagnosable without a build environment.  Their check lives here, next to the other
        // things you look at when you want to know what this device is.
        { .txt = "I/O Test", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_IO_TEST }
#ifdef CONFIG_BOARD_TYPE_JADE_ANY
        // Legal screens only apply to official Jade hw
        ,
        { .txt = "Legal", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_LEGAL }
#endif
    };

    gui_activity_t* const act = make_menu_activity("Info", hdrbtns, 2, menubtns, sizeof(menubtns) / sizeof(btn_data_t));

    // NOTE: can only set scrolling *after* gui tree created
    gui_set_text_scroll_selected(fwver, true, TFT_BLACK, gui_get_highlight_color());

    return act;
}

// BBB-AIRGAP: upstream does have a hardware check (main/smoketest.c) but it is built only into QA
// firmware and ends by powering the device off, so it is not something a user can reach.  This is
// the reachable version, and it covers the two parts this port rewired.
gui_activity_t* make_io_test_activity(void)
{
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_IO_TEST_EXIT },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    btn_data_t menubtns[] = {
        { .txt = "Screen", .font = GUI_DEFAULT_FONT, .ev_id = BTN_IO_TEST_SCREEN },
#ifdef CONFIG_LIBJADE
        // BBB-AIRGAP: the buttons check is only offered where its exit exists.  It leaves on
        // GUI_ALT_EVENT and nothing else, and the only caller of gui_alt_click() is
        // libjade_input() (libjade/libjade.c); none of the native handlers in main/input/ has a
        // third key to call it with.  On an official Jade board this row would open a screen with
        // no way out of it.
        { .txt = "Buttons", .font = GUI_DEFAULT_FONT, .ev_id = BTN_IO_TEST_BUTTONS },
#endif
#ifdef CONFIG_HAS_CAMERA
        { .txt = "Camera", .font = GUI_DEFAULT_FONT, .ev_id = BTN_IO_TEST_CAMERA },
#endif
    };

    return make_menu_activity("I/O Test", hdrbtns, 2, menubtns, sizeof(menubtns) / sizeof(btn_data_t));
}

// BBB-AIRGAP: the strip along the top keeps the instruction visible while the rest of the panel is
// painted whatever colour is being checked - including white, which would swallow white text, and
// black, which would swallow the border of a button.  Splitting it off is what makes the colour
// area a flat field with nothing drawn over it, which is what a dead pixel has to show against.
gui_activity_t* make_io_test_screen_activity(gui_view_node_t** colour_fill)
{
    JADE_INIT_OUT_PPTR(colour_fill);

    gui_activity_t* const act = gui_make_activity();

    gui_view_node_t* vsplit;
    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 2, 18, 82);
    gui_set_parent(vsplit, act->root_node);

    gui_view_node_t* label;
    gui_make_text(&label, "Click for next color", TFT_WHITE);
    gui_set_align(label, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(label, vsplit);

    gui_make_fill(colour_fill, TFT_BLACK, FILL_PLAIN, vsplit);

    return act;
}

// BBB-AIRGAP: the buttons check.  The board has eight keys and the firmware above libjade sees
// seven inputs, because KEY2 and the joystick centre are wired to the same one
// (pijade/host/pijade_host.c).  So the pair lights two marks together and the screen says so; which of the
// two was pressed is known to the person pressing it, and a key that is dead marks nothing.
//
// Nothing here is selectable.  The marks are painted by handle_io_test_buttons()
// (main/process/dashboard.c) as the events arrive, and the glyphs come from the symbols font
// (main/fonts/jade_symbols_16x16.c): K and L are the small up and down triangles, H and I the
// left and right ones, and M the hollow circle standing for the centre press.
gui_activity_t* make_io_test_buttons_activity(gui_view_node_t** marks, gui_view_node_t** note)
{
    JADE_ASSERT(marks);
    JADE_ASSERT(note);

    gui_activity_t* const act = gui_make_activity();

    gui_view_node_t* vsplit;
    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 3, 20, 56, 24);
    gui_set_parent(vsplit, act->root_node);

    gui_view_node_t* title;
    gui_make_text(&title, "Press each button", TFT_WHITE);
    gui_set_align(title, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(title, vsplit);

    gui_view_node_t* body;
    gui_make_hsplit(&body, GUI_SPLIT_RELATIVE, 2, 55, 45);
    gui_set_parent(body, vsplit);

    // Joystick: up on its own row, then left/centre/right, then down.
    gui_view_node_t* pad;
    gui_make_vsplit(&pad, GUI_SPLIT_RELATIVE, 3, 33, 34, 33);
    gui_set_parent(pad, body);

    gui_make_text_font(&marks[IO_TEST_MARK_UP], "K", TFT_DARKGREY, JADE_SYMBOLS_16x16_FONT);
    gui_set_align(marks[IO_TEST_MARK_UP], GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(marks[IO_TEST_MARK_UP], pad);

    gui_view_node_t* midrow;
    gui_make_hsplit(&midrow, GUI_SPLIT_RELATIVE, 3, 33, 34, 33);
    gui_set_parent(midrow, pad);

    gui_make_text_font(&marks[IO_TEST_MARK_LEFT], "H", TFT_DARKGREY, JADE_SYMBOLS_16x16_FONT);
    gui_set_align(marks[IO_TEST_MARK_LEFT], GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(marks[IO_TEST_MARK_LEFT], midrow);

    gui_make_text_font(&marks[IO_TEST_MARK_CLICK], "M", TFT_DARKGREY, JADE_SYMBOLS_16x16_FONT);
    gui_set_align(marks[IO_TEST_MARK_CLICK], GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(marks[IO_TEST_MARK_CLICK], midrow);

    gui_make_text_font(&marks[IO_TEST_MARK_RIGHT], "I", TFT_DARKGREY, JADE_SYMBOLS_16x16_FONT);
    gui_set_align(marks[IO_TEST_MARK_RIGHT], GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(marks[IO_TEST_MARK_RIGHT], midrow);

    gui_make_text_font(&marks[IO_TEST_MARK_DOWN], "L", TFT_DARKGREY, JADE_SYMBOLS_16x16_FONT);
    gui_set_align(marks[IO_TEST_MARK_DOWN], GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(marks[IO_TEST_MARK_DOWN], pad);

    // The three keys down the right hand edge, in the order they sit on the board.
    gui_view_node_t* keys;
    gui_make_vsplit(&keys, GUI_SPLIT_RELATIVE, 3, 33, 34, 33);
    gui_set_parent(keys, body);

    gui_make_text(&marks[IO_TEST_MARK_KEY1], "K1 First", TFT_DARKGREY);
    gui_set_align(marks[IO_TEST_MARK_KEY1], GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(marks[IO_TEST_MARK_KEY1], keys);

    gui_make_text(&marks[IO_TEST_MARK_KEY2], "K2 Click", TFT_DARKGREY);
    gui_set_align(marks[IO_TEST_MARK_KEY2], GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(marks[IO_TEST_MARK_KEY2], keys);

    gui_view_node_t* key3;
    gui_make_text(&key3, "K3 Exit", TFT_WHITE);
    gui_set_align(key3, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(key3, keys);

    // BBB-AIRGAP: the caller rewrites this line on every press, so it says what the button that
    // was just pressed does rather than carrying one fixed remark (handle_io_test_buttons(),
    // main/process/dashboard.c).  It opens on KEY3 because leaving the screen is KEY3's own test:
    // pressing it ends the screen, so its line would never be readable if it waited for a press.
    // The fill behind it is what wipes the previous string: gui_update_text() repaints the text
    // node's parent, and a text node drawn straight onto the split leaves the old glyphs in place,
    // so the two lines pile up on each other (measured on the emulator).  The home screen carries
    // the same background for the same reason (make_home_screen_panel_item() above).
    gui_view_node_t* notefill;
    gui_make_fill(&notefill, TFT_BLACK, FILL_PLAIN, vsplit);
    gui_make_text(note, IO_TEST_NOTE_KEY3, TFT_WHITE);
    gui_set_align(*note, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(*note, notefill);

    return act;
}

gui_activity_t* make_device_info_activity(const bool show_ble)
{
    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_SETTINGS_DEVICE_INFO_EXIT },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    btn_data_t menubtns[] = { { .txt = "MAC Address", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DEVICE_INFO_MAC },
#ifdef CONFIG_HAS_BATTERY
        { .txt = "Battery Volts", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DEVICE_INFO_BATTERY },
#endif
        { .txt = "Storage", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_DEVICE_INFO_STORAGE },
        // BBB-AIRGAP: the radio row sits here rather than among the preferences, because on this
        // hardware it cannot act: the Bluetooth circuit is cut and handle_ble() only says the
        // firmware has it disabled.  A row that states a fact about the device belongs with the
        // other facts about the device.  It keeps its event id and its behaviour, so a build that
        // does have Bluetooth still opens the real screen from here.
        //
        // Last in the array so 'show_ble' can drop it by shortening the count.  It has to be
        // droppable: upstream offered this row only when the device was not locked (the locked
        // device got Change PIN in its place, main/process/dashboard.c before the move), and on a
        // build with the radio present handle_ble() switches Bluetooth on and deletes pairings -
        // neither of which should be reachable by someone who has not entered the PIN.
        { .txt = "Bluetooth", .font = GUI_DEFAULT_FONT, .ev_id = BTN_SETTINGS_BLE } };

    const size_t num_menubtns = sizeof(menubtns) / sizeof(btn_data_t) - (show_ble ? 0 : 1);
    return make_menu_activity("Device Info", hdrbtns, 2, menubtns, num_menubtns);
}

#ifdef CONFIG_BOARD_TYPE_JADE_ANY

#ifdef CONFIG_BOARD_TYPE_JADE
#define JADE_FCC_ID "2AWI3BLOCK\n STREAMJD1"
#define MAX_LEGAL_PAGE 5
#else
#define JADE_FCC_ID "2AWI3BLOCK\n STREAMJD2"
#define MAX_LEGAL_PAGE 6
#endif

static void make_legal_page(link_activity_t* page_act, int legal_page)
{
    JADE_ASSERT(page_act);

    const bool first_page = (legal_page == 0);
    const bool last_page = (legal_page == MAX_LEGAL_PAGE);

    // 'prev' and 'next' buttons - give 'exit' event on first/last page
    btn_data_t hdrbtns[]
        = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = first_page ? BTN_LEGAL_EXIT : BTN_LEGAL_PREV },
              { .txt = ">", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = last_page ? BTN_LEGAL_EXIT : BTN_LEGAL_NEXT } };

    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* const parent = add_title_bar(act, "Certifications", hdrbtns, 2, NULL);
    gui_view_node_t* node;

    switch (legal_page) {
    case 0: {
        gui_view_node_t* hsplit;
        gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 2, 35, 65);
        gui_set_padding(hsplit, GUI_MARGIN_ALL_DIFFERENT, 8, 4, 2, 4);
        gui_set_parent(hsplit, parent);

        Picture* const pic = get_picture(fccstart, fccend);
        gui_make_picture(&node, pic);
        gui_set_parent(node, hsplit);
        gui_set_align(node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);

        gui_view_node_t* vsplit;
        gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 2, 30, 70);
        gui_set_parent(vsplit, hsplit);

        gui_view_node_t* title;
        gui_make_text(&title, "FCC ID", TFT_WHITE);
        gui_set_parent(title, vsplit);
        gui_set_align(title, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);

        gui_make_text(&node, JADE_FCC_ID, TFT_WHITE);
        gui_set_padding(node, GUI_MARGIN_ALL_DIFFERENT, 8, 0, 0, 12);
        gui_set_parent(node, vsplit);
        gui_set_align(node, GUI_ALIGN_TOP, GUI_ALIGN_LEFT);
        break;
    }
    case 1: {
        gui_make_text(&node,
            "This device complies\n"
            "with Part 15 of the FCC\n"
            "rules. Operation is\n"
            "subject to the following\n"
            "two conditions: (1) this",
            TFT_WHITE);
        gui_set_parent(node, parent);
        gui_set_align(node, GUI_ALIGN_LEFT, GUI_ALIGN_TOP);
        gui_set_padding(node, GUI_MARGIN_ALL_DIFFERENT, 8, 0, 0, 4);
        break;
    }
    case 2: {
        gui_make_text(&node,
            "device may not cause\n"
            "harmful interference\n"
            "and (2) this device\n"
            "must accept any\n"
            "interference received",
            TFT_WHITE);
        gui_set_parent(node, parent);
        gui_set_align(node, GUI_ALIGN_LEFT, GUI_ALIGN_TOP);
        gui_set_padding(node, GUI_MARGIN_ALL_DIFFERENT, 8, 0, 0, 4);
        break;
    }
    case 3: {
        gui_make_text(&node,
            "including interference\n"
            "that may cause\n"
            "undesired operation.",
            TFT_WHITE);
        gui_set_parent(node, parent);
        gui_set_align(node, GUI_ALIGN_LEFT, GUI_ALIGN_TOP);
        gui_set_padding(node, GUI_MARGIN_ALL_DIFFERENT, 8, 0, 0, 4);
        break;
    }
    case 4: {
        Picture* const pic = get_picture(cestart, ceend);
        gui_make_picture(&node, pic);
        gui_set_parent(node, parent);
        gui_set_align(node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
        gui_set_padding(node, GUI_MARGIN_ALL_EQUAL, 12);
        break;
    }
    case 5: {
        Picture* const pic = get_picture(weeestart, weeeend);
        gui_make_picture(&node, pic);
        gui_set_parent(node, parent);
        gui_set_align(node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
        gui_set_padding(node, GUI_MARGIN_ALL_EQUAL, 12);
        break;
    }
#if defined(CONFIG_BOARD_TYPE_JADE_V1_1) || defined(CONFIG_BOARD_TYPE_JADE_V2_ANY)
    case 6: {
        gui_view_node_t* hsplit;
        gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 3, 36, 10, 54);
        gui_set_parent(hsplit, parent);

        Picture* const pic = get_picture(telecstart, telecend);
        gui_make_picture(&node, pic);
        gui_set_parent(node, hsplit);
        gui_set_align(node, GUI_ALIGN_RIGHT, GUI_ALIGN_MIDDLE);
        gui_set_padding(node, GUI_MARGIN_ALL_DIFFERENT, 0, 8, 0, 0);

        gui_make_text_font(&node, "O", TFT_WHITE, JADE_SYMBOLS_16x16_FONT);
        gui_set_parent(node, hsplit);
        gui_set_align(node, GUI_ALIGN_RIGHT, GUI_ALIGN_MIDDLE);
        gui_set_padding(node, GUI_MARGIN_ALL_DIFFERENT, 0, 4, 2, 0);

#if defined(CONFIG_BOARD_TYPE_JADE_V1_1)
#define JP_COMPLIANCE_TEXT "211-210802"
#else
#define JP_COMPLIANCE_TEXT "219-259339"
#endif
        gui_make_text(&node, JP_COMPLIANCE_TEXT, TFT_WHITE);
        gui_set_parent(node, hsplit);
        gui_set_align(node, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
        gui_set_padding(node, GUI_MARGIN_ALL_DIFFERENT, 0, 0, 0, 4);
        break;
    }
#endif
    default: {
        JADE_ASSERT(false);
    }
    }

    // Set the intially selected item to the next/verify (ie. the last) button
    gui_set_activity_initial_selection(hdrbtns[1].btn);

    // Copy activity and prev and next buttons into output struct
    page_act->activity = act;
    page_act->prev_button = first_page ? NULL : hdrbtns[0].btn;
    page_act->next_button = last_page ? NULL : hdrbtns[1].btn;
}

gui_activity_t* make_legal_certifications_activity(void)
{
    // Chain the legal screen activities
    link_activity_t page_act = {};
    linked_activities_info_t act_info = {};
    for (size_t j = 0; j <= MAX_LEGAL_PAGE; ++j) {
        make_legal_page(&page_act, j);
        gui_chain_activities(&page_act, &act_info);
    }

    return act_info.first_activity;
}
#endif

gui_activity_t* make_storage_stats_activity(const size_t entries_used, const size_t entries_free)
{
    btn_data_t hdrbtns[]
        = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_SETTINGS_DEVICE_INFO_STORAGE_EXIT },
              { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* const parent = add_title_bar(act, "Storage", hdrbtns, 2, NULL);
    gui_view_node_t* hsplit;
    gui_view_node_t* node;

    const size_t entries_total = entries_used + entries_free;
    char buf[16];

    gui_view_node_t* vsplit;
    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 4, 25, 25, 25);
    gui_set_padding(vsplit, GUI_MARGIN_ALL_DIFFERENT, 2, 2, 2, 2);
    gui_set_parent(vsplit, parent);

    // % used
    gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 2, 75, 25);
    gui_set_parent(hsplit, vsplit);

    gui_make_text(&node, "Percentage Used", TFT_WHITE);
    gui_set_align(node, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(node, hsplit);

    const int32_t pcnt_used = 100 * entries_used / entries_total;
    int ret = snprintf(buf, sizeof(buf), "%ld%%", pcnt_used);
    JADE_ASSERT(ret > 0 && ret < sizeof(buf));

    gui_make_text(&node, buf, TFT_WHITE);
    gui_set_align(node, GUI_ALIGN_RIGHT, GUI_ALIGN_MIDDLE);
    gui_set_parent(node, hsplit);

    // Entries used
    gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 2, 60, 40);
    gui_set_parent(hsplit, vsplit);

    gui_make_text(&node, "Entries Used", TFT_WHITE);
    gui_set_align(node, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(node, hsplit);

    ret = snprintf(buf, sizeof(buf), "%d / %d", entries_used, entries_total);
    JADE_ASSERT(ret > 0 && ret < sizeof(buf));

    gui_make_text(&node, buf, TFT_WHITE);
    gui_set_align(node, GUI_ALIGN_RIGHT, GUI_ALIGN_MIDDLE);
    gui_set_parent(node, hsplit);

    // Entries free
    gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 2, 60, 40);
    gui_set_parent(hsplit, vsplit);

    gui_make_text(&node, "Entries Free", TFT_WHITE);
    gui_set_align(node, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(node, hsplit);

    ret = snprintf(buf, sizeof(buf), "%d", entries_free);
    JADE_ASSERT(ret > 0 && ret < sizeof(buf));

    gui_make_text(&node, buf, TFT_WHITE);
    gui_set_align(node, GUI_ALIGN_RIGHT, GUI_ALIGN_MIDDLE);
    gui_set_parent(node, hsplit);

    return act;
}
#endif // AMALGAMATED_BUILD
