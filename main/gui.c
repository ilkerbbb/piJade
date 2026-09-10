#ifndef AMALGAMATED_BUILD
#include <stdarg.h>
#include <stdatomic.h> // BBB-AIRGAP: see activity_generation below.
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <wally_core.h>

#include "display.h"

#include "ble/ble.h"
#include "button_events.h"
#include "gui.h"
#include "idletimer.h"
#include "jade_assert.h"
#include "jade_tasks.h"
#include "power.h"
#include "qrcode.h"
#include "random.h"
#include "serial.h"
#include "storage.h"
#include "utils/event.h"
#include "utils/malloc_ext.h"
#include "utils/util.h"

// A genuine production v2 Jade may be awaiting mandatory attestation data
#if defined(CONFIG_BOARD_TYPE_JADE_V2_ANY) && defined(CONFIG_SECURE_BOOT)                                              \
    && defined(CONFIG_SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS)
#include "attestation/attestation.h"
static inline bool gui_awaiting_attestation_data(void) { return !attestation_initialised(); }
#else
// Jade v1.x and diy devices are never awaiting mandatory attestation data
static inline bool gui_awaiting_attestation_data(void) { return false; }
#endif

ESP_EVENT_DEFINE_BASE(GUI_BUTTON_EVENT);
ESP_EVENT_DEFINE_BASE(GUI_EVENT);

const color_t GUI_BLOCKSTREAM_JADE_GREEN = 0x4C04;
const color_t GUI_BLOCKSTREAM_BUTTONBORDER_GREY = 0x0421;
const color_t GUI_BLOCKSTREAM_HIGHTLIGHT_DEFAULT = GUI_BLOCKSTREAM_JADE_GREEN;
const color_t GUI_BLOCKSTREAM_HIGHTLIGHT_ORANGE = 0xE0D3;
const color_t GUI_BLOCKSTREAM_HIGHTLIGHT_BLUE = 0xD318;
const color_t GUI_BLOCKSTREAM_HIGHTLIGHT_DARKGREY = 0xA210;
const color_t GUI_BLOCKSTREAM_HIGHTLIGHT_LIGHTGREY = 0xB294;
const color_t GUI_BLOCKSTREAM_UNHIGHTLIGHTED_DEFAULT = 0x494A;

typedef struct _activity_holder_t activity_holder_t;
struct _activity_holder_t {
    gui_activity_t activity;
    activity_holder_t* next;
};

typedef struct {
    gui_view_node_t* node_to_repaint; // Node to repaint (repaint request)
    gui_activity_t* new_activity; // New activity to set
    activity_holder_t* to_free; // List of activities to free
    SemaphoreHandle_t done; // Optional: signaled after job completes
} gui_task_job_t;

// Main mutex used to synchronize all gui node and activity data
// notably the current activity and the list of managed activities
// and any calls to the underlying screen-driver library.
static SemaphoreHandle_t gui_mutex = NULL;
// current activity being drawn on screen
static gui_activity_t* current_activity = NULL;

// BBB-AIRGAP: which screen is current, as a number that changes every time the gui task swaps the
// current activity, and at no other time.  A host that dispatches button presses uses it to tell
// that the screen under the user has changed since the last frame it wrote, including when nothing
// the user did caused the change: auto-scan leaves the camera loop the moment a QR decodes
// (main/camera.c:538) and the caller swaps straight to a confirm screen.  A repaint is NOT a
// change of screen and does not move this, which is what keeps a camera preview frame from looking
// like a screen the user has not read yet.  Read through gui_get_activity_generation() and,
// outside the firmware, libjade_activity_generation().
// Bumped only here on the gui task, which also renders and flushes, so a frame can never carry a
// number for a screen it does not show. BBB-AIRGAP: publish immediately before replacing the
// input target, with acquire/release ordering to keep that assignment after the announcement.
// Readers only compare the atomic number for equality and can use relaxed loads.
// Starts at zero and is bumped before the first screen is drawn, so a host starting from zero
// treats the first frame as a change, which is the conservative direction.
static _Atomic uint32_t activity_generation = 0;

// BBB-AIRGAP: how many jobs have been handed to the gui task, and how many it has taken off its
// queue.  Together they let a host outside the firmware ask one question it cannot answer on its
// own: does the frame now being written carry the work my last button press caused?  A press
// dispatched from another thread posts its repaint or activity swap before libjade_input()
// returns, so reading 'posted' at that moment names every job that press produced; the queue is
// FIFO, so any frame flushed after 'drained' has reached that number was composed with all of
// them applied.  Compare with the wraparound-safe (int32_t)(drained - posted) >= 0, never with a
// bare >=.
// BBB-AIRGAP: gui_post_mutex serializes enqueue AND publication across producers. Otherwise a
// producer paused after enqueue could leave an uncounted job ahead of a later input's work,
// letting an earlier frame meet that input's undercounted mark. Readers need no lock: each
// published count names a FIFO prefix, and gui_post() publishes before returning to its caller.
// The consumer never takes this mutex, so it can free queue space while a producer waits to send.
// 'posted' is bumped on whichever thread posts, so it is atomic; 'drained' is bumped only on the
// gui task, but a host reads it from its flush handler, which the gui task calls, and that is the
// only reader.  Relaxed is enough for both: each is a counter compared against a value read
// earlier, and the job itself travels through the ring buffer's own synchronisation.
static _Atomic uint32_t gui_jobs_posted = 0;
static _Atomic uint32_t gui_jobs_drained = 0;
static SemaphoreHandle_t gui_post_mutex = NULL;

// stack of activities that currently exist
static activity_holder_t* existing_activities = NULL;

// handle to the task running to update the gui
static TaskHandle_t gui_task_handle = NULL;
// queue for gui task to receive items to process (eg. repaint node, switch activities, etc.)
static RingbufHandle_t gui_input_queue = NULL;
// flag to indicate whether the gui task should stop. This is set
// to true when the gui task starts, and is only ever set to false
// by libjade (on shutdown).
static volatile bool gui_task_should_run = false;
// flag indicating the gui task is running. As above, this is only
// set to false when libjade has exited the gui task.
static volatile bool gui_task_running = false;

// Click/select event (ie. which button counts as 'click'/select)
// and which gui highlight colour is in use
static gui_event_t gui_click_event = GUI_FRONT_CLICK_EVENT;
static color_t gui_highlight_color = 0;
static const color_t gui_qrcode_colors[5] = { 0xa210, 0x494a, 0xef7b, 0x18c6, 0xffff };
// BBB-AIRGAP: upstream picks the high-contrast background on the S3 for exactly the reason that
// applies here - index 1 is 0x494a, about RGB(74,40,82), which against black modules is roughly a
// 2:1 contrast ratio and a poor thing to ship as the default. Index 4 is white. The button that
// cycles these is still there, so anyone whose scanner prefers a dimmer code can still get one.
static uint8_t gui_qrcode_color_idx = 4;
static bool gui_orientation_flipped = false;

// BBB-AIRGAP: the KEY3 escape; see gui_escape_request() in main/gui.h for what it is and why it
// is a flag rather than an event id.  Written from the thread that delivers input and read from
// the task that runs the screens, so a single aligned bool with no other state hanging off it.
static volatile bool gui_escape_flag = false;

// BBB-AIRGAP: see gui_set_input_echo(), at the bottom of this file, for what this is for.  It sits
// up here with the other screen-wide state because gui_stop() clears it, and gui_stop() is defined
// well above the input handling that reads it.
static bool gui_input_echo = false;

// status bar
struct {
    gui_view_node_t* root;

    gui_view_node_t* title;
    gui_view_node_t* battery_text;
    gui_view_node_t* usb_text;
    gui_view_node_t* ble_text;

    uint8_t last_battery_val;
    bool last_usb_val;
    bool last_ble_val;
    uint8_t battery_update_counter;

    TaskHandle_t task_handle;

    bool updated;
} status_bar;

// Utils
static void gui_task(void* args);
static void repaint_node(gui_view_node_t* node);

#ifdef CONFIG_LIBJADE
#define statusbar_logo_end _binary_statusbar_large_bin_gz_end
#define statusbar_logo_start _binary_statusbar_large_bin_gz_start
#else
#if HOME_SCREEN_DEEP_STATUS_BAR
extern const uint8_t statusbar_logo_start[] asm("_binary_statusbar_large_bin_gz_start");
extern const uint8_t statusbar_logo_end[] asm("_binary_statusbar_large_bin_gz_end");
#else
extern const uint8_t statusbar_logo_start[] asm("_binary_statusbar_small_bin_gz_start");
extern const uint8_t statusbar_logo_end[] asm("_binary_statusbar_small_bin_gz_end");
#endif
#endif // CONFIG_LIBJADE

static void free_view_node(gui_view_node_t* node);

static void make_status_bar(void)
{
    gui_view_node_t* status_parent = NULL;
    enum gui_horizontal_align name_alignment = GUI_ALIGN_CENTER;
    // BBB-AIRGAP: set alongside the alignment below; the two status-bar shapes give the serial
    // different room, so they do not use the same font.  No "unset" sentinel here the way
    // name_alignment has one: DEFAULT_FONT is 0 (main/display.h:69), so 0 is a real value.
    uint8_t name_font;

    // Black fill background as the root node
    gui_make_fill(&status_bar.root, TFT_BLACK, FILL_PLAIN, NULL);
    status_bar.root->parent = NULL;

    gui_view_node_t* name_parent;
    gui_make_fill(&name_parent, TFT_BLACK, FILL_PLAIN, NULL);

    // Status bar logo image (size appropriate)
    const Picture* const logopic = get_picture(statusbar_logo_start, statusbar_logo_end);

#if HOME_SCREEN_DEEP_STATUS_BAR
    // Make an hsplit for the logo on the left, and info on the right
    // BBB-AIRGAP: the right-hand column carries the unit's serial ('Jade ABCDEF', 11 characters).
    // At 65/35 that column is 81px on our 240x240 panel while the serial needs up to 96px, so the
    // serial was silently clipped - no ellipsis - and, because the font is proportional, a different
    // number of characters survived for different serials, which read as the id changing on every
    // boot (measured 2026-09-07: 'Jade 7A9FB1' rendered as 'Jade 7A9').  The logo picture is
    // 134x42 (logo/statusbar_large.bin.gz), so the left column only needs 134 of the 153px it had;
    // 58/42 gives the logo 136px and the serial 100px.  Widths here are the renderer's own advance
    // sums (display_get_string_width), taken over the widest hex digit in each font, so the fit
    // holds for every possible serial, not just this unit's.
    gui_view_node_t* hsplit;
    gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 2, 58, 42);
    gui_set_padding(hsplit, GUI_MARGIN_ALL_DIFFERENT, 0, 2, 0, 2);
    gui_set_parent(hsplit, status_bar.root);

    // LHS - logo image
    gui_view_node_t* logo;
    gui_make_picture(&logo, logopic);
    gui_set_align(logo, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(logo, hsplit);

    // RHS - Info ; vsplit, the status icons above and the name below
    gui_view_node_t* vsplit;
    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 2, 50, 50);
    gui_set_parent(vsplit, hsplit);

    // Status icons - hsplit above the name
#ifdef CONFIG_HAS_BATTERY
    gui_make_hsplit(&status_parent, GUI_SPLIT_RELATIVE, 3, 28, 28, 44);
#else
    // BBB-AIRGAP: this board has no battery, so the battery icon is painted in the
    // background colour and is never seen (see the CONFIG_HAS_BATTERY branch in
    // gui_set_battery()).  Its column is nonetheless the widest of the three and it sits
    // on the right, which is what pushed the two icons that ARE visible away from the
    // right edge.  Here the empty column takes the left instead and the visible pair ends
    // up against the edge; the widths themselves are unchanged, only their order.
    gui_make_hsplit(&status_parent, GUI_SPLIT_RELATIVE, 3, 44, 28, 28);
#endif
    gui_set_padding(status_parent, GUI_MARGIN_ALL_DIFFERENT, 0, 4, 0, 2);
    gui_set_parent(status_parent, vsplit);

    // The name beneath, aligned to the right
    gui_set_parent(name_parent, vsplit);
    name_alignment = GUI_ALIGN_RIGHT;
    name_font = DEFAULT_FONT;
#else
    // Make an hsplit for the icon, name, and status icons
    gui_make_hsplit(&status_parent, GUI_SPLIT_RELATIVE, 5, 10, 57, 8, 8, 17);
    gui_set_padding(status_parent, GUI_MARGIN_ALL_DIFFERENT, 3, 0, 0, 4);
    gui_set_parent(status_parent, status_bar.root);

    // LHS - logo image
    gui_view_node_t* logo;
    gui_make_picture(&logo, logopic);
    gui_set_align(logo, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(logo, status_parent);

    // The first part is for the name, aligned left
    gui_set_parent(name_parent, status_parent);
    name_alignment = GUI_ALIGN_LEFT;
    // The shallow bar gives the name 57% of the panel (136px at 240 wide) and the widest
    // possible serial measures 115px in UBUNTU16, so this shape never clipped and is left alone.
    name_font = GUI_TITLE_FONT;
#endif // HOME_SCREEN_DEEP_STATUS_BAR

    // Status icons onto the status parent (hsplit)
    JADE_ASSERT(status_parent);
    JADE_ASSERT(status_parent->kind == HSPLIT);
    gui_make_text_font(&status_bar.usb_text, "D", TFT_WHITE, JADE_SYMBOLS_16x16_FONT);
    gui_set_align(status_bar.usb_text, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);

    gui_make_text_font(&status_bar.ble_text, "F", TFT_WHITE, JADE_SYMBOLS_16x16_FONT);
#if HOME_SCREEN_DEEP_STATUS_BAR && !defined(CONFIG_HAS_BATTERY)
    // BBB-AIRGAP: with the battery column moved out of the way this is the last icon in
    // the row, and centring it in its own column would leave a gap at the panel edge that
    // the battery icon never left: upstream draws that one GUI_ALIGN_RIGHT.  Aligning it
    // the same way puts the visible pair exactly where the old rightmost element sat.
    gui_set_align(status_bar.ble_text, GUI_ALIGN_RIGHT, GUI_ALIGN_MIDDLE);
#else
    gui_set_align(status_bar.ble_text, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
#endif

    gui_make_text_font(&status_bar.battery_text, "0", TFT_WHITE, JADE_SYMBOLS_16x32_FONT);
    gui_set_align(status_bar.battery_text, GUI_ALIGN_RIGHT, GUI_ALIGN_MIDDLE);

    // A child takes the split column that matches the order it is added in, so the order
    // below is the layout.  Upstream's order is usb, ble, battery; the deep bar on a
    // board without a battery puts the empty battery column first instead, to match the
    // widths chosen above.  The shallow bar keeps upstream's order in every build: this
    // firmware never draws it (CONFIG_DISPLAY_HEIGHT is 240, see main/gui.h) so its
    // layout cannot be measured here, and an unmeasured change is not made.
#if HOME_SCREEN_DEEP_STATUS_BAR && !defined(CONFIG_HAS_BATTERY)
    gui_set_parent(status_bar.battery_text, status_parent);
    gui_set_parent(status_bar.usb_text, status_parent);
    gui_set_parent(status_bar.ble_text, status_parent);
#else
    gui_set_parent(status_bar.usb_text, status_parent);
    gui_set_parent(status_bar.ble_text, status_parent);
    gui_set_parent(status_bar.battery_text, status_parent);
#endif

    JADE_ASSERT(name_parent);
    JADE_ASSERT(name_parent->kind == FILL);
    JADE_ASSERT(name_alignment != GUI_ALIGN_CENTER);
    // BBB-AIRGAP: the deep bar asks for DEFAULT_FONT rather than GUI_TITLE_FONT (UBUNTU16); the
    // widest possible serial measures 96px there against 115px in UBUNTU16, which is what makes it
    // fit the column above.  Only the home screen carries a status bar (main/ui/dashboard.c:65 is
    // the sole gui_make_activity_ex() call with one outside smoketest), so no other screen's title
    // changes size.
    gui_make_text_font(&status_bar.title, "Jade", TFT_WHITE, name_font);
    gui_set_align(status_bar.title, name_alignment, GUI_ALIGN_MIDDLE);
    gui_set_parent(status_bar.title, name_parent);

    // Status bar data fields for tracking updates
    status_bar.updated = false;
    status_bar.last_battery_val = 0xFF;
    status_bar.battery_update_counter = 0;
}

gui_event_t gui_get_click_event(void) { return gui_click_event; }

void gui_set_click_event(const bool use_wheel_click)
{
    gui_click_event = use_wheel_click ? GUI_WHEEL_CLICK_EVENT : GUI_FRONT_CLICK_EVENT;
}

color_t gui_get_highlight_color(void) { return gui_highlight_color; }

void gui_set_highlight_color(const uint8_t theme)
{
    switch (theme) {
    case 1:
        gui_highlight_color = GUI_BLOCKSTREAM_HIGHTLIGHT_ORANGE;
        break;
    case 2:
        gui_highlight_color = GUI_BLOCKSTREAM_HIGHTLIGHT_BLUE;
        break;
    case 3:
        gui_highlight_color = GUI_BLOCKSTREAM_HIGHTLIGHT_DARKGREY;
        break;
    case 4:
        gui_highlight_color = GUI_BLOCKSTREAM_HIGHTLIGHT_LIGHTGREY;
        break;
    default:
        gui_highlight_color = GUI_BLOCKSTREAM_HIGHTLIGHT_DEFAULT; // jade green
        break;
    }
}

color_t gui_get_qrcode_color(void) { return gui_qrcode_colors[gui_qrcode_color_idx]; }

void gui_next_qrcode_color(void)
{
    uint8_t idx = gui_qrcode_color_idx + 1;
    if (idx >= sizeof(gui_qrcode_colors) / sizeof(gui_qrcode_colors[0])) {
        idx = 0; // wrap around
    }
    gui_qrcode_color_idx = idx;
}

bool gui_get_flipped_orientation(void) { return gui_orientation_flipped; }

uint32_t gui_get_activity_generation(void)
{
    return atomic_load_explicit(&activity_generation, memory_order_relaxed);
}

// BBB-AIRGAP: see gui_jobs_posted / gui_jobs_drained.
uint32_t gui_get_jobs_posted(void)
{
    return atomic_load_explicit(&gui_jobs_posted, memory_order_relaxed);
}

uint32_t gui_get_jobs_drained(void)
{
    return atomic_load_explicit(&gui_jobs_drained, memory_order_relaxed);
}

// BBB-AIRGAP: see gui.h. Nothing to tell the hardware about - the camera frames arrive the same
// way whatever this is set to, and main/camera.c picks the copy that turns them the right way up.
static uint8_t gui_camera_rotation = CAMERA_ROTATION_DEFAULT;

uint8_t gui_get_camera_rotation(void) { return gui_camera_rotation; }

uint8_t gui_set_camera_rotation(const uint8_t quarter_turns)
{
    JADE_ASSERT(quarter_turns < CAMERA_ROTATION_NUM_VALUES);
    gui_camera_rotation = quarter_turns;
    return gui_camera_rotation;
}

uint8_t gui_camera_rotation_from_flags(const uint8_t gui_flags)
{
    const uint8_t stored = (gui_flags & GUI_FLAGS_CAMERA_ROTATION_MASK) >> GUI_FLAGS_CAMERA_ROTATION_SHIFT;
    return (stored + CAMERA_ROTATION_DEFAULT) % CAMERA_ROTATION_NUM_VALUES;
}

uint8_t gui_camera_rotation_to_flags(const uint8_t gui_flags, const uint8_t quarter_turns)
{
    JADE_ASSERT(quarter_turns < CAMERA_ROTATION_NUM_VALUES);
    const uint8_t stored = (quarter_turns + CAMERA_ROTATION_NUM_VALUES - CAMERA_ROTATION_DEFAULT)
        % CAMERA_ROTATION_NUM_VALUES;
    return (gui_flags & ~GUI_FLAGS_CAMERA_ROTATION_MASK)
        | (uint8_t)(stored << GUI_FLAGS_CAMERA_ROTATION_SHIFT);
}

bool gui_set_flipped_orientation(const bool flipped_orientation)
{
    gui_orientation_flipped = display_flip_orientation(flipped_orientation);
    return gui_orientation_flipped;
}

void gui_init(TaskHandle_t* gui_h, const bool create_event_loop)
{
    // Create mutex semaphore
    gui_mutex = xSemaphoreCreateMutex();
    JADE_ASSERT(gui_mutex);

    // BBB-AIRGAP: separate from gui_mutex so queue draining can progress during a blocked send.
    gui_post_mutex = xSemaphoreCreateMutex();
    JADE_ASSERT(gui_post_mutex);

    // Which button event are we to use as a click / 'select item'
    // and which menu highlight colour to use
    const uint8_t gui_flags = storage_get_gui_flags();
    gui_set_click_event(gui_flags & GUI_FLAGS_USE_WHEEL_CLICK);
    gui_set_highlight_color(gui_flags & GUI_FLAGS_THEMES_MASK);
    gui_set_flipped_orientation(gui_flags & GUI_FLAGS_FLIP_ORIENTATION);
    gui_set_camera_rotation(gui_camera_rotation_from_flags(gui_flags));

    // create a blank activity
    current_activity = gui_make_activity();

    if (create_event_loop) {
        // create the default event loop used by btns
        const esp_err_t rc = esp_event_loop_create_default();
        JADE_ASSERT(rc == ESP_OK);
    }

    // Create main input queue (ringbuffer)
    gui_input_queue = xRingbufferCreate(32 * sizeof(gui_task_job_t), RINGBUF_TYPE_NOSPLIT);
    JADE_ASSERT(gui_input_queue);

    // Create status-bar
    make_status_bar();

    // Create (high priority) gui task
    BaseType_t retval
        = xTaskCreatePinnedToCore(gui_task, "gui", 3 * 1024 + 256, NULL, JADE_TASK_PRIO_GUI, gui_h, JADE_CORE_GUI);
    JADE_ASSERT_MSG(retval == pdPASS, "Failed to create GUI task, xTaskCreatePinnedToCore() returned %d", retval);
}

void gui_stop(void)
{
    // BBB-AIRGAP: the screen that owns the echo turns it off before it returns, but it can be
    // killed where it stands: libjade_stop() unblocks sync_wait_event(), which leaves through
    // pthread_exit() and runs no more of the caller.  Clearing it here means the flag cannot
    // outlive the session and disable navigation in the next one.  See gui_set_input_echo().
    gui_input_echo = false;

#ifdef CONFIG_LIBJADE
    // Only used by libjade
    JADE_ASSERT(gui_task_handle);
    JADE_ASSERT(gui_input_queue);
    JADE_ASSERT(gui_mutex);

    // Stop the gui task and wait for it to fully exit.
    gui_task_should_run = false;
    while (gui_task_running) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
#endif // CONFIG_LIBJADE
}

bool gui_initialized(void) { return gui_task_handle; }
bool gui_is_gui_task(void) { return gui_task_handle && xTaskGetCurrentTaskHandle() == gui_task_handle; }

// Is this kind of node selectable?
static inline bool is_kind_selectable(enum view_node_kind kind) { return kind == BUTTON; }

// Is node a "before" node b on-screen? (i.e. is it above or more left?)
static inline bool is_before(const selectable_t* a, const selectable_t* b)
{
    return a->y < b->y || (a->y == b->y && a->x < b->x);
}

// Traverse the tree from `node` downward and set the `is_selected` of every node to `value`
static void set_tree_selection(gui_view_node_t* node, bool value)
{
    JADE_ASSERT(node);

    node->is_selected = value;

    gui_view_node_t* child = node->child;
    while (child) {
        set_tree_selection(child, value);
        child = child->sibling;
    }
}

// Traverse the tree from `node` downward and set the `is_active` of every node to `value`
static void set_tree_active(gui_view_node_t* node, bool value)
{
    JADE_ASSERT(node);

    node->is_active = value;

    gui_view_node_t* child = node->child;
    while (child) {
        set_tree_active(child, value);
        child = child->sibling;
    }
}

// Function to set the passed node as active or inactive, depending on 'value'.
void gui_set_active(gui_view_node_t* node, const bool value)
{
    JADE_ASSERT(node);

    // Set passed node to active/inactive and redraw
    set_tree_active(node, value);
    if (!node->render_data.is_first_time) {
        gui_repaint(node); // Repaint since screen is "live"
    }
}

static gui_view_node_t* get_first_active_node(gui_activity_t* activity)
{
    JADE_ASSERT(activity);

    // Ignore on screen with no selectable elements
    if (activity->selectables) {
        selectable_t* current = activity->selectables;
        selectable_t* const end = current;
        do {
            // Return the first node that is flagged as 'active'
            if (current->node->is_active) {
                return current->node;
            }
            current = current->next;
        } while (current != end);
    }
    return NULL;
}

// select the previous item in the selectables list
// Returns true if the selection is 'moved' to a prior item, or false if not (and selection left unchanged)
// eg. no current item selected, no other selectable items, no prior selectable items [and not wrapping] etc.
static bool select_prev(gui_activity_t* activity)
{
    JADE_ASSERT(activity);

    // Ignore next/prev on screen with no selectable elements
    if (!activity->selectables) {
        return false;
    }

    selectable_t* const end = activity->selectables->next;
    selectable_t* current = activity->selectables;

    // Look for a selected node
    while (current != end && !current->node->is_selected) {
        current = current->prev;
    }

    // no selected nodes
    if (current == end && !current->node->is_selected) {
        return false;
    }

    // no wrapping
    if (!activity->selectables_wrap && current->is_first) {
        return false;
    }

    selectable_t* prev_active = current->prev;
    while (!prev_active->node->is_active || prev_active->node->nav_skip) {
        // end condition, we couldn't find any other active node
        if (prev_active == current) {
            return false;
        }
        // we are about to wrap, return if it's disabled
        if (!activity->selectables_wrap && prev_active->is_first) {
            return false;
        }

        prev_active = prev_active->prev;
    }

    set_tree_selection(current->node, false);
    gui_repaint(current->node);

    set_tree_selection(prev_active->node, true);
    gui_repaint(prev_active->node);

    activity->selectables = prev_active;

    return true;
}

// Note: node must exist and be active/selectable.
// Any prior selection will be cleared.
static void select_node(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->activity);
    JADE_ASSERT(node->is_active);

    // If there are no selectables, it (probably) means the gui element
    // has not been fully initialised/rendered yet.
    // In this case we mark the node as the one to initially select when
    // the activity is drawn for the first time, rather than trying to
    // set the selection immediately.
    if (!node->activity->selectables) {
        gui_set_activity_initial_selection(node);
        return;
    }

    selectable_t* const begin = node->activity->selectables;
    selectable_t* current = begin;

    selectable_t* old_selected = NULL;
    selectable_t* new_selected = NULL;

    // look for both the selected node and `node`
    do {
        JADE_ASSERT(current->node->activity == node->activity);
        if (current->node->is_selected) {
            old_selected = current;
        }
        if (current->node == node) {
            new_selected = current;
        }
        current = current->next;
    } while (current != begin && (!new_selected || !old_selected));

    // Must have found node
    JADE_ASSERT(new_selected);
    JADE_ASSERT(new_selected->node == node);

    // Deactivate prior selection
    if (old_selected) {
        set_tree_selection(old_selected->node, false);
        gui_repaint(old_selected->node);
    }

    // Select passed node
    set_tree_selection(new_selected->node, true);
    gui_repaint(new_selected->node);

    node->activity->selectables = new_selected;
}

// select the next item in the selectables list
// Returns true if the selection is 'moved' to a subsequent item, or false if not (and selection left unchanged)
// eg. no current item selected, no other selectable items, no later selectable items [and not wrapping] etc.
static bool select_next(gui_activity_t* activity)
{
    JADE_ASSERT(activity);

    // Ignore next/prev on screen with no selectable elements
    if (!activity->selectables) {
        return false;
    }

    selectable_t* const end = activity->selectables->prev;
    selectable_t* current = activity->selectables;

    // Look for a selected node
    while (current != end && !current->node->is_selected) {
        current = current->next;
    }

    // no selected nodes
    if (current == end && !current->node->is_selected) {
        return false;
    }

    // no wrapping
    if (!activity->selectables_wrap && current->next->is_first) {
        return false;
    }

    selectable_t* next_active = current->next;
    while (!next_active->node->is_active || next_active->node->nav_skip) {
        // end condition, we couldn't find any other active node
        if (next_active == current) {
            return false;
        }
        // we are about to wrap, return if it's disabled
        if (!activity->selectables_wrap && next_active->is_first) {
            return false;
        }

        next_active = next_active->next;
    }

    // remove selection from `current`
    set_tree_selection(current->node, false);
    gui_repaint(current->node);

    // add selection to `next_active`
    set_tree_selection(next_active->node, true);
    gui_repaint(next_active->node);

    activity->selectables = next_active;

    return true;
}

// trigger the action for the selected element
static void select_action(gui_activity_t* activity)
{
    JADE_ASSERT(activity);

    selectable_t* const current = activity->selectables;
    if (current && current->node->is_selected && current->node->button->click_event_id != GUI_BUTTON_EVENT_NONE) {
        JADE_ASSERT(current->node->activity == activity);
        const esp_err_t rc = esp_event_post(GUI_BUTTON_EVENT, current->node->button->click_event_id,
            &current->node->button->args, sizeof(void*), 100 / portTICK_PERIOD_MS);
        JADE_ASSERT(rc == ESP_OK);
    }
}

// Mark a collection of nodes as active or inactive, select one, and redraw the entire activity.
void gui_activity_set_active_selection(gui_activity_t* activity, gui_view_node_t** nodes, const size_t num_nodes,
    const bool* active, gui_view_node_t* selected)
{
    JADE_ASSERT(activity);
    JADE_ASSERT(nodes);
    JADE_ASSERT(num_nodes);
    JADE_ASSERT(active);
    JADE_ASSERT(selected);

    bool set_selected = false;
    JADE_SEMAPHORE_TAKE(gui_mutex);
    for (size_t i = 0; i < num_nodes; ++i) {
        JADE_ASSERT(nodes[i]->activity == activity);
        set_tree_active(nodes[i], active[i]);
        if (nodes[i] == selected) {
            JADE_ASSERT(active[i]); // can only select active node
            select_node(nodes[i]);
            set_selected = true;
        }
    }
    JADE_SEMAPHORE_GIVE(gui_mutex);

    // 'selected' should have been seen in 'nodes'
    JADE_ASSERT(set_selected);

    if (activity->root_node && !activity->root_node->render_data.is_first_time) {
        // Screen is "live": repaint the whole activity
        gui_repaint(activity->root_node);
    }
}

// push a selectable element to the `selectables` list of `activity`
static void push_selectable(gui_activity_t* activity, gui_view_node_t* node, uint16_t x, uint16_t y)
{
    JADE_ASSERT(activity);
    JADE_ASSERT(node);
    JADE_ASSERT(is_kind_selectable(node->kind));

    selectable_t* us = JADE_CALLOC(1, sizeof(selectable_t));

    us->node = node;
    us->x = x;
    us->y = y;

    // first one
    if (!activity->selectables) {
        us->is_first = true;

        us->prev = us;
        us->next = us;

        activity->selectables = us;
    } else {
        selectable_t* const begin = activity->selectables->prev;
        selectable_t* current = activity->selectables;
        while (begin != current && is_before(current->next, us)) {
            current = current->next;
        }

        us->prev = current;
        current->next->prev = us;

        us->next = current->next;
        current->next = us;

        // TODO: is the second condition required??
        if (us->next->is_first && is_before(us, us->next)) {
            // we are first now
            us->is_first = true;
            us->next->is_first = false;

            activity->selectables = us;
        }
    }
}

static void push_updatable(gui_view_node_t* node, gui_updatable_callback_t callback, void* extra_args)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->activity);

    // allocate & fill all the fields
    updatable_t* us = JADE_CALLOC(1, sizeof(updatable_t));

    us->node = node;

    us->callback = callback;
    us->extra_args = extra_args;

    // first one!
    gui_activity_t* const activity = node->activity;
    if (!activity->updatables) {
        activity->updatables = us;
    } else {
        // add to tail
        updatable_t* current = activity->updatables;
        while (current->next) {
            current = current->next;
        }
        current->next = us;
    }
}

// Create a new/initialised activity
// If 'managed' (the preferred/default), add to the stack of existing activities - these
// activities must not be explicitly freed by the caller but are freed by a subsequent
// call to 'gui_set_current_activity_ex()' passing 'free_other_activities' as true.
// eg. when the main loop reaches some known point (eg. getting back to the main dashboard
// screen between actions) it can call this to 'garbage collect' all outstanding gui elements.
// If not 'managed', we are making the dashboard activity, which persists as long as the
// firmware is running, and will never be freed.
void gui_make_activity_ex(gui_activity_t** ppact, const bool has_status_bar, const char* title, const bool managed)
{
    JADE_INIT_OUT_PPTR(ppact);
    JADE_ASSERT(!title || has_status_bar);

    activity_holder_t* holder = NULL;
    gui_activity_t* activity = NULL;

    if (managed) {
        // Managed activity - create a holder and return its activity
        holder = JADE_CALLOC(1, sizeof(activity_holder_t));
        activity = &holder->activity;
    } else {
        // Unmanaged - just create the activity to return
        activity = JADE_CALLOC(1, sizeof(gui_activity_t));
        JADE_LOGW("Created unmanaged gui activity at %p", activity);
    }

    // Initialise any non-NULL activity fields
    activity->win = GUI_DISPLAY_WINDOW;
    if (has_status_bar) {
        // offset the display window since the top-part will contain the status bar
        activity->win.y1 += GUI_STATUS_BAR_HEIGHT;
    }
    activity->status_bar = has_status_bar;

    if (title) {
        activity->title = strdup(title);
        JADE_ASSERT(activity->title);
    }

    gui_make_fill(&activity->root_node, TFT_BLACK, FILL_PLAIN, NULL);
    activity->root_node->activity = activity;
#ifdef CONFIG_UI_WRAP_ALL_MENUS
    activity->selectables_wrap = true; // allow the button cursor to wrap
#endif

    if (holder) {
        // Managed activity - add to the stack of existing activities
        JADE_SEMAPHORE_TAKE(gui_mutex);
        holder->next = existing_activities;
        existing_activities = holder;
        JADE_SEMAPHORE_GIVE(gui_mutex);
    }
    *ppact = activity; // return to the caller
}

// Create a new/initialised 'managed' activity without a status bar,
// and with the 'wrapped' selection style
gui_activity_t* gui_make_activity(void)
{
    gui_activity_t* activity = NULL;
    gui_make_activity_ex(&activity, false, NULL, true);
    JADE_ASSERT(activity);
    activity->selectables_wrap = true;
    return activity;
}

// free a linked list of selectable_t
static void free_selectables(gui_activity_t* activity)
{
    JADE_ASSERT(activity);

    selectable_t* const begin = activity->selectables;
    if (begin) {
        selectable_t* current = begin->next;
        free(begin);

        while (current != begin) {
            selectable_t* const next = current->next;
            free(current);
            current = next;
        }
    }
}

// free a linked list of updatable_t
static void free_updatables(gui_activity_t* activity)
{
    JADE_ASSERT(activity);

    updatable_t* current = activity->updatables;
    while (current) {
        updatable_t* const next = current->next;
        free(current);
        current = next;
    }
}

// free a linked list of activity_event_t
static void free_activity_events(gui_activity_t* activity)
{
    JADE_ASSERT(activity);

    activity_event_t* current = activity->activity_events;
    while (current) {
        activity_event_t* const next = current->next;
        free(current);
        current = next;
    }
}

// free a linked list of wait_data_t
static void free_wait_data_items(gui_activity_t* activity)
{
    JADE_ASSERT(activity);

    wait_data_t* current = activity->wait_data_items;
    while (current) {
        wait_data_t* const next = current->next;
        free_wait_event_data(current->event_data);
        free(current);
        current = next;
    }
}

// Free all of the activity contents (title, selectables/updatables etc.)
// (but note, not the 'activity' itself)
static void free_activity_internals(gui_activity_t* activity)
{
    JADE_ASSERT(activity);
    JADE_ASSERT(activity != current_activity);

    free_selectables(activity);
    free_updatables(activity);
    free_activity_events(activity);
    free_wait_data_items(activity);

    free_view_node(activity->root_node);

    if (activity->title) {
        free(activity->title);
    }
}

// Free an activity-holder and all of the activity contents (title, selectables/updatables etc.)
static void free_managed_activity(activity_holder_t* holder)
{
    JADE_ASSERT(holder);
    free_activity_internals(&holder->activity);
    free(holder);
}

static void switch_activity_callback(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data)
{
    JADE_ASSERT(handler_arg);
    gui_activity_t* activity = (gui_activity_t*)handler_arg;
    gui_set_current_activity(activity);
}

static void connect_button_activity(gui_view_node_t* node, gui_activity_t* activity)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->activity);
    JADE_ASSERT(node->kind == BUTTON);
    JADE_ASSERT(activity);

    gui_activity_register_event(
        node->activity, GUI_BUTTON_EVENT, node->button->click_event_id, switch_activity_callback, activity);
}

// Link activities eg. by prev/next buttons
void gui_chain_activities(const link_activity_t* link_act, linked_activities_info_t* pActInfo)
{
    JADE_ASSERT(link_act);
    JADE_ASSERT(link_act->activity);
    JADE_ASSERT(pActInfo);

    // Record the first activity
    if (!pActInfo->first_activity) {
        pActInfo->first_activity = link_act->activity;
    }

    // Link activities together by prev and next buttons
    if (pActInfo->last_activity) {
        if (link_act->prev_button) {
            // connect our "prev" btn to prev activity
            connect_button_activity(link_act->prev_button, pActInfo->last_activity);
        }

        // connect prev "next" btn to this activity
        if (pActInfo->last_activity_next_button) {
            connect_button_activity(pActInfo->last_activity_next_button, link_act->activity);
        }
    }

    // Update 'last activity' information to this new activity
    pActInfo->last_activity = link_act->activity;
    pActInfo->last_activity_next_button = link_act->next_button;
}

// attach a view node (recusively) to an activity, by inheriting the activity from the parent
static void set_tree_activity(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->parent);

    // node should not already have an activity
    JADE_ASSERT(!node->activity);

    // NOTE: activity can be null if ultimate parent not attached yet
    // ie. making a node subtree before attaching subtree root to activity tree
    node->activity = node->parent->activity;

    // set child nodes (depth first recursion)
    gui_view_node_t* child = node->child;
    while (child) {
        set_tree_activity(child);
        child = child->sibling;
    }
}

// Helper function to check if node or any of its ancestors are selectable
static bool has_selectable_ancestor(gui_view_node_t* node)
{
    while (node) {
        if (is_kind_selectable(node->kind)) {
            return true;
        }
        node = node->parent;
    }
    return false;
}

void gui_set_parent(gui_view_node_t* child, gui_view_node_t* parent)
{
    JADE_ASSERT(child);
    JADE_ASSERT(parent);
    // NOTE: parent->activity can be null if parent not attached yet
    // ie. making a node subtree before attaching subtree root to activity tree

    // child should not already have a parent
    JADE_ASSERT(!child->parent);

    // Check that if child is selectable, none of its ancestors are selectable
    if (is_kind_selectable(child->kind)) {
        JADE_ASSERT_MSG(
            !has_selectable_ancestor(parent), "Cannot set parent: selectable child cannot have selectable ancestors");
    }

    child->parent = parent;

    // also inherits the activity
    set_tree_activity(child);

    // first child
    if (!parent->child) {
        parent->child = child;
    } else {
        // Add to tail of siblings list
        gui_view_node_t* ptr = parent->child;
        while (ptr->sibling) {
            ptr = ptr->sibling;
        };
        ptr->sibling = child;
    }
}

// Free a view_node
void free_view_node(gui_view_node_t* node)
{
    JADE_ASSERT(node);

    // call the destructor if it's set
    if (node->free_callback) {
        node->free_callback(node->data);
    }

    // free any borders
    free(node->borders);

    // free the extra data struct
    free(node->data);

    if (node->child) {
        free_view_node(node->child);
    }

    if (node->sibling) {
        free_view_node(node->sibling);
    }

    free(node);
}

// destructor for {v,h}split nodes
static void free_view_node_split_data(void* vdata)
{
    JADE_ASSERT(vdata);
    struct view_node_split_data* data = vdata;
    free(data->values);
}

// destructor for text nodes
static void free_view_node_text_data(void* vdata)
{
    JADE_ASSERT(vdata);
    struct view_node_text_data* data = vdata;

    // free the char* that we allocated
    // Use wally_free_string in case the text is sensitive
    wally_free_string(data->text);

    // also the scroll struct if present
    if (data->scroll) {
        free(data->scroll);
    }

    // and also the noise struct if present
    if (data->noise) {
        free(data->noise);
    }
}

// destructor for text nodes
static void free_view_node_icon_data(void* vdata)
{
    JADE_ASSERT(vdata);
    struct view_node_icon_data* data = vdata;

    // free the animation struct if present
    if (data->animation) {
        // NOTE: we owned the animation frames
        for (int i = 0; i < data->animation->num_icons; ++i) {
            // Free the icon data
            // BBB-AIRGAP: animation icons only ever come from the qrcode.c allocator (the sole
            // gui_set_icon_animation() caller is make_qrcode() in ui/qrmode.c), and their pixels
            // are a reversible encoding of exported wallet data, so wipe before freeing.
            qrcode_freeIconData(&data->animation->icons[i]);
        }
        free(data->animation->icons);
        free(data->animation);
    }
}

// destructor for picture nodes
static void free_view_node_picture_data(void* vdata)
{
    JADE_ASSERT(vdata);
    struct view_node_picture_data* data = vdata;
    JADE_ASSERT(data->picture);
    JADE_ASSERT(data->picture->data_8);
    free((void*)data->picture->data_8);
    free((void*)data->picture);
}

// make the underlying view node, common across all the gui_make_* functions
static void make_view_node(gui_view_node_t** ptr, enum view_node_kind kind, void* data, free_callback_t free_callback)
{
    JADE_INIT_OUT_PPTR(ptr);

    *ptr = JADE_CALLOC(1, sizeof(gui_view_node_t));

    (*ptr)->render_data.is_first_time = true;

    (*ptr)->is_selected = false;
    // by default active
    (*ptr)->is_active = true;

    (*ptr)->kind = kind;
    (*ptr)->data = data;
    (*ptr)->free_callback = free_callback;
}

// Generic function to make a {v,h}split node
static void make_split_node(
    gui_view_node_t** ptr, enum view_node_kind split_kind, enum gui_split_type kind, uint8_t parts, va_list values)
{
    JADE_INIT_OUT_PPTR(ptr);
    JADE_ASSERT(split_kind == HSPLIT || split_kind == VSPLIT);

    struct view_node_split_data* data = JADE_CALLOC(1, sizeof(struct view_node_split_data));

    data->kind = kind;
    data->parts = parts;

    // copy the values
    data->values = JADE_CALLOC(1, sizeof(uint16_t) * parts);

    for (uint8_t i = 0; i < parts; ++i) {
        data->values[i] = (uint16_t)va_arg(values, uint32_t);
    };

    // ... and also set a destructor to free them later
    make_view_node(ptr, split_kind, data, free_view_node_split_data);
}

void gui_make_hsplit(gui_view_node_t** ptr, enum gui_split_type kind, uint8_t parts, ...)
{
    JADE_INIT_OUT_PPTR(ptr);

    va_list args;
    va_start(args, parts);
    make_split_node(ptr, HSPLIT, kind, parts, args);
    va_end(args);
}

void gui_make_vsplit(gui_view_node_t** ptr, enum gui_split_type kind, uint8_t parts, ...)
{
    JADE_INIT_OUT_PPTR(ptr);

    va_list args;
    va_start(args, parts);
    make_split_node(ptr, VSPLIT, kind, parts, args);
    va_end(args);
}

void gui_make_button(
    gui_view_node_t** ptr, const color_t color, const color_t selected_color, const uint32_t event_id, void* args)
{
    JADE_INIT_OUT_PPTR(ptr);

    struct view_node_button_data* data = JADE_CALLOC(1, sizeof(struct view_node_button_data));

    // If the un-selected colour is the same as the selected colour, it implies
    // the button is transparent when not selected, so we can skip filling the content.
    // NOTE: requires the parent is redrawn otherwise button will remain in 'selected' appearance
    // when selection moves on to another item.
    data->color = color;
    data->selected_color = selected_color;

    data->click_event_id = event_id;
    data->args = args;

    make_view_node(ptr, BUTTON, data, NULL);
}

void gui_make_fill(gui_view_node_t** ptr, color_t color, enum fill_node_kind fill_type, gui_view_node_t* parent)
{
    JADE_INIT_OUT_PPTR(ptr);

    struct view_node_fill_data* data = JADE_CALLOC(1, sizeof(struct view_node_fill_data));

    // by default same color
    data->color = color;
    data->selected_color = color;
    data->fill_type = fill_type;

    make_view_node(ptr, FILL, data, NULL);
    if (parent) {
        gui_set_parent(*ptr, parent);
    }
}

void gui_make_text(gui_view_node_t** ptr, const char* text, color_t color)
{
    gui_make_text_font(ptr, text, color, GUI_DEFAULT_FONT);
}

void gui_make_text_font(gui_view_node_t** ptr, const char* text, color_t color, uint32_t font)
{
    JADE_INIT_OUT_PPTR(ptr);
    JADE_ASSERT(text);

    struct view_node_text_data* data = JADE_CALLOC(1, sizeof(struct view_node_text_data));

    // max chars limited to GUI_MAX_TEXT_LENGTH
    const size_t len = min_u16(GUI_MAX_TEXT_LENGTH, strlen(text) + 1);
    data->text = JADE_MALLOC(len);
    const int ret = snprintf(data->text, len, "%s", text); // cut to len
    JADE_ASSERT(ret >= 0); // truncation is acceptable here, as is empty string

    // by default same color
    data->color = color;
    data->selected_color = color;

    // default font initially
    data->font = font;

    // and top-left
    data->halign = GUI_ALIGN_LEFT;
    data->valign = GUI_ALIGN_TOP;

    // without scroll
    data->scroll = NULL;

    // without noise
    data->noise = NULL;

    // also set free_view_node_text_data as destructor to free data->text
    make_view_node(ptr, TEXT, data, free_view_node_text_data);
}

void gui_make_icon(gui_view_node_t** ptr, const Icon* icon, color_t color, const color_t* bg_color)
{
    JADE_INIT_OUT_PPTR(ptr);
    JADE_ASSERT(icon);

    struct view_node_icon_data* data = JADE_CALLOC(1, sizeof(struct view_node_icon_data));

    data->icon = *icon;

    // by default same color
    data->color = color;
    data->selected_color = color;

    // background color is set to foreground color to imply transparency
    data->bg_color = bg_color ? *bg_color : color;

    // without animation
    data->animation = NULL;

    // and top-left, normal icon
    data->halign = GUI_ALIGN_LEFT;
    data->valign = GUI_ALIGN_TOP;
    data->icon_type = ICON_PLAIN;

    // also set free_view_node_icon_data as destructor to free any animation data
    make_view_node(ptr, ICON, data, free_view_node_icon_data);
}

void gui_make_qrguide(gui_view_node_t** ptr, color_t color)
{
    JADE_INIT_OUT_PPTR(ptr);

    struct view_node_qrguide_data* data = JADE_CALLOC(1, sizeof(struct view_node_qrguide_data));

    data->color = color;

    make_view_node(ptr, QRGUIDE, data, NULL);
}

static bool icon_animation_frame_callback(gui_view_node_t* node, void* extra_args)
{
    // no node, invalid node, not yet rendered...
    if (!node || node->kind != ICON || node->render_data.is_first_time) {
        return false;
    }

    // animation not applicable
    struct view_node_icon_animation_data* animation_data = node->icon->animation;
    if (!animation_data || !animation_data->frames_per_icon || animation_data->num_icons <= 1) {
        return false;
    }

    if (animation_data->current_frame > 0) {
        // do nothing this frame, just count
        --animation_data->current_frame;
        return false;
    }

    // Update main icon
    animation_data->current_icon = (animation_data->current_icon + 1) % animation_data->num_icons;
    node->icon->icon = animation_data->icons[animation_data->current_icon];

    // Reset frame counter
    animation_data->current_frame = animation_data->frames_per_icon;

    // Redraw icon
    return true;
}

// NOTE: takes ownership of icons
void gui_set_icon_animation(gui_view_node_t* node, Icon* icons, const size_t num_icons, const size_t frames_per_icon)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == ICON);
    JADE_ASSERT(icons);
    JADE_ASSERT(num_icons);
    JADE_ASSERT(frames_per_icon || num_icons == 1);

    struct view_node_icon_animation_data* animation_data = JADE_CALLOC(1, sizeof(struct view_node_icon_animation_data));

    animation_data->icons = icons;
    animation_data->num_icons = num_icons;
    animation_data->current_icon = 0;

    animation_data->frames_per_icon = frames_per_icon;
    animation_data->current_frame = 0;

    node->icon->animation = animation_data;

    // If there are multiple icons, push this to the list of updatable elements so
    // that the image gets periodically updated.
    if (num_icons > 1) {
        push_updatable(node, icon_animation_frame_callback, NULL);
    }
}

void gui_set_icon_to_qr(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    node->icon->icon_type = ICON_QR;
}

void gui_make_picture(gui_view_node_t** ptr, const Picture* picture)
{
    JADE_INIT_OUT_PPTR(ptr);
    // picture optional at creation time

    struct view_node_picture_data* data = JADE_CALLOC(1, sizeof(struct view_node_picture_data));

    data->picture = picture;

    // top-left by default
    data->halign = GUI_ALIGN_LEFT;
    data->valign = GUI_ALIGN_TOP;

    // if the picture node is created without providing a picture then the caller
    // is responsable for freeing the picture data
    make_view_node(ptr, PICTURE, data, picture ? free_view_node_picture_data : NULL);
}

static void set_vals_with_varargs(gui_margin_t* margins, const uint8_t sides, va_list args)
{
    JADE_ASSERT(margins);

    int val;

    switch (sides) {
    case GUI_MARGIN_ALL_EQUAL:
        // we only pop one value
        val = va_arg(args, int);
        JADE_ASSERT(val <= UINT8_MAX);
        margins->top = val;
        margins->right = val;
        margins->bottom = val;
        margins->left = val;
        break;

    case GUI_MARGIN_TWO_VALUES:
        // two values, top/bottom and right/left
        val = va_arg(args, int);
        JADE_ASSERT(val <= UINT8_MAX);
        margins->top = val;
        margins->bottom = val;

        val = va_arg(args, int);
        JADE_ASSERT(val <= UINT8_MAX);
        margins->right = val;
        margins->left = val;
        break;

    case GUI_MARGIN_ALL_DIFFERENT:
        // four different values
        val = va_arg(args, int);
        JADE_ASSERT(val <= UINT8_MAX);
        margins->top = val;

        val = va_arg(args, int);
        JADE_ASSERT(val <= UINT8_MAX);
        margins->right = val;

        val = va_arg(args, int);
        JADE_ASSERT(val <= UINT8_MAX);
        margins->bottom = val;

        val = va_arg(args, int);
        JADE_ASSERT(val <= UINT8_MAX);
        margins->left = val;
        break;

    default:
        JADE_ASSERT_MSG(false, "set_vals_with_varargs() - unexpected 'sides' value: %u", sides);
    }
}

// get the thickness for border "border_bit" (which should have the value of one of the BIT constants)
static inline uint16_t get_border_thickness(gui_border_t* const borders, const uint8_t border_bit)
{
    // thickness is either "border->thickness" if that specific border is enabled or 0
    return borders ? borders->thickness * ((borders->borders >> border_bit) & 1) : 0;
}

static void calc_render_data(gui_view_node_t* node)
{
    JADE_ASSERT(node);

    // constraints haven't been set yet, we can't do much
    if (node->render_data.is_first_time) {
        return;
    }

    dispWin_t constraints = node->render_data.original_constraints;

    // margins affect borders and all contents
    constraints.y1 += node->margins.top;
    constraints.x2 -= node->margins.right;
    constraints.y2 -= node->margins.bottom;
    constraints.x1 += node->margins.left;

    // if we have borders, remove the border thickness
    if (node->borders) {
        constraints.y1 += get_border_thickness(node->borders, GUI_BORDER_TOP_BIT);
        constraints.x2 -= get_border_thickness(node->borders, GUI_BORDER_RIGHT_BIT);
        constraints.y2 -= get_border_thickness(node->borders, GUI_BORDER_BOTTOM_BIT);
        constraints.x1 += get_border_thickness(node->borders, GUI_BORDER_LEFT_BIT);
    }

    // apply padding
    constraints.y1 += node->padding.top;
    constraints.x2 -= node->padding.right;
    constraints.y2 -= node->padding.bottom;
    constraints.x1 += node->padding.left;

    // cache these padded constraints
    node->render_data.padded_constraints = constraints;
}

void gui_set_margins(gui_view_node_t* node, uint32_t sides, ...)
{
    JADE_ASSERT(node);

    va_list args;
    va_start(args, sides);
    set_vals_with_varargs(&node->margins, sides, args);
    va_end(args);

    // update constraints
    calc_render_data(node);
}

void gui_set_padding(gui_view_node_t* node, uint32_t sides, ...)
{
    JADE_ASSERT(node);

    va_list args;
    va_start(args, sides);
    set_vals_with_varargs(&node->padding, sides, args);
    va_end(args);

    // update constraints
    calc_render_data(node);
}

void gui_set_borders(gui_view_node_t* node, const color_t color, const uint16_t thickness, const uint8_t borders)
{
    JADE_ASSERT(node);

    if (!node->borders) {
        node->borders = JADE_MALLOC(sizeof(gui_border_t));
    }
    // by default same color
    node->borders->color = color;
    node->borders->selected_color = color;
    node->borders->inactive_color = color;

    node->borders->thickness = thickness;
    node->borders->borders = borders;

    // update constraints
    calc_render_data(node);
}

void gui_set_borders_selected_color(gui_view_node_t* node, color_t selected_color)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->borders);
    node->borders->selected_color = selected_color;
}

void gui_set_borders_inactive_color(gui_view_node_t* node, color_t inactive_color)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->borders);
    node->borders->inactive_color = inactive_color;
}

void gui_set_colors(gui_view_node_t* node, color_t color, color_t selected_color)
{
    JADE_ASSERT(node);

    switch (node->kind) {
    case TEXT:
        node->text->color = color;
        node->text->selected_color = selected_color;
        break;
    case FILL:
        node->fill->color = color;
        node->fill->selected_color = selected_color;
        break;
    case BUTTON:
        node->button->color = color;
        node->button->selected_color = selected_color;
        break;
    case ICON:
        node->icon->color = color;
        node->icon->selected_color = selected_color;
        break;
    default:
        JADE_ASSERT_MSG(false, "gui_set_colors() - Unexpected node kind: %u", node->kind);
    }
}

void gui_set_color(gui_view_node_t* node, color_t color) { gui_set_colors(node, color, color); }

void gui_set_align(gui_view_node_t* node, enum gui_horizontal_align halign, enum gui_vertical_align valign)
{
    JADE_ASSERT(node);

    enum gui_horizontal_align* halign_ptr;
    enum gui_vertical_align* valign_ptr;
    switch (node->kind) {
    case TEXT:
        halign_ptr = &node->text->halign;
        valign_ptr = &node->text->valign;
        break;
    case ICON:
        halign_ptr = &node->icon->halign;
        valign_ptr = &node->icon->valign;
        break;
    case PICTURE:
        halign_ptr = &node->picture->halign;
        valign_ptr = &node->picture->valign;
        break;

    default:
        JADE_ASSERT_MSG(false, "gui_set_align() - Unexpected node kind: %u", node->kind);
    }

    *halign_ptr = halign;
    *valign_ptr = valign;
}

static inline bool can_text_fit(const char* text, uint32_t font, dispWin_t cs)
{
    JADE_ASSERT(text);

    display_set_font(font); // measure relative to this font
    return display_get_string_width(text) <= cs.x2 - cs.x1;
}

// move to the next frame of a scrolling text node
static bool text_scroll_frame_callback(gui_view_node_t* node, void* extra_args)
{
    // no node, invalid node, not yet rendered...
    if (!node || node->kind != TEXT || node->render_data.is_first_time) {
        return false;
    }

    if (!node->text->text) {
        return false; // Empty string
    }

    // check if scrolling is only enabled when the item is selected, and node
    // NOT currently selected - if so redraw at 'start' position
    if (!node->is_selected && node->text->scroll->only_when_selected) {
        // if text already at start position, just exit, nothing to do
        if (!node->text->scroll->going_back && !node->text->scroll->offset) {
            return false;
        }

        // set text to start position and return true so item repainted
        node->text->scroll->prev_offset = node->text->scroll->offset;
        node->text->scroll->going_back = false;
        node->text->scroll->offset = 0;
        node->text->scroll->wait = GUI_SCROLL_WAIT_END;
        return true;
    }

    // do nothing this frame
    if (node->text->scroll->wait > 0) {
        node->text->scroll->wait--;
        return false;
    }

    // the string can fit entirely in its box, no need to scroll. we might need to reset stuff though, if the text has
    // changed
    if (can_text_fit(node->text->text, node->text->font, node->render_data.padded_constraints)) {
        const size_t old_offset = node->text->scroll->offset;

        // set offset to zero and wait a little before checking again
        node->text->scroll->going_back = false;
        node->text->scroll->offset = 0;
        node->text->scroll->wait = GUI_SCROLL_WAIT_END;

        // only repaint on screen if the offset was not zero
        return old_offset != 0;
    }

    // update the offset based on the direction
    const size_t text_length = strlen(node->text->text);
    node->text->scroll->prev_offset = node->text->scroll->offset;
    if (node->text->scroll->going_back) {
        JADE_ASSERT(node->text->scroll->offset > 0); // we should "catch" this before and set going_back to false
        node->text->scroll->offset--;
    } else if (node->text->scroll->offset <= text_length - 1) {
        // never go out of bounds with the offset
        node->text->scroll->offset++;
    }

    // since we scrolled this frame, wait some frames before doing the next one
    node->text->scroll->wait = GUI_SCROLL_WAIT_FRAME;

    // check if we are done going forward
    if (!node->text->scroll->going_back) {
        bool can_fit = can_text_fit(
            node->text->text + node->text->scroll->offset, node->text->font, node->render_data.padded_constraints);
        bool end_of_string = node->text->scroll->offset == text_length - 1;

        // done, let's go back. we can fit OR we reached the end of the string
        if (can_fit || end_of_string) {
            node->text->scroll->going_back = true;
            node->text->scroll->wait = GUI_SCROLL_WAIT_END;
        }
    }

    // start again
    if (node->text->scroll->going_back && node->text->scroll->offset == 0) {
        node->text->scroll->going_back = false;
        node->text->scroll->wait = GUI_SCROLL_WAIT_END;
    }

    // repaint on screen
    return true;
}

void gui_set_text_scroll(gui_view_node_t* node, color_t background_color)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == TEXT);
    JADE_ASSERT(!node->text->scroll); // the node is not already scrolling...
    JADE_ASSERT(!node->text->noise); // if the node has noise added we will not allow scrolling ...

    struct view_node_text_scroll_data* scroll_data = JADE_CALLOC(1, sizeof(struct view_node_text_scroll_data));

    // wait a little before it starts moving
    scroll_data->offset = 0;
    scroll_data->wait = GUI_SCROLL_WAIT_END;
    scroll_data->background_color = background_color;
    scroll_data->selected_background_color = background_color;

    node->text->scroll = scroll_data;

    // now push this to the list of updatable elements so that it gets updated every frame
    push_updatable(node, text_scroll_frame_callback, NULL);
}

void gui_set_text_scroll_selected(
    gui_view_node_t* node, bool only_when_selected, color_t background_color, color_t selected_background_color)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == TEXT);

    gui_set_text_scroll(node, background_color);

    node->text->scroll->only_when_selected = only_when_selected;
    node->text->scroll->background_color = background_color;
    node->text->scroll->selected_background_color = selected_background_color;
}

void gui_set_text_noise(gui_view_node_t* node, color_t background_color)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == TEXT);
    JADE_ASSERT(!node->text->scroll); // if the node is scrolling we will not allow adding noise ...

    struct view_node_text_noise_data* noise_data = JADE_MALLOC(sizeof(struct view_node_text_noise_data));
    noise_data->background_color = background_color;

    node->text->noise = noise_data;
}

void gui_set_text_font(gui_view_node_t* node, uint32_t font)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == TEXT);

    // TODO: "validate" the font?
    node->text->font = font;
}

void gui_set_text_default_font(gui_view_node_t* node) { gui_set_text_font(node, GUI_DEFAULT_FONT); }

// Helper function to just update the text node internal text data - does not repaint,
// so several nodes can be updated then a single repaint issued - eg. the status bar
static void update_text_node_text(gui_view_node_t* node, const char* text)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == TEXT);
    JADE_ASSERT(text);

    // max chars limited to GUI_MAX_TEXT_LENGTH
    const size_t len = min_u16(GUI_MAX_TEXT_LENGTH, strlen(text) + 1);
    char* new_text = JADE_MALLOC(len);
    const int ret = snprintf(new_text, len, "%s", text);
    JADE_ASSERT(ret >= 0); // truncation is acceptable here, as is empty string

    // free the old text node and replace with the new pointer
    wally_free_string(node->text->text);
    node->text->text = new_text;
}

// Takes the gui_mutex, updates the text node, and then only draws the
// updated item if it is part of the 'current activity'.
void gui_update_text(gui_view_node_t* node, const char* text)
{
    JADE_ASSERT(node);
    JADE_ASSERT(text);

    // Get the activity mutex, update the text node text and
    // if part of current activity release the mutex and post
    // a message to the gui task to repaint it.
    JADE_SEMAPHORE_TAKE(gui_mutex);
    update_text_node_text(node, text);
    const bool repaint = current_activity && node->activity == current_activity;
    JADE_SEMAPHORE_GIVE(gui_mutex);

    if (repaint) {
        // repaint the parent (so that the old string is cleared). Usually a parent should
        // be present, because it's unlikely that a root node is of type "text"
        if (node->parent) {
            gui_repaint(node->parent);
        } else {
            gui_repaint(node);
        }
    }
}

// Takes the gui_mutex, updates the icon, and then only draws the
// updated item if it is part of the 'current activity'.
void gui_update_icon(gui_view_node_t* node, const Icon icon, const bool repaint_parent)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == ICON);
    JADE_ASSERT(!node->icon->animation); // animated

    // Get the activity mutex, update the icon data and
    // if part of current activity release the mutex and post
    // a message to the gui task to repaint it.
    JADE_SEMAPHORE_TAKE(gui_mutex);
    node->icon->icon = icon;
    const bool repaint = current_activity && node->activity == current_activity;
    JADE_SEMAPHORE_GIVE(gui_mutex);

    if (repaint) {
        // Maybe repaint the parent (so that the old icon is cleared). Usually a parent should
        // be present, because it's unlikely that a root node is of type "icon"
        if (repaint_parent && node->parent) {
            // Redraw parent (ie. background), then children
            gui_repaint(node->parent);
        } else {
            // Simply redraw over the top - eg. if icon same size or larger and not transparent
            gui_repaint(node);
        }
    }
}

// Takes the gui_mutex, updates the picture, and then only draws the
// updated item if it is part of the 'current activity'.
// picture may be null to remove a picture so that the caller can free it.
void gui_update_picture(gui_view_node_t* node, const Picture* picture, const bool repaint_parent)
{
    JADE_ASSERT(node && node->kind == PICTURE);

    // Get the activity mutex, update the picture data and
    // if part of current activity release the mutex and post
    // a message to the gui task to repaint it.
    JADE_SEMAPHORE_TAKE(gui_mutex);
    node->picture->picture = picture;
    const bool repaint = picture && current_activity && node->activity == current_activity;
    JADE_SEMAPHORE_GIVE(gui_mutex);

    // If part of current activity, draw it immediately
    if (repaint) {
        // Maybe repaint the parent (so that the old picture is cleared). Usually a parent should
        // be present, because it's unlikely that a root node is of type "picture"
        if (repaint_parent && node->parent) {
            // Redraw parent (ie. background), then children
            gui_repaint(node->parent);
        } else {
            // Simply redraw over the top - eg. if picture same size or larger
            gui_repaint(node);
        }
    }
}

// get the "step" based on the width of the parent element, our value and the type of split
static inline uint16_t get_step(enum gui_split_type kind, uint16_t total, uint16_t value)
{
    switch (kind) {
    case GUI_SPLIT_ABSOLUTE:
        return value;
        break;

    case GUI_SPLIT_RELATIVE:
        return total * value / 100;
        break;
    }

    return 0;
}

// Re-calculate node constraints, push elements to the selectables list, etc
static void pre_render_node(gui_view_node_t* node, const dispWin_t* const cs)
{
    JADE_ASSERT(node);

    if (node->render_data.is_first_time) {
        // now that we know the coordinates of this node we can push it to the list of selectable elements
        if (is_kind_selectable(node->kind)) {
            push_selectable(node->activity, node, cs->x1, cs->y1);
        }
        node->render_data.is_first_time = false;
    }

    // remember the original constrains, we will calculate the others based on those
    node->render_data.original_constraints = *cs;
    calc_render_data(node);
    // node is now ready for repainting
}

// pre-render a node to the given constraints then paint it on-screen
// always-inline in order to avoid pushing a stack frame when recursing
static inline __attribute__((always_inline)) void render_node(gui_view_node_t* node, const dispWin_t* const cs)
{
    pre_render_node(node, cs);
    repaint_node(node); // actually paint the node on-screen
}

static void render_button(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == BUTTON);

    const dispWin_t* const cs = &node->render_data.padded_constraints;

    // If the un-selected colour is the same as the selected colour, it implies
    // the button is transparent when not selected, so we can skip filling the content.
    // NOTE: requires the parent is redrawn otherwise button will remain in 'selected' appearance
    // when selection moves on to another item.
    if (node->is_selected || node->button->color != node->button->selected_color) {
        display_fill_rect(cs->x1, cs->y1, cs->x2 - cs->x1, cs->y2 - cs->y1,
            node->is_selected ? node->button->selected_color : node->button->color);
    }

    // Draw any children directly over the current node
    if (node->child) {
        render_node(node->child, cs);
    }
}

static void render_vsplit(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == VSPLIT);

    const dispWin_t* const cs = &node->render_data.padded_constraints;

    uint16_t count = 0;
    uint16_t y = cs->y1;
    const uint16_t max_y = cs->y2;
    const uint16_t width = max_y - y;

    // Draw children in the divided area parts
    gui_view_node_t* ptr = node->child;
    while (ptr && count < node->split->parts) {
        uint16_t step;

        if (node->split->values[count] == GUI_SPLIT_FILL_REMAINING) {
            step = max_y - y;
        } else {
            step = get_step(node->split->kind, width, node->split->values[count]);
        }

        {
            // Pre-render the node explicitly to reduce stack usage
            const dispWin_t child_cs = {
                .x1 = cs->x1,
                .x2 = cs->x2,
                .y1 = y,
                .y2 = min_u16(y + step, max_y),
            };
            pre_render_node(ptr, &child_cs);
            y = child_cs.y2;
        }
        repaint_node(ptr); // actually paint the node on-screen

        ++count;
        ptr = ptr->sibling;
    }
}

static void render_hsplit(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == HSPLIT);

    const dispWin_t* const cs = &node->render_data.padded_constraints;

    uint16_t count = 0;
    uint16_t x = cs->x1;
    const uint16_t max_x = cs->x2;
    const uint16_t width = max_x - x;

    // Draw children in the divided area parts
    gui_view_node_t* ptr = node->child;
    while (ptr && count < node->split->parts) {
        uint16_t step;
        if (node->split->values[count] == GUI_SPLIT_FILL_REMAINING) {
            step = max_x - x;
        } else {
            step = get_step(node->split->kind, width, node->split->values[count]);
        }

        {
            // Pre-render the node explicitly to reduce stack usage
            const dispWin_t child_cs = { .x1 = x, .x2 = min_u16(x + step, max_x), .y1 = cs->y1, .y2 = cs->y2 };
            pre_render_node(ptr, &child_cs);
            x = child_cs.x2;
        }
        repaint_node(ptr); // actually paint the node on-screen

        ++count;
        ptr = ptr->sibling;
    }
}

static void render_fill(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == FILL);

    color_t color;
    if (node->fill->fill_type == FILL_PLAIN) {
        color = node->is_selected ? node->fill->selected_color : node->fill->color;
    } else if (node->fill->fill_type == FILL_HIGHLIGHT) {
        color = gui_get_highlight_color();
    } else if (node->fill->fill_type == FILL_QR) {
        color = gui_get_qrcode_color();
    } else {
        JADE_ASSERT(false); // Unknown fill type
    }

    const dispWin_t* const cs = &node->render_data.padded_constraints;
    display_fill_rect(cs->x1, cs->y1, cs->x2 - cs->x1, cs->y2 - cs->y1, color);

    // Draw any children directly over the current node
    if (node->child) {
        render_node(node->child, cs);
    }
}

static inline int resolve_halign(int x, enum gui_horizontal_align halign)
{
    switch (halign) {
    case GUI_ALIGN_LEFT:
        return 0;
    case GUI_ALIGN_CENTER:
        return CENTER;
    case GUI_ALIGN_RIGHT:
        return RIGHT;
    }

    // no modifiers
    return x;
}

static inline int resolve_valign(int y, enum gui_vertical_align valign)
{
    switch (valign) {
    case GUI_ALIGN_TOP:
        return 0;
    case GUI_ALIGN_MIDDLE:
        return CENTER;
    case GUI_ALIGN_BOTTOM:
        return BOTTOM;
    }

    // no modifiers
    return y;
}

// render a text node to screen in the window constrained by cs
static void render_text(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == TEXT);

    const dispWin_t* const cs = &node->render_data.padded_constraints;

    display_set_font(node->text->font);

    if (node->text->scroll) {
        // this text has the scroll enable, so disable wrap

        // set the foreground color to the "background color" to remove the previous string
        _fg = node->is_selected ? node->text->scroll->selected_background_color : node->text->scroll->background_color;
        display_print_in_area(node->text->text + node->text->scroll->prev_offset, resolve_halign(0, node->text->halign),
            resolve_valign(0, node->text->valign), cs, 0);

        // and now we write the new one using the correct color
        _fg = node->is_selected ? node->text->selected_color : node->text->color;
        display_print_in_area(node->text->text + node->text->scroll->offset, resolve_halign(0, node->text->halign),
            resolve_valign(0, node->text->valign), cs, 0);

    } else {
        // normal print with wrap
        if (node->text->noise) { // with noise
            const color_t color = node->is_selected ? node->text->selected_color : node->text->color;

            int pos_x = 0;
            switch (node->text->halign) {
            case GUI_ALIGN_LEFT:
                pos_x = 0;
                break;
            case GUI_ALIGN_CENTER:
                pos_x = (cs->x2 - cs->x1 - display_get_string_width(node->text->text)) / 2;
                break;
            case GUI_ALIGN_RIGHT:
                pos_x = cs->x2 - cs->x1 - display_get_string_width(node->text->text);
                break;
            }

            const int pos_y = resolve_valign(0, node->text->valign);

            const size_t text_length = strlen(node->text->text);
            uint16_t offset_x = 0;
            uint16_t offset_y = 0;
            char buf[2] = { '\0', '\0' };
            for (size_t i = 0; i < text_length; ++i) {
                buf[0] = node->text->text[i];
                const int char_width = display_get_string_width(buf);
                if (pos_x + offset_x + char_width >= cs->x2 - cs->x1) {
                    offset_y += display_get_font_height();
                    offset_x = 0;
                }

                _fg = node->text->noise->background_color;
                buf[0] = 0x61 + get_uniform_random_byte(0x7a - 0x61);
                display_print_in_area(buf, pos_x + offset_x, pos_y + offset_y, cs, 1);
                _fg = color;
                buf[0] = node->text->text[i];
                display_print_in_area(buf, pos_x + offset_x, pos_y + offset_y, cs, 1);
                offset_x += char_width;
            }
        } else { // without noise
            _fg = node->is_selected ? node->text->selected_color : node->text->color;

            display_print_in_area(
                node->text->text, resolve_halign(0, node->text->halign), resolve_valign(0, node->text->valign), cs, 1);
        }
    }
}

// render an icon to screen
static void render_icon(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == ICON);

    const dispWin_t* const cs = &node->render_data.padded_constraints;

    if (node->icon) {
        color_t color, bg_color;
        if (node->icon->icon_type == ICON_PLAIN) {
            color = node->is_selected ? node->icon->selected_color : node->icon->color;
            bg_color = node->icon->bg_color;
        } else if (node->icon->icon_type == ICON_QR) {
            color = node->is_selected ? node->icon->selected_color : node->icon->color;
            bg_color = gui_get_qrcode_color();
        } else {
            JADE_ASSERT(false); // Unknown fill type
        }

        const bool transparent = bg_color == color;
        display_icon(&node->icon->icon, resolve_halign(0, node->icon->halign), resolve_valign(0, node->icon->valign),
            color, cs, transparent ? NULL : &bg_color);
    }

    // Draw any children directly over the current node
    if (node->child) {
        render_node(node->child, cs);
    }
}

// render a picture to screen
static void render_picture(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == PICTURE);

    const dispWin_t* const cs = &node->render_data.padded_constraints;

    if (node->picture && node->picture->picture) {
        display_picture(node->picture->picture, resolve_halign(0, node->picture->halign),
            resolve_valign(0, node->picture->valign), cs);
    }

    // Draw any children directly over the current node
    if (node->child) {
        render_node(node->child, cs);
    }
}

// render a qrguide to screen
static void render_qrguide(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->kind == QRGUIDE);

    const dispWin_t* const cs = &node->render_data.padded_constraints;

    // guide dimensions
    const uint16_t gwidth = 2;
    const uint16_t glength = 30;
    const uint16_t gnubbin = 2;

    // maximum square that fits in the constraints
    const uint16_t width = cs->x2 - cs->x1;
    const uint16_t height = cs->y2 - cs->y1;
    const uint16_t square_size = min_u16(width, height);
#if defined(CONFIG_BOARD_TYPE_JADE_V1_ANY)
    // guides 9% inset
    const uint16_t inset = square_size / 11;
#else
    // guides 3% inset
    const uint16_t inset = square_size / 30;
#endif
    // guide boundaries
    const uint16_t left = cs->x1 + (width - square_size) / 2 + inset;
    const uint16_t right = cs->x2 - (width - square_size) / 2 - inset;
    const uint16_t top = cs->y1 + (height - square_size) / 2 + inset;
    const uint16_t bottom = cs->y2 - (height - square_size) / 2 - inset;
    // top-left
    display_fill_rect(left, top, gwidth, glength, node->qrguide->color);
    display_fill_rect(left, top, glength, gwidth, node->qrguide->color);
    display_fill_rect(left + glength, top, gnubbin, gwidth / 2, node->qrguide->color);
    display_fill_rect(left, top + glength, gwidth / 2, gnubbin, node->qrguide->color);
    // top-right
    display_fill_rect(right - gwidth, top, gwidth, glength, node->qrguide->color);
    display_fill_rect(right - glength, top, glength, gwidth, node->qrguide->color);
    display_fill_rect(right - glength - gnubbin, top, gnubbin, gwidth / 2, node->qrguide->color);
    display_fill_rect(right - gwidth / 2, top + glength, gwidth / 2, gnubbin, node->qrguide->color);
    // bottom-left
    display_fill_rect(left, bottom - glength, gwidth, glength, node->qrguide->color);
    display_fill_rect(left, bottom - gwidth, glength, gwidth, node->qrguide->color);
    display_fill_rect(left + glength, bottom - gwidth / 2, gnubbin, gwidth / 2, node->qrguide->color);
    display_fill_rect(left, bottom - glength - gnubbin, gwidth / 2, gnubbin, node->qrguide->color);
    // bottom-right
    display_fill_rect(right - gwidth, bottom - glength, gwidth, glength, node->qrguide->color);
    display_fill_rect(right - glength, bottom - gwidth, glength, gwidth, node->qrguide->color);
    display_fill_rect(right - glength - gnubbin, bottom - gwidth / 2, gnubbin, gwidth / 2, node->qrguide->color);
    display_fill_rect(right - gwidth / 2, bottom - glength - gnubbin, gwidth / 2, gnubbin, node->qrguide->color);

    // Draw any children directly over the current node
    if (node->child) {
        render_node(node->child, cs);
    }
}

// paint the borders for a view_node
static void paint_borders(gui_view_node_t* node, const dispWin_t* const cs)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->borders);

    const uint16_t width = cs->x2 - cs->x1;
    const uint16_t height = cs->y2 - cs->y1;

    color_t* color = NULL;
    if (node->is_selected) {
        color = &node->borders->selected_color;
    } else if (!node->is_active) {
        color = &node->borders->inactive_color;
    } else {
        color = &node->borders->color;
    }

    JADE_ASSERT(color);

    uint16_t thickness;

    if ((thickness = get_border_thickness(node->borders, GUI_BORDER_TOP_BIT))) {
        display_fill_rect(cs->x1, cs->y1, width, thickness, *color); // top
    }
    if ((thickness = get_border_thickness(node->borders, GUI_BORDER_RIGHT_BIT))) {
        display_fill_rect(cs->x2 - thickness, cs->y1, thickness, height, *color); // right
    }
    if ((thickness = get_border_thickness(node->borders, GUI_BORDER_BOTTOM_BIT))) {
        display_fill_rect(cs->x1, cs->y2 - thickness, width, thickness, *color); // bottom
    }
    if ((thickness = get_border_thickness(node->borders, GUI_BORDER_LEFT_BIT))) {
        display_fill_rect(cs->x1, cs->y1, thickness, height, *color); // left
    }
}

// Actually repaint a node on the display - calls underlying display library
static void repaint_node(gui_view_node_t* node)
{
    JADE_ASSERT(node);

    // Ensure we only call the underlying display library from the gui_task
    JADE_ASSERT_MSG(gui_is_gui_task(), "ERROR: repaint_node() called from non-gui-task: %s", pcTaskGetName(NULL));

    // borders use the un-padded constraints
    if (node->borders) {
        dispWin_t cs = node->render_data.original_constraints;

        // margins affect borders
        cs.y1 += node->margins.top;
        cs.x2 -= node->margins.right;
        cs.y2 -= node->margins.bottom;
        cs.x1 += node->margins.left;

        paint_borders(node, &cs);
    }

    switch (node->kind) {
    case HSPLIT:
        render_hsplit(node);
        break;
    case VSPLIT:
        render_vsplit(node);
        break;
    case TEXT:
        render_text(node);
        break;
    case FILL:
        render_fill(node);
        break;
    case BUTTON:
        render_button(node);
        break;
    case ICON:
        render_icon(node);
        break;
    case PICTURE:
        render_picture(node);
        break;
    case QRGUIDE:
        render_qrguide(node);
        break;
    }
}

static void render_activity(gui_activity_t* activity)
{
    JADE_ASSERT(activity);
    JADE_ASSERT(activity->root_node);

    const bool first_time = activity->root_node->render_data.is_first_time;
    render_node(activity->root_node, &activity->win);

    if (first_time && activity->selectables) {
        // If the activity has an 'initial_selection' and it appears active, select it now
        // If not, select the first active item
        if (activity->initial_selection && activity->initial_selection->is_active) {
            JADE_ASSERT(activity->initial_selection->activity == activity);
            select_node(activity->initial_selection);
        } else {
            gui_view_node_t* const node = get_first_active_node(activity);
            if (node) {
                JADE_ASSERT(node->activity == activity);
                select_node(node);
            }
        }
    }
}

static void free_activities(activity_holder_t* to_free)
{
    while (to_free) {
        JADE_ASSERT(&to_free->activity != current_activity);
        activity_holder_t* const next = to_free->next;
        free_managed_activity(to_free);
        to_free = next;
    }
}

// update the status bar
static bool update_status_bar(const bool force_redraw)
{
    // No-op if no status bar
    if (!current_activity || !current_activity->status_bar) {
        return false;
    }

    bool updated = false;

    // NOTE: we use the internal 'update_text_node_text()' method here
    // since we don't want to redraw each update individually, but rather
    // capture in a single repaint after all nodes are updated.
    if ((status_bar.battery_update_counter % 10) == 0) {
#ifdef CONFIG_BT_ENABLED
        const bool new_ble = ble_enabled();
#else
        const bool new_ble = false;
#endif

        if (new_ble != status_bar.last_ble_val) {
            status_bar.last_ble_val = new_ble;
            if (new_ble) {
                update_text_node_text(status_bar.ble_text, (char[]){ 'E', '\0' });
            } else {
                update_text_node_text(status_bar.ble_text, (char[]){ 'F', '\0' });
            }
            status_bar.updated = true;
        }

        const bool new_usb = usb_is_powered();
        if (new_usb != status_bar.last_usb_val) {
            status_bar.last_usb_val = new_usb;
            if (new_usb) {
                update_text_node_text(status_bar.usb_text, (char[]){ 'C', '\0' });
                // FIXME: change to charging rather than usb connected
                // serial_start();
            } else {
                update_text_node_text(status_bar.usb_text, (char[]){ 'D', '\0' });
                // FIXME: change to no power rather than usb connected
                // serial_stop();
            }
            status_bar.updated = true;
            status_bar.battery_update_counter = 0; // Force battery icon update
        }
    }

    if (status_bar.battery_update_counter == 0) {
        uint8_t new_bat = power_get_battery_status();
#ifdef CONFIG_HAS_BATTERY
        color_t color = new_bat == 0 ? TFT_RED : new_bat == 1 ? TFT_ORANGE : TFT_WHITE;
#else
        // If no battery on the device then hide the battery icon with background color
        color_t color = status_bar.root->fill->color;
#endif
        if (power_get_battery_charging()) {
            new_bat = new_bat + 12;
        }
        if (new_bat != status_bar.last_battery_val) {
            status_bar.last_battery_val = new_bat;
            gui_set_color(status_bar.battery_text, color);
            update_text_node_text(status_bar.battery_text, (char[]){ new_bat + '0', '\0' });
            status_bar.updated = true;
        }
        status_bar.battery_update_counter = 60;
    }

    status_bar.battery_update_counter--;

    if (status_bar.updated || force_redraw) {
        {
            // Pre-render the status bar explicitly to reduce stack usage
            dispWin_t status_bar_cs = GUI_DISPLAY_WINDOW;
            status_bar_cs.y2 = status_bar_cs.y1 + GUI_STATUS_BAR_HEIGHT;
            pre_render_node(status_bar.root, &status_bar_cs);
        }
        repaint_node(status_bar.root); // actually paint the status bar on-screen
        status_bar.updated = false;
        updated = true;
    }

    return updated;
}

// Process queue of jobs - always drain entire queue
static size_t handle_gui_input_queue(bool* switched_activities)
{
    JADE_ASSERT(switched_activities);
    JADE_ASSERT(gui_input_queue);

    size_t jobs_handled = 0;
    *switched_activities = false;

    gui_task_job_t* job = NULL;
    size_t item_size = 0;

    while ((job = xRingbufferReceive(gui_input_queue, &item_size, 10 / portTICK_PERIOD_MS))) {
        JADE_ASSERT(item_size == sizeof(gui_task_job_t));

        // A job can be be ONE of the following:
        // - repainting a node, OR
        // - moving to another activity and optionally freeing other activities
        activity_holder_t* to_free = job->to_free;

        if (job->node_to_repaint) {
            // Repaint job
            JADE_ASSERT(!job->new_activity && !job->to_free);
            if (job->node_to_repaint->activity == current_activity) {
                // Node belongs to the current activity: repaint it
                repaint_node(job->node_to_repaint);
            }
        } else if (!job->new_activity) {
            JADE_ASSERT(false); // Not a repaint or a move to new activity job
        } else if (job->new_activity != current_activity) {
            *switched_activities = true;

            // Unregister the old activity's event handlers
            if (current_activity) {
                activity_event_t* l = current_activity->activity_events;
                while (l) {
                    esp_event_handler_instance_unregister(l->event_base, l->event_id, l->instance);
                    l->instance = NULL;
                    l = l->next;
                }
            }

            // BBB-AIRGAP: announce the swap BEFORE changing the input target. A pause between
            // these operations now rejects conservatively instead of approving the old screen's
            // generation while a click already targets the new one. Acquire/release keeps the
            // following assignment after this announcement; rendering and flushing follow both.
            atomic_fetch_add_explicit(&activity_generation, 1, memory_order_acq_rel);
            current_activity = job->new_activity;

            // If passed a 'to_free' list, free these activities now.
            // This does not really need to be protected by the semaphore - however we want to
            // free the old activities *before* the code below runs, as it makes allocations.
            // If we defer the 'frees' until later, we end up fragmenting the memory, which is
            // particularly detrimental to no-psram devices.
            free_activities(to_free);
            to_free = NULL;

            // Update the status bar text for the new activity
            if (current_activity->status_bar) {
                const bool force_redraw = true;
                update_text_node_text(status_bar.title, current_activity->title ? current_activity->title : "");
                update_status_bar(force_redraw);
            }

            // Draw the new activity
            render_activity(current_activity);

            // Register new events
            activity_event_t* l = current_activity->activity_events;
            while (l) {
                JADE_ASSERT(!l->instance);
                esp_event_handler_instance_register(l->event_base, l->event_id, l->handler, l->args, &(l->instance));
                l = l->next;
            }
        }

        // Save done semaphore before returning the ringbuffer slot
        SemaphoreHandle_t done = job->done;

        // Return the ringbuffer slot
        vRingbufferReturnItem(gui_input_queue, job);

        // Free any outstanding activities, if required (and not already done)
        free_activities(to_free);

        // Signal completion if a semaphore was provided
        if (done) {
            xSemaphoreGive(done);
        }

        // Count jobs handled so we can return
        ++jobs_handled;
    }

    return jobs_handled;
}

// updatables task, this task runs to update elements in the `updatables` list of the current activity
static bool update_updateables(void)
{
    if (!current_activity) {
        return false;
    }

    bool updated = false;

    updatable_t* current = current_activity->updatables;
    while (current) {
        // this shouldn't really happen but better add a check anyways
        if (!current->callback) {
            continue;
        }

        // let's see if we need to repaint this
        bool result = current->callback(current->node, current->extra_args);
        if (result) {
            // repaint the node on-screen
            // TODO: we are ignoring the return code here...
            repaint_node(current->node);
            updated = true;
        }
        current = current->next;
    }
    return updated;
}

// gui task, for managing display/activities
static void gui_task(void* args)
{
    // Set the global handle for this task
    gui_task_handle = xTaskGetCurrentTaskHandle();

    // Flush/clear display as soon as we're able
    JADE_SEMAPHORE_TAKE(gui_mutex);
    display_flush();
    JADE_SEMAPHORE_GIVE(gui_mutex);

    // Loop to periodically handle gui events
    const TickType_t period = 1000 / GUI_TARGET_FRAMERATE / portTICK_PERIOD_MS;
    TickType_t last_wake = xTaskGetTickCount();

    gui_task_should_run = true;
    gui_task_running = true;
    while (gui_task_should_run) {

        // Wait for the next frame
        // Note: this task is never suspended, so no need to re-fetch the tick-
        // time each loop, just let vTaskDelayUntil() track the 'last_wake' count.
        vTaskDelayUntil(&last_wake, period);

        // Take the gui semaphore while the gui task is awake
        JADE_SEMAPHORE_TAKE(gui_mutex);

        // Check the input queue - repaint node or set new activity if need be
        // Note: this can also free all the old/completed activities
        bool switched_activities = false;
        const size_t jobs_handled = handle_gui_input_queue(&switched_activities);
        if (jobs_handled) {
            // BBB-AIRGAP: published before the flush below, so the frame that flush produces is
            // seen by the host as carrying these jobs.  See gui_jobs_drained.
            atomic_fetch_add_explicit(&gui_jobs_drained, (uint32_t)jobs_handled, memory_order_relaxed);
        }
        if (jobs_handled > 4) {
            JADE_LOGW("gui task handled %u jobs", jobs_handled);
        }

        bool updated = true;
        if (!switched_activities) {
            // Not switching activities, update any 'updatable' gui elements on this activity
            updated = update_updateables();
        }

        // Update status bar if required
        const bool force_redraw = false;
        if (update_status_bar(force_redraw) || updated || jobs_handled) {
            // Flush
            display_flush();
        }

        JADE_SEMAPHORE_GIVE(gui_mutex);
    }

#ifdef CONFIG_LIBJADE
    // gui task is exiting - only happens for libjade.
    // Free all activities
    current_activity = NULL;
    free_activities(existing_activities);
    existing_activities = NULL;

    // Delete the main input queue
    if (gui_input_queue) {
        vRingbufferDelete(gui_input_queue);
        gui_input_queue = NULL;
    }

    // BBB-AIRGAP: release the producer publication mutex with the GUI queue it protects.
    if (gui_post_mutex) {
        vSemaphoreDelete(gui_post_mutex);
        gui_post_mutex = NULL;
    }

    // Delete the mutex semaphore
    if (gui_mutex) {
        vSemaphoreDelete(gui_mutex);
        gui_mutex = NULL;
    }

    // Clear handle and running flag.
    gui_task_handle = NULL;
    gui_task_running = false;
    vTaskDelete(NULL);
#endif // CONFIG_LIBJADE
}

// TODO: different functions for different types of click
void gui_wheel_click(void)
{
    gui_escape_clear(); // BBB-AIRGAP: any other press means the user changed their mind
    if (!idletimer_register_activity(true)) {
        if (gui_click_event == GUI_WHEEL_CLICK_EVENT) {
            select_action(current_activity);
        }
        esp_event_post(GUI_EVENT, GUI_WHEEL_CLICK_EVENT, NULL, 0, 50 / portTICK_PERIOD_MS);
    }
}

void gui_front_click(void)
{
    gui_escape_clear(); // BBB-AIRGAP: any other press means the user changed their mind
    if (!idletimer_register_activity(true)) {
        if (gui_click_event == GUI_FRONT_CLICK_EVENT) {
            select_action(current_activity);
        }
        esp_event_post(GUI_EVENT, GUI_FRONT_CLICK_EVENT, NULL, 0, 50 / portTICK_PERIOD_MS);
    }
}

void select_next_right(void)
{
    if (!idletimer_register_activity(true)) {
        select_next(current_activity);
        esp_event_post(GUI_EVENT, GUI_WHEEL_RIGHT_EVENT, NULL, 0, 50 / portTICK_PERIOD_MS);
    }
}

void select_prev_left(void)
{
    if (!idletimer_register_activity(true)) {
        select_prev(current_activity);
        esp_event_post(GUI_EVENT, GUI_WHEEL_LEFT_EVENT, NULL, 0, 50 / portTICK_PERIOD_MS);
    }
}

// BBB-AIRGAP: Select the closest active item on the next row along the requested vertical axis.
static bool select_vertical(gui_activity_t* const activity, const bool down)
{
    if (!activity || !activity->selectables) {
        return false;
    }

    selectable_t* const begin = activity->selectables;
    selectable_t* current = begin;
    selectable_t* selected = NULL;
    do {
        if (current->node->is_selected) {
            selected = current;
            break;
        }
        current = current->next;
    } while (current != begin);
    if (!selected) {
        return false;
    }

    bool target_row_found = false;
    uint16_t target_y = 0;
    current = begin;
    do {
        if (current->node->is_active && !current->node->nav_skip
            && ((down && current->y > selected->y) || (!down && current->y < selected->y))
            && (!target_row_found || (down ? current->y < target_y : current->y > target_y))) {
            target_y = current->y;
            target_row_found = true;
        }
        current = current->next;
    } while (current != begin);
    if (!target_row_found) {
        return false;
    }

    selectable_t* list_begin = begin;
    current = begin;
    do {
        if (current->is_first) {
            list_begin = current;
            break;
        }
        current = current->next;
    } while (current != begin);

    selectable_t* target = NULL;
    uint16_t target_distance = 0;
    current = list_begin;
    do {
        if (current->node->is_active && !current->node->nav_skip && current->y == target_y) {
            const uint16_t distance
                = current->x > selected->x ? current->x - selected->x : selected->x - current->x;
            if (!target || distance < target_distance) {
                target = current;
                target_distance = distance;
            }
        }
        current = current->next;
    } while (current != list_begin);
    JADE_ASSERT(target);

    set_tree_selection(selected->node, false);
    gui_repaint(selected->node);
    set_tree_selection(target->node, true);
    gui_repaint(target->node);
    activity->selectables = target;
    return true;
}

// BBB-AIRGAP: post the event naming the input and tell the caller to do nothing else.  The idle
// timer is registered first, exactly as the navigation paths do, so that the buttons check keeps
// the device awake while it is being used and so that the first press on a screen that has gone
// dark still only wakes it.
static bool input_echo(const int32_t event_id)
{
    if (!gui_input_echo) {
        return false;
    }
    if (!idletimer_register_activity(true)) {
        esp_event_post(GUI_EVENT, event_id, NULL, 0, 50 / portTICK_PERIOD_MS);
    }
    return true;
}

static void select_vertical_or_wheel(const bool down)
{
    if (idletimer_register_activity(true)) {
        return;
    }
    if (select_vertical(current_activity, down)) {
        esp_event_post(
            GUI_EVENT, down ? GUI_WHEEL_DOWN_EVENT : GUI_WHEEL_UP_EVENT, NULL, 0, 50 / portTICK_PERIOD_MS);
        return;
    }

    // Dice and QR option screens use the existing wheel events to change values.  Falling back
    // preserves that behavior, while posting exactly one event avoids overwriting single-slot waits.
    down ? select_next_right() : select_prev_left();
}

// BBB-AIRGAP: measure a string against a width from outside the gui task.  The display driver
// keeps the font to draw with in shared state (display.c), and the gui task holds gui_mutex for
// the whole of its awake period, rendering included (gui_task(), above) - so a caller that has to
// select a font in order to measure with it must hold the same mutex, or it can change the font
// under a frame that is being drawn.  The font is left selected afterwards, which harms nothing:
// every text node sets its own font before it draws (render_text(), above).
bool gui_text_fits_width(const char* text, const uint32_t font, const uint16_t width)
{
    JADE_ASSERT(text);
    JADE_ASSERT(gui_mutex);

    JADE_SEMAPHORE_TAKE(gui_mutex);
    display_set_font(font);
    const int text_width = display_get_string_width(text);
    JADE_SEMAPHORE_GIVE(gui_mutex);

    return text_width <= width;
}

void gui_next(void)
{
    gui_escape_clear(); // BBB-AIRGAP: any other press means the user changed their mind
    if (gui_orientation_flipped) {
        select_prev_left();
    } else {
        select_next_right();
    }
}

void gui_prev(void)
{
    gui_escape_clear(); // BBB-AIRGAP: any other press means the user changed their mind
    if (gui_orientation_flipped) {
        select_next_right();
    } else {
        select_prev_left();
    }
}

// BBB-AIRGAP: the echo follows the display orientation, exactly as the navigation call below it
// does.  The buttons check draws its marks as ordinary view nodes, so they turn over with the rest
// of the picture: on a flipped screen the mark laid out at the bottom is displayed at the top.
// Naming the raw switch instead would light the mark at the far end from where the user pushed.
// Posting what the navigation would have posted keeps the lit mark under the thumb that lit it,
// and a dead switch still shows up as the one mark that never lights.  The horizontal pair needs
// nothing here: gui_prev() and gui_next() already choose the call by the flag, and the two calls
// post the event for the direction the selection moves in.
void gui_up(void)
{
    gui_escape_clear(); // BBB-AIRGAP: any other press means the user changed their mind
    if (input_echo(gui_orientation_flipped ? GUI_WHEEL_DOWN_EVENT : GUI_WHEEL_UP_EVENT)) {
        return;
    }
    select_vertical_or_wheel(gui_orientation_flipped);
}

void gui_down(void)
{
    gui_escape_clear(); // BBB-AIRGAP: any other press means the user changed their mind
    if (input_echo(gui_orientation_flipped ? GUI_WHEEL_UP_EVENT : GUI_WHEEL_DOWN_EVENT)) {
        return;
    }
    select_vertical_or_wheel(!gui_orientation_flipped);
}

void gui_select_first(void)
{
    gui_escape_clear(); // BBB-AIRGAP: any other press means the user changed their mind
    if (input_echo(GUI_SELECT_FIRST_EVENT)) {
        return;
    }
    if (idletimer_register_activity(true) || !current_activity || !current_activity->selectables) {
        return;
    }

    selectable_t* const begin = current_activity->selectables;
    selectable_t* first = begin;
    selectable_t* current = begin;
    do {
        if (current->is_first) {
            first = current;
            break;
        }
        current = current->next;
    } while (current != begin);

    current = first;
    do {
        if (current->node->is_active) {
            select_node(current->node);
            esp_event_post(GUI_EVENT, GUI_SELECT_FIRST_EVENT, NULL, 0, 50 / portTICK_PERIOD_MS);
            return;
        }
        current = current->next;
    } while (current != first);
}

// BBB-AIRGAP: the scrolling list (main/ui/dialogs.c run_list_activity) turns the engine's wrap
// off so that a press at the edge of the window can scroll it, and wraps the selection itself
// once the window is at the end.  Called from the task that runs the list, as the input handlers
// above call select_node() from theirs.
void gui_select_node(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    select_node(node);
}

void gui_escape_request(void)
{
    gui_escape_flag = true;
}

bool gui_escape_pending(void) { return gui_escape_flag; }

void gui_escape_clear(void) { gui_escape_flag = false; }

void gui_activity_set_escape(gui_activity_t* const activity, const bool enabled)
{
    JADE_ASSERT(activity);
    activity->escape_disabled = !enabled;
}

void gui_alt_click(void)
{
    if (!idletimer_register_activity(true)) {
        // BBB-AIRGAP: raise the escape before posting, so a screen woken by the event already sees
        // it.  Not raised at all on a screen that opted out - the keyboard, where KEY3 is shift:
        // otherwise shifting a letter would arm a cancel that fired the moment the keyboard
        // returned to whatever opened it.  Reading current_activity from this thread is the same
        // inherited race as the navigation handlers above, and has the same shape: the worst case
        // is that the flag follows the screen the user was looking at a moment earlier.
        if (!current_activity || !current_activity->escape_disabled) {
            gui_escape_request();
        }
        esp_event_post(GUI_EVENT, GUI_ALT_EVENT, NULL, 0, 50 / portTICK_PERIOD_MS);
    }
}

// BBB-AIRGAP: see the declaration in main/gui.h for what this is for.  One screen owns the echo
// at a time and turns it off before it returns, so no state is left behind for the next screen.
void gui_set_input_echo(const bool value) { gui_input_echo = value; }

// Set the item to be initally selected when the activity is activated/switched-to
// 'node' can be NULL to unset any specific initial selection
void gui_set_activity_initial_selection(gui_view_node_t* node)
{
    JADE_ASSERT(node);
    JADE_ASSERT(node->activity);
    node->activity->initial_selection = node;
}

static void gui_post(const gui_task_job_t* task, const char* task_name)
{
    // BBB-AIRGAP: no later producer may enqueue until this job's count is published. Keep the
    // consumer independent of this lock, including while a full queue makes the send retry.
    JADE_SEMAPHORE_TAKE(gui_post_mutex);
    while (xRingbufferSend(gui_input_queue, task, sizeof(*task), 500 / portTICK_PERIOD_MS) != pdTRUE) {
        JADE_LOGW("Failed to send %s to gui", task_name);
    }

    // BBB-AIRGAP: counted only once the job is really on the queue, so the number never promises
    // work the gui task cannot find.  See gui_jobs_posted.
    atomic_fetch_add_explicit(&gui_jobs_posted, 1, memory_order_relaxed);
    JADE_SEMAPHORE_GIVE(gui_post_mutex);
}

// Post a node to the gui task to be repainted
// Should ultimately result in a call to repaint_node() from the gui_task.
// This ensures all calls to the undlerying display driver come from the gui_task
// and are serialised, such that no mutexing should be required.
void gui_repaint(gui_view_node_t* node)
{
    JADE_ASSERT(node);

    // If we are called from the gui task we can immediately repaint the node.
    // If not, we should enqueue a message to the gui task to repaint.
    if (gui_is_gui_task()) {
        repaint_node(node);
        return;
    }

    const gui_task_job_t node_repaint_info = { .node_to_repaint = node };
    gui_post(&node_repaint_info, "repaint");
}

// Call to initiate a change of current activity - optionally freeing other managed activities
// (either all of them, if free_managed_activities is true, or only to_destroy if given).
void gui_set_current_activity_impl(
    gui_activity_t* new_current, gui_activity_t* to_destroy, const bool free_managed_activities, SemaphoreHandle_t done)
{
    JADE_ASSERT(new_current);
    JADE_ASSERT(!to_destroy || !free_managed_activities); // Either one, all or none

    // job info initially includes just the new activity
    gui_task_job_t switch_info = { .new_activity = new_current };

    if (to_destroy || free_managed_activities) {
        // Freeing other activity/activities: partition into keep and free lists
        JADE_SEMAPHORE_TAKE(gui_mutex);
        activity_holder_t* holder = existing_activities;
        existing_activities = NULL;

        while (holder) {
            activity_holder_t* const next = holder->next;

            if (&holder->activity == new_current || (to_destroy && &holder->activity != to_destroy)) {
                // Retain this activity
                holder->next = existing_activities;
                existing_activities = holder;
            } else {
                // Discard this activity
                holder->next = switch_info.to_free;
                switch_info.to_free = holder;
            }
            holder = next;
        }

        // Sanity check
        if (!to_destroy && existing_activities) {
            // existing_activities should be the new current activity only
            JADE_ASSERT(&existing_activities->activity == new_current && !existing_activities->next);
        }

        JADE_SEMAPHORE_GIVE(gui_mutex);
    }

    // Post the new activity and the list to free to the gui task
    switch_info.done = done;
    gui_post(&switch_info, "new activity");
}

void gui_set_current_activity_ex(gui_activity_t* new_current, const bool free_managed_activities)
{
    gui_set_current_activity_impl(new_current, NULL, free_managed_activities, NULL);
}

// BBB-AIRGAP: synchronous variant of gui_set_current_activity_ex(). gui_set_current_activity_ex()
// only enqueues the switch (main/gui.c handle_gui_input_queue(), which unregisters the outgoing
// activity's handlers and registers this one's only when it drains that job later, on the gui
// task); a caller that needs the new activity's handlers to already be live when this returns -
// see main/ui/dialogs.c run_list_activity(), which drains stale input right after switching -
// cannot rely on that. Same pattern as gui_destroy_current_activity() below: pass a semaphore
// through and block until the gui task gives it back once the switch job has fully run. Calling
// this from the gui task itself would deadlock, since that task would be blocked waiting on the
// very job it needs to process to give the semaphore; the current caller runs on the firmware
// task, not the gui task, so this is safe.
void gui_set_current_activity_sync(gui_activity_t* new_current, const bool free_managed_activities)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    JADE_ASSERT(done);

    gui_set_current_activity_impl(new_current, NULL, free_managed_activities, done);

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

// Initiate change of 'current' activity
void gui_set_current_activity(gui_activity_t* new_current)
{
    // Set a new activity without freeing any other activities
    gui_set_current_activity_impl(new_current, NULL, false, NULL);
}

void gui_destroy_current_activity(gui_activity_t* current_act, gui_activity_t* prev_act)
{
    // Create a semaphore to be signaled when the gui task finishes
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    JADE_ASSERT(done);

    gui_set_current_activity_impl(prev_act, current_act, false, done);

    // Wait for the gui task to finish
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

// Create a new event_data structure, and attach to the activity
// (so it has the same lifetime as the parent activity)
wait_event_data_t* gui_activity_make_wait_event_data(gui_activity_t* activity)
{
    JADE_ASSERT(activity);

    // Create item to hold new event data object
    wait_data_t* const item = JADE_MALLOC(sizeof(wait_data_t));
    item->event_data = make_wait_event_data();

    // Put into activity's list
    item->next = activity->wait_data_items;
    activity->wait_data_items = item;

    // Return new wait_event_data
    return item->event_data;
}

void gui_activity_register_event(
    gui_activity_t* activity, const char* event_base, uint32_t event_id, esp_event_handler_t handler, void* args)
{
    JADE_ASSERT(activity);
    JADE_ASSERT(event_base);

    // Store the event registration so we can re-apply when switching between activities
    activity_event_t* link = JADE_CALLOC(1, sizeof(activity_event_t));

    link->event_base = event_base;
    link->event_id = event_id;
    link->handler = handler;
    link->args = args;

    // Get the main gui mutex before we update the activity events
    // or check the current activity, as can be concurrent with 'handle_gui_input_queue()'
    JADE_SEMAPHORE_TAKE(gui_mutex);

    if (!activity->activity_events) {
        activity->activity_events = link;
    } else {
        activity_event_t* last = activity->activity_events;
        while (last->next) {
            last = last->next;
        }
        last->next = link;
    }

    // If this activity is already active, immediately add the event handler
    if (activity == current_activity) {
        const esp_err_t rc
            = esp_event_handler_instance_register(event_base, event_id, handler, args, &(link->instance));
        JADE_ASSERT(rc == ESP_OK);
    }

    // Return the main gui mutex
    JADE_SEMAPHORE_GIVE(gui_mutex);
}

// Registers an event handler, then blocks waiting for it to fire.  A timeout can be passed.
// Returns true if the event fires, false if the timeout elapsed without the event occuring.
bool gui_activity_wait_event(gui_activity_t* activity, const char* event_base, uint32_t event_id,
    esp_event_base_t* trigger_event_base, int32_t* trigger_event_id, void** trigger_event_data, TickType_t max_wait)
{
    JADE_ASSERT(activity);

    // create a new wait-event-data structure and attach to the activity, which takes ownership
    wait_event_data_t* const wait_event_data = gui_activity_make_wait_event_data(activity);
    JADE_ASSERT(wait_event_data);

    // register it so that it gets removed when the activity is swapped out
    gui_activity_register_event(activity, event_base, event_id, sync_wait_event_handler, wait_event_data);

    // BBB-AIRGAP: wake this wait on KEY3 as well, so the escape reaches every screen that waits
    // through here without each of them registering for it.  Only when the caller's own base
    // would miss it: a GUI_EVENT waiter already receives GUI_ALT_EVENT, and registering a second
    // time would give the semaphore twice for one press, leaving the next wait to return
    // immediately with the previous event's data.  Costs one more activity_event_t per call on
    // the screens that need it, which is the same per-call registration this function has always
    // made; it is freed with the activity.
    const bool alt_already_covered
        = event_base == GUI_EVENT && (event_id == ESP_EVENT_ANY_ID || event_id == (uint32_t)GUI_ALT_EVENT);
    if (!alt_already_covered && !activity->escape_disabled) {
        gui_activity_register_event(activity, GUI_EVENT, GUI_ALT_EVENT, sync_wait_event_handler, wait_event_data);
    }

    // immediately start waiting
    esp_event_base_t triggered_base = NULL;
    int32_t triggered_id = 0;
    const esp_err_t ret = sync_wait_event(wait_event_data, &triggered_base, &triggered_id, trigger_event_data, max_wait);
    if (ret != ESP_OK) {
        return false;
    }

    // BBB-AIRGAP: report the escape in the caller's own numbering.  gui_event_t and
    // button_event_id both start at zero and overlap (GUI_ALT_EVENT is 6, which is
    // BTN_QR_BRIGHTNESS; GUI_FRONT_CLICK_EVENT is 3, which is BTN_YES), so handing a button loop
    // the raw event id would fire whichever button shares the number.
    if (triggered_base == GUI_EVENT && triggered_id == GUI_ALT_EVENT && !alt_already_covered) {
        triggered_base = event_base;
        triggered_id = BTN_ESCAPE_HOME;
    }

    if (trigger_event_base) {
        *trigger_event_base = triggered_base;
    }
    if (trigger_event_id) {
        *trigger_event_id = triggered_id;
    }
    return true;
}

int32_t gui_activity_wait_button(gui_activity_t* activity, const int32_t default_event_id)
{
    int32_t ev_id = default_event_id;
#ifndef CONFIG_DEBUG_UNATTENDED_CI
    if (!gui_activity_wait_event(activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) {
        ev_id = BTN_EVENT_TIMEOUT;
    }
#else
    gui_activity_wait_event(activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, NULL, NULL,
        CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS / portTICK_PERIOD_MS);
#endif
    return ev_id;
}

// Update the title associated with the passed activity
void gui_set_activity_title(gui_activity_t* activity, const char* title)
{
    JADE_ASSERT(activity);
    JADE_ASSERT(title);

    JADE_SEMAPHORE_TAKE(gui_mutex);
    if (activity->title) {
        free(activity->title);
    }
    activity->title = strdup(title);

    // If setting title for the current activity, update status bar
    const bool repaint = current_activity && activity == current_activity;
    JADE_SEMAPHORE_GIVE(gui_mutex);

    if (repaint) {
        gui_repaint(status_bar.root);
    }
}

gui_activity_t* gui_current_activity(void) { return current_activity; }

#ifdef CONFIG_BOARD_TYPE_JADE_ANY
extern const uint8_t splashstart[] asm("_binary_splash_bin_gz_start");
extern const uint8_t splashend[] asm("_binary_splash_bin_gz_end");
#endif

gui_activity_t* gui_display_splash(void)
{
    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* splash_node = NULL;

    // Blank screen while awaiting attestation data upload
    if (!gui_awaiting_attestation_data()) {
#ifdef CONFIG_BOARD_TYPE_JADE_ANY
        Picture* const pic = get_picture(splashstart, splashend);
        gui_make_picture(&splash_node, pic);
#else
        gui_make_text(&splash_node, "Jade DIY", TFT_WHITE);
#endif
        gui_set_align(splash_node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
        gui_set_parent(splash_node, act->root_node);
    }

    // set the current activity and draw it on screen
    gui_set_current_activity(act);
    return act;
}
#endif // AMALGAMATED_BUILD
