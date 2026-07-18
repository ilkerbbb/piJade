#ifndef AMALGAMATED_BUILD
#include <wally_bip39.h>

#include "../bcur.h"
#include "../button_events.h"
#include "../entropy_sources.h"
#include "../jade_assert.h"
#include "../jade_wally_verify.h"
#include "../keychain.h"
#include "../process.h"
#include "../qrcode.h"
#include "../qrmode.h"
#include "../qrscan.h"
#include "../random.h"
#include "../seedqr.h"
#include "../sensitive.h"
#include "../storage.h"
#include "../ui.h"
#include "../utils/cbor_rpc.h"
#include "../utils/network.h"
#include "../utils/util.h"

#include "process_utils.h"

#include <cdecoder.h>
#include <ctype.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define MAX_NUM_FINAL_WORDS 128
#define NUM_WORDS_SELECT 10
#define WORD_ENTRY_KEYS_LEN 26

#define WORDLIST_PASSPHRASE_MAX_WORDS 10

#define BIP85_INDEX_MAX 1000000

typedef enum { MNEMONIC_SIMPLE, MNEMONIC_ADVANCED, WORDLIST_PASSPHRASE } wordlist_purpose_t;

// main/ui/mnemonic.c
gui_activity_t* make_mnemonic_setup_type_activity(void);
gui_activity_t* make_mnemonic_setup_method_activity(bool advanced);
gui_activity_t* make_new_mnemonic_activity(void);
gui_activity_t* make_restore_mnemonic_activity(bool temporary_restore);
gui_activity_t* make_restore_mnemonic_method_activity(size_t nwords);

void make_show_mnemonic_activities(gui_activity_t** first_activity_ptr, gui_activity_t** last_activity_ptr,
    const char* mnemonic, uint16_t word_offs[], size_t nwords);
gui_activity_t* make_confirm_mnemonic_word_activity(gui_view_node_t** text_box_ptr, uint8_t first_word_index,
    uint8_t offset_word_to_confirm, const char* mnemonic, uint16_t word_offs[], size_t nwords);
gui_activity_t* make_enter_wordlist_word_activity(gui_view_node_t** titletext, bool show_enter_btn,
    gui_view_node_t** textbox, gui_view_node_t** backspace, gui_view_node_t** enter, gui_view_node_t** keys,
    size_t keys_len);
gui_activity_t* make_calculate_final_word_activity(void);

gui_activity_t* make_confirm_passphrase_activity(const char* passphrase, gui_view_node_t** textbox);

gui_activity_t* make_export_qr_overview_activity(bool initial);
gui_activity_t* make_export_qr_fullscreen_activity(const Icon* icon);
gui_activity_t* make_export_qr_fragment_activity(
    const Icon* icon, bool context_available, gui_view_node_t** icon_node, gui_view_node_t** label_node);

gui_activity_t* make_bip85_mnemonic_words_activity(void);

// Checks if the given number of mnemonic words is supported by this implementation
static inline bool is_valid_mnemonic_length(const size_t nwords) { return nwords == 12 || nwords == 24; }

// Returns the size of a buffer needed for storing the given number of mnemonic words
static inline size_t mnemonic_buffer_size(const size_t nwords) { return (MNEMONIC_MAX_WORD_LEN + 1) * nwords; }

#ifdef CONFIG_HAS_CAMERA
// Export a mnemonic by asking the user to transcribe it to hard copy, then
// scanning that hard copy back in and verifying the data matches.
// NOTE: both SeedSigner formats are offered - 'CompactSeedQR' (raw entropy) and the
// 'Standard SeedQR' digit string (four digits per BIP39 word index).
// NOTE: a 'true' return means the user either completed the QR copy and verification
// process, OR they decided to abandon/skip it - in either case move on to the next step.
// 'false' implies they pressed a 'back' button and we should NOT move forward.
static bool mnemonic_export_qr(const char* mnemonic, bool* export_qr_verified)
{
    JADE_ASSERT(mnemonic && export_qr_verified);

    // Will be set if scan succeeds
    *export_qr_verified = false;

    const char* question[] = { "Draw the SeedQR", "for use with", "QR Mode." };
    if (!await_skipyes_activity(NULL, question, 3, true, "blkstrm.com/seedqr")) {
        // User decided against it at this time - 'true' return implies a definitive
        // decision by the user - as opposed to a simple 'back' button press.
        return true;
    }

    // CompactSeedQR is simply the mnemonic entropy; the Standard format is a digit string
    // derived from it.  Only 12 or 24 word mnemonics are supported (ie. 128 & 256 bit entropy)
    size_t entropy_len = 0;
    uint8_t entropy[BIP32_ENTROPY_LEN_256]; // Sufficient for 12 and 24 words
    SENSITIVE_PUSH(entropy, sizeof(entropy));
    JADE_WALLY_VERIFY(bip39_mnemonic_to_bytes(NULL, mnemonic, entropy, sizeof(entropy), &entropy_len));
    JADE_ASSERT(entropy_len == BIP32_ENTROPY_LEN_128 || entropy_len == BIP32_ENTROPY_LEN_256);

    // BBB-AIRGAP: offer the two SeedSigner formats.  'Compact' is the raw entropy (the only
    // format Jade produced before) and stays the default; 'Standard' is the digit string.
    const char* format_question[] = { "Which SeedQR", "format?" };
    const bool compact
        = await_choice_activity(NULL, format_question, 2, "Compact", "Standard", true, "blkstrm.com/seedqr");

    // BBB-AIRGAP: 'Standard' and a KEY3 escape both come back as false, and Standard goes on to
    // derive the seed as a digit string.  The loops below would stop at their first escape check
    // anyway, but by then that seed-equivalent material has been built for a user who asked to
    // leave; there is no reason to compute it at all, so the escape is answered first.  'false'
    // is the 'back' return, and the callers treat a pending escape as the end of the export.
    if (gui_escape_pending()) {
        SENSITIVE_POP(entropy);
        return false;
    }

    // BBB-AIRGAP: the Standard format is the seed as a digit string, so it is seed-equivalent
    char digits[97]; // 96 digits plus the NUL
    size_t digits_len = 0;
    SENSITIVE_PUSH(digits, sizeof(digits));
    if (!compact) {
        JADE_ASSERT(seedqr_digits_from_entropy(entropy, entropy_len, digits, sizeof(digits), &digits_len));
    }

    // Convert the payload into a small (v1 to v3) qr-code
    QRCode qrcode;
    const uint8_t qrcode_version = compact ? (entropy_len == BIP32_ENTROPY_LEN_128 ? 1 : 2)
                                           : (entropy_len == BIP32_ENTROPY_LEN_128 ? 2 : 3);
    uint8_t qrbuffer[112]; // underlying qrcode data/work area - opaque; v3 needs 106
    JADE_ASSERT(sizeof(qrbuffer) > qrcode_getBufferSize(qrcode_version));
    SENSITIVE_PUSH(qrbuffer, sizeof(qrbuffer));
    // BBB-AIRGAP: qrcode_initText() returns 0 even when the payload does not fit - it silently
    // truncates (measured, see pijade/tools/v3_capacity_probe.c).  The version/length pairing is
    // therefore asserted here and never inferred from the return value.
    JADE_ASSERT(compact
        || (digits_len == 48 && qrcode_version == 2) || (digits_len == 96 && qrcode_version == 3));
    const int qret = compact
        ? qrcode_initBytes(&qrcode, qrbuffer, qrcode_version, ECC_LOW, entropy, entropy_len)
        : qrcode_initText(&qrcode, qrbuffer, qrcode_version, ECC_LOW, digits);
    JADE_ASSERT(qret == 0);

    const uint16_t fragment_row_height = CONFIG_DISPLAY_HEIGHT * QRCODE_FRAGMENT_ROW_PERCENT / 100;
    const uint16_t fragment_target_size
        = CONFIG_DISPLAY_WIDTH < fragment_row_height ? CONFIG_DISPLAY_WIDTH : fragment_row_height;

    Icon qr_fullscreen;
    qrcode_toIcon(&qrcode, &qr_fullscreen, qr_fullscreen_scale_factor(qrcode_version));

    // Make a bag of icons for square fragments of the qr
    Icon* icons = NULL;
    size_t num_icons = 0;
    const bool show_grid = true;
    const uint8_t expected_grid_size = qrcode_version == 1 ? 3 : (qrcode_version == 2 ? 5 : 6);
    JADE_ASSERT(qrcode_toFragmentsIcons(&qrcode, fragment_target_size, show_grid, 0, &icons, &num_icons));
    JADE_ASSERT(num_icons == expected_grid_size * expected_grid_size);

    const uint8_t context_modules = 2;
    const bool context_available
        = qrcode_fragmentsContextFits(qrcode_version, fragment_target_size, context_modules);
    Icon* context_icons = NULL;
    size_t num_context_icons = 0;

    // Show the overview and magnified fragments, and when the user
    // is done try to scan the qr code they have made and verify it
    // scans and the data imported matches the expected entropy.
    // NOTE: the gui activities are created and freed (by frequent calls to
    // gui_set_current_activity_ex()) inside the loop - this is less efficient
    // but keeps the maximum amout of free DRAM available at all times.
    // This is vital to prevent fragmentation occuring which then causes the large
    // allocations made by the qr-scanner to fail (even if there appears to be
    // sufficient DRAM available).
    bool retval = true;
    bool first_attempt = true;
    int32_t ev_id;
    while (true) {
        while (true) {
            // Show the overview QR
            uint8_t ipart = 0; // fragment to show
            gui_activity_t* const act_overview_qr = make_export_qr_overview_activity(first_attempt);
            bool code_shown = false;
            gui_set_current_activity_ex(act_overview_qr, true);
            while (true) {
                // BBB-AIRGAP: KEY3 leaves the export entirely, not just this screen.  Leaving
                // only the innermost loop left ev_id short of BTN_QR_EXPORT_DONE, so the loop
                // around it rebuilt the overview and the fragments again with the flag still
                // set; the screens allocate on every pass, so that was a growing loop rather
                // than a stall.  The export has one exit and the escape takes it.
                if (gui_escape_pending()) {
                    retval = false;
                    goto cleanup;
                }

                ev_id = wait_export_screen_button(
                    act_overview_qr, first_attempt ? BTN_QR_EXPORT_NEXT : BTN_QR_EXPORT_DONE, &code_shown);
                if (ev_id == BTN_QR_SHOW_FULLSCREEN) {
                    gui_activity_t* const act_fullscreen = make_export_qr_fullscreen_activity(&qr_fullscreen);
                    gui_set_current_activity(act_fullscreen);
                    gui_activity_wait_button(act_fullscreen, BTN_QR_FULLSCREEN_EXIT);
#ifdef CONFIG_DEBUG_UNATTENDED_CI
                    // BBB-AIRGAP: The CI wait returns before the GUI task draws; qrmode.c:286 holds it too.
                    vTaskDelay(500 / portTICK_PERIOD_MS);
#endif
                    gui_destroy_current_activity(act_fullscreen, act_overview_qr);
                    continue;
                }
                if (ev_id == BTN_QR_EXPORT_DONE) {
                    // We are done viewing/copying the qr
                    break;
                } else if (ev_id == BTN_QR_EXPORT_NEXT) {
                    // Move to fragments carousel, showing the first fragment
                    ipart = 0;
                } else if (ev_id == BTN_QR_EXPORT_PREV) {
                    // BBB-AIRGAP: 'back' leaves the export, on this pass and every later one.
                    // Upstream sent it into the fragment carousel from the end once the carousel
                    // had been visited, and the carousel's own 'back' at the first fragment
                    // returns here (ipart wraps to num_icons, the loop above), so the two screens
                    // handed each other back and forth with no way out: the device round of
                    // 2026-09-02 hit exactly that, pressing back repeatedly and never leaving.
                    // Nothing is lost by exiting instead, because that screen grew a '>' header
                    // button on later passes (main/ui/mnemonic.c, the !initial branch) which
                    // enters the carousel forwards from the first fragment.
                    // A false return implies a 'back' option was pressed.
                    retval = false;
                    goto cleanup;
                } else {
                    // Unexpected button event, continue waiting
                    continue;
                }
                break;
            }
            if (ev_id == BTN_QR_EXPORT_DONE) {
                break;
            }
            first_attempt = false;

            // Make a screen to display qr-code fragment icons
            gui_view_node_t* icon_node = NULL;
            gui_view_node_t* text_node = NULL;
            gui_activity_t* const act_qr_part
                = make_export_qr_fragment_activity(&icons[0], context_available, &icon_node, &text_node);
            JADE_ASSERT(icon_node && text_node);
            bool show_context = false;
            bool part_changed = true;

            // Show QR parts, using the buttons to navigate to previous/next fragment
            while (true) {
                // BBB-AIRGAP: KEY3 leaves the export entirely, not just this screen.  Leaving
                // only the innermost loop left ev_id short of BTN_QR_EXPORT_DONE, so the loop
                // around it rebuilt the overview and the fragments again with the flag still
                // set; the screens allocate on every pass, so that was a growing loop rather
                // than a stall.  The export has one exit and the escape takes it.
                if (gui_escape_pending()) {
                    retval = false;
                    goto cleanup;
                }

                // Update display - ipart == num_icons implies going back to the 'overview' of the entire qr-code
                if (ipart >= num_icons) {
                    // Done showing fragments - back to qr overview screen
                    break;
                }
                if (part_changed) {
                    char label[12];
                    const int ret = snprintf(label, sizeof(label), "Grid: %c%u", 'A' + (ipart / expected_grid_size),
                        1 + (ipart % expected_grid_size));
                    JADE_ASSERT(ret > 0 && ret < sizeof(label));
                    gui_update_icon(icon_node, show_context ? context_icons[ipart] : icons[ipart], false);
                    gui_update_text(text_node, label);
                    part_changed = false;
                }

                gui_set_current_activity_ex(act_qr_part, true);
                if (gui_activity_wait_event(act_qr_part, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
                    switch (ev_id) {
                    case BTN_QR_EXPORT_PREV:
                        ipart = (ipart + num_icons) % (num_icons + 1);
                        part_changed = true;
                        break;
                    case BTN_QR_EXPORT_NEXT:
                        ipart = (ipart + 1) % (num_icons + 1);
                        part_changed = true;
                        break;
                    case BTN_QR_EXPORT_CONTEXT:
                        if (!context_icons) {
                            JADE_ASSERT(qrcode_toFragmentsIcons(&qrcode, fragment_target_size, show_grid,
                                context_modules, &context_icons, &num_context_icons));
                            JADE_ASSERT(num_context_icons == num_icons);
                        }
                        show_context = !show_context;
                        gui_update_icon(icon_node, show_context ? context_icons[ipart] : icons[ipart], true);
                        break;
                    default:
                        break;
                    }
                }
            }
        }

        // Verify QR by scanning it back
        qr_data_t* qr_data = JADE_CALLOC(1, sizeof(qr_data_t));
        const bool scan_success = jade_camera_scan_qr(qr_data, "Scan to verify", QR_GUIDE_SHOW, "blkstrm.com/seedqr");
        const bool qr_found = scan_success && qr_data->len != 0;
        const bool is_match = qr_found
            && (compact ? (qr_data->len == entropy_len && !memcmp(qr_data->data, entropy, entropy_len))
                        : (qr_data->len == digits_len && !memcmp(qr_data->data, digits, digits_len)));
        wally_bzero(qr_data, sizeof(qr_data_t));
        free(qr_data);
        // BBB-AIRGAP: leaving the verification camera must not open the Retry question.
        if (gui_escape_pending()) {
            retval = false;
            goto cleanup;
        }
        if (is_match) {
            // QR Code scanned, and it matched expected entropy
            await_message("QR Code Verified");
            *export_qr_verified = true;
            break; // done
        } else {
            const char* question[] = { qr_found ? "QR code does not match" : "No QR code captured", "Retry?" };
            if (await_skipyes_activity(NULL, question, 2, true, NULL)) {
                // User agreed to retry, so go back to displaying qr fragments
                continue;
            } else {
                // User decided to abandon, just break out of loop
                break;
            }
        }
    }

cleanup:
    SENSITIVE_POP(qrbuffer);
    SENSITIVE_POP(digits);
    SENSITIVE_POP(entropy);
    for (size_t i = 0; i < num_context_icons; ++i) {
        qrcode_freeIconData(&context_icons[i]);
    }
    free(context_icons);
    for (int i = 0; i < num_icons; ++i) {
        qrcode_freeIconData(&icons[i]);
    }
    free(icons);
    qrcode_freeIconData(&qr_fullscreen);

    // Return 'true' if done, or 'false' if 'back' was pressed
    return retval;
}

// BBB-AIRGAP: the backup-menu entry point to the screens above.  Upstream reaches them once,
// from the setup flow, with the mnemonic the user just typed still in scope; this reaches the
// same screens for the wallet in use, building the words from the entropy its slot holds and
// wiping them again on the way out.  Doing it here rather than in the caller keeps the words
// inside the file that already knows how to draw them.
static void export_wallet_seedqr(void)
{
    char* mnemonic = NULL;
    if (!keychain_export_mnemonic(&mnemonic)) {
        // The menu only offers this for a wallet that has entropy, so this is not a state the
        // user can reach - say so rather than showing an empty screen.
        JADE_LOGE("SeedQR export requested for a wallet with no entropy");
        await_message("Export unavailable");
        return;
    }

    SENSITIVE_PUSH(mnemonic, strlen(mnemonic));
    bool export_qr_verified = false;
    // A 'false' return is the 'back' button on the first screen, which from here means "leave
    // export", so unlike the setup flow there is nothing to loop over.
    mnemonic_export_qr(mnemonic, &export_qr_verified);
    SENSITIVE_POP(mnemonic);
    JADE_WALLY_VERIFY(wally_free_string(mnemonic));
}
#endif // CONFIG_HAS_CAMERA

// Function to change the mnemonic word separator and provide offsets to
// the start of the words.  Used when confirming one word at a time.
static void change_mnemonic_word_separator(char* mnemonic, const size_t len, const char old_separator,
    const char new_separator, uint16_t word_offs[], const size_t nwords)
{
    JADE_ASSERT(mnemonic && len < 16384u && word_offs);

    size_t word = 0, i = 0;
    for (/*nothing*/; i < len && word < nwords; ++i, ++word) {
        word_offs[word] = i; // Offset of the start of each word
        for (/*nothing*/; i < len; ++i) {
            if (mnemonic[i] == old_separator) {
                mnemonic[i] = new_separator;
                break;
            }
        }
    }
    JADE_ASSERT(word == nwords && i == len + 1);
}

// BBB-AIRGAP: waits on the mnemonic pages for the button that leaves them, and returns its id.
// Two properties of this wait matter, and neither is free.  First, the registration is made once
// and held for the whole wait: the page-turn buttons post GUI_BUTTON_EVENT too (see
// connect_button_activity() in main/gui.c), so a registration made per iteration - as
// sync_await_single_event() does - is torn down and rebuilt on every page turn, and a press landing
// in that gap is lost.  Second, only the two buttons that leave are registered, not
// ESP_EVENT_ANY_ID: the handler overwrites a single event-id field and gives a binary semaphore
// (main/utils/event.c), so a page turn arriving before the waiting task is scheduled would both
// overwrite the id being waited for and be swallowed by the already-given semaphore.  Either
// failure leaves the words drawn on screen with nothing waiting for them.
static int32_t await_mnemonic_pages_exit(gui_activity_t* first_activity)
{
    JADE_ASSERT(first_activity);

    // BBB-AIRGAP: an escape from the incorrect-word message has already consumed its event.
    if (gui_escape_pending()) {
        return BTN_MNEMONIC_EXIT;
    }

    esp_event_handler_instance_t ctx_exit = NULL;
    esp_event_handler_instance_t ctx_verify = NULL;
    esp_event_handler_instance_t ctx_alt = NULL;
    wait_event_data_t* const wait_data = make_wait_event_data();
    JADE_ASSERT(wait_data);
    JADE_ZERO_VERIFY(esp_event_handler_instance_register(
        GUI_BUTTON_EVENT, BTN_MNEMONIC_EXIT, sync_wait_event_handler, wait_data, &ctx_exit));
    JADE_ZERO_VERIFY(esp_event_handler_instance_register(
        GUI_BUTTON_EVENT, BTN_MNEMONIC_VERIFY, sync_wait_event_handler, wait_data, &ctx_verify));
    // BBB-AIRGAP: page navigation changes activities, so keep ALT registered across all pages
    // just like the two exit buttons. KEY3 posts on GUI_EVENT, which those registrations miss.
    JADE_ZERO_VERIFY(esp_event_handler_instance_register(
        GUI_EVENT, GUI_ALT_EVENT, sync_wait_event_handler, wait_data, &ctx_alt));

    gui_set_current_activity(first_activity);

    // A blocking wait (max_wait 0), so this returns only once one of the registered exit events has
    // fired - the page-turn buttons walk the chain of activities without waking it.
    int32_t ev_id = ESP_EVENT_ANY_ID;
    esp_event_base_t ev_base = NULL;
    JADE_ZERO_VERIFY(sync_wait_event(wait_data, &ev_base, &ev_id, NULL, 0));
    // BBB-AIRGAP: the event namespaces overlap; ALT can only mean abandon, never verify.
    if (ev_base == GUI_EVENT && ev_id == GUI_ALT_EVENT) {
        ev_id = BTN_MNEMONIC_EXIT;
    }
    JADE_ASSERT(ev_id == BTN_MNEMONIC_EXIT || ev_id == BTN_MNEMONIC_VERIFY);

    // Unregister before the data is freed - a handler left registered would write into freed memory
    // on the next button press.
    JADE_ZERO_VERIFY(esp_event_handler_instance_unregister(GUI_BUTTON_EVENT, BTN_MNEMONIC_EXIT, ctx_exit));
    JADE_ZERO_VERIFY(esp_event_handler_instance_unregister(GUI_BUTTON_EVENT, BTN_MNEMONIC_VERIFY, ctx_verify));
    // BBB-AIRGAP: remove the added handler before freeing its shared wait data.
    JADE_ZERO_VERIFY(esp_event_handler_instance_unregister(GUI_EVENT, GUI_ALT_EVENT, ctx_alt));
    free_wait_event_data(wait_data);
    return ev_id;
}

// Helper to display mnemonic words, and then have the user confirm some
// NOTE: this function replaces spaces with \0's in the passed mnemonic!
static bool display_confirm_mnemonic(const size_t nwords, char* mnemonic, const size_t mnemonic_len)
{
    // Support 12-word and 24-word mnemonics only
    JADE_ASSERT(is_valid_mnemonic_length(nwords) && mnemonic);

    // Show the warning banner screen, user to confirm
    // BBB-AIRGAP: a device can be set to leave this banner out (Features).  What it guards is
    // the user's attention, not the words: the screens either side of it are unchanged, and the
    // words are shown on the next screen whichever way this one is answered other than 'back'.
    if (storage_get_feature_flags() & FEATURE_FLAGS_HARSH_WARNINGS) {
        const char* message[] = { "These words are your", "wallet. Keep them", "protected and offline." };
        if (!await_continueback_activity(NULL, message, 3, true, "blkstrm.com/phrase")) {
            // Abandon before we begin
            return false;
        }
    }

    // Change the word separator to a null so we can treat each word as a terminated string.
    uint16_t word_offs[MNEMONIC_MAXWORDS]; // large enough for 12 and 24 word mnemonic
    change_mnemonic_word_separator(mnemonic, mnemonic_len, ' ', '\0', word_offs, nwords);
    bool mnemonic_confirmed = false;

    // create the "show mnemonic" activities only once and then reuse them
    gui_activity_t* first_activity = NULL;
    gui_activity_t* last_activity = NULL;
    make_show_mnemonic_activities(&first_activity, &last_activity, mnemonic, word_offs, nwords);
    JADE_ASSERT(first_activity && last_activity);

    while (!mnemonic_confirmed) {
        int32_t ev_id = await_mnemonic_pages_exit(first_activity);
        if (ev_id == BTN_MNEMONIC_EXIT) {
            // User abandonded
            JADE_LOGD("user abandoned noting mnemonic");
            goto cleanup;
        }

        // User ready to verify mnemonic
        JADE_ASSERT(ev_id == BTN_MNEMONIC_VERIFY);
        JADE_LOGD("moving on to confirm mnemonic");

        // Confirm the mnemonic - show groups of three consecutive words
        // and have user confirm one of them at random.
        // Ensures all words are at the very least displayed.
        mnemonic_confirmed = true; // will be set to false if wrong word selected
        const size_t num_words_options = nwords == MNEMONIC_MAXWORDS ? 8 : 6;
        for (size_t i = 0; i < nwords; i += 3) {
            const size_t offset_word_to_confirm = get_uniform_random_byte(3);
            const size_t selected = i + offset_word_to_confirm;
            gui_view_node_t* textbox = NULL;
            // BBB-AIRGAP: the backup check opts out of the KEY3 escape.  The words are on the
            // screen the user is being asked about; a stray press must not end the check and leave
            // a wallet whose backup was never confirmed.  Its own 'incorrect' path is the way out.
            gui_activity_t* const confirm_act
                = make_confirm_mnemonic_word_activity(&textbox, i, offset_word_to_confirm, mnemonic, word_offs, nwords);
            gui_activity_set_escape(confirm_act, false);
            JADE_LOGD("selected = %zu", selected);

            // Pick some other words from the mnemonic as options, but avoid
            // the words currently displayed on screen (neighbouring words).
            // Large enough for 12 and 24 word mnemonic
            bool already_picked[MNEMONIC_MAXWORDS] = { false };
            already_picked[i] = true;
            already_picked[i + 1] = true;
            already_picked[i + 2] = true;

            // Large enough for 12 and 24 word mnemonic
            // (Only really needs to be as big as 'num_words_options' so MAXWORDS is plenty)
            size_t random_words[MNEMONIC_MAXWORDS] = { 0 };
            random_words[0] = selected;

            for (size_t j = 1; j < num_words_options; ++j) {
                size_t new_word;
                do {
                    new_word = get_uniform_random_byte(nwords);
                } while (already_picked[new_word]);

                already_picked[new_word] = true;
                random_words[j] = new_word;
            }

            // set the first word
            uint8_t index = get_uniform_random_byte(num_words_options);
            gui_update_text(textbox, mnemonic + word_offs[random_words[index]]);

            // BBB-AIRGAP: keep one handler live for this whole confirmation screen. Re-registering
            // after every wheel/click event leaves a gap where the next press is delivered to the
            // previous semaphore and lost, with the recovery phrase still drawn and no waiter for
            // that press. The activity owns both the registration and its wait data, so switching
            // away removes the handler before freeing the data.
            wait_event_data_t* const event_data = gui_activity_make_wait_event_data(confirm_act);
            gui_activity_register_event(confirm_act, GUI_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);

            // Switched synchronously so the drain below has something to drain: the asynchronous
            // gui_set_current_activity() only queues the switch, and this activity's handlers go
            // live later, on the gui task.  What is discarded is the second half of the press that
            // opened this screen - a single press posts GUI_BUTTON_EVENT via select_action() and
            // then, unconditionally, its own GUI_EVENT click (main/gui.c:2556-2573), and the
            // registration above takes any GUI_EVENT.  This screen is rebuilt for every word, so
            // the press that confirmed the previous word is exactly what would land here and
            // confirm this one at whichever option it opened on - almost always the wrong one,
            // sending the user back to check a recovery phrase that was written down correctly.
            // Same 10ms idle timeout as run_list_activity() (main/ui/dialogs.c).
            gui_set_current_activity_sync(confirm_act, false);
            while (sync_wait_event(event_data, NULL, NULL, NULL, 10 / portTICK_PERIOD_MS) == ESP_OK) {
                // discard - see comment above
            }

            bool stop = false;
            while (!stop) {
                // wait for a GUI event
                ev_id = ESP_EVENT_ANY_ID;
                JADE_ZERO_VERIFY(sync_wait_event(event_data, NULL, &ev_id, NULL, 0));

                switch (ev_id) {
                case GUI_WHEEL_LEFT_EVENT:
                    index = (index + num_words_options - 1) % num_words_options;
                    gui_update_text(textbox, mnemonic + word_offs[random_words[index]]);
                    break;

                case GUI_WHEEL_RIGHT_EVENT:
                    index = (index + 1) % num_words_options;
                    gui_update_text(textbox, mnemonic + word_offs[random_words[index]]);
                    break;

                default:
                    // Stop the loop on a 'click' event
                    stop = (ev_id == gui_get_click_event());
                    break;
                }
            }

            JADE_LOGD("selected word at index %u", index);

            // the wrong word has been selected
            if (random_words[index] != selected) {
                await_error_3("Incorrect. Check your", "recovery phrase and", "try again.");
                mnemonic_confirmed = false;
                break;
            }
        }
    }

    JADE_ASSERT(mnemonic_confirmed);
    JADE_LOGD("mnemonic confirmed");

cleanup:
    return mnemonic_confirmed;
}

// BBB-AIRGAP: the backup screens exist only where the entropy does, and entropy is held only on a
// device with a camera (see derive_keychain below) - so the whole surface is built under the same
// condition rather than laid out and then never reached.
#ifdef CONFIG_HAS_CAMERA
// BBB-AIRGAP: how many words a mnemonic holds, counted from its separators rather than derived
// from the entropy - the caller only needs the count, and deriving it would mean holding the
// entropy in another buffer for no other reason.
static size_t count_mnemonic_words(const char* mnemonic)
{
    JADE_ASSERT(mnemonic);

    size_t nwords = 1;
    for (const char* p = mnemonic; *p; ++p) {
        if (*p == ' ') {
            ++nwords;
        }
    }
    return nwords;
}

// BBB-AIRGAP: the words of the wallet in use, shown and nothing more.  The setup flow reaches the
// same screens on its way to the verification quiz (display_confirm_mnemonic above); here there is
// nothing after them, so the forward button on the last page leaves just as the back button does.
static void show_wallet_words(char* mnemonic, const size_t mnemonic_len, const size_t nwords)
{
    JADE_ASSERT(mnemonic);
    JADE_ASSERT(nwords == 12 || nwords == 24);

    // BBB-AIRGAP: as in display_confirm_mnemonic() above - the same banner, and the same setting.
    if (storage_get_feature_flags() & FEATURE_FLAGS_HARSH_WARNINGS) {
        const char* message[] = { "These words are your", "wallet. Keep them", "protected and offline." };
        if (!await_continueback_activity(NULL, message, 3, true, "blkstrm.com/phrase")) {
            return;
        }
    }

    // NOTE: this replaces the spaces in the passed mnemonic with NULs, as the screens below take
    // each word as its own string.
    uint16_t word_offs[MNEMONIC_MAXWORDS];
    change_mnemonic_word_separator(mnemonic, mnemonic_len, ' ', '\0', word_offs, nwords);

    gui_activity_t* first_activity = NULL;
    gui_activity_t* last_activity = NULL;
    make_show_mnemonic_activities(&first_activity, &last_activity, mnemonic, word_offs, nwords);
    JADE_ASSERT(first_activity);

    // Either end of the chain leaves: there is nothing after these screens to move on to.
    await_mnemonic_pages_exit(first_activity);
}

// BBB-AIRGAP: builds the words of the wallet in use from the entropy its slot holds, hands them to
// 'fn', and wipes them again.  Every backup screen needs them the same way, so the fetching and
// the wiping live in one place rather than in each of them.
static void with_wallet_words(void (*fn)(char*, size_t, size_t))
{
    JADE_ASSERT(fn);

    char* mnemonic = NULL;
    if (!keychain_export_mnemonic(&mnemonic)) {
        // The menu only offers these screens for a wallet that has entropy, so this is not a state
        // the user can reach - say so rather than showing an empty screen.
        JADE_LOGE("Backup screen requested for a wallet with no entropy");
        await_message("Backup unavailable");
        return;
    }

    const size_t mnemonic_len = strlen(mnemonic);
    SENSITIVE_PUSH(mnemonic, mnemonic_len);
    fn(mnemonic, mnemonic_len, count_mnemonic_words(mnemonic));
    SENSITIVE_POP(mnemonic);
    JADE_WALLY_VERIFY(wally_free_string(mnemonic));
}

static void verify_wallet_words(char* mnemonic, const size_t mnemonic_len, const size_t nwords)
{
    if (display_confirm_mnemonic(nwords, mnemonic, mnemonic_len)) {
        // Said out loud because this screen is reached on purpose, unlike the setup flow where
        // passing the quiz simply moves on to the next step.
        await_message("Backup Verified");
    }
}

// BBB-AIRGAP: the backup screens for the wallet in use, grouped the way SeedSigner groups them
// (View words / Export SeedQR / Verify backup).  All three need the words, which exist only for a
// wallet whose slot still holds the entropy they were built from, so the caller offers this menu
// on that condition - see handle_session() in main/process/dashboard.c.
void handle_wallet_backup(void)
{
    size_t selected = 0;
    while (true) {
        // BBB-AIRGAP: an escape started on a screen this menu opened has to keep going; the list
        // itself only sees KEY3 while it is the one waiting.
        if (gui_escape_pending()) {
            return;
        }
        list_item_t items[3];
        size_t num_items = 0;
        items[num_items++] = (list_item_t){ .txt = "View Words", .ev_id = BTN_WALLET_BACKUP_VIEW };
        items[num_items++] = (list_item_t){ .txt = "Export SeedQR", .ev_id = BTN_WALLET_BACKUP_SEEDQR };
        items[num_items++] = (list_item_t){ .txt = "Verify Backup", .ev_id = BTN_WALLET_BACKUP_VERIFY };
        JADE_ASSERT(num_items <= sizeof(items) / sizeof(items[0]));

        const int32_t ev_id = run_list_activity("Backup", BTN_WALLET_BACKUP_EXIT, items, num_items, &selected);
        switch (ev_id) {
        case BTN_WALLET_BACKUP_VIEW:
            with_wallet_words(show_wallet_words);
            break;

        case BTN_WALLET_BACKUP_SEEDQR:
            export_wallet_seedqr();
            break;

        case BTN_WALLET_BACKUP_VERIFY:
            with_wallet_words(verify_wallet_words);
            break;

        case BTN_WALLET_BACKUP_EXIT:
        default:
            return;
        }
    }
}
#endif // CONFIG_HAS_CAMERA

// NOTE: only the English wordlist is supported.
// BBB-AIRGAP: 'entropy' is user-supplied (eg. dice rolls); pass NULL to use the
// device RNG, which is the upstream behaviour.
static bool mnemonic_new(
    const size_t nwords, const uint8_t* entropy, const size_t entropy_len, char* mnemonic, const size_t mnemonic_len)
{
    // Support 12-word and 24-word mnemonics only
    JADE_ASSERT(is_valid_mnemonic_length(nwords) && mnemonic && mnemonic_len == MNEMONIC_BUFLEN);

    // Generate and show the mnemonic - NOTE: only the English wordlist is supported.
    char* new_mnemonic = NULL;
    if (entropy) {
        JADE_ASSERT(entropy_len == (nwords == 12 ? BIP39_ENTROPY_LEN_128 : BIP39_ENTROPY_LEN_256));
        JADE_WALLY_VERIFY(bip39_mnemonic_from_bytes(NULL, entropy, entropy_len, &new_mnemonic));
        JADE_WALLY_VERIFY(bip39_mnemonic_validate(NULL, new_mnemonic));
    } else {
        keychain_get_new_mnemonic(&new_mnemonic, nwords);
    }
    JADE_ASSERT(new_mnemonic);
    const size_t new_mnemonic_len = strnlen(new_mnemonic, MNEMONIC_BUFLEN);
    JADE_ASSERT(new_mnemonic_len < MNEMONIC_BUFLEN); // buffer should be large enough for any mnemonic
    SENSITIVE_PUSH(new_mnemonic, new_mnemonic_len);

    // Copy into output buffer
    strcpy(mnemonic, new_mnemonic);

    // Have user view and confirm mnemonic words
    const bool mnemonic_confirmed = display_confirm_mnemonic(nwords, new_mnemonic, new_mnemonic_len);

    SENSITIVE_POP(new_mnemonic);
    JADE_WALLY_VERIFY(wally_free_string(new_mnemonic));

    return mnemonic_confirmed;
}

// NOTE: only the English wordlist is supported.
static void enable_relevant_chars(const bool is_mnemonic, const char* word, const size_t word_len,
    const size_t* filter_word_list, const size_t filter_word_list_size, gui_activity_t* act, gui_view_node_t* backspace,
    gui_view_node_t* enter, gui_view_node_t** btns, const size_t btns_len)
{
    // word_len may be zero if no word entered as yet
    // input_wordlist is optional
    JADE_ASSERT(word && (filter_word_list || !filter_word_list_size));
    // btns_len is expected to be 26 (A->Z)
    JADE_ASSERT(act && backspace && enter && btns && btns_len == 26);
    JADE_ASSERT(backspace->activity == act && enter->activity == act);

    JADE_LOGD("word = %s, word_len = %zu", word, word_len);

    // Enable enter if a) not entering a mnemonic, and b) not part-way through entering a word
    // Enable backspace in all cases.
    gui_set_active(enter, !is_mnemonic && !word_len);
    gui_set_active(backspace, true);

    // TODO: are there any invalid characters to start the word?

    // No characters currently selected (ie. no word stem)
    bool enabled[26] = { false };
    uint8_t num_enabled = 0;

    // If an 'filter_word_list' is passed, we iterate that and use the entries as a lookup
    // into the bip39 wordlist - if not passed we iterate the entire bip39 wordlist directly.
    const size_t limit = filter_word_list ? filter_word_list_size : BIP39_WORDLIST_LEN;
    for (size_t index = 0; index < limit; ++index) {
        const size_t wordlist_index = filter_word_list ? filter_word_list[index] : index;
        JADE_ASSERT(wordlist_index < BIP39_WORDLIST_LEN);

        // TODO: check strlen(wordlist_extracted)
        const char* wordlist_extracted = bip39_get_word_by_index(NULL, wordlist_index);
        JADE_ASSERT(wordlist_extracted);

        // If we have the first letter(s) typed, we can a) skip all preceding words
        // and also b) exit once we have passed beyond the relevant words.
        if (word_len > 0) {
            const int32_t res = strncmp(wordlist_extracted, word, word_len);
            if (res < 0) {
                // Not yet reached words with 'word' stem - loop to next word
                continue;
            } else if (res > 0) {
                // Gone past words with 'word' stem - may as well break
                break;
            }
        }

        // Wordlist word starts with given 'word' stem
        // See what the next letter is, and ensure that character is enabled
        // (Consider first letter of word if no given stem).
        const size_t char_index = wordlist_extracted[word_len] - 'a';
        if (!enabled[char_index]) {
            enabled[char_index] = true;
            ++num_enabled;
        }
    }
    JADE_ASSERT(num_enabled > 0);

    // Select a random active letter as the selected one
    uint8_t iselected = get_uniform_random_byte(num_enabled);
    gui_view_node_t* selected = NULL;
    for (size_t i = 0; i < btns_len; ++i) {
        JADE_ASSERT(btns[i]->activity == act);

        // Set item to select
        if (enabled[i] && !iselected--) {
            JADE_ASSERT(!selected);
            selected = btns[i];
        }
    }
    JADE_ASSERT(selected);

    // Update the ui
    gui_activity_set_active_selection(act, btns, btns_len, enabled, selected);
}

// NOTE: only the English wordlist is supported.
static size_t valid_words(const char* word, const size_t word_len, const size_t* filter_word_list,
    const size_t filter_word_list_size, size_t* output_word_list, const size_t output_word_list_len, bool* exact_match)
{
    // word_len may be zero if no word entered as yet
    // input_wordlist is optional
    JADE_ASSERT(word && (filter_word_list || !filter_word_list_size));
    JADE_ASSERT(output_word_list && output_word_list_len && exact_match);

    *exact_match = false;
    size_t num_possible_words = 0;

    // If no word stem or filter_word_list is given we can trivially return 'the whole wordlist'
    if (!word_len && !filter_word_list) {
        for (size_t i = 0; i < output_word_list_len; ++i) {
            output_word_list[i] = i;
        }
        return BIP39_WORDLIST_LEN;
    }

    // Otherwise we need to check the word prefixes match
    // If an 'filter_word_list' is passed, we iterate that and use the entries as a lookup
    // into the bip39 wordlist - if not passed we iterate the entire bip39 wordlist directly.
    const size_t limit = filter_word_list ? filter_word_list_size : BIP39_WORDLIST_LEN;
    for (size_t index = 0; index < limit; ++index) {
        const size_t wordlist_index = filter_word_list ? filter_word_list[index] : index;
        JADE_ASSERT(wordlist_index < BIP39_WORDLIST_LEN);

        // TODO: check strlen(wordlist_extracted)
        const char* wordlist_extracted = bip39_get_word_by_index(NULL, wordlist_index);
        JADE_ASSERT(wordlist_extracted);

        // Test if passed 'word' is a valid prefix of the wordlist word
        const int32_t res = strncmp(wordlist_extracted, word, word_len);

        if (res < 0) {
            // No there yet, continue to next word
            continue;
        } else if (res > 0) {
            // Too late - gone past word - may as well abandon
            break;
        }

        // If prefix matches, see if it is an exact match for the entire word
        // (ie. word lengths are also same)
        if (wordlist_extracted[word_len] == '\0') {
            JADE_ASSERT(!num_possible_words); // should only happen on first match ...
            JADE_ASSERT(!*exact_match); // and so should only happen at most once!
            *exact_match = true;
        }

        // Return at most first output_word_list_len compatible words
        if (num_possible_words < output_word_list_len) {
            output_word_list[num_possible_words] = wordlist_index;
        }

        ++num_possible_words;
    }

    return num_possible_words;
}

// NOTE: only the English wordlist is supported.
static size_t valid_final_words(const char** mnemonic_words, const size_t num_mnemonic_words,
    size_t* possible_word_list, const size_t possible_word_list_len)
{
    JADE_ASSERT(mnemonic_words && num_mnemonic_words < SIZE_MAX && is_valid_mnemonic_length(num_mnemonic_words + 1));
    JADE_ASSERT(possible_word_list && possible_word_list_len);

    // Copy the mnemonic-thus-far into a work area
    char buf[MNEMONIC_BUFLEN];
    SENSITIVE_PUSH(buf, sizeof(buf));
    size_t offset = 0;
    for (size_t i = 0; i < num_mnemonic_words; ++i) {
        const size_t remaining = sizeof(buf) - offset;
        const int ret = snprintf(buf + offset, remaining, "%s", mnemonic_words[i]);
        JADE_ASSERT(ret > 0 && ret < remaining);
        offset += ret;
        buf[offset++] = ' ';
    }

    size_t num_possible_words = 0;
    for (size_t wordlist_index = 0; wordlist_index < BIP39_WORDLIST_LEN; ++wordlist_index) {
        const char* wordlist_extracted = bip39_get_word_by_index(NULL, wordlist_index);
        JADE_ASSERT(wordlist_extracted);
        const size_t remaining = sizeof(buf) - offset;
        const int ret = snprintf(buf + offset, remaining, "%s", wordlist_extracted);
        JADE_ASSERT(ret >= 3 && ret < remaining && buf[offset + ret] == '\0');

        if (bip39_mnemonic_validate(NULL, buf) == WALLY_OK) {
            // Return first possible_word_list_len valid words
            if (num_possible_words < possible_word_list_len) {
                possible_word_list[num_possible_words] = wordlist_index;
            }
            ++num_possible_words;
        }
    }

    SENSITIVE_POP(buf);
    return num_possible_words;
}

typedef enum { FINAL_WORD_EXISTING, FINAL_WORD_CALCULATE, FINAL_WORD_ABANDON } final_word_action_t;

static final_word_action_t select_final_word_action(void)
{
    gui_activity_t* const act = make_calculate_final_word_activity();
    while (true) {
        // BBB-AIRGAP: this question and its help screen are escapable even though the word entry
        // they sit in front of is exempt, so an escape raised on either is reported to the caller
        // rather than swallowed by reopening the question.
        if (gui_escape_pending()) {
            return FINAL_WORD_ABANDON;
        }
        gui_set_current_activity(act);

        int32_t ev_id = GUI_BUTTON_EVENT_NONE;
        if (gui_activity_wait_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
            if (ev_id == BTN_MNEMONIC_FINAL_WORD_EXISTING) {
                return FINAL_WORD_EXISTING;
            } else if (ev_id == BTN_MNEMONIC_FINAL_WORD_CALCULATE) {
                return FINAL_WORD_CALCULATE;
            } else if (ev_id == BTN_MNEMONIC_FINAL_WORD_HELP) {
                await_qr_help_activity("blkstrm.com/finalword");
            }
        }
    }
}

static size_t calculate_valid_final_words(
    const char* wordlist_words[], const size_t word_index, const size_t nwords, size_t* final_words)
{
    JADE_ASSERT(wordlist_words && is_valid_mnemonic_length(nwords) && word_index == nwords - 1 && final_words);

    const size_t num_words = valid_final_words(wordlist_words, word_index, final_words, MAX_NUM_FINAL_WORDS);
    JADE_ASSERT(num_words == (nwords == 12 ? 128 : 8)); // expected due to checksum bits
    return num_words;
}

static void write_wordlist_words(
    const char* wordlist_words[], const size_t nwords, char* output, const size_t output_len)
{
    JADE_ASSERT(wordlist_words && nwords <= MNEMONIC_MAXWORDS);
    JADE_ASSERT(output && output_len >= mnemonic_buffer_size(nwords));

    output[0] = '\0';
    size_t offset = 0;
    for (size_t word_index = 0; word_index < nwords; ++word_index) {
        JADE_ASSERT(wordlist_words[word_index]);
        if (offset > 0) {
            output[offset++] = ' ';
        }
        const int ret = snprintf(output + offset, output_len - offset, "%s", wordlist_words[word_index]);
        JADE_ASSERT(ret > 0 && ret < output_len - offset);
        offset += ret;
    }
}

typedef enum { WORDLIST_WORD_SELECTED, WORDLIST_WORD_BACKSPACE, WORDLIST_WORD_DONE } wordlist_word_result_t;

// Nodes shared by the keyboard and carousel used to select a BIP39 word.
typedef struct {
    gui_activity_t* enter_word_activity;
    gui_view_node_t* titletext;
    gui_view_node_t* textbox;
    gui_view_node_t* backspace;
    gui_view_node_t* enter;
    gui_view_node_t* keys[WORD_ENTRY_KEYS_LEN];
    gui_activity_t* choose_word_activity;
    gui_view_node_t* label;
    gui_view_node_t* text_selection;
} word_entry_ui_t;

static void make_word_entry_ui(word_entry_ui_t* ui, const bool show_enter_btn, const char* select_word_title)
{
    JADE_ASSERT(ui && select_word_title);

    ui->enter_word_activity = make_enter_wordlist_word_activity(
        &ui->titletext, show_enter_btn, &ui->textbox, &ui->backspace, &ui->enter, ui->keys, WORD_ENTRY_KEYS_LEN);
    JADE_ASSERT(ui->enter);
    ui->enter->is_active = show_enter_btn;

    ui->choose_word_activity = make_carousel_activity(select_word_title, &ui->label, &ui->text_selection);

    // BBB-AIRGAP: both word entry screens opt out of the KEY3 escape - one press must not throw
    // away the letters typed for this word, nor the words entered before it.  The screens' own
    // backspace and 'back' remain the way out.
    gui_activity_set_escape(ui->enter_word_activity, false);
    gui_activity_set_escape(ui->choose_word_activity, false);
}

static wordlist_word_result_t select_wordlist_word(const bool is_mnemonic, const size_t word_index,
    const char* wordlist_words[], const size_t* p_filter_words, const size_t num_filter_words,
    const bool random_first_selection_word, word_entry_ui_t* ui, const char** selected_word)
{
    JADE_ASSERT(word_index < MNEMONIC_MAXWORDS && wordlist_words);
    JADE_ASSERT(!p_filter_words == !num_filter_words); // p_filter_words and num_filter_words are optional
    JADE_ASSERT(ui && ui->enter_word_activity && ui->textbox && ui->backspace && ui->enter && ui->choose_word_activity
        && ui->label && ui->text_selection);
    JADE_ASSERT(selected_word);
    *selected_word = NULL;

    size_t char_index = 0;
    char word[MNEMONIC_MAX_WORD_LEN + 1] = { 0 };
    SENSITIVE_PUSH(word, sizeof(word));
    gui_update_text(ui->textbox, word);
    wordlist_word_result_t result = WORDLIST_WORD_DONE;

    while (true) {
        JADE_ASSERT(char_index < 6); // must have found a word by then!

        size_t possible_word_list[NUM_WORDS_SELECT];
        bool exact_match = false; // not interested in any case
        const size_t possible_words = valid_words(
            word, char_index, p_filter_words, num_filter_words, possible_word_list, NUM_WORDS_SELECT, &exact_match);
        JADE_ASSERT(possible_words > 0);

        bool selected_backspace = false;
        if (possible_words <= NUM_WORDS_SELECT) {
            // 'Small' number of words - allow user to select from these words
            char choose_word_title[16]; // sufficient
            const int ret = snprintf(choose_word_title, sizeof(choose_word_title), "Select word %zu", word_index + 1);
            JADE_ASSERT(ret > 0 && ret < sizeof(choose_word_title));
            gui_update_text(ui->label, choose_word_title);

            bool stop = false;
            size_t selected = random_first_selection_word ? get_uniform_random_byte(possible_words) : 0;
            const char* wordlist_extracted = NULL;
            while (!stop) {
                JADE_ASSERT(selected <= possible_words && !wordlist_extracted);

                // Update current selection
                if (selected == possible_words) { // delete
                    gui_set_text_font(ui->text_selection, DEJAVU24_FONT);
                    gui_update_text(ui->text_selection, "|");
                } else {
                    // word from wordlist
                    wordlist_extracted = bip39_get_word_by_index(NULL, possible_word_list[selected]);
                    JADE_ASSERT(wordlist_extracted);
                    gui_set_text_font(ui->text_selection, GUI_DEFAULT_FONT);
                    gui_update_text(ui->text_selection, wordlist_extracted);
                }

                // Ensure activity displayed
                gui_set_current_activity(ui->choose_word_activity);

                int32_t ev_id = GUI_BUTTON_EVENT_NONE;
                gui_activity_wait_event(ui->choose_word_activity, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);

                switch (ev_id) {
                case GUI_WHEEL_LEFT_EVENT:
                    // Avoid unsigned wrapping below zero
                    selected = (selected + (possible_words + 1) - 1) % (possible_words + 1);
                    break;

                case GUI_WHEEL_RIGHT_EVENT:
                    selected = (selected + 1) % (possible_words + 1);
                    break;

                default:
                    // Stop the loop on a 'click' event
                    stop = (ev_id == gui_get_click_event());
                }

                // If looping to new word, NULL the current word
                if (!stop && wordlist_extracted) {
                    wordlist_extracted = NULL;
                }
            } // while !stop

            // Word (or backspace) selected
            JADE_ASSERT(selected <= possible_words);
            selected_backspace = (selected == possible_words);

            if (!selected_backspace) {
                JADE_ASSERT(wordlist_extracted);
                *selected_word = wordlist_extracted;
                result = WORDLIST_WORD_SELECTED;
                break;
            }
        } else {
            // 'Large' number of words for any typed stem - use keyboard screen to further restrict words

            // Update the typed word and ensure activity set as current
            if (is_mnemonic) {
                // For a mnemonic, show only the current word
                gui_update_text(ui->textbox, word);
            } else {
                // Otherwise show last 3 words
                char buf[32]; // sufficient
                const char* shown[3] = { "", "", "" };
                if (word_index == 0) {
                    shown[0] = word;
                } else if (word_index == 1) {
                    shown[0] = wordlist_words[0];
                    shown[1] = word;
                } else if (word_index == 2) {
                    shown[0] = wordlist_words[word_index - 2];
                    shown[1] = wordlist_words[word_index - 1];
                    shown[2] = word;
                } else if (char_index == 0) {
                    shown[0] = wordlist_words[word_index - 3];
                    shown[1] = wordlist_words[word_index - 2];
                    shown[2] = wordlist_words[word_index - 1];
                } else {
                    shown[0] = wordlist_words[word_index - 2];
                    shown[1] = wordlist_words[word_index - 1];
                    shown[2] = word;
                }
                const bool show_ellipsis = (word_index > 3) || (word_index == 3 && char_index > 0);
                const char* prefix = show_ellipsis ? "... " : "";
                const int ret = snprintf(buf, sizeof(buf), "%s%s %s %s", prefix, shown[0], shown[1], shown[2]);
                JADE_ASSERT(ret >= 0 && ret < sizeof(buf));
                gui_update_text(ui->textbox, buf);
            }
            gui_set_current_activity(ui->enter_word_activity);

            // Update which letters are active/available
            enable_relevant_chars(is_mnemonic, word, char_index, p_filter_words, num_filter_words,
                ui->enter_word_activity, ui->backspace, ui->enter, ui->keys, WORD_ENTRY_KEYS_LEN);

            int32_t ev_id = GUI_BUTTON_EVENT_NONE;
            gui_activity_wait_event(ui->enter_word_activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);
            selected_backspace = (ev_id == BTN_KEYBOARD_BACKSPACE);
            if (ev_id == BTN_KEYBOARD_ENTER) {
                result = WORDLIST_WORD_DONE;
                break;
            }
            if (!selected_backspace) {
                // Character/letter was clicked
                const char letter_selected = ev_id - BTN_KEYBOARD_ASCII_OFFSET;
                if (letter_selected >= 'A' && letter_selected <= 'Z') {
                    word[char_index] = tolower(letter_selected);
                    word[++char_index] = '\0';
                }
            }
        }

        // Handle any backspace/delete option
        if (selected_backspace) {
            if (char_index > 0) {
                // Go back one character
                word[--char_index] = '\0';
            } else {
                result = WORDLIST_WORD_BACKSPACE;
                break;
            }
        }
    }

    SENSITIVE_POP(word);
    return result;
}

static wordlist_word_result_t select_resolved_word_number(const size_t word_index, const char* word,
    gui_activity_t* choose_word_activity, gui_view_node_t* label, gui_view_node_t* text_selection)
{
    JADE_ASSERT(word && choose_word_activity && label && text_selection);

    char confirm_word_title[16]; // sufficient
    const int ret = snprintf(confirm_word_title, sizeof(confirm_word_title), "Confirm word %zu", word_index + 1);
    JADE_ASSERT(ret > 0 && ret < sizeof(confirm_word_title));
    gui_update_text(label, confirm_word_title);

    size_t selected = 0;
    while (true) {
        if (selected == 0) {
            gui_set_text_font(text_selection, GUI_DEFAULT_FONT);
            gui_update_text(text_selection, word);
        } else {
            gui_set_text_font(text_selection, DEJAVU24_FONT);
            gui_update_text(text_selection, "|");
        }

        gui_set_current_activity(choose_word_activity);

        int32_t ev_id = GUI_BUTTON_EVENT_NONE;
        gui_activity_wait_event(choose_word_activity, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);

        switch (ev_id) {
        case GUI_WHEEL_LEFT_EVENT:
        case GUI_WHEEL_RIGHT_EVENT:
            selected = (selected + 1) % 2;
            break;

        default:
            if (ev_id == gui_get_click_event()) {
                return selected == 0 ? WORDLIST_WORD_SELECTED : WORDLIST_WORD_BACKSPACE;
            }
            break;
        }
    }
}

// NOTE: only the English wordlist is supported.
static size_t get_wordlist_words(
    const wordlist_purpose_t purpose, const size_t nwords, char* output, const size_t output_len)
{
    // 'title' is optional (and will default if not provided)
    JADE_ASSERT(nwords <= MNEMONIC_MAXWORDS);
    JADE_ASSERT(output && output_len >= mnemonic_buffer_size(nwords)); // words plus trailing space

    // Only 12 and 24 word mnemonics are supported
    const bool is_mnemonic = (purpose == MNEMONIC_SIMPLE) || (purpose == MNEMONIC_ADVANCED);
    JADE_ASSERT(is_valid_mnemonic_length(nwords) || !is_mnemonic);

    const bool show_enter_btn = !is_mnemonic; // Don't show 'done' button when entering mnemonic words
    const char* select_word_title = purpose == WORDLIST_PASSPHRASE ? "Enter Passphrase" : "Recover Wallet";
    word_entry_ui_t ui = { 0 };
    make_word_entry_ui(&ui, show_enter_btn, select_word_title);

    JADE_ASSERT(ui.titletext);
    if (purpose == WORDLIST_PASSPHRASE) {
        // Fixed title for all words
        gui_update_text(ui.titletext, "Enter Passphrase");
    }

    // For each word
    const char* wordlist_words[MNEMONIC_MAXWORDS] = { 0 };
    SENSITIVE_PUSH(wordlist_words, sizeof(wordlist_words));
    size_t word_index = 0;
    bool done_entering_words = false;
    while (word_index < nwords && !done_entering_words) {
        JADE_ASSERT(!wordlist_words[word_index]);

        // When in 'Advanced' mode, if this is the final mnemonic word, have the option to additionally
        // filter to valid final words - ie. ones where the checksum is correct for the mnemonic as a whole.
        const size_t* p_filter_words = NULL;
        size_t num_filter_words = 0;
        size_t final_words[MAX_NUM_FINAL_WORDS];
        SENSITIVE_PUSH(final_words, sizeof(final_words));
        bool random_first_selection_word = false;
        if (purpose == MNEMONIC_ADVANCED && word_index == nwords - 1) {
            const final_word_action_t action = select_final_word_action();
            if (action == FINAL_WORD_ABANDON) {
                // BBB-AIRGAP: the escape ends the whole entry, as a backspace on the first word
                // does - the caller reads a count of zero as abandoned.
                SENSITIVE_POP(final_words);
                SENSITIVE_POP(wordlist_words);
                return 0;
            }
            if (action == FINAL_WORD_CALCULATE) {
                // Fetch valid final words to use as additional filter
                display_processing_message_activity();
                num_filter_words = calculate_valid_final_words(wordlist_words, word_index, nwords, final_words);
                p_filter_words = final_words;

                // When we select from the valid words, randomise the initally selected word
                random_first_selection_word = true;
            }
        }

        // Reset default title for next word when entering mnemonic phrase
        if (is_mnemonic) {
            char enter_word_title[16];
            const int ret = snprintf(enter_word_title, sizeof(enter_word_title), "Insert word %zu", word_index + 1);
            JADE_ASSERT(ret > 0 && ret < sizeof(enter_word_title));
            gui_update_text(ui.titletext, enter_word_title);
        }

        const char* wordlist_extracted = NULL;
        const wordlist_word_result_t word_rslt = select_wordlist_word(is_mnemonic, word_index, wordlist_words,
            p_filter_words, num_filter_words, random_first_selection_word, &ui, &wordlist_extracted);

        switch (word_rslt) {
        case WORDLIST_WORD_SELECTED:
            // Store the matched word in the selected words array
            JADE_ASSERT(wordlist_extracted && !wordlist_words[word_index]);
            wordlist_words[word_index++] = wordlist_extracted;
            break;

        case WORDLIST_WORD_DONE:
            done_entering_words = true;
            break;

        case WORDLIST_WORD_BACKSPACE:
            if (word_index > 0) {
                // Deleting when no characters entered for this word
                // Go back to previous word - this breaks out of the 'per character'
                // loop so we go back round the outer 'per word' loop.
                JADE_ASSERT(!wordlist_words[word_index]);
                --word_index;

                // NULL the cached previous word, as we start that one from scratch
                JADE_ASSERT(wordlist_words[word_index]);
                wordlist_words[word_index] = NULL;
            } else {
                // Backspace at start of first word -
                // - if entering a mnemonic, abandon mnemonic entry back to previous screen
                // - if not entering a mnemonic, ignore this button at this time - user can
                //   use 'enter' button to select empty string / no words.
                JADE_ASSERT(!wordlist_words[word_index]);
                if (is_mnemonic) {
                    SENSITIVE_POP(final_words);
                    SENSITIVE_POP(wordlist_words);
                    return 0; // no words entered
                }
            }
            break;
        }

        SENSITIVE_POP(final_words);
    } // cycle on words

    // If entering mnemonic should have 'nwords' word indices in 'wordlist_words'
    const size_t words_entered = word_index;
    JADE_ASSERT(words_entered == nwords || !is_mnemonic);

    // Convert array of wally wordlist strings to a single string
    write_wordlist_words(wordlist_words, words_entered, output, output_len);
    SENSITIVE_POP(wordlist_words);
    return words_entered;
}

static void clear_word_number_restore_ui(
    digit_entry_t* digit_entry, word_entry_ui_t* calc_ui, gui_view_node_t* number_text_selection)
{
    JADE_ASSERT(digit_entry && calc_ui && calc_ui->textbox && calc_ui->text_selection && number_text_selection);

    reset_digit_entry(digit_entry, "Word Number");
    gui_update_text(calc_ui->textbox, "");
    gui_update_text(calc_ui->text_selection, "");
    gui_update_text(number_text_selection, "");
}

static size_t get_word_number_words(
    const size_t nwords, const bool advanced_mode, char* output, const size_t output_len)
{
    JADE_ASSERT(is_valid_mnemonic_length(nwords));
    JADE_ASSERT(output && output_len >= mnemonic_buffer_size(nwords));

    digit_entry_t digit_entry = { .entry_type = DIGIT_ENTRY_WORD_NUMBER,
        .initial_state = RANDOM,
        .digits_shown = true,
        .max_digits = DIGIT_ENTRY_WORD_NUMBER_SIZE,
        .max_value = BIP39_WORDLIST_LEN };
    make_digit_entry_activity(&digit_entry, "Word Number", NULL);
    JADE_ASSERT(digit_entry.activity);
    SENSITIVE_PUSH(&digit_entry, sizeof(digit_entry));

    word_entry_ui_t calc_ui = { 0 };
    make_word_entry_ui(&calc_ui, false, "Recover Wallet");

    gui_view_node_t* number_text_selection = NULL;
    gui_view_node_t* number_label = NULL;
    gui_activity_t* const number_choose_word_activity
        = make_carousel_activity("Recover Wallet", &number_label, &number_text_selection);

    const char* wordlist_words[MNEMONIC_MAXWORDS] = { 0 };
    SENSITIVE_PUSH(wordlist_words, sizeof(wordlist_words));
    size_t word_index = 0;
    while (word_index < nwords) {
        JADE_ASSERT(!wordlist_words[word_index]);

        char title[24];
        const int ret = snprintf(title, sizeof(title), "Word %zu/%zu", word_index + 1, nwords);
        JADE_ASSERT(ret > 0 && ret < sizeof(title));

        const char* word = NULL;
        bool word_selected = false;
        if (advanced_mode && word_index == nwords - 1) {
            const final_word_action_t action = select_final_word_action();
            if (action == FINAL_WORD_ABANDON) {
                // BBB-AIRGAP: the escape ends the whole recovery, as the number entry does on the
                // first word - the caller reads a count of zero as abandoned.
                clear_word_number_restore_ui(&digit_entry, &calc_ui, number_text_selection);
                word_index = 0;
                goto cleanup;
            }
            if (action == FINAL_WORD_CALCULATE) {
                size_t final_words[MAX_NUM_FINAL_WORDS];
                SENSITIVE_PUSH(final_words, sizeof(final_words));
                // Fetch valid final words to use as additional filter
                display_processing_message_activity();
                const size_t num_filter_words
                    = calculate_valid_final_words(wordlist_words, word_index, nwords, final_words);

                char enter_word_title[16];
                const int ret = snprintf(enter_word_title, sizeof(enter_word_title), "Insert word %zu", word_index + 1);
                JADE_ASSERT(ret > 0 && ret < sizeof(enter_word_title));
                gui_update_text(calc_ui.titletext, enter_word_title);

                const wordlist_word_result_t word_rslt = select_wordlist_word(
                    true, word_index, wordlist_words, final_words, num_filter_words, true, &calc_ui, &word);
                if (word_rslt == WORDLIST_WORD_BACKSPACE) {
                    // Go back to the previous accepted word so it can be replaced.
                    wordlist_words[--word_index] = NULL;
                } else {
                    JADE_ASSERT(word_rslt == WORDLIST_WORD_SELECTED && word);
                    word_selected = true;
                }
                SENSITIVE_POP(final_words);
            }
        }

        if (!word) {
            reset_digit_entry(&digit_entry, title);
            gui_set_current_activity(digit_entry.activity);
            if (!run_digit_entry_loop(&digit_entry)) {
                if (word_index == 0) {
                    clear_word_number_restore_ui(&digit_entry, &calc_ui, number_text_selection);
                    word_index = 0;
                    goto cleanup;
                }

                // Go back to the previous accepted word so it can be replaced.
                wordlist_words[--word_index] = NULL;
                continue;
            }

            const uint32_t word_number = get_entry_as_number(&digit_entry);
            if (word_number == 0 || word_number > BIP39_WORDLIST_LEN) {
                await_error("Invalid word number");
                continue;
            }

            word = bip39_get_word_by_index(NULL, word_number - 1);
        }
        JADE_ASSERT(word);

        if (!word_selected) {
            const wordlist_word_result_t word_rslt = select_resolved_word_number(
                word_index, word, number_choose_word_activity, number_label, number_text_selection);
            if (word_rslt == WORDLIST_WORD_BACKSPACE) {
                continue;
            }
            JADE_ASSERT(word_rslt == WORDLIST_WORD_SELECTED);
        }

        wordlist_words[word_index++] = word;
    }

    write_wordlist_words(wordlist_words, nwords, output, output_len);
    clear_word_number_restore_ui(&digit_entry, &calc_ui, number_text_selection);
cleanup:
    SENSITIVE_POP(wordlist_words);
    SENSITIVE_POP(&digit_entry);
    return word_index;
}

typedef enum { RECOVERY_WORDS, RECOVERY_WORD_NUMBERS } recovery_method_t;

// NOTE: only the English wordlist is supported.
static bool mnemonic_recover(const size_t nwords, const bool advanced_mode, const recovery_method_t recovery_method,
    char* mnemonic, const size_t mnemonic_len)
{
    // Support 12-word and 24-word mnemonics only
    JADE_ASSERT(is_valid_mnemonic_length(nwords));
    JADE_ASSERT(recovery_method == RECOVERY_WORDS || recovery_method == RECOVERY_WORD_NUMBERS);
    JADE_ASSERT(mnemonic && mnemonic_len == MNEMONIC_BUFLEN);

    size_t words_entered;
    if (recovery_method == RECOVERY_WORD_NUMBERS) {
        words_entered = get_word_number_words(nwords, advanced_mode, mnemonic, mnemonic_len);
    } else {
        const wordlist_purpose_t purpose = advanced_mode ? MNEMONIC_ADVANCED : MNEMONIC_SIMPLE;
        words_entered = get_wordlist_words(purpose, nwords, mnemonic, mnemonic_len);
    }

    if (!words_entered) {
        // Mnemonic entry abandoned
        return false;
    }

    if (words_entered != nwords || bip39_mnemonic_validate(NULL, mnemonic) != WALLY_OK) {
        // Invalid mnemonic entered
        if (recovery_method == RECOVERY_WORD_NUMBERS) {
            JADE_LOGW("Invalid mnemonic entered using word numbers");
        } else {
            JADE_LOGW("Invalid mnemonic entered");
        }
        await_error("Invalid recovery phrase");
        return false;
    }

    return true;
}

// Take a nul terminated string of space-separated mnemonic-word prefixes, and populate a string of
// space-separated full mnemonic words (also nul terminated).
// Returns true if it works!  Returns false if any of the prefixes are not a prefix for exactly one
// valid mnemonic word, or if the expanded string is too large for the provided buffer.
// NOTE: There are a load of three-letter words in the bip39 list that are a) valid words in their
// own right, and also b) prefixes to other words.
// eg: bar, barely, bargain, barrel; pen, penalty, pencil; ski, skill, skin, skirt
// In this case we allow/prefer an exact match even when the word is an prefix of other words.
// NOTE: only the English wordlist is supported.
static bool expand_words(const uint8_t* bytes, const size_t bytes_len, char* buf, const size_t buf_len, size_t* written)
{
    JADE_ASSERT(bytes && buf && buf_len);
    JADE_INIT_OUT_SIZE(written);

    JADE_ASSERT(bytes[bytes_len] == '\0');

    size_t write_pos = 0;
    const char* read_ptr = (const char*)bytes;
    const char* end_ptr = read_ptr;

    // Must be a string of printable characters
    if (!string_all((const char*)bytes, isprint)) {
        return false;
    }

    while (*end_ptr != '\0' && write_pos < buf_len) {
        // Find the end of this word/prefix
        end_ptr = strchr(read_ptr, ' ');
        if (!end_ptr) {
            // Not found, point to end of string
            end_ptr = (const char*)bytes + bytes_len;
            JADE_ASSERT(*end_ptr == '\0');
        }
        JADE_ASSERT(end_ptr <= (const char*)bytes + bytes_len);

        // Lookup prefix in the default (English) wordlist, ensuring exactly one match
        size_t possible_match = 0;
        bool exact_match = false;
        const size_t nmatches = valid_words(read_ptr, (end_ptr - read_ptr), NULL, 0, &possible_match, 1, &exact_match);
        if (nmatches != 1 && !exact_match) {
            JADE_LOGW("%d matches for prefix: %.*s", nmatches, (end_ptr - read_ptr), read_ptr);
            return false;
        }

        const char* wordlist_extracted = bip39_get_word_by_index(NULL, possible_match);
        JADE_ASSERT(wordlist_extracted);
        const size_t word_len = strlen(wordlist_extracted);
        if (write_pos + word_len >= buf_len) {
            JADE_LOGW("Expanded mnemonic too long");
            return false;
        }

        // Copy the expanded word into the output buffer
        memcpy(buf + write_pos, wordlist_extracted, word_len);
        write_pos += word_len;

        // Copy space separator or nul terminator
        JADE_ASSERT(*end_ptr == ' ' || *end_ptr == '\0');
        buf[write_pos++] = *end_ptr;

        // Update read pointer to be after the whitespace
        read_ptr = end_ptr + 1;
    }

    // Return true if we have successfully consumed all input and
    // expanded at least one word
    JADE_ASSERT(write_pos <= buf_len);
    *written = write_pos;
    return write_pos != 0 && *end_ptr == '\0';
}

static bool import_bcur_bip39(
    const uint8_t* bytes, const size_t bytes_len, char* buf, const size_t buf_len, size_t* written)
{
    JADE_ASSERT(bytes && buf && buf_len);
    JADE_INIT_OUT_SIZE(written);

    JADE_ASSERT(bytes[bytes_len] == '\0');

    // Quick check to see if it looks like a bcur bip39 string
    const char bcqrtag[] = "UR:CRYPTO-BIP39";
    if (bytes_len <= sizeof(bcqrtag) || strncasecmp(bcqrtag, (const char*)bytes, sizeof(bcqrtag) - 1)) {
        return false;
    }

    // Decode bcur string
    return bcur_parse_bip39_wrapper((const char*)bytes, bytes_len, buf, buf_len, written);
}

// SeedSigner SeedQR support (ie string of 4-digit word indices).
// NOTE: only the English wordlist is supported.
static bool import_seedqr(
    const uint8_t* bytes, const size_t bytes_len, char* buf, const size_t buf_len, size_t* written)
{
    JADE_ASSERT(bytes && buf && buf_len && bytes[bytes_len] == '\0');
    JADE_INIT_OUT_SIZE(written);

    // Must be a string of appropriate length and all digits
    if ((bytes_len != 48 && bytes_len != 96) || !string_all((const char*)bytes, isdigit)) {
        return false;
    }

    // Read out 4-digit (ie. 0-padded) indices, and lookup word
    char index_code[5];
    SENSITIVE_PUSH(index_code, sizeof(index_code));
    index_code[4] = '\0';

    size_t write_pos = 0;
    const size_t num_words = bytes_len == 48 ? 12 : 24;
    for (size_t i = 0; i < num_words; ++i) {
        memcpy(index_code, bytes + (i * 4), 4);
        const size_t index = strtol(index_code, NULL, 10);
        if (index > 2047) {
            JADE_LOGE("Error, provided a bip39 word out of range");
            goto cleanup;
        }

        const char* wordlist_extracted = bip39_get_word_by_index(NULL, index);
        JADE_ASSERT(wordlist_extracted);
        const size_t wordlen = strlen(wordlist_extracted);
        if (write_pos + 1 + wordlen + 1 >= buf_len) {
            // Not enough remaining for space, word, nul
            JADE_LOGE("Error, expanded mnemonic string too large for buffer");
            goto cleanup;
        }

        if (i > 0) {
            // Add space separator
            buf[write_pos++] = ' ';
        }

        // Copy word
        memcpy(buf + write_pos, wordlist_extracted, wordlen);
        write_pos += wordlen;
    }

    buf[write_pos++] = '\0';
    *written = write_pos;
cleanup:
    SENSITIVE_POP(index_code);
    return *written != 0;
}

// SeedSigner CompactSeedQR support (ie raw entropy).
// NOTE: only the English wordlist is supported.
static bool import_compactseedqr(
    const uint8_t* bytes, const size_t bytes_len, char* buf, const size_t buf_len, size_t* written)
{
    JADE_ASSERT(bytes && buf && buf_len);
    JADE_INIT_OUT_SIZE(written);

    // Any buffer of appropriate length will work as a compactseedqr as it's just raw entropy
    if ((bytes_len != BIP32_ENTROPY_LEN_128 && bytes_len != BIP32_ENTROPY_LEN_256)) {
        return false;
    }

    // Convert binary entropy to mnemonic string
    char* mnemonic = NULL;
    JADE_WALLY_VERIFY(bip39_mnemonic_from_bytes(NULL, bytes, bytes_len, &mnemonic));
    JADE_ASSERT(mnemonic);
    const size_t mnemonic_len = strnlen(mnemonic, buf_len);
    JADE_ASSERT(mnemonic_len < buf_len); // buffer should be large enough for any mnemonic

    // Copy into output buffer and zero and free wally string
    strcpy(buf, mnemonic);
    *written = mnemonic_len + 1; // Report actual number of bytes written including the nul-terminator

    JADE_WALLY_VERIFY(wally_bzero(mnemonic, mnemonic_len));
    JADE_WALLY_VERIFY(wally_free_string(mnemonic));
    return true;
}

// Attempt to import mnemonic from supported formats
bool import_mnemonic(const uint8_t* bytes, const size_t bytes_len, char* buf, const size_t buf_len, size_t* written)
{
    JADE_ASSERT(bytes && buf && buf_len >= MNEMONIC_BUFLEN);
    JADE_INIT_OUT_SIZE(written);

    JADE_ASSERT(bytes[bytes_len] == '\0');

    // 1. Try seedsigner compact format (ie. raw 128bit or 256bit entropy)
    // 2. Try seedsigner standard format (string of 4-digit indicies, no spaces)
    // 3. Try bcur bip39 format (starts with a specific string prefix)
    // 4. Try to read word prefixes or whole words (space separated)
    return import_compactseedqr(bytes, bytes_len, buf, buf_len, written)
        || import_seedqr(bytes, bytes_len, buf, buf_len, written)
        || import_bcur_bip39(bytes, bytes_len, buf, MNEMONIC_BUFLEN, written)
        || expand_words(bytes, bytes_len, buf, buf_len, written);
}

// Function to validate qr scanned is (or expands to) a valid mnemonic
// (Passed to the qr-scanner so scanning only stops when this is satisfied)
// NOTE: not 'static' here as also called from debug/test code.
bool import_and_validate_mnemonic(qr_data_t* qr_data)
{
    JADE_ASSERT(qr_data && qr_data->len < sizeof(qr_data->data) && qr_data->data[qr_data->len] == '\0');

    char mnemonic[sizeof(qr_data->data)];
    SENSITIVE_PUSH(mnemonic, sizeof(mnemonic));

    // Try to import mnemonic, validate, and if all good copy over into the qr_data
    size_t written = 0;
    bool ret;
    if (import_mnemonic(qr_data->data, qr_data->len, mnemonic, sizeof(mnemonic), &written) && written
        && written <= sizeof(mnemonic) && mnemonic[written - 1] == '\0'
        && bip39_mnemonic_validate(NULL, mnemonic) == WALLY_OK) {

        memcpy(qr_data->data, mnemonic, written);
        qr_data->len = written - 1; // Do not include nul-terminator
        ret = true;
    } else {
        // Show the user that a valid qr was scanned, but the string data
        // did not constitute (or expand to) a valid bip39 mnemonic string.
        await_error("Invalid recovery phrase");
        qr_data->len = 0;
        ret = false;
    }
    SENSITIVE_POP(mnemonic);
    return ret;
}

// BBB-AIRGAP: no longer static - the Options list reaches this to load a second wallet
// (BTN_SETTINGS_ADD_WALLET, main/process/dashboard.c).  It is the scanner that accepts a recovery
// phrase and nothing else, which is what that row promises; the generic scanner would also take a
// PSBT or an address.
bool mnemonic_qr(char* mnemonic, const size_t mnemonic_len)
{
    JADE_ASSERT(mnemonic && mnemonic_len == MNEMONIC_BUFLEN);

    // Pass validation callback above to qr scanner
    qr_data_t qr_data = { .len = 0, .is_valid = import_and_validate_mnemonic };
    SENSITIVE_PUSH(&qr_data, sizeof(qr_data));
    mnemonic[0] = '\0';

    // We return 'true' if we scanned any string data at all
    const bool qr_scanned
        = jade_camera_scan_qr(&qr_data, "Seed QR", QR_GUIDE_SHOW, "blkstrm.com/scanwallet") && qr_data.len > 0;
    if (!qr_scanned) {
        JADE_LOGW("No qr code scanned");
        goto cleanup;
    }

    if (qr_data.len >= mnemonic_len) {
        JADE_LOGW("String data from qr unexpectedly long - ignored: %zu", qr_data.len);
        goto cleanup;
    }

    // Result looks good, copy into mnemonic buffer
    JADE_ASSERT(qr_data.data[qr_data.len] == '\0');
    strcpy(mnemonic, (const char*)qr_data.data);

cleanup:
    SENSITIVE_POP(&qr_data);
    return qr_scanned;
}

static void get_freetext_passphrase(char* passphrase, const size_t passphrase_len)
{
    JADE_ASSERT(passphrase && passphrase_len);
    passphrase[0] = '\0';

    // We will need this activity later when confirming
    gui_view_node_t* text_to_confirm = NULL;
    gui_activity_t* const confirm_passphrase_activity = make_confirm_passphrase_activity(passphrase, &text_to_confirm);
    int32_t ev_id;

    // For passphrase we want all 4 keyboards
    keyboard_entry_t kb_entry = { .max_allowed_len = passphrase_len - 1 };
    kb_entry.keyboards[0] = KB_LOWER_CASE_CHARS;
    kb_entry.keyboards[1] = KB_UPPER_CASE_CHARS;
    kb_entry.keyboards[2] = KB_NUMBERS_SYMBOLS;
    kb_entry.keyboards[3] = KB_REMAINING_SYMBOLS;
    kb_entry.num_kbs = 4;

    make_keyboard_entry_activity(&kb_entry, "Enter Passphrase");
    JADE_ASSERT(kb_entry.activity);

    SENSITIVE_PUSH(kb_entry.strdata, sizeof(kb_entry.strdata));
    bool is_confirmed = false;
    while (!is_confirmed) {
        // Run the keyboard entry loop to get a typed passphrase
        run_keyboard_entry_loop(&kb_entry);

        // Ask user to confirm passphrase
        gui_update_text(text_to_confirm, kb_entry.len > 0 ? kb_entry.strdata : "<no passphrase>");
        gui_set_current_activity(confirm_passphrase_activity);
        gui_activity_wait_event(confirm_passphrase_activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);

        // BBB-AIRGAP: leaving the confirmation abandons the passphrase.  Without this the loop
        // would reopen the keyboard, which is exempt from the escape, and the first key typed
        // there clears the flag - so the cancel could no longer be recovered by the checks in
        // get_passphrase()'s callers.  The empty string is what they then see.
        if (ev_id == BTN_ESCAPE_HOME) {
            kb_entry.len = 0;
            kb_entry.strdata[0] = '\0';
            break;
        }
        is_confirmed = (ev_id == BTN_YES);
    }

    JADE_ASSERT(kb_entry.len < passphrase_len);
    strcpy(passphrase, kb_entry.strdata);
    SENSITIVE_POP(kb_entry.strdata);
}

// BBB-AIRGAP: the entry itself, without the question that precedes it in get_passphrase().  Split
// out for the one caller that has already asked its own version of that question and must not ask
// twice (derive_keychain, when the words scanned turn out to be a wallet already held).
static void enter_passphrase(char* passphrase, const size_t passphrase_len)
{
    JADE_ASSERT(passphrase);
    JADE_ASSERT(passphrase_len);

    if (keychain_get_passphrase_type() == PASSPHRASE_WORDLIST) {
        // Passphrase made up only of bip39 wordlist words
        // BBB-AIRGAP: the word count narrows the passphrase search space, so it is not logged.
        get_wordlist_words(WORDLIST_PASSPHRASE, WORDLIST_PASSPHRASE_MAX_WORDS, passphrase, passphrase_len);
    } else {
        // Free-text passphrase
        get_freetext_passphrase(passphrase, passphrase_len);
    }
}

void get_passphrase(char* passphrase, const size_t passphrase_len)
{
    JADE_ASSERT(passphrase && passphrase_len);
    passphrase[0] = '\0';

    if (keychain_get_passphrase_freq() == PASSPHRASE_NEVER) {
        // Auto apply the empty passphrase - return empty immediately
        return;
    }

    // BBB-AIRGAP: asked before the keyboard rather than opening straight onto it.  Carrying on
    // without a passphrase was always possible - leave the keyboard empty and confirm the
    // '<no passphrase>' it then shows - but the keyboard reads as "type something", so a setting
    // of 'always ask' looked like a requirement.  The choice is now a screen of its own, and Skip
    // is the initial selection because it is the ending that leaves the wallet as it was.
    const char* question[] = { "Enter a passphrase?" };
    if (!await_choice_activity("Passphrase", question, 1, "Enter", "Skip", false, NULL)) {
        // Skipped - the empty passphrase, which is what an empty keyboard gave before
        return;
    }

    enter_passphrase(passphrase, passphrase_len);
}

// BBB-AIRGAP: the wallet already held is made the one in use.  Scanning its QR says which wallet
// the user wants; the device has it, so it switches to it instead of reporting an error or opening
// a second slot for the same keys (which is what it used to do - the session list then showed one
// fingerprint twice and looked like two wallets).
static void switch_to_held_wallet(const size_t position)
{
    // BBB-AIRGAP: this both tells the user and CHANGES which wallet is active.  A KEY3 escape
    // reaches the callers as the same 'No' that means "keep the one already loaded", but leaving
    // is not that answer: it must not activate a slot, and it must not stop the cascade at a
    // message screen either.  Guarded here rather than at each caller, because every path into
    // this function arrives from a question the escape can answer.
    if (gui_escape_pending()) {
        return;
    }
    await_message_2("This wallet is", "already loaded");
    // BBB-AIRGAP: KEY3 can also dismiss this message; it must not activate the named wallet.
    if (gui_escape_pending()) {
        return;
    }
    keychain_slot_activate(position);
}

derive_keychain_result_t derive_keychain(const bool temporary_restore, const char* mnemonic, const bool into_free_slot)
{
    JADE_ASSERT(mnemonic);
    // NOTE: mnemnonic should be valid at this point for best UX

    derive_keychain_result_t result = DERIVE_KEYCHAIN_FAILED;
    size_t held_position = 0;
    bool passphrase_asked = false;

    keychain_t keydata = { 0 };
    SENSITIVE_PUSH(&keydata, sizeof(keydata));

    // Get any passphrase, if relevant
    char passphrase[PASSPHRASE_MAX_LEN + 1]; // max chars plus '\0'
    SENSITIVE_PUSH(passphrase, sizeof(passphrase));
    passphrase[0] = '\0';

    // BBB-AIRGAP: a scanned wallet is checked against the ones already held before the passphrase
    // screen opens.  A wallet's identity is its master key, so it cannot be known until a wallet
    // has been built from the words - but it can be built with the empty passphrase, which is what
    // a device set to never ask uses anyway.  So the words are derived once here, and that
    // derivation is either the answer (no passphrase entered) or the check that lets the device
    // say "this one is already loaded" before asking for anything.
    if (into_free_slot) {
        display_processing_message_activity();
        if (!keychain_derive_from_mnemonic(mnemonic, passphrase, &keydata)) {
            JADE_LOGE("Failed to derive wallet");
            goto cleanup;
        }

        // BBB-AIRGAP: the words scanned may be a wallet already held, and switching to that one
        // needs no slot, so a full table is not refused here.  It is refused below, once the
        // wallet that would be loaded is known - including the wallet a passphrase makes, because
        // that one can be a wallet already held too.
        if (keychain_find_slot(&keydata, &held_position)) {
            // The words are already loaded.  A passphrase would make a different wallet out of
            // them, so that is offered - unless the device is set never to ask, when the wallet
            // held is the only ending these words have and there is nothing to offer.  A full
            // table is deliberately not a reason to skip the offer: the passphrase wallet these
            // words make can be held too, and switching to that one needs no slot either.
            if (keychain_get_passphrase_freq() == PASSPHRASE_NEVER) {
                switch_to_held_wallet(held_position);
                result = DERIVE_KEYCHAIN_ABORTED;
                goto cleanup;
            }

            // BBB-AIRGAP: a yes/no question rather than two named buttons, because the naming is
            // what was hard to get right: the answer decides whether a SECOND wallet is made out
            // of these words, and saying that in the question leaves the buttons nothing to
            // explain.  'No' is the initial selection - the wallet is already there, so doing
            // nothing new is the safe ending.
            const char* question[] = { "This wallet is already", "loaded. Add it again", "with a passphrase?" };
            if (!await_yesno_activity("Add Wallet", question, 3, false, NULL)) {
                switch_to_held_wallet(held_position);
                result = DERIVE_KEYCHAIN_ABORTED;
                goto cleanup;
            }
            // The passphrase question has just been answered, so the entry screen is opened
            // directly rather than asking again in get_passphrase().
            passphrase_asked = true;
        }
    }

    if (passphrase_asked) {
        enter_passphrase(passphrase, sizeof(passphrase));
    } else {
        get_passphrase(passphrase, sizeof(passphrase));
    }

    // BBB-AIRGAP: 'Skip' and a KEY3 escape both come back from that question as false, and Skip
    // goes on to derive and install the empty-passphrase wallet.  The escape means leave, so it
    // must not change which wallet is in use; the flag is the only thing that separates the two.
    if (gui_escape_pending()) {
        result = DERIVE_KEYCHAIN_ABORTED;
        goto cleanup;
    }
    const size_t passphrase_len = strnlen(passphrase, sizeof(passphrase));
    JADE_ASSERT(passphrase_len < sizeof(passphrase));

    // If the mnemonic is valid derive temporary keychain from it.
    // Otherwise break/return here.
    // BBB-AIRGAP: the empty-passphrase derivation above is this wallet when no passphrase was
    // entered, so it is not repeated; a passphrase makes a different wallet and has to be derived.
    bool wallet_created = true;
    if (!into_free_slot || passphrase_len) {
        display_processing_message_activity();
        wallet_created = keychain_derive_from_mnemonic(mnemonic, passphrase, &keydata);
    }
    SENSITIVE_POP(passphrase);

    if (!wallet_created) {
        JADE_LOGE("Failed to derive wallet");
        goto cleanup_keydata;
    }

    if (into_free_slot) {
        // BBB-AIRGAP: checked again, and unconditionally: a passphrase makes a different wallet
        // and that one can be held too, while leaving the passphrase screen empty after choosing
        // to continue gives back the very wallet the first check found.  The check is two memcmps
        // over keys that are already in memory, so it is not narrowed to the cases that need it
        // and cannot be reasoned wrong later.
        if (keychain_find_slot(&keydata, &held_position)) {
            switch_to_held_wallet(held_position);
            result = DERIVE_KEYCHAIN_ABORTED;
            goto cleanup_keydata;
        }

        // BBB-AIRGAP: capacity is decided here and nowhere earlier, because this is the first
        // point at which the device knows a slot is actually needed: every ending above leaves
        // the table as it was.  The cost is that a passphrase can be entered before the refusal
        // is shown; the alternative was refusing scans that needed no slot at all.
        if (!keychain_has_free_slot()) {
            await_error_2("No free wallet slot -", "forget one or log out");
            result = DERIVE_KEYCHAIN_ABORTED;
            goto cleanup_keydata;
        }

        // BBB-AIRGAP: the confirmation names the wallet being loaded rather than asking about "this
        // wallet", and it is asked here rather than before the scan is worked through, because the
        // fingerprint is what makes the question answerable - the user compares it with the one
        // they expect.  'No' is the initial selection: a wallet QR arrives from somewhere, and
        // whoever prepared it knows the words, so loading one is not the harmless default.
        char label[2 * BIP32_KEY_FINGERPRINT_LEN + 1];
        char* fphex = NULL;
        JADE_WALLY_VERIFY(wally_hex_from_bytes(keydata.xpriv.hash160, BIP32_KEY_FINGERPRINT_LEN, &fphex));
        map_string(fphex, toupper);
        const int ret = snprintf(label, sizeof(label), "%s", fphex);
        JADE_ASSERT(ret > 0 && ret < sizeof(label));
        JADE_WALLY_VERIFY(wally_free_string(fphex));

        char question0[32];
        const int qret = snprintf(question0, sizeof(question0), "Load wallet %s?", label);
        JADE_ASSERT(qret > 0 && qret < sizeof(question0));
        const char* question[] = { question0, "Whoever made this QR", "knows this wallet." };
        if (!await_yesno_activity("Load Wallet", question, 3, false, "blkstrm.com/temporary")) {
            result = DERIVE_KEYCHAIN_ABORTED;
            goto cleanup_keydata;
        }
    }

    // All good - push temporary into main in-memory keychain
    if (into_free_slot) {
        // BBB-AIRGAP: loading a further wallet must not disturb the ones already in memory, and
        // must not relax the network-type restriction either - that restriction is device policy
        // rather than a property of a wallet, so only the single-wallet path below clears it.
        if (!keychain_load_into_free_slot(&keydata, SOURCE_NONE, temporary_restore)) {
            JADE_LOGE("No free wallet slot to load into");
            goto cleanup_keydata;
        }
    } else {
        // and remove the restriction on network-types.
        keychain_set(&keydata, SOURCE_NONE, temporary_restore);
        keychain_clear_network_type_restriction();
    }

#ifdef CONFIG_HAS_CAMERA
    // BBB-AIRGAP: keep the entropy with the wallet so its SeedQR can be drawn from the session
    // menu, not only during setup.  This function is the one path that turns a mnemonic the user
    // presented into a wallet, so holding the entropy here is what limits it to wallets whose
    // words were shown in this session - a PIN-unlocked wallet arrives through keychain_load()
    // instead and gets none.  Guarded on the camera because the export screens verify the drawn
    // code by scanning it back: with no camera there is no export, so there is no reason to hold
    // the entropy at all.
    //
    // A passphrase wallet is left out on purpose.  A SeedQR carries the words and nothing else,
    // so the code drawn for such a wallet opens a DIFFERENT wallet when it is scanned back - a
    // backup that looks complete and is not.  Upstream never hits this because it offers export
    // before the passphrase is asked for (see initialise_with_mnemonic below); reaching the same
    // screens from a menu titled with the wallet's fingerprint would read as "this wallet's
    // backup", so the row is not offered rather than qualified with a warning.
    if (!passphrase_len) {
        keychain_set_entropy(mnemonic);
    }
#endif

    result = DERIVE_KEYCHAIN_OK;

cleanup_keydata:
    SENSITIVE_POP(&keydata);
    return result;

cleanup:
    // The passphrase buffer is still on the sensitive stack here: this is the exit taken before
    // it is read, so it has to come off before the keychain it sits above.
    SENSITIVE_POP(passphrase);
    SENSITIVE_POP(&keydata);
    return result;
}

void initialise_with_mnemonic(const bool temporary_restore, const bool force_qr_scan, bool* offer_qr_temporary)
{
    // At this point we should not have any keys in-memory
    JADE_ASSERT(!keychain_get() && offer_qr_temporary);

    // Initially false, set after wallet creation depending on path/routes/options
    *offer_qr_temporary = false;

    // We only allow setting new keys when encrypted keys are persisted if
    // we are doing a temporary restore.
    JADE_ASSERT(temporary_restore || !keychain_has_pin());

    char mnemonic[MNEMONIC_BUFLEN]; // buffer should be large enough for any mnemonic
    SENSITIVE_PUSH(mnemonic, sizeof(mnemonic));

    // NOTE: temporary wallets default to 'advanced mode'
    bool advanced_mode = temporary_restore;
    bool qr_scanned = force_qr_scan;
    gui_activity_t* act = NULL;
    if (force_qr_scan) {
        // Jump directly to scan-qr
        if (!mnemonic_qr(mnemonic, sizeof(mnemonic))) {
            // User abandoned scanning
            goto cleanup;
        }
    } else {
        // Initial welcome screen, or straight to 'recovery' screen if doing temporary restore
        if (temporary_restore) {
            act = make_restore_mnemonic_activity(temporary_restore);
        } else {
            const char* message[] = { "For setup instructions", "visit blockstream.com/", "jade" };
            if (await_continueback_activity(NULL, message, 3, true, "blkstrm.com/jade")) {
                act = make_mnemonic_setup_type_activity();
            } else {
                // User decided against it
                goto cleanup;
            }
        }

        bool got_mnemonic = false;
        while (!got_mnemonic) {
            // BBB-AIRGAP: this loop also reaches screens through helpers that wait on their own
            // (the Advanced Setup question, the word entry), so the escape gets back here as a
            // flag with no event left to wake anything; checking at the head is what carries the
            // cancel on out of the setup flow instead of stopping one screen short.
            if (gui_escape_pending()) {
                goto cleanup;
            }

            gui_set_current_activity_ex(act, true);

            const int32_t ev_id = gui_activity_wait_button(act, BTN_EVENT_TIMEOUT);
            switch (ev_id) {
            case BTN_EVENT_TIMEOUT:
#ifdef CONFIG_DEBUG_UNATTENDED_CI
                // In a debug unattended ci build, use hardcoded mnemonic after a short delay
                strcpy(mnemonic,
                    "fish inner face ginger orchard permit useful method fence kidney chuckle party favorite sunset "
                    "draw "
                    "limb "
                    "science crane oval letter slot invite sadness banana");
                got_mnemonic = true;
                break;
#else
                continue;
#endif
            // BBB-AIRGAP: KEY3 abandons setup through the screen's own exit
            case BTN_ESCAPE_HOME:
            case BTN_MNEMONIC_EXIT:
                // Abandon setting up mnemonic altogether
                goto cleanup;

            // Change screens and continue to await button events
            case BTN_MNEMONIC_TYPE:
                advanced_mode = false;
                act = make_mnemonic_setup_type_activity();
                continue;

            case BTN_MNEMONIC_ADVANCED:
                const char* message[] = { "Technical features", "will be presented.", "Proceed with caution." };
                advanced_mode = await_continueback_activity("Advanced Setup", message, 3, true, "blkstrm.com/advanced");
                if (advanced_mode) {
                    act = make_mnemonic_setup_method_activity(advanced_mode);
                } else {
                    act = make_mnemonic_setup_type_activity();
                }
                continue;

            case BTN_MNEMONIC_METHOD:
                act = make_mnemonic_setup_method_activity(advanced_mode);
                continue;

            case BTN_NEW_MNEMONIC:
                // BBB-AIRGAP: no button raises this any more; the advanced arm of the setup-method
                // menu now raises BTN_NEW_MNEMONIC_SOURCE, and the word-count screen this opens is
                // reached from await_new_mnemonic_nwords() instead. Upstream's branch is left as it
                // stands so that merging a new upstream release does not conflict here.
                act = make_new_mnemonic_activity();
                continue;

            case BTN_RESTORE_MNEMONIC:
                act = make_restore_mnemonic_activity(temporary_restore);
                continue;

            // Await user mnemonic entry/confirmation
            case BTN_NEW_MNEMONIC_12:
                got_mnemonic = mnemonic_new(12, NULL, 0, mnemonic, sizeof(mnemonic));
                break;

            case BTN_NEW_MNEMONIC_24:
                got_mnemonic = mnemonic_new(24, NULL, 0, mnemonic, sizeof(mnemonic));
                break;

            // BBB-AIRGAP: choose where the entropy for a new wallet comes from
            case BTN_NEW_MNEMONIC_SOURCE:
                act = make_new_mnemonic_source_activity();
                continue;

            case BTN_NEW_MNEMONIC_DEVICE:
            case BTN_NEW_MNEMONIC_DICE:
            case BTN_NEW_MNEMONIC_CAMERA:
            case BTN_NEW_MNEMONIC_COMBINED: {
                const size_t nwords = await_new_mnemonic_nwords();
                if (!nwords) {
                    // 'back' - the entropy-source menu is still the current activity
                    break;
                }
                if (ev_id == BTN_NEW_MNEMONIC_DEVICE) {
                    // No user-supplied entropy, so the rng is used - the upstream behaviour
                    got_mnemonic = mnemonic_new(nwords, NULL, 0, mnemonic, sizeof(mnemonic));
                    break;
                }

                const size_t entropy_len = nwords == 12 ? BIP39_ENTROPY_LEN_128 : BIP39_ENTROPY_LEN_256;
                uint8_t entropy[BIP39_ENTROPY_LEN_256];
                SENSITIVE_PUSH(entropy, sizeof(entropy));
                bool have_entropy = false;
                if (ev_id == BTN_NEW_MNEMONIC_DICE) {
                    have_entropy = gather_dice_entropy(nwords, entropy, entropy_len);
                }
#ifdef HAVE_CAMERA_ENTROPY
                else if (ev_id == BTN_NEW_MNEMONIC_CAMERA) {
                    have_entropy = gather_camera_entropy(nwords, entropy, entropy_len);
                }
#endif
                else if (ev_id == BTN_NEW_MNEMONIC_COMBINED) {
                    have_entropy = gather_combined_entropy(nwords, entropy, entropy_len);
                }
                if (have_entropy) {
                    got_mnemonic = mnemonic_new(nwords, entropy, entropy_len, mnemonic, sizeof(mnemonic));
                }
                SENSITIVE_POP(entropy);
                break;
            }

            case BTN_RESTORE_MNEMONIC_12:
                if (advanced_mode) {
                    act = make_restore_mnemonic_method_activity(12);
                    continue;
                }
                got_mnemonic = mnemonic_recover(12, advanced_mode, RECOVERY_WORDS, mnemonic, sizeof(mnemonic));
                break;

            case BTN_RESTORE_MNEMONIC_24:
                if (advanced_mode) {
                    act = make_restore_mnemonic_method_activity(24);
                    continue;
                }
                got_mnemonic = mnemonic_recover(24, advanced_mode, RECOVERY_WORDS, mnemonic, sizeof(mnemonic));
                break;

            case BTN_RESTORE_MNEMONIC_WORDS_12:
                got_mnemonic = mnemonic_recover(12, advanced_mode, RECOVERY_WORDS, mnemonic, sizeof(mnemonic));
                break;

            case BTN_RESTORE_MNEMONIC_WORDS_24:
                got_mnemonic = mnemonic_recover(24, advanced_mode, RECOVERY_WORDS, mnemonic, sizeof(mnemonic));
                break;

            case BTN_RESTORE_MNEMONIC_WORD_NUMBERS_12:
                got_mnemonic = mnemonic_recover(12, advanced_mode, RECOVERY_WORD_NUMBERS, mnemonic, sizeof(mnemonic));
                break;

            case BTN_RESTORE_MNEMONIC_WORD_NUMBERS_24:
                got_mnemonic = mnemonic_recover(24, advanced_mode, RECOVERY_WORD_NUMBERS, mnemonic, sizeof(mnemonic));
                break;

            case BTN_RESTORE_MNEMONIC_QR:
                got_mnemonic = mnemonic_qr(mnemonic, sizeof(mnemonic));
                qr_scanned = got_mnemonic;
                break;
            default:
                // Unknown event, ignore
                continue;
            }
        }
    }

    // Mnemonic should be populated and *valid* at this point
    // a. newly created mnemonics should always be valid
    // b. manual restore methods include explicit validation
    // c. qr-scanner includes a validation check before returning the scanned mnemonic
    if (bip39_mnemonic_validate(NULL, mnemonic) != WALLY_OK) {
        JADE_LOGE("Invalid mnemonic unexpected");
        await_error("Invalid recovery phrase");
        goto cleanup;
    }

#ifdef CONFIG_HAS_CAMERA
    // Offer export via qr for true Jade hw's (ie. with camera) and the flag is set
    // ie. a) if 'Advanced' setup was used, and b) we did not already scan a QR
    if (advanced_mode) {
        // If the user scanned a qr for a non-temporary login, we may double-check
        // they don't prefer the temporary qr-mode wallet, in case that's what they intended.
        *offer_qr_temporary = qr_scanned && !temporary_restore;

        // If the user did not scan a QR, offer the chance to export (ie. draw) one now
        if (!qr_scanned) {
            const char* question[] = { "Export recovery phrase", "as a SeedQR?" };
            bool export_qr = await_yesno_activity(NULL, question, 2, true, "blkstrm.com/seedqr");

            bool export_qr_verified = false;
            while (export_qr) {
                // Call export function - it returns 'true' when the step is complete (or skipped)
                // (it returns 'false' if the user presses 'back' to restart the process)
                export_qr = !mnemonic_export_qr(mnemonic, &export_qr_verified);

                // BBB-AIRGAP: 'back' restarts the export, but a KEY3 escape ends it.  Both
                // arrive here as false, so the flag is what tells them apart; without this the
                // escape would be answered by opening the export again.
                if (gui_escape_pending()) {
                    break;
                }
            }

            // BBB-AIRGAP: and the escape ends the whole setup, not just the export.  Carrying on
            // would reach derive_keychain() below, which now returns DERIVE_KEYCHAIN_ABORTED for
            // the same escape and used to be reported as 'Failed to create wallet' on a blocking
            // screen the user never asked for.
            if (gui_escape_pending()) {
                goto cleanup;
            }

            // If the user successfully exported the qr for a non-temporary login, we may double-check
            // they don't prefer the temporary qr-mode wallet, in case that's what they intended.
            *offer_qr_temporary = export_qr_verified && !temporary_restore;
        }
    }
#else
    // Flag unused if no camera available - silence compiler warning
    (void)qr_scanned;
#endif // CONFIG_HAS_CAMERA

    // Set flag indicating whether we should ask the user before exporting the master blinding key
    // (In advanced mode we ask the user, in default/basic mode we always silently export the key.)
    keychain_set_confirm_export_blinding_key(advanced_mode);

    const bool into_free_slot = false;
    // BBB-AIRGAP: this path CAN now be aborted inside derive_keychain(): the passphrase question
    // it opens is a screen the escape can end, and that is a cancel rather than a failure.  (The
    // note that used to stand here said the opposite, and was written before the escape existed.)
    // Leaving quietly is the whole point, so the abort gets no error screen.
    const derive_keychain_result_t derived = derive_keychain(temporary_restore, mnemonic, into_free_slot);
    if (derived == DERIVE_KEYCHAIN_ABORTED) {
        JADE_LOGI("Wallet setup abandoned");
        goto cleanup;
    }
    if (derived != DERIVE_KEYCHAIN_OK) {
        // Error making wallet...
        JADE_LOGE("Failed to derive keychain from valid mnemonic");
        await_error("Failed to create wallet");
        goto cleanup;
    }

    if (!temporary_restore) {
        // We need to cache the root mnemonic entropy as it is this that we will persist
        // encrypted to local flash (requiring a passphrase to derive the wallet master key).
        keychain_cache_mnemonic_entropy(mnemonic);
    }

cleanup:
    SENSITIVE_POP(mnemonic);
}

// Function to calculate a bip85 bip39 mnemonic phrase
// Caller must free with 'wally_free_string()
// NOTE: only the English wordlist is supported.
void get_bip85_mnemonic(const uint32_t nwords, const uint32_t index, char** new_mnemonic)
{
    JADE_ASSERT(is_valid_mnemonic_length(nwords) && index < BIP85_INDEX_MAX);
    JADE_INIT_OUT_PPTR(new_mnemonic);
    JADE_ASSERT(keychain_get());

    size_t entropy_len = 0;
    uint8_t entropy[HMAC_SHA512_LEN];
    SENSITIVE_PUSH(entropy, sizeof(entropy));
    wallet_get_bip85_bip39_entropy(nwords, index, entropy, sizeof(entropy), &entropy_len);
    JADE_ASSERT(entropy_len == (nwords == 12 ? BIP39_ENTROPY_LEN_128 : BIP39_ENTROPY_LEN_256));

    JADE_WALLY_VERIFY(bip39_mnemonic_from_bytes(NULL, entropy, entropy_len, new_mnemonic));
    JADE_ASSERT(new_mnemonic);
    SENSITIVE_POP(entropy);
}

// Offer the user the option to generate a bip39 recovery phrase using entropy
// calculated as per bip85.  User provides number of words and also path index.
// NOTE: only the English wordlist is supported.
void handle_bip85_mnemonic()
{
    JADE_ASSERT(keychain_get());

    const char* message[] = { "Create a new recovery", "phrase derived from", "wallet and index" };
    if (!await_continueback_activity("BIP85", message, 3, true, "blkstrm.com/bip85")) {
        // User declined
        return;
    }

    gui_activity_t* act = make_bip85_mnemonic_words_activity();
    gui_set_current_activity(act);
    uint8_t nwords = 0;

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return;
        }

        const int32_t ev_id = gui_activity_wait_button(act, BTN_BIP85_12_WORDS);
        if (ev_id == BTN_BIP85_12_WORDS) {
            nwords = 12;
            break;
        } else if (ev_id == BTN_BIP85_24_WORDS) {
            nwords = 24;
            break;
        } else if (ev_id == BTN_BIP85_EXIT) {
            // User declined
            return;
        }
    }
    JADE_ASSERT(is_valid_mnemonic_length(nwords));

    // Fetch index (uses pin-entry screen)
    digit_entry_t digit_entry = { .entry_type = DIGIT_ENTRY_INDEX, .initial_state = ZERO, .digits_shown = true };
    make_digit_entry_activity(&digit_entry, "BIP85", "Index #:");
    JADE_ASSERT(digit_entry.activity);

    uint32_t index = 0;
    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return;
        }
        reset_digit_entry(&digit_entry, "BIP85");
        gui_set_current_activity(digit_entry.activity);
        if (!run_digit_entry_loop(&digit_entry)) {
            // User abandoned index entry
            JADE_LOGI("User abandoned selecting index");
            return;
        }

        // Get entered digits as single numeric value
        index = get_entry_as_number(&digit_entry);
        JADE_ASSERT(index < BIP85_INDEX_MAX);

        // User to confirm
        char buf[8];
        const int ret = snprintf(buf, sizeof(buf), "%" PRIu32, index);
        JADE_ASSERT(ret > 0 && ret < sizeof(buf));
        const char* message[] = { "BIP85 index selected:", buf };
        if (await_continueback_activity("BIP85", message, 2, true, "blkstrm.com/bip85")) {
            JADE_LOGI("BIP85 index number selected: %" PRIu32, index);
            break;
        }
    }
    JADE_ASSERT(index < BIP85_INDEX_MAX);

    // Generate bip39 mnemonic phrase from bip85 entropy
    char* new_mnemonic = NULL;
    get_bip85_mnemonic(nwords, index, &new_mnemonic);
    JADE_ASSERT(new_mnemonic);
    const size_t mnemonic_len = strnlen(new_mnemonic, MNEMONIC_BUFLEN);
    JADE_ASSERT(mnemonic_len < MNEMONIC_BUFLEN);
    SENSITIVE_PUSH(new_mnemonic, mnemonic_len);

    // Display and confirm mnemonic phrase
    if (display_confirm_mnemonic(nwords, new_mnemonic, mnemonic_len)) {
        await_message_2("Recovery Phrase", "Confirmed");
    }

    // Cleanup
    SENSITIVE_POP(new_mnemonic);
    JADE_WALLY_VERIFY(wally_free_string(new_mnemonic));
}
#endif // AMALGAMATED_BUILD
