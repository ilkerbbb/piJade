#ifndef AMALGAMATED_BUILD
#include "../button_events.h"
#include "../jade_assert.h"
#include "../storage.h"
#include "../ui.h"

// BBB-AIRGAP: shown in place of the wallet type when the stored record cannot be opened with the
// wallet in use.  The type is the less useful of the two facts here - what a reader needs before
// pressing is whether this record is usable - and it is still on the screen the record opens onto.
// The wording does not name an owner on purpose: a failed seal check cannot tell another wallet's
// record apart from this wallet's own corrupted one, and claiming the former would mislead a
// deletion decision.  The same reasoning already governs the Name In Use question text.
#define OTHER_WALLET_LABEL "Record Unreadable"

// BBB-AIRGAP: an absent ownership array means the caller has already narrowed its list to the
// records this wallet can read, so every row on screen is owned and there is nothing to mark.
#define IS_OWNED(owned, i) (!(owned) || (owned)[(i)])

// BBB-AIRGAP: the row index is used throughout, where the name lookup previously read 'selected'.
// The two agreed at every call site, so nothing was wrong on screen; they no longer have to.
#define UPDATE_WALLET_CAROUSEL(i)                                                                                      \
    do {                                                                                                               \
        const size_t idx = (i);                                                                                        \
        if (idx < num_multisigs) {                                                                                     \
            gui_update_text(label, IS_OWNED(owned_multisigs, idx) ? "Multisig Wallet" : OTHER_WALLET_LABEL);           \
            gui_update_text(walletname, multisig_names[idx]);                                                          \
        } else if (idx < num_registered_wallets) {                                                                     \
            const size_t descriptor_idx = idx - num_multisigs;                                                         \
            gui_update_text(                                                                                           \
                label, IS_OWNED(owned_descriptors, descriptor_idx) ? "Descriptor Wallet" : OTHER_WALLET_LABEL);        \
            gui_update_text(walletname, descriptor_names[descriptor_idx]);                                             \
        } else {                                                                                                       \
            gui_update_text(label, "");                                                                                \
            gui_update_text(walletname, "[Cancel]");                                                                   \
        }                                                                                                              \
    } while (false)

bool select_registered_wallet(const char multisig_names[][NVS_KEY_NAME_MAX_SIZE], const size_t num_multisigs,
    const bool* owned_multisigs, const char descriptor_names[][NVS_KEY_NAME_MAX_SIZE], const size_t num_descriptors,
    const bool* owned_descriptors, const char** wallet_name_out, bool* is_multisig)
{
    // owned_multisigs/owned_descriptors are optional - see IS_OWNED() above
    JADE_ASSERT(!num_multisigs || multisig_names);
    JADE_ASSERT(!num_descriptors || descriptor_names);
    JADE_ASSERT(num_multisigs || num_descriptors);
    JADE_INIT_OUT_PPTR(wallet_name_out);
    JADE_ASSERT(is_multisig);

    const size_t num_registered_wallets = num_multisigs + num_descriptors;

    size_t selected = 0;
    gui_view_node_t* label = NULL;
    gui_view_node_t* walletname = NULL;
    gui_activity_t* const act = make_carousel_activity("View Wallet", &label, &walletname);
    UPDATE_WALLET_CAROUSEL(0);

    // BBB-AIRGAP: one registration owned by the activity, rather than a fresh one per pass.
    // gui_activity_wait_event() makes a new wait record every call (main/gui.c), so a wheel event
    // that arrived while the loop was redrawing the carousel signalled the previous pass' record
    // and was lost - the wallet on screen would stop moving until the next turn of the wheel.  The
    // record below lives as long as the activity and keeps that event.
    wait_event_data_t* const event_data = gui_activity_make_wait_event_data(act);
    JADE_ASSERT(event_data);
    gui_activity_register_event(act, GUI_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);

    // Switched synchronously so the drain below has something to drain: the asynchronous
    // gui_set_current_activity() only queues the switch, and this activity's handlers go live
    // later, on the gui task.  What is discarded is the second half of the press that opened this
    // carousel - select_action() posts the caller's GUI_BUTTON_EVENT and then gui_front_click()
    // posts its own GUI_EVENT click for the same press (main/gui.c:2556-2573), and the
    // registration above takes any GUI_EVENT, so that click would otherwise be read here as the
    // user picking whichever wallet the carousel opened on.  Same 10ms idle timeout as
    // run_list_activity() (main/ui/dialogs.c).
    gui_set_current_activity_sync(act, false);
    while (sync_wait_event(event_data, NULL, NULL, NULL, 10 / portTICK_PERIOD_MS) == ESP_OK) {
        // discard - see comment above
    }

    int32_t ev_id;

    const size_t limit = num_registered_wallets + 1;
    bool done = false;
    while (!done) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return false;
        }

        JADE_ASSERT(selected < limit);
        UPDATE_WALLET_CAROUSEL(selected);

        if (sync_wait_event(event_data, NULL, &ev_id, NULL, 0) == ESP_OK) {
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
    if (selected >= num_registered_wallets) {
        // Back/exit
        return false;
    }

    *is_multisig = selected < num_multisigs;
    *wallet_name_out = *is_multisig ? multisig_names[selected] : descriptor_names[selected - num_multisigs];
    return true;
}
#endif // AMALGAMATED_BUILD
