#ifndef UI_H_
#define UI_H_

#include "assets.h"
#include "gui.h"

struct wally_tx;

// Maximum length of message which can be fully displayed on
// 'sign-message' screen - longer messages display the hash
#define MAX_DISPLAY_MESSAGE_LEN 192

// BBB-AIRGAP: the share of the screen width the title bar gives its middle cell, between the two
// header buttons (populate_title_bar, main/ui/dialogs.c).  Named here rather than left as a bare
// number in that split, because a caller that has to decide whether its title will fit needs the
// same figure, and two copies of it would drift apart.
#define TITLE_CELL_PCNT 70

// BBB-AIRGAP: the share of the camera screen's height taken by its header row, and by the matching
// footer row that holds the progress bar (make_camera_activity, main/ui/camera.c).  Named here
// because main/camera.c needs the same figure to know how many rows of the image the header covers
// and therefore dims as it copies them; two copies of the number would drift apart.
#define CAMERA_HEADER_PCNT 20

// Keyboard entry screens
#define MAX_KB_ENTRY_LEN 256

// NOTE: final value is a sentinel/count, not a valid enum value
typedef enum {
    KB_LOWER_CASE_CHARS = 0,
    KB_UPPER_CASE_CHARS,
    KB_NUMBERS_SYMBOLS,
    KB_REMAINING_SYMBOLS,
    NUM_KBS
} keyboard_type_t;

typedef struct {
    char strdata[MAX_KB_ENTRY_LEN];
    size_t max_allowed_len;
    size_t len;

    keyboard_type_t keyboards[NUM_KBS];
    size_t num_kbs;
    size_t current_kb;
    const char* blocked_chars;

    gui_activity_t* activity;
    gui_view_node_t* textbox_nodes[NUM_KBS];
} keyboard_entry_t;

// digit entry
#define DIGIT_ENTRY_SIZE 6

enum __attribute__((__packed__)) digit_entry_type {
    DIGIT_ENTRY_INVALID = 0,
    DIGIT_ENTRY_PIN,
    DIGIT_ENTRY_INDEX,
};

enum __attribute__((__packed__)) digit_entry_initial_state { RANDOM, ZERO, POSITION };

enum __attribute__((__packed__)) digit_entry_status {
    EMPTY,
    SELECTED,
    SET,
};

typedef struct {
    gui_view_node_t* fill_node;
    gui_view_node_t* up_arrow_node;
    gui_view_node_t* digit_node;
    gui_view_node_t* down_arrow_node;
} digit_entry_node_t;

typedef struct {
    const enum digit_entry_type entry_type;
    const enum digit_entry_initial_state initial_state;
    const bool digits_shown;

    uint8_t digit[DIGIT_ENTRY_SIZE];
    enum digit_entry_status digit_status[DIGIT_ENTRY_SIZE];
    gui_activity_t* activity;
    gui_view_node_t* title;

    digit_entry_node_t digit_nodes[DIGIT_ENTRY_SIZE];

    uint8_t selected_digit;
    uint8_t current_selected_value;
} digit_entry_t;

typedef struct {
    gui_view_node_t* symbol;
    gui_view_node_t* text;
} home_menu_entry_t;

// Whether QR Frame Guides (box corners) should be shown
typedef enum { QR_GUIDE_HIDE, QR_GUIDE_SHOW } qr_guide_type_t;

// Output is confidential/blinded
#define OUTPUT_FLAG_CONFIDENTIAL (1 << 0)
// Output has a blinding public key
#define OUTPUT_FLAG_HAS_BLINDING_KEY (1 << 1)
// Output is ours (belongs to a wallet controlled by this Jade)
#define OUTPUT_FLAG_IS_OURS (1 << 2)
// Output is a change output for this wallet (only set with OUTPUT_FLAG_IS_OURS)
#define OUTPUT_FLAG_CHANGE (1 << 3)
// Output has unblinded asset and value
#define OUTPUT_FLAG_HAS_UNBLINDED (1 << 4)

// Progress bar
typedef struct {
    bool transparent;
    gui_view_node_t* container;
    gui_view_node_t* progress_bar;
    gui_view_node_t* pcnt_txt;
    uint8_t percent_last_value;
} progress_bar_t;

// Button bars/menus etc.
typedef enum { UI_ROW, UI_COLUMN } ui_button_layout_t;

typedef struct {
    gui_view_node_t* btn;
    gui_view_node_t* content;
    const char* txt;
    uint32_t font;
    uint32_t ev_id;
    uint8_t borders;
} btn_data_t;

// Helper to update dynamic menu item label (name: value)
void update_menu_item(gui_view_node_t* node, const char* label, const char* value);

// BBB-AIRGAP: writes a bitcoin amount into 'buf' in BTC or in satoshis, whichever the device is set
// to (Features), and returns the ticker to show beside it.  Defined in main/ui/sign_tx.c,
// where the tx screens use it; the mining screen shows a block reward through it too.
const char* format_btc_amount(uint64_t satoshi, char* buf, size_t buf_len);

// Helper to create an even split
gui_view_node_t* make_even_split(ui_button_layout_t layout, uint8_t num_splits);

// Helpers to create standard (look and feel) buttons in a row or column
void add_button(gui_view_node_t* parent, btn_data_t* btn_info);
void add_buttons(gui_view_node_t* parent, ui_button_layout_t layout, btn_data_t* btns, size_t num_btns);

// Helpers to create and populate the common title bar
void populate_title_bar(
    gui_view_node_t* bar, const char* title, btn_data_t* btns, size_t num_btns, gui_view_node_t** title_node);
gui_view_node_t* add_title_bar(
    gui_activity_t* activity, const char* title, btn_data_t* btns, size_t num_btns, gui_view_node_t** title_node);

// Helper to create an activity which is a grid of (up to 7x7) text items
gui_activity_t* make_text_grid_activity(const char* title, btn_data_t* hdrbtns, size_t num_hdrbtns, size_t toppad,
    uint8_t xcells, uint8_t ycells, const char* texts, size_t num_texts, uint32_t font, const char** remaining_texts);

// Helper to create a vertical menu of 2, 3 or 4 buttons
gui_activity_t* make_menu_activity(
    const char* title, btn_data_t* hdrbtns, const size_t num_hdrbtns, btn_data_t* menubtns, size_t num_menubtns);

// BBB-AIRGAP: the buttons check paints one mark per input it can see.  KEY3 has no mark because
// pressing it leaves the screen, which is the whole of its test; the centre press and KEY2 have
// one each but light together, because the two keys are wired to the same input
// (pijade/host/pijade_host.c).  Built by make_io_test_buttons_activity() (main/ui/dashboard.c),
// coloured in by handle_io_test_buttons() (main/process/dashboard.c).
typedef enum {
    IO_TEST_MARK_UP,
    IO_TEST_MARK_LEFT,
    IO_TEST_MARK_CLICK,
    IO_TEST_MARK_RIGHT,
    IO_TEST_MARK_DOWN,
    IO_TEST_MARK_KEY1,
    IO_TEST_MARK_KEY2,
    IO_TEST_NUM_MARKS
} io_test_mark_t;

// BBB-AIRGAP: what the line under the marks says.  It opens on KEY3 and is rewritten on every
// press, so a user who does not know the board can learn each button by pressing it.  The wording
// is measured, not guessed: the joystick and the three keys reach Jade through libjade_input()
// (pijade/host/pijade_host.c), where KEY1 is gui_select_first(), KEY2 is the same input as the
// centre press (gui_front_click()) and KEY3 is gui_alt_click().  Up and down fall back to prev
// and next on a screen with no vertical neighbour (gui.c select_vertical_or_wheel()), which is
// why their lines name both.  Keep these within the width the screen already fits.
// KEY1 and KEY3 are described by what the user sees them do, not by the gui call behind them
// (ROADMAP item 71): the first selectable item on every screen is the back arrow in the header,
// so gui_select_first() lands on it; and gui_alt_click() raises the escape flag that every
// screen up the stack honours (gui_escape_request()), so KEY3 does not leave this screen only, it
// unwinds to the home screen.  The keyboard is the one exception - KEY3 is shift there.
// Measured on the emulator (tur 9): the note line holds about twenty characters of this font
// before it wraps mid-word, so these two are the short forms of what they say.
#define IO_TEST_NOTE_KEY3 "K3: exit to home"
#define IO_TEST_NOTE_KEY1 "K1: go to back arrow"
#define IO_TEST_NOTE_CLICK "Press or K2: select"
#define IO_TEST_NOTE_LEFT "Left: previous item"
#define IO_TEST_NOTE_RIGHT "Right: next item"
#define IO_TEST_NOTE_UP "Up: up, or previous"
#define IO_TEST_NOTE_DOWN "Down: down, or next"

// BBB-AIRGAP: scrolling list. Jade's menus stop at four items (make_menu_activity above) and the
// gui engine has neither vertical scrolling nor clipping, so a longer menu cannot simply be drawn
// taller. Instead the screen keeps four rows of the usual height and a window moves over the
// items: the row labels are rewritten as the selection reaches an edge. Row height, fonts and
// borders are the ones the four-item menu already uses.
#define LIST_VISIBLE_ROWS 4

typedef struct {
    const char* txt;
    // BBB-AIRGAP: optional single character drawn in the symbols font at the right of the row,
    // for a mark the label itself cannot carry. NULL leaves that space empty. A list where no
    // item has one is laid out exactly as before, without the extra column.
    const char* symbol;
    int32_t ev_id;
} list_item_t;

// Builds the list screen itself. 'rowbtns' are the visible rows, at most LIST_VISIBLE_ROWS of
// them; their labels are owned by the caller through the returned nodes (btn_data_t::content).
// 'scrollbar_cells' is optional: pass an array of LIST_VISIBLE_ROWS nodes to get a scroll
// indicator down the right edge, whose cells the caller lights through update_list_scrollbar(),
// or NULL for a list that fits on screen and needs none. 'symbol_nodes' is optional in the same
// way: pass an array to get a symbol column beside the labels, whose text the caller sets, or
// NULL for rows that are label-only. Either way rowbtns[i].content comes back as the label node.
gui_activity_t* make_list_activity(const char* title, btn_data_t* hdrbtns, size_t num_hdrbtns, btn_data_t* rowbtns,
    size_t num_rows, gui_view_node_t** scrollbar_cells, gui_view_node_t** symbol_nodes);

// Runs the list until the user picks an item, and returns that item's ev_id; returns 'exit_ev_id'
// if they leave through the title-bar button instead. '*io_selected' is the selected item on entry
// and on exit, so a caller that returns from a sub-screen reopens the list where it was left.
int32_t run_list_activity(
    const char* title, int32_t exit_ev_id, const list_item_t* items, size_t num_items, size_t* io_selected);

// Helper to create an activity to show a message on a single central label
gui_activity_t* make_show_message_activity(const char* message[], size_t message_size, const char* title,
    btn_data_t* hdrbtns, size_t num_hdrbtns, btn_data_t* ftrbtns, size_t num_ftrbtns);

// Activity to show a single value
gui_activity_t* make_show_single_value_activity(const char* name, const char* value, const bool show_helpbtn);

// Make activity that displays a simple message - cannot be dismissed by caller
gui_activity_t* display_message_activity(const char* message[], size_t message_size);
gui_activity_t* display_processing_message_activity();

// Run activity that displays a message and awaits an 'ack' button click
void await_message(const char* msg);
// BBB-AIRGAP: the same screen as await_message(), returning true when KEY3 dismissed it.  The
// answer is taken inside the wait, so a later press cannot clear it into consent; use it wherever
// what follows the notice is destructive, irreversible or outward-facing.
bool await_message_escaped(const char* message[], size_t message_size);
void await_titled_message(const char* title, const char* msg);
void await_message_2(const char* msg1, const char* msg2);
void await_message_3(const char* msg1, const char* msg2, const char* msg3);
void await_message_4(const char* msg1, const char* msg2, const char* msg3, const char* msg4);
void await_error(const char* msg);
void await_error_2(const char* msg1, const char* msg2);
void await_error_3(const char* msg1, const char* msg2, const char* msg3);

// Activity that displays a message and awaits a 'Yes'/'Continue' or 'No'/'Skip'/'Back' event
bool await_yesno_activity(
    const char* title, const char* message[], size_t message_size, bool default_selection, const char* help_url);
bool await_skipyes_activity(
    const char* title, const char* message[], size_t message_size, bool default_selection, const char* help_url);
// BBB-AIRGAP: two-option question with caller-supplied labels.  Returns true for the first.
bool await_choice_activity(const char* title, const char* message[], size_t message_size, const char* yes_txt,
    const char* no_txt, bool default_selection, const char* help_url);
bool await_continueback_activity(
    const char* title, const char* message[], size_t message_size, bool default_selection, const char* help_url);

// Updatable label with left/right arrows
gui_activity_t* make_carousel_activity(const char* title, gui_view_node_t** label, gui_view_node_t** item);
// BBB-AIRGAP: Jade changes a setting on a screen of its own - the title names the setting, the value
// in use sits between the arrows, left/right move through the values and the click keeps the one
// shown (Display > Brightness, QR Settings > QR Density).  Upstream writes that loop out in every
// handler; this fork has settings of its own, so the loop lives here once.  Shows 'labels[initial]'
// first and returns the index of the label the click landed on.
size_t await_carousel_activity(const char* title, const char* const* labels, size_t num_labels, size_t initial);
void update_carousel_highlight_color(const gui_view_node_t* text_label, color_t color, bool repaint);

// Functions for keyboard entry
void make_keyboard_entry_activity(keyboard_entry_t* kb_entry, const char* title);
void run_keyboard_entry_loop(keyboard_entry_t* kb_entry);

// Functions for number entry
void make_digit_entry_activity(digit_entry_t* digit_entry, const char* title, const char* message);
bool run_digit_entry_loop(digit_entry_t* digit_entry);
void reset_digit_entry(digit_entry_t* digit_entry, const char* title);
uint32_t get_entry_as_number(const digit_entry_t* digit_entry);

// Generic progress-bar
void make_progress_bar(gui_view_node_t* parent, progress_bar_t* progress_bar);
gui_activity_t* make_progress_bar_activity(const char* title, const char* message, progress_bar_t* progress_bar);
void update_progress_bar(progress_bar_t* progress_bar, size_t total, size_t current);

#endif /* UI_H_ */
