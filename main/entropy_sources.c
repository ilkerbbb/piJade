#ifndef AMALGAMATED_BUILD
#include "entropy_sources.h"

#include "button_events.h"
#include "camera.h"
#include "display.h"
#include "jade_assert.h"
#include "jade_wally_verify.h"
#include "random.h"
#include "sensitive.h"
#include "ui.h"
#include "utils/event.h"

#include <mbedtls/sha256.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <wally_bip39.h>
#include <wally_crypto.h>

#include <stdio.h>
#include <string.h>

// Defined in ui/mnemonic.c
gui_activity_t* make_new_mnemonic_activity(void);

// Values the wheel cycles through when entering rolls; '<' undoes the last roll.
static const char DICE_CHARS[] = "123456<";
#define NUM_DICE_CHARS (sizeof(DICE_CHARS) - 1)

// Width of the entry cell, as in ui/digit_entry.c - one cell rather than six.
#define DICE_CELL_WIDTH 35

// Body layout, by panel height.  add_title_bar() takes 20% of the panel, so the three bands
// share 192 px at 240x240, 136 px at 320x170 and 108 px on the 240x135 DIY board.  Measured font
// heights (display_get_font_height): DEJAVU24 25 px, DEJAVU18 18 px, the arrow symbols 16 px.
// The cell splits 25/50/25, so its value band must hold 25 px plus DICE_VALUE_TOPPAD, and each
// arrow band 16 px; text that does not fit is skipped (display.c, display_print_in_area).
// make_progress_bar() spends 8 px of container margins, 4 px of border and 4 px of child margins
// of its band, ie. it needs more than 16 px: below that the child's y1 passes its y2, and
// paint_borders() computes its height as a uint16_t, so the bar's left border comes out 65535 px
// tall and display_fill_rect() clamps it to the bottom edge of the screen - a wide highlight
// block, not a bar.  Hence the short panels get a smaller cell and a bigger bar than the
// 240x240 one; ui/digit_entry.c scales its own cell padding with CONFIG_DISPLAY_HEIGHT for the
// same reason, and at 135 px even upstream's cell loses its arrows and keeps only the value.
#define DICE_TALL_PANEL (CONFIG_DISPLAY_HEIGHT > 200)
#define DICE_COUNT_BAND_PCNT (DICE_TALL_PANEL ? 22 : 18)
#define DICE_CELL_BAND_PCNT (DICE_TALL_PANEL ? 56 : 60)
#define DICE_BAR_BAND_PCNT 22 // the same everywhere: 42 px at 240x240, 29 at 170, 23 at 135
#define DICE_CELL_TOPPAD (DICE_TALL_PANEL ? 6 : 2)
// The value sits a little low in its band without this nudge, but at 135 px the band is 27 px
// and the 25 px value only fits without it - the same trade ui/digit_entry.c makes at that height.
#define DICE_VALUE_TOPPAD (CONFIG_DISPLAY_HEIGHT > 160 ? 5 : 0)

// The text nodes sit inside plain fill nodes so that repainting the fill clears
// the previous text - see ui/digit_entry.c, which uses the same arrangement.
typedef struct {
    gui_view_node_t* count_fill;
    gui_view_node_t* count_text;
    gui_view_node_t* value_fill;
    gui_view_node_t* value_text;
    progress_bar_t bar;
} dice_display_t;

static void update_dice_display(
    dice_display_t* display, const char selected, const size_t nrolls, const size_t total_rolls)
{
    JADE_ASSERT(display);
    JADE_ASSERT(nrolls < total_rolls);

    const char value[2] = { selected, '\0' };
    gui_update_text(display->value_text, value);

    char buf[24];
    const int ret = snprintf(buf, sizeof(buf), "Roll %u of %u", (unsigned)(nrolls + 1), (unsigned)total_rolls);
    JADE_ASSERT(ret > 0 && ret < sizeof(buf));
    gui_update_text(display->count_text, buf);

    update_progress_bar(&display->bar, total_rolls, nrolls);

    gui_repaint(display->value_fill);
    gui_repaint(display->count_fill);
}

// Collect 'total_rolls' dice rolls into 'rolls' as ascii '1'-'6'.
// Returns false if the user backs out past the first roll.
static bool run_dice_entry(char* rolls, const size_t total_rolls)
{
    JADE_ASSERT(rolls);
    JADE_ASSERT(total_rolls);

    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* const parent = add_title_bar(act, "Dice Rolls", NULL, 0, NULL);

    gui_view_node_t* vsplit;
    gui_make_vsplit(
        &vsplit, GUI_SPLIT_RELATIVE, 3, DICE_COUNT_BAND_PCNT, DICE_CELL_BAND_PCNT, DICE_BAR_BAND_PCNT);
    gui_set_parent(vsplit, parent);

    dice_display_t display = { 0 };
    gui_view_node_t* node;

    // Roll counter.  The bar below gives the proportion, this gives the exact position -
    // ui/qrmode.c pairs them the same way for address verification.
    gui_make_fill(&display.count_fill, TFT_BLACK, FILL_PLAIN, vsplit);
    gui_make_text(&display.count_text, "", TFT_WHITE);
    gui_set_align(display.count_text, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(display.count_text, display.count_fill);

    // The roll being entered, in the cell Jade uses for every wheel-driven value: a
    // highlight-bordered box holding an up arrow, the value in DEJAVU24 and a down arrow.
    // Same geometry as ui/digit_entry.c - DICE_CELL_WIDTH wide, 25/50/25 vertically - so
    // that this screen reads as part of the same device.  Without the arrows the value
    // looks like a label rather than something the wheel changes.
    const size_t lrpad = (CONFIG_DISPLAY_WIDTH - DICE_CELL_WIDTH) / 2;
    gui_view_node_t* hsplit;
    gui_make_hsplit(&hsplit, GUI_SPLIT_ABSOLUTE, 1, DICE_CELL_WIDTH);
    gui_set_margins(hsplit, GUI_MARGIN_ALL_DIFFERENT, DICE_CELL_TOPPAD, lrpad, DICE_CELL_TOPPAD * 2, lrpad);
    gui_set_parent(hsplit, vsplit);

    gui_make_fill(&display.value_fill, TFT_BLACK, FILL_PLAIN, hsplit);
    gui_set_borders(display.value_fill, gui_get_highlight_color(), 2, GUI_BORDER_ALL);

    gui_view_node_t* cellsplit;
    gui_make_vsplit(&cellsplit, GUI_SPLIT_RELATIVE, 3, 25, 50, 25);
    gui_set_parent(cellsplit, display.value_fill);

    gui_make_text_font(&node, "K", TFT_WHITE, JADE_SYMBOLS_16x16_FONT);
    gui_set_align(node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(node, cellsplit);

    gui_make_text_font(&display.value_text, "", TFT_WHITE, DEJAVU24_FONT);
    gui_set_align(display.value_text, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_padding(display.value_text, GUI_MARGIN_ALL_DIFFERENT, DICE_VALUE_TOPPAD, 0, 0, 0);
    gui_set_parent(display.value_text, cellsplit);

    gui_make_text_font(&node, "L", TFT_WHITE, JADE_SYMBOLS_16x16_FONT);
    gui_set_align(node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(node, cellsplit);

    // Progress across the whole run.  A count alone gives no sense of how much of a 50 or 99
    // roll run is left.  Opaque, not the transparent treatment ui/camera.c uses: '<' undoes a
    // roll, so the value here DECREASES, and make_progress_bar() notes that a transparent bar
    // then needs its parent redrawn (ui/dialogs.c:684).  There is no image behind this bar to
    // show through, so the plain fill upstream uses everywhere else is both correct and simpler.
    make_progress_bar(vsplit, &display.bar);

    // Register a single handler up-front and await it in the loop below, as camera.c and
    // qrmode.c do for their tight loops.  gui_activity_wait_event() would register a fresh
    // handler (and a fresh semaphore) on every iteration, so any event arriving while the
    // screen is being redrawn would be signalled to the previous semaphore and lost.
    wait_event_data_t* const event_data = gui_activity_make_wait_event_data(act);
    JADE_ASSERT(event_data);
    gui_activity_register_event(act, GUI_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);

    // Switched synchronously so the drain below has something to drain: the asynchronous
    // gui_set_current_activity() only queues the switch, and this activity's handlers go live
    // later, on the gui task.  What is discarded is the second half of the press that opened this
    // screen - a single press posts the menu's GUI_BUTTON_EVENT via select_action() and then,
    // unconditionally, its own GUI_EVENT click (main/gui.c:2556-2573), and the registration above
    // takes any GUI_EVENT.  Here that click would be read as a roll the user never made: the loop
    // below records DICE_CHARS[selected] on a click, and 'selected' still points at the first
    // face.  Same 10ms idle timeout as run_list_activity() (main/ui/dialogs.c).
    gui_set_current_activity_sync(act, false);
    while (sync_wait_event(event_data, NULL, NULL, NULL, 10 / portTICK_PERIOD_MS) == ESP_OK) {
        // discard - see comment above
    }

    size_t nrolls = 0;
    size_t selected = 0;
    update_dice_display(&display, DICE_CHARS[selected], nrolls, total_rolls);

    while (nrolls < total_rolls) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return false;
        }

        int32_t ev_id;
        if (sync_wait_event(event_data, NULL, &ev_id, NULL, 0) != ESP_OK) {
            continue;
        }

        if (ev_id == GUI_WHEEL_LEFT_EVENT) {
            selected = (selected + NUM_DICE_CHARS - 1) % NUM_DICE_CHARS;
        } else if (ev_id == GUI_WHEEL_RIGHT_EVENT) {
            selected = (selected + 1) % NUM_DICE_CHARS;
        } else if (ev_id == gui_get_click_event()) {
            if (DICE_CHARS[selected] == '<') {
                if (!nrolls) {
                    // Backed out of the first roll - entry abandoned
                    return false;
                }
                --nrolls;
            } else {
                rolls[nrolls] = DICE_CHARS[selected];
                ++nrolls;
                if (nrolls == total_rolls) {
                    break;
                }
            }
        } else {
            continue;
        }

        update_dice_display(&display, DICE_CHARS[selected], nrolls, total_rolls);
    }

    return true;
}

gui_activity_t* make_new_mnemonic_source_activity(void)
{
    // BBB-AIRGAP: only the advanced arm of the setup-method menu opens this screen
    // (make_mnemonic_setup_method_activity), so 'back' always returns there.
    btn_data_t hdrbtns[] = { { .txt = "=",
                                 .font = JADE_SYMBOLS_16x16_FONT,
                                 .ev_id = BTN_MNEMONIC_METHOD },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    // BBB-AIRGAP: 'Combined' is last because it is the longest ceremony, and it is named for what
    // it actually mixes: without a camera it is dice plus the device CSPRNG, and saying so is the
    // point - a menu entry that silently drops a source is worse than one that never offered it.
    // make_menu_activity() takes at most four entries (dialogs.c), which this exactly fills.
    btn_data_t menubtns[] = { { .txt = "Device", .font = GUI_DEFAULT_FONT, .ev_id = BTN_NEW_MNEMONIC_DEVICE },
        { .txt = "Dice Rolls", .font = GUI_DEFAULT_FONT, .ev_id = BTN_NEW_MNEMONIC_DICE },
#ifdef HAVE_CAMERA_ENTROPY
        { .txt = "Camera", .font = GUI_DEFAULT_FONT, .ev_id = BTN_NEW_MNEMONIC_CAMERA },
        { .txt = "Combined", .font = GUI_DEFAULT_FONT, .ev_id = BTN_NEW_MNEMONIC_COMBINED } };
#else
        { .txt = "Dice + Device", .font = GUI_DEFAULT_FONT, .ev_id = BTN_NEW_MNEMONIC_COMBINED } };
#endif

    const size_t nbtns = sizeof(menubtns) / sizeof(menubtns[0]);

    gui_activity_t* const act = make_menu_activity("Entropy Source", hdrbtns, 2, menubtns, nbtns);
    gui_set_activity_initial_selection(menubtns[0].btn);
    return act;
}

size_t await_new_mnemonic_nwords(void)
{
    gui_activity_t* const act = make_new_mnemonic_activity();
    gui_set_current_activity(act);

    while (true) {
        // BBB-AIRGAP: KEY3 leaves this screen; see gui_escape_request() in main/gui.h.
        if (gui_escape_pending()) {
            return 0;
        }

        const int32_t ev_id = gui_activity_wait_button(act, BTN_EVENT_TIMEOUT);
        if (ev_id == BTN_NEW_MNEMONIC_12) {
            return 12;
        } else if (ev_id == BTN_NEW_MNEMONIC_24) {
            return 24;
        } else if (ev_id == BTN_MNEMONIC_METHOD) {
            // 'back' in the title bar of that screen
            return 0;
        }
    }
}

bool gather_dice_entropy(const size_t nwords, uint8_t* entropy_out, const size_t entropy_len)
{
    JADE_ASSERT(nwords == 12 || nwords == 24);
    JADE_ASSERT(entropy_out);
    JADE_ASSERT(entropy_len == (nwords == 12 ? BIP39_ENTROPY_LEN_128 : BIP39_ENTROPY_LEN_256));

    const size_t total_rolls = nwords == 12 ? DICE_ROLLS_12WORD : DICE_ROLLS_24WORD;

    char rolls[DICE_ROLLS_24WORD];
    SENSITIVE_PUSH(rolls, sizeof(rolls));

    bool ret = false;
    if (run_dice_entry(rolls, total_rolls)) {
        // As SeedSigner does: hash the rolls as an ascii string, take the leading
        // bytes. No device randomness is mixed in, so the result stays verifiable.
        uint8_t hash[SHA256_LEN];
        SENSITIVE_PUSH(hash, sizeof(hash));
        JADE_WALLY_VERIFY(wally_sha256((const uint8_t*)rolls, total_rolls, hash, sizeof(hash)));
        memcpy(entropy_out, hash, entropy_len);
        SENSITIVE_POP(hash);
        ret = true;
    }

    SENSITIVE_POP(rolls);
    return ret;
}

#ifdef HAVE_CAMERA_ENTROPY
typedef struct {
    mbedtls_sha256_context hasher;
    progress_bar_t* progress_bar;
    // BBB-AIRGAP: the on-screen label, so the user is told when they may finish. Set by the
    // camera task before the first frame arrives; NULL if there is no ui.
    gui_view_node_t** label_node;
    bool label_says_enough;
    size_t nframes;
    // BBB-AIRGAP: why a frame was turned away, for the log. Not shown on screen: the honest
    // feedback the user needs is the bar refusing to move, which they can see without numbers.
    size_t nrejected_flat;
    size_t nrejected_still;
    size_t nrejected_repeat;
    // BBB-AIRGAP: ring buffer over the last CAMERA_ENTROPY_FRAMES accepted frames. Collection is
    // no longer bounded by the frame count, so this cannot be indexed by it.
    uint8_t frame_digests[CAMERA_ENTROPY_FRAMES][SHA256_LEN];
    // The current frame's signature lives here rather than on the callback's stack so that it is
    // covered by the same SENSITIVE_PUSH as the rest of the context, like the frame digests.
    uint8_t signature[CAMERA_SIGNATURE_BLOCKS];
    uint8_t last_signature[CAMERA_SIGNATURE_BLOCKS];
    bool have_signature;
} camera_entropy_ctx_t;

// BBB-AIRGAP: reduce a frame to an 8x8 grid of block averages - see CAMERA_SIGNATURE_SIDE.
static void frame_signature(const size_t width, const size_t height, const uint8_t* data, uint8_t* signature)
{
    JADE_ASSERT(data);
    JADE_ASSERT(signature);

    const size_t block_width = width / CAMERA_SIGNATURE_SIDE;
    const size_t block_height = height / CAMERA_SIGNATURE_SIDE;
    JADE_ASSERT(block_width);
    JADE_ASSERT(block_height);
    const uint32_t block_pixels = (uint32_t)block_width * block_height;

    for (size_t by = 0; by < CAMERA_SIGNATURE_SIDE; ++by) {
        for (size_t bx = 0; bx < CAMERA_SIGNATURE_SIDE; ++bx) {
            uint32_t sum = 0;
            for (size_t y = 0; y < block_height; ++y) {
                const uint8_t* const row = data + ((by * block_height) + y) * width + (bx * block_width);
                for (size_t x = 0; x < block_width; ++x) {
                    sum += row[x];
                }
            }
            signature[(by * CAMERA_SIGNATURE_SIDE) + bx] = (uint8_t)(sum / block_pixels);
        }
    }
}

// BBB-AIRGAP: how much the scene moved between two signatures. The mean change is subtracted
// first, so a uniform brightness step (auto-exposure) contributes nothing. Note what that also
// means: any movement that shifts every block by the same amount reads as exposure and is turned
// away. A featureless gradient panned across the frame is the pure case of this - and it is the
// right answer, since such a frame carries no detail to begin with.
static uint32_t signature_change(const uint8_t* previous, const uint8_t* current)
{
    JADE_ASSERT(previous);
    JADE_ASSERT(current);

    int32_t total = 0;
    for (size_t i = 0; i < CAMERA_SIGNATURE_BLOCKS; ++i) {
        total += (int32_t)current[i] - (int32_t)previous[i];
    }
    const int32_t mean = total / (int32_t)CAMERA_SIGNATURE_BLOCKS;

    uint32_t change = 0;
    for (size_t i = 0; i < CAMERA_SIGNATURE_BLOCKS; ++i) {
        const int32_t delta = ((int32_t)current[i] - (int32_t)previous[i]) - mean;
        change += (uint32_t)(delta < 0 ? -delta : delta);
    }
    return change;
}

// Camera frame callback, in the same shape as main.c's rnd_camera_feed().
// BBB-AIRGAP: always returns false. The camera is never stopped by a frame count; the user ends
// the collection with the exit button, and gather_camera_entropy() then decides whether enough
// frames arrived. So a user who wants more than the minimum simply keeps going.
static bool camera_entropy_feed(
    const size_t width, const size_t height, const uint8_t* data, const size_t len, void* ctx_data)
{
    JADE_ASSERT(data);
    JADE_ASSERT(len);
    JADE_ASSERT(ctx_data);

    camera_entropy_ctx_t* const ctx = (camera_entropy_ctx_t*)ctx_data;

    // Ignore a frame whose pixels are all but identical - see CAMERA_FRAME_MIN_RANGE.
    // Such frames can differ from each other (so the duplicate check below passes them)
    // while carrying no entropy at all.
    uint8_t darkest = 0xff;
    uint8_t lightest = 0;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] < darkest) {
            darkest = data[i];
        }
        if (data[i] > lightest) {
            lightest = data[i];
        }
        if (lightest - darkest >= CAMERA_FRAME_MIN_RANGE) {
            break;
        }
    }
    if (lightest - darkest < CAMERA_FRAME_MIN_RANGE) {
        ++ctx->nrejected_flat;
        return false;
    }

    // BBB-AIRGAP: ignore a frame that shows the same scene as the last accepted one. A camera
    // lying still delivers frames that differ in sensor noise alone: every one of them is a
    // fresh sha256 (so the duplicate check below waves them through) and every one of them can
    // clear the contrast threshold above, yet between them they carry almost nothing. Requiring
    // visible movement makes a half-frozen sensor fail closed, and it hands the pace of the
    // collection to the user instead of the frame rate.
    frame_signature(width, height, data, ctx->signature);
    if (ctx->have_signature && signature_change(ctx->last_signature, ctx->signature) < CAMERA_FRAME_MIN_CHANGE) {
        ++ctx->nrejected_still;
        return false;
    }

    // Ignore a frame identical to a recently accepted one - it carries no entropy.
    // esp_camera_fb_get() hands back the previous buffer when no new frame arrives
    // (libjade/esp_camera.c), so without this a stalled camera would silently reduce
    // the entropy to the device id and the tick count. The history is the last
    // CAMERA_ENTROPY_FRAMES digests rather than every frame ever accepted, which also
    // catches a sensor cycling through a small number of stale buffers. A cycle longer
    // than that window escapes the check, but a cycle that long is no longer the stall
    // pattern this gate exists for; see the entropy claim note in the audit document.
    uint8_t digest[SHA256_LEN];
    SENSITIVE_PUSH(digest, sizeof(digest));
    JADE_WALLY_VERIFY(wally_sha256(data, len, digest, sizeof(digest)));
    const size_t nkept = ctx->nframes < CAMERA_ENTROPY_FRAMES ? ctx->nframes : CAMERA_ENTROPY_FRAMES;
    for (size_t i = 0; i < nkept; ++i) {
        if (!memcmp(digest, ctx->frame_digests[i], sizeof(digest))) {
            ++ctx->nrejected_repeat;
            SENSITIVE_POP(digest);
            return false;
        }
    }
    memcpy(ctx->frame_digests[ctx->nframes % CAMERA_ENTROPY_FRAMES], digest, sizeof(digest));
    SENSITIVE_POP(digest);

    JADE_ZERO_VERIFY(mbedtls_sha256_update(&ctx->hasher, data, len));
    memcpy(ctx->last_signature, ctx->signature, sizeof(ctx->last_signature));
    ctx->have_signature = true;
    ++ctx->nframes;

    // The bar fills at the minimum and stays full; past that the count is no longer a countdown
    // to anything, and update_progress_bar() asserts current <= total in any case.
    const size_t shown = ctx->nframes < CAMERA_ENTROPY_FRAMES ? ctx->nframes : CAMERA_ENTROPY_FRAMES;
    update_progress_bar(ctx->progress_bar, CAMERA_ENTROPY_FRAMES, shown);

    // BBB-AIRGAP: the exit button means "cancel" before the minimum and "finish" after it. Say so
    // the moment that changes, otherwise a user who stops early loses the collection without ever
    // being told there was a line to cross.  "Enough" is carried by the text changing at all, and
    // by the progress bar underneath filling up; the words are spent on the half a user cannot
    // infer, which is that leaving now keeps the frames rather than throwing them away.  The
    // camera's label lives in the header row (make_camera_activity, main/ui/camera.c), so this
    // has to fit 144 pixels of DejaVu18 on one line - display_print_in_area() drops a wrapped
    // second line that will not fit the cell's height, which would silently truncate it.
    if (!ctx->label_says_enough && ctx->nframes >= CAMERA_ENTROPY_FRAMES && ctx->label_node && *ctx->label_node) {
        gui_update_text(*ctx->label_node, "Exit to finish");
        ctx->label_says_enough = true;
    }
    return false;
}

bool gather_camera_entropy(const size_t nwords, uint8_t* entropy_out, const size_t entropy_len)
{
    JADE_ASSERT(nwords == 12 || nwords == 24);
    JADE_ASSERT(entropy_out);
    JADE_ASSERT(entropy_len == (nwords == 12 ? BIP39_ENTROPY_LEN_128 : BIP39_ENTROPY_LEN_256));

    // Same order as SeedSigner: the device id, then the time, then the frames.
    // 'macid' is Jade's own device identifier - see main.c.
    extern uint8_t macid[6];
    const TickType_t ticks = xTaskGetTickCount();

    progress_bar_t progress_bar = {};
    gui_view_node_t* label_node = NULL;
    camera_entropy_ctx_t ctx = { .progress_bar = &progress_bar, .label_node = &label_node, .nframes = 0 };
    SENSITIVE_PUSH(&ctx, sizeof(ctx));
    mbedtls_sha256_init(&ctx.hasher);

    const int is224 = 0;
    JADE_ZERO_VERIFY(mbedtls_sha256_starts(&ctx.hasher, is224));
    JADE_ZERO_VERIFY(mbedtls_sha256_update(&ctx.hasher, macid, sizeof(macid)));
    JADE_ZERO_VERIFY(mbedtls_sha256_update(&ctx.hasher, (const uint8_t*)&ticks, sizeof(ticks)));

    const bool show_ui = true;
    const bool show_click_button = false;
    jade_camera_process_images(camera_entropy_feed, &ctx, show_ui, "Move camera", show_click_button, QR_GUIDE_HIDE,
        NULL, &progress_bar, NULL, &label_node);

    // BBB-AIRGAP: the tallies are the measurement surface for the change gate - a threshold that
    // is still a provisional floor (see CAMERA_FRAME_MIN_CHANGE). They say nothing about the
    // entropy collected and are not shown to the user.
    JADE_LOGI("Camera entropy: %u frames accepted, rejected %u flat, %u unchanged, %u repeated", (unsigned)ctx.nframes,
        (unsigned)ctx.nrejected_flat, (unsigned)ctx.nrejected_still, (unsigned)ctx.nrejected_repeat);

    bool ret = false;
    // BBB-AIRGAP: KEY3 abandons collection even after enough frames; only the normal camera
    // exit completes it. Keep the hasher and sensitive-buffer cleanup below on both paths.
    if (!gui_escape_pending() && ctx.nframes >= CAMERA_ENTROPY_FRAMES) {
        // BBB-AIRGAP: mix the device CSPRNG into the chain as its final input, so the camera is a
        // layer added on top of a proven floor rather than the sole source. Note the frame count
        // is still required first: mixing does not turn a camera that never delivered into a valid
        // source, it only removes the assumption that the frames themselves carried 256 bits.
        uint8_t devrnd[SHA256_LEN];
        SENSITIVE_PUSH(devrnd, sizeof(devrnd));
        get_random(devrnd, sizeof(devrnd));
        JADE_ZERO_VERIFY(mbedtls_sha256_update(&ctx.hasher, devrnd, sizeof(devrnd)));
        SENSITIVE_POP(devrnd);

        uint8_t hash[SHA256_LEN];
        SENSITIVE_PUSH(hash, sizeof(hash));
        JADE_ZERO_VERIFY(mbedtls_sha256_finish(&ctx.hasher, hash));
        memcpy(entropy_out, hash, entropy_len);
        SENSITIVE_POP(hash);
        ret = true;
    }

    // Frees the hasher's copy of the frame data
    mbedtls_sha256_free(&ctx.hasher);
    SENSITIVE_POP(&ctx);
    return ret;
}
#endif // HAVE_CAMERA_ENTROPY

// BBB-AIRGAP: the two collectors are left exactly as they are and only their outputs are joined,
// so the pure dice path keeps the property that made it worth having - a user with paper, a die
// and sha256 can reproduce it. Combining at the output rather than sharing one hasher is
// cryptographically the same thing here: both halves are already 256-bit digests, and hashing
// them together is the standard way to end up no weaker than the stronger half.
bool gather_combined_entropy(const size_t nwords, uint8_t* entropy_out, const size_t entropy_len)
{
    JADE_ASSERT(nwords == 12 || nwords == 24);
    JADE_ASSERT(entropy_out);
    JADE_ASSERT(entropy_len == (nwords == 12 ? BIP39_ENTROPY_LEN_128 : BIP39_ENTROPY_LEN_256));

    // Dice first, deliberately: the user picks how many rolls to make while nothing has been
    // gathered yet, and the camera is then added on top of a number they chose themselves.
    uint8_t parts[2 * BIP39_ENTROPY_LEN_256];
    SENSITIVE_PUSH(parts, sizeof(parts));
    uint8_t* const from_dice = parts;
    uint8_t* const from_second = parts + entropy_len;

    bool ret = false;
    if (gather_dice_entropy(nwords, from_dice, entropy_len)) {
#ifdef HAVE_CAMERA_ENTROPY
        // Abandoning the camera abandons the whole ceremony, rolls included. That is the honest
        // outcome: keeping the dice half alone would hand the user a wallet from a source they
        // did not choose, under a menu entry that promised two.
        const bool have_second = gather_camera_entropy(nwords, from_second, entropy_len);
#else
        get_random(from_second, entropy_len);
        const bool have_second = true;
#endif
        if (have_second) {
            // Both halves are fixed length, so the concatenation hashed here is unambiguous.
            // On the 12-word path the 32-byte digest is then truncated to the 16-byte target;
            // truncating a hash is lossless at a 128-bit target.
            uint8_t hash[SHA256_LEN];
            SENSITIVE_PUSH(hash, sizeof(hash));
            JADE_WALLY_VERIFY(wally_sha256(parts, 2 * entropy_len, hash, sizeof(hash)));
            memcpy(entropy_out, hash, entropy_len);
            SENSITIVE_POP(hash);
            ret = true;
        }
    }

    SENSITIVE_POP(parts);
    return ret;
}
#endif // AMALGAMATED_BUILD
