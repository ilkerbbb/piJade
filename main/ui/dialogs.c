#ifndef AMALGAMATED_BUILD
#include "../button_events.h"
#include "../jade_assert.h"
#include "../ui.h"

void await_qr_help_activity(const char* url);

// releative
#define TITLE_BAR_HEIGHT_PCNT 20
#define FOOTER_BUTTONS_HEIGHT_PCNT 25

// absolute, appropriate for font being used and adjusted slightly for larger screens
#define MESSAGE_LINE_ROW_HEIGHT (CONFIG_DISPLAY_HEIGHT >= 150 ? 22 : 20)

// Helper to update dynamic menu item label (name: value)
void update_menu_item(gui_view_node_t* node, const char* label, const char* value)
{
    char buf[32];
    const int ret = snprintf(buf, sizeof(buf), "%s: %s", label, value);
    JADE_ASSERT(ret > 0 && ret < sizeof(buf));
    gui_update_text(node, buf);
}

// Handles up to 7 splits, row or column.
// layout == UI_ROW -> [ x | y | z ] items in a row -> an hsplit
// layout == UI_COLUMN -> [ x / y / z ] items in a column -> a vsplit
gui_view_node_t* make_even_split(const ui_button_layout_t layout, const uint8_t num_splits)
{
    JADE_ASSERT(layout == UI_ROW || layout == UI_COLUMN);
    // num_splits range asserted in switch below

    // Make the split relevant for the number of buttons
    typedef void (*make_split_fn)(gui_view_node_t** ptr, enum gui_split_type kind, int parts, ...);
    make_split_fn make_split = (layout == UI_COLUMN) ? gui_make_vsplit : gui_make_hsplit;

    // Make a split for the number of buttons (if greater than one)
    gui_view_node_t* split = NULL;
    switch (num_splits) {
    case 2:
        make_split(&split, GUI_SPLIT_RELATIVE, 2, 50, 50);
        break;
    case 3:
        make_split(&split, GUI_SPLIT_RELATIVE, 3, 33, 34, 33);
        break;
    case 4:
        make_split(&split, GUI_SPLIT_RELATIVE, 4, 25, 25, 25, 25);
        break;
    case 5:
        make_split(&split, GUI_SPLIT_RELATIVE, 5, 20, 20, 20, 20, 20);
        break;
    case 6:
        make_split(&split, GUI_SPLIT_RELATIVE, 6, 17, 16, 17, 17, 16, 17);
        break;
    case 7:
        make_split(&split, GUI_SPLIT_RELATIVE, 7, 14, 15, 14, 14, 14, 15, 14);
        break;
    default:
        JADE_ASSERT_MSG(false, "Unsupported split size");
    }
    return split;
}

// Helper to make a standard button, for consistent look and feel behaviour
void add_button(gui_view_node_t* parent, btn_data_t* btn_info)
{
    JADE_ASSERT(btn_info);

    // Cannot specify both 'text label' and 'explicit content'
    JADE_ASSERT(!btn_info->txt || !btn_info->content);

    gui_view_node_t* btn;

    // No event implies no 'pressable' button in this position - use an empty 'vsplit' as a spacer
    if (btn_info->ev_id == GUI_BUTTON_EVENT_NONE) {
        gui_make_vsplit(&btn, GUI_SPLIT_RELATIVE, 1, 100); // no-op spacer
    } else {
        gui_make_button(&btn, TFT_BLACK, gui_get_highlight_color(), btn_info->ev_id, NULL);
    }
    gui_set_parent(btn, parent);

    // If borders explicitly specified, show in dark grey
    // 0 implies default behaviour - no visible borders when not selected
    if (btn_info->borders) {
        gui_set_borders(btn, GUI_BLOCKSTREAM_BUTTONBORDER_GREY, 1, btn_info->borders);
    } else {
        gui_set_borders(btn, TFT_BLACK, 1, GUI_BORDER_ALL);
    }

    // Add any simple text label
    if (btn_info->txt) {
        gui_view_node_t* text;
        gui_make_text_font(&text, btn_info->txt, TFT_WHITE, btn_info->font);
        gui_set_parent(text, btn);
        gui_set_align(text, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    } else if (btn_info->content) {
        // In more complex cases caller can prepare content and pass instead
        gui_set_parent(btn_info->content, btn);
    }

    // Set the (btn) control back in the info struct
    btn_info->btn = btn;
}

// Helper to create buttons in a row or column
void add_buttons(gui_view_node_t* parent, const ui_button_layout_t layout, btn_data_t* btns, const size_t num_btns)
{
    JADE_ASSERT(layout == UI_ROW || layout == UI_COLUMN);
    JADE_ASSERT(btns);
    JADE_ASSERT(num_btns);

    if (num_btns == 1) {
        // skip intermediate split, apply button directly to parent
        // ('layout' (row or column) is irrelevant in this case)
        add_button(parent, btns);
        return;
    }

    // Make a split for the number of buttons (if greater than one)
    gui_view_node_t* const split = make_even_split(layout, num_btns);
    gui_set_parent(split, parent);

    // Add buttons to split
    for (size_t i = 0; i < num_btns; ++i) {
        add_button(split, btns + i);
    }
}

static inline btn_data_t* add_default_border(btn_data_t* btn, const uint32_t default_borders)
{
    if (!btn->borders && btn->ev_id != GUI_BUTTON_EVENT_NONE) {
        btn->borders = default_borders;
    }
    return btn;
}

// Helper to populate the common title bar
void populate_title_bar(
    gui_view_node_t* bar, const char* title, btn_data_t* btns, const size_t num_btns, gui_view_node_t** title_node)
{
    JADE_ASSERT(title || btns);
    JADE_ASSERT((btns && num_btns == 2) || !num_btns);
    JADE_ASSERT(!title_node || title);
    // title is optional but do not expect neither title nor buttons
    // buttons are optional, but must have zero or two (can be placeholder)
    // title_node is optional, but can only be passed if a title (even an empty string) is passed

    // Create the title text
    // If the caller has asked for the node to be returned it probably means they are expecting
    // to update it - in which case inject an intermediate fill (so updated text redraws properly).
    gui_view_node_t* titlenode;
    if (title) {
        gui_make_text_font(&titlenode, title, TFT_WHITE, GUI_TITLE_FONT);
        gui_set_align(titlenode, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);

        if (title_node) {
            *title_node = titlenode;
            gui_make_fill(&titlenode, TFT_BLACK, FILL_PLAIN, NULL);
            gui_set_parent(*title_node, titlenode);
        }
    } else {
        // No title, just a blank space
        gui_make_fill(&titlenode, TFT_BLACK, FILL_PLAIN, NULL);
    }

    if (!num_btns) {
        // Just a title, no buttons - just apply straight to the bar node
        gui_set_parent(titlenode, bar);
    } else {
        // Split the bar into three sections, [lbtn | title | rbtn]
        gui_view_node_t* hsplit;
        gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 3, (100 - TITLE_CELL_PCNT) / 2, TITLE_CELL_PCNT,
            (100 - TITLE_CELL_PCNT) / 2);
        gui_set_parent(hsplit, bar);

        // If not otherwise specified, put a border around the buttons
        add_button(hsplit, add_default_border(btns, GUI_BORDER_ALL));
        gui_set_parent(titlenode, hsplit);
        add_button(hsplit, add_default_border(btns + 1, GUI_BORDER_ALL));
    }
}

// Helper to create and populate the common title bar
gui_view_node_t* add_title_bar(
    gui_activity_t* activity, const char* title, btn_data_t* btns, const size_t num_btns, gui_view_node_t** title_node)
{
    JADE_ASSERT(activity);
    JADE_ASSERT(btns || !num_btns);

    // Split off the top 20% as the title bar
    gui_view_node_t* vsplit;
    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 2, TITLE_BAR_HEIGHT_PCNT, 100 - TITLE_BAR_HEIGHT_PCNT);
    gui_set_parent(vsplit, activity->root_node);

    // Populate the title bar
    populate_title_bar(vsplit, title, btns, num_btns, title_node);

    // Return the new parent for further ui elements
    return vsplit;
}

// Helper to create an activity which is a grid of (up to 7x7) text items
// NOTE: the text is passed as one char large array containing embedded terminators
// to delineate the separate texts - eg: ... , "abc\0def\0ghi\0j\0", 4)
gui_activity_t* make_text_grid_activity(const char* title, btn_data_t* hdrbtns, const size_t num_hdrbtns,
    const size_t toppad, const uint8_t xcells, const uint8_t ycells, const char* texts, const size_t num_texts,
    const uint32_t font, const char** remaining_texts)
{
    // Title and header are optional
    JADE_ASSERT(hdrbtns || !num_hdrbtns);
    JADE_ASSERT(xcells);
    JADE_ASSERT(ycells);
    JADE_ASSERT(texts);
    JADE_ASSERT(num_texts);
    JADE_ASSERT(num_texts <= xcells * ycells);
    // remaining_texts pointer is optional

    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* parent = act->root_node;

    // Add a titlebar if deisred
    const bool have_hdr = title || num_hdrbtns;
    if (have_hdr) {
        parent = add_title_bar(act, title, hdrbtns, num_hdrbtns, NULL);
    }

    // Make grid - reads across then down
    gui_view_node_t* const vsplit = make_even_split(UI_COLUMN, ycells);
    gui_set_padding(vsplit, GUI_MARGIN_ALL_DIFFERENT, toppad, 2, 0, 2);
    gui_set_parent(vsplit, parent);

    const char* text_item = texts;
    for (uint8_t y = 0; y < ycells; ++y) {
        gui_view_node_t* hsplit = make_even_split(UI_ROW, xcells);
        gui_set_parent(hsplit, vsplit);

        for (uint8_t x = 0; x < xcells; ++x) {
            const int itxt = (y * xcells) + x;
            if (itxt < num_texts) {
                gui_view_node_t* node;
                gui_make_text_font(&node, text_item, TFT_WHITE, font);
                gui_set_align(node, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
                gui_set_parent(node, hsplit);
                text_item += strlen(text_item) + 1;
            }
        }
    }

    // Return pointer indicating where we have read/displayed up to
    // NOTE: could be off the end of the 'texts' string if entire string consumed.
    if (remaining_texts) {
        *remaining_texts = text_item;
    }

    return act;
}

// Helper to create an activity to show a vertical menu
// Must pass title-bar information - supports up to 4 menu buttons
gui_activity_t* make_menu_activity(
    const char* title, btn_data_t* hdrbtns, const size_t num_hdrbtns, btn_data_t* menubtns, const size_t num_menubtns)
{
    JADE_ASSERT(title);
    // Header buttons are optional
    JADE_ASSERT(menubtns);
    JADE_ASSERT(num_menubtns);
    JADE_ASSERT(num_menubtns < 5);

    // Explicitly set just left|top|right borders around header buttons when menu is 'full'
    // as bottom edge will be covered by upper line above top menu item.
    if (num_menubtns > 2) {
        for (size_t i = 0; i < num_hdrbtns; ++i) {
            add_default_border(&hdrbtns[i], GUI_BORDER_SIDES | GUI_BORDER_TOP);
        }
    }

    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* parent = add_title_bar(act, title, hdrbtns, num_hdrbtns, NULL);

    // Add any padding for smaller number of items
    if (num_menubtns < 3) {
        gui_view_node_t* vsplit;
        const uint32_t split = num_menubtns == 2 ? 65 : 35;
        gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 2, split, 100 - split);
        gui_set_padding(vsplit, GUI_MARGIN_ALL_DIFFERENT, 12, 0, 0, 0);
        gui_set_parent(vsplit, parent);
        parent = vsplit;
    }

    // Add default borders between menu items
    for (size_t i = 0; i < num_menubtns; ++i) {
        add_default_border(&menubtns[i], i == 0 ? GUI_BORDER_TOPBOTTOM : GUI_BORDER_BOTTOM);
    }

    // Add menu buttons
    add_buttons(parent, UI_COLUMN, menubtns, num_menubtns);

    return act;
}

// BBB-AIRGAP: builds the scrolling list screen - see ui.h for why it exists.
// BBB-AIRGAP: width of the list scroll indicator, given as the percentage of the screen left to
// the rows beside it. The indicator itself takes what remains, so nothing is lost to rounding:
// a percentage on both sides of the split would floor each one separately (get_step(),
// main/gui.c:1780) and leave a dead column at the screen edge. Three percent is 8px at 240px
// wide - enough to read at arm's length without taking width the row labels need.
#define LIST_SCROLLBAR_PERCENT 3

// BBB-AIRGAP: width of the optional symbol column, as the percentage of the row left to the label
// beside it; the symbol takes what remains, for the same rounding reason as the scrollbar above.
// Eight percent is 18px of the 232px the rows have, enough for the 16x16 symbols font.
#define LIST_SYMBOL_PERCENT 8

// BBB-AIRGAP: light the cells covering the visible window and clear the rest. The lit run is sized
// by how much of the list is on screen and positioned by how far down the window has moved, so it
// reads like an ordinary scrollbar even though it moves in whole cells.
static void update_list_scrollbar(
    gui_view_node_t** cells, const size_t offset, const size_t num_visible, const size_t num_items)
{
    JADE_ASSERT(cells);
    JADE_ASSERT(num_visible);
    JADE_ASSERT(num_items > num_visible);

    // At least one cell, however long the list gets
    size_t lit = (num_visible * LIST_VISIBLE_ROWS) / num_items;
    if (!lit) {
        lit = 1;
    }

    // BBB-AIRGAP: the two ends of the bar are claims about the list, so only the two end offsets
    // may make them: the run touches the top exactly at offset 0 and the bottom exactly at the
    // last offset.  Rounding a single ratio to nearest cannot hold that - it collapses the
    // offsets adjacent to an end onto the end itself (measured 2026-09-08 on an eight-item list:
    // lit 2, travel 2, max_offset 4, so offsets 3 and 4 both lit the bottom pair and the bar said
    // 'last row' one row early).  Rounding down fixes the bottom and breaks the top; rounding up
    // does the reverse.  So the ends are assigned outright and only the offsets between them share
    // the positions between them.  travel is at least 2 whenever an interior offset exists: an
    // interior offset needs max_offset >= 2, ie. num_items >= num_visible + 2, and with
    // num_visible == LIST_VISIBLE_ROWS (the only shape that gets a scrollbar, asserted at
    // make_list_activity()) that puts lit at most 2.
    const size_t max_offset = num_items - num_visible;
    const size_t travel = LIST_VISIBLE_ROWS - lit;
    size_t first;
    if (offset == 0) {
        first = 0;
    } else if (offset >= max_offset) {
        first = travel;
    } else {
        JADE_ASSERT(travel >= 2);
        first = 1 + (offset * (travel - 1)) / max_offset;
    }

    for (size_t i = 0; i < LIST_VISIBLE_ROWS; ++i) {
        gui_set_color(cells[i], i >= first && i < first + lit ? TFT_WHITE : TFT_BLACK);
        gui_repaint(cells[i]);
    }
}

// BBB-AIRGAP: the symbols follow the window the same way the labels do. An item without one
// leaves its cell empty rather than the column disappearing, so the labels stay aligned.
static void update_list_symbols(
    gui_view_node_t** symbol_nodes, const list_item_t* items, const size_t offset, const size_t num_visible)
{
    JADE_ASSERT(symbol_nodes);
    JADE_ASSERT(items);

    for (size_t i = 0; i < num_visible; ++i) {
        const char* const symbol = items[offset + i].symbol;
        gui_update_text(symbol_nodes[i], symbol ? symbol : "");
    }
}

// BBB-AIRGAP: a list can be longer than the four rows on screen, and the engine has neither
// clipping nor vertical scrolling, so nothing on screen says there is more below.  A thin bar down
// the right edge carries that.  It is built from LIST_VISIBLE_ROWS cells whose colour changes
// rather than one block that moves, because a split's proportions are fixed once the node tree is
// built - gui_set_colors() (main/gui.c:1470) is the only thing that can be changed afterwards.
// This mirrors make_menu_activity() for the rows themselves, rather than calling it, because the
// rows have to become one side of a horizontal split and that function owns its own layout.
static gui_activity_t* make_list_activity_with_scrollbar(const char* title, btn_data_t* hdrbtns,
    const size_t num_hdrbtns, btn_data_t* rowbtns, const size_t num_rows, gui_view_node_t** scrollbar_cells)
{
    JADE_ASSERT(num_rows == LIST_VISIBLE_ROWS);
    JADE_ASSERT(scrollbar_cells);

    // Borders as the four-item menu sets them: the rows' top line covers the header's bottom edge
    for (size_t i = 0; i < num_hdrbtns; ++i) {
        add_default_border(&hdrbtns[i], GUI_BORDER_SIDES | GUI_BORDER_TOP);
    }
    for (size_t i = 0; i < num_rows; ++i) {
        add_default_border(&rowbtns[i], i == 0 ? GUI_BORDER_TOPBOTTOM : GUI_BORDER_BOTTOM);
    }

    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* const parent = add_title_bar(act, title, hdrbtns, num_hdrbtns, NULL);

    gui_view_node_t* hsplit;
    gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 2, 100 - LIST_SCROLLBAR_PERCENT, GUI_SPLIT_FILL_REMAINING);
    gui_set_parent(hsplit, parent);

    add_buttons(hsplit, UI_COLUMN, rowbtns, num_rows);

    gui_view_node_t* bar;
    // Last cell takes what is left over, for the same reason the split above does
    gui_make_vsplit(&bar, GUI_SPLIT_RELATIVE, LIST_VISIBLE_ROWS, 25, 25, 25, GUI_SPLIT_FILL_REMAINING);
    gui_set_parent(bar, hsplit);
    for (size_t i = 0; i < LIST_VISIBLE_ROWS; ++i) {
        gui_make_fill(&scrollbar_cells[i], TFT_BLACK, FILL_PLAIN, bar);
    }

    return act;
}

gui_activity_t* make_list_activity(const char* title, btn_data_t* hdrbtns, const size_t num_hdrbtns,
    btn_data_t* rowbtns, const size_t num_rows, gui_view_node_t** scrollbar_cells, gui_view_node_t** symbol_nodes)
{
    JADE_ASSERT(title);
    JADE_ASSERT(rowbtns);
    JADE_ASSERT(num_rows);
    JADE_ASSERT(num_rows <= LIST_VISIBLE_ROWS);
    // A scroll indicator only means anything when the window is full and there is more beyond it
    JADE_ASSERT(!scrollbar_cells || num_rows == LIST_VISIBLE_ROWS);

    // Each row carries its own text node instead of a plain label, because the labels are
    // rewritten as the window moves. add_button() only hands the node back when it is passed as
    // 'content' (see above); given 'txt' it builds the node locally and the caller cannot reach it.
    // With a symbol column each row is a split holding the label and the symbol, so the split is
    // what the button gets. The labels are kept here and put back into 'content' afterwards,
    // because that is where the caller reads them from to rewrite as the window moves.
    gui_view_node_t* labels[LIST_VISIBLE_ROWS] = { 0 };

    for (size_t i = 0; i < num_rows; ++i) {
        JADE_ASSERT(rowbtns[i].txt);
        JADE_ASSERT(!rowbtns[i].content);
        gui_make_text_font(&labels[i], rowbtns[i].txt, TFT_WHITE, rowbtns[i].font);
        gui_set_align(labels[i], GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
        rowbtns[i].txt = NULL;

        if (!symbol_nodes) {
            rowbtns[i].content = labels[i];
            continue;
        }

        gui_view_node_t* row_split;
        gui_make_hsplit(&row_split, GUI_SPLIT_RELATIVE, 2, 100 - LIST_SYMBOL_PERCENT, GUI_SPLIT_FILL_REMAINING);
        gui_set_parent(labels[i], row_split);
        gui_make_text_font(&symbol_nodes[i], "", TFT_WHITE, JADE_SYMBOLS_16x16_FONT);
        gui_set_align(symbol_nodes[i], GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
        gui_set_parent(symbol_nodes[i], row_split);
        rowbtns[i].content = row_split;
    }

    gui_activity_t* const act = scrollbar_cells
        ? make_list_activity_with_scrollbar(title, hdrbtns, num_hdrbtns, rowbtns, num_rows, scrollbar_cells)
        : make_menu_activity(title, hdrbtns, num_hdrbtns, rowbtns, num_rows);

    // Restore the contract the caller relies on: 'content' is the label node, whatever the row
    // was built from
    if (symbol_nodes) {
        for (size_t i = 0; i < num_rows; ++i) {
            rowbtns[i].content = labels[i];
        }
    }

    // Selection must not wrap: on the last row 'down' has to leave the selection where it is, so
    // that run_list_activity() can read that press as "move the window" instead. Measured on the
    // emulator: the press still reaches the activity, as select_next_right() posts an event
    // whether or not it moved the selection (main/gui.c:2576-2590).
    act->selectables_wrap = false;

    return act;
}

// BBB-AIRGAP: runs a list of any length over LIST_VISIBLE_ROWS physical rows.
// The window and the engine stay in step because they follow the same rule: while the selection
// has somewhere to go the engine moves it and we count along, and when it does not, we scroll.
// BBB-AIRGAP: which row the engine is highlighting right now. The list cannot keep its own
// selection counter: gui_select_first() (KEY1 on this hardware) moves the highlight without
// posting any event (main/gui.c, select_node() is silent), so a counter would drift away from
// what is on screen and the next press would act on the wrong item. Reading the engine back on
// every event makes that drift impossible.
static bool list_selected_row(const btn_data_t* rowbtns, const size_t num_visible, size_t* row)
{
    JADE_ASSERT(rowbtns);
    JADE_ASSERT(row);

    for (size_t i = 0; i < num_visible; ++i) {
        if (rowbtns[i].btn->is_selected) {
            *row = i;
            return true;
        }
    }
    return false; // the selection is on the title bar above the rows
}

int32_t run_list_activity(
    const char* title, const int32_t exit_ev_id, const list_item_t* items, const size_t num_items, size_t* io_selected)
{
    JADE_ASSERT(title);
    JADE_ASSERT(items);
    JADE_ASSERT(num_items);
    JADE_ASSERT(io_selected);
    JADE_ASSERT(*io_selected < num_items);
    // The row ids and the caller's exit id share one GUI_BUTTON_EVENT space, and the loop below
    // tests the row range first, so an exit id inside that range would be activated as a row.
    JADE_ASSERT(exit_ev_id < BTN_LIST_ROW_0 || exit_ev_id >= BTN_LIST_ROW_0 + LIST_VISIBLE_ROWS);

    const size_t num_visible = num_items < LIST_VISIBLE_ROWS ? num_items : LIST_VISIBLE_ROWS;

    // Open with the remembered selection on screen
    size_t offset = *io_selected < num_visible ? 0 : *io_selected - num_visible + 1;

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = exit_ev_id },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    btn_data_t rowbtns[LIST_VISIBLE_ROWS] = { 0 };
    for (size_t i = 0; i < num_visible; ++i) {
        rowbtns[i].txt = items[offset + i].txt;
        rowbtns[i].font = GUI_DEFAULT_FONT;
        rowbtns[i].ev_id = BTN_LIST_ROW_0 + i;
    }

    // The indicator is only built when there is something off screen to indicate, and the symbol
    // column only when some item actually carries one
    const bool has_scrollbar = num_items > num_visible;
    bool has_symbols = false;
    for (size_t i = 0; i < num_items; ++i) {
        if (items[i].symbol) {
            has_symbols = true;
            break;
        }
    }

    gui_view_node_t* scrollbar_cells[LIST_VISIBLE_ROWS] = { 0 };
    gui_view_node_t* symbol_nodes[LIST_VISIBLE_ROWS] = { 0 };

    gui_activity_t* const act = make_list_activity(title, hdrbtns, 2, rowbtns, num_visible,
        has_scrollbar ? scrollbar_cells : NULL, has_symbols ? symbol_nodes : NULL);

    // While the window is shifted the title bar stops being a directional target, so 'up' from
    // row 0 scrolls the list instead of leaving it. KEY1 ignores the flag and still reaches the
    // exit from anywhere. Kept in step with 'offset' wherever the window moves, below.
    hdrbtns[0].btn->nav_skip = offset > 0;

    if (has_scrollbar) {
        update_list_scrollbar(scrollbar_cells, offset, num_visible, num_items);
    }
    if (has_symbols) {
        update_list_symbols(symbol_nodes, items, offset, num_visible);
    }

    // Register the handlers up-front and await them in the loop below, as camera.c and qrmode.c
    // do for their tight loops. gui_activity_wait_event() would register a fresh handler (and a
    // fresh semaphore) on every iteration, so a press arriving while the four row labels are
    // being rewritten would be signalled to the previous semaphore and lost - exactly the moment
    // this list redraws, when the window scrolls.
    wait_event_data_t* const event_data = gui_activity_make_wait_event_data(act);
    JADE_ASSERT(event_data);

    // Navigate: only the four wheel/dpad ids and KEY1's jump, not ESP_EVENT_ANY_ID. On a single press,
    // gui_wheel_click()/gui_front_click() (main/gui.c:2556-2573) post GUI_BUTTON_EVENT via
    // select_action() and then, unconditionally, their own GUI_EVENT (GUI_WHEEL_CLICK_EVENT/
    // GUI_FRONT_CLICK_EVENT) for the same press. If that click event also matched this
    // registration, both dispatches would give the same event_data: sync_wait_event_handler()
    // (main/utils/event.c) overwrites a single trigger_event_base/id slot, so the second
    // dispatch would erase the first's payload before the loop wakes to read it - dropping the
    // click. The semaphore does not save us either way: libjade backs it with a counting POSIX
    // sem (libjade/include/freertos/semphr.h:38-43), so the loop just wakes twice on the same
    // overwritten slot. Registering only the navigation ids avoids this: the dispatch loop
    // (libjade/esp_event.c:78-79) matches a registration's event_id only when it is
    // ESP_EVENT_ANY_ID or an exact equal, so a click's GUI_EVENT id never matches these
    // registrations and each physical input still produces exactly one dispatch into event_data.
    gui_activity_register_event(act, GUI_EVENT, GUI_WHEEL_UP_EVENT, sync_wait_event_handler, event_data);
    gui_activity_register_event(act, GUI_EVENT, GUI_WHEEL_DOWN_EVENT, sync_wait_event_handler, event_data);
    gui_activity_register_event(act, GUI_EVENT, GUI_WHEEL_LEFT_EVENT, sync_wait_event_handler, event_data);
    gui_activity_register_event(act, GUI_EVENT, GUI_WHEEL_RIGHT_EVENT, sync_wait_event_handler, event_data);
    // KEY1 (gui_select_first, main/gui.c) jumps the selection to the title without a wheel
    // event. The loop below judges each press against where the selection was before it, so
    // it has to see the jump: otherwise 'left' straight after KEY1 finds the title already
    // selected, blames the press for that, and the wrap round to the last item needs a second
    // press. The event is handled by the switch's default case, which is just the state refresh.
    gui_activity_register_event(act, GUI_EVENT, GUI_SELECT_FIRST_EVENT, sync_wait_event_handler, event_data);

    // BBB-AIRGAP: KEY3 leaves the list the same way its title bar does.  Registered by id rather
    // than through ESP_EVENT_ANY_ID for the reason spelled out above: gui_alt_click() (main/gui.c)
    // posts this one event and nothing else for the press, so it still produces exactly one
    // dispatch into event_data.
    gui_activity_register_event(act, GUI_EVENT, GUI_ALT_EVENT, sync_wait_event_handler, event_data);

    // Activate: select_action() (main/gui.c:591-596) only posts GUI_BUTTON_EVENT for the click
    // control configured via gui_click_event, and only when the selected node's click_event_id
    // is not GUI_BUTTON_EVENT_NONE, so ESP_EVENT_ANY_ID here cannot pick up a press on the
    // unconfigured control or the header's blank second button (ev_id GUI_BUTTON_EVENT_NONE
    // above).
    gui_activity_register_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);

    gui_set_activity_initial_selection(rowbtns[*io_selected - offset].btn);
    gui_set_current_activity_sync(act, true);

    // Drain input still in flight from the screen this list replaced. gui_set_current_activity_sync()
    // above blocks until the gui task has fully run the switch (main/gui.c:2389-2424): the
    // outgoing activity's handlers are unregistered and this activity's are registered before it
    // returns, so by this point this list's handlers are live and nothing here has had time to be
    // a real reaction to this screen. libjade still dispatches events on its own pthread
    // (libjade/esp_event.c, _default_event_loop), independently of when they were posted, and
    // every list reuses the same BTN_LIST_ROW_0..3 ids - so a row click posted against the
    // previous list before the switch (e.g. the second click of a fast double-click on the
    // Session fingerprint entry) can still be sitting undispatched and land on this activity's
    // handlers once they go live, silently activating a row nobody pressed here (row 0 = Export
    // Xpub on the Wallet list that opens next). Everything in event_data at this point is that
    // kind of leftover, so it is safe to discard - until the queue is quiet for 10ms, the same
    // idle timeout camera.c:543 and qrmode.c:907 use.
    while (sync_wait_event(event_data, NULL, NULL, NULL, 10 / portTICK_PERIOD_MS) == ESP_OK) {
        // discard - see comment above
    }

    // Read the starting selection from the engine rather than assuming *io_selected is still
    // where it landed: the drain above can discard a navigation event that already moved the
    // engine's selection, which would leave *io_selected pointing at a row that is no longer
    // highlighted. This is safe to read here because it is already set: render_activity()
    // selects the initial node while gui_set_current_activity_sync() is still switching
    // (main/gui.c:2417, first_time path), and that call does not return until the gui task has
    // gone on to save 'done' (main/gui.c:2429) and give it back (main/gui.c:2439).
    size_t row = 0;
    bool on_row = list_selected_row(rowbtns, num_visible, &row);
    bool on_title = hdrbtns[0].btn->is_selected;

    // The title-bar exit must report the absolute item that was last highlighted on a row.
    size_t last_item = on_row ? offset + row : *io_selected;

    while (true) {
        // BBB-AIRGAP: the entry drain can consume KEY3 along with the opening click's tail.
        // Its flag must still leave through the list's own exit before an indefinite wait.
        if (gui_escape_pending()) {
            *io_selected = last_item;
            return exit_ev_id;
        }
        int32_t ev_id;
        esp_event_base_t ev_base;
        if (sync_wait_event(event_data, &ev_base, &ev_id, NULL, 0) != ESP_OK) {
            continue;
        }

        // Activation always comes from GUI_BUTTON_EVENT - the row/exit button's own ev_id,
        // captured at the moment of the click by select_action() (main/gui.c:591-596) - never
        // from the engine's live selection: select_next_right()/select_prev_left() (main/gui.c)
        // can move the highlight and post their own GUI_EVENT before this loop drains a queued
        // click, so reading is_selected here would race with the very next keypress - the bug
        // this fixes. GUI_EVENT is only ever one of the five ids registered above.
        if (ev_base == GUI_BUTTON_EVENT) {
            const int32_t clicked_row = ev_id - BTN_LIST_ROW_0;
            if (clicked_row >= 0 && (size_t)clicked_row < num_visible) {
                *io_selected = offset + (size_t)clicked_row;
                return items[*io_selected].ev_id;
            }
            if (ev_id == exit_ev_id) {
                // last_item is the last row that was highlighted, so leaving through the title
                // bar still reports where the user was; the caller reopens the list there.
                *io_selected = last_item;
                return exit_ev_id;
            }
            continue; // not a button this list owns - ignore and wait for the next event
        }

        // BBB-AIRGAP: the escape leaves through the list's own exit, so the caller sees exactly
        // what it would have seen had the user clicked the title bar, and its own check of
        // gui_escape_pending() carries the cancel on outwards.
        if (ev_id == GUI_ALT_EVENT) {
            *io_selected = last_item;
            return exit_ev_id;
        }

        size_t new_row = 0;
        bool new_on_row = list_selected_row(rowbtns, num_visible, &new_row);

        bool window_moved = false;
        gui_view_node_t* wrap_to = NULL;
        switch (ev_id) {
        // Forward: 'down', plus the 'right' the engine sends when 'down' has nowhere to go
        case GUI_WHEEL_DOWN_EVENT:
        case GUI_WHEEL_RIGHT_EVENT:
            // Selection still on the last row after the press means the engine could not move it,
            // so the window moves under the selection instead.
            if (on_row && new_on_row && new_row == row && new_row == num_visible - 1) {
                if (offset + num_visible < num_items) {
                    ++offset;
                    window_moved = true;
                } else {
                    // BBB-AIRGAP: Jade's menus wrap - past the last item the selection goes round
                    // to the title bar (main/gui.c, selectables_wrap). The engine's wrap is off
                    // here (make_list_activity above), so the list wraps itself once the window
                    // is at the end, and takes the window back to the top so that the next
                    // 'right' lands on item 0, as it does in a menu.
                    if (offset) {
                        offset = 0;
                        window_moved = true;
                    }
                    wrap_to = hdrbtns[0].btn;
                }
            }
            break;

        // Backward mirrors forward while the window is shifted. The skipped title makes gui_up()
        // fall back through select_prev_left() and post GUI_WHEEL_LEFT_EVENT when row 0 cannot move.
        // At offset zero the title becomes a directional target again, keeping the exit reachable.
        case GUI_WHEEL_UP_EVENT:
        case GUI_WHEEL_LEFT_EVENT:
            if (on_row && new_on_row && new_row == row && !new_row && offset) {
                --offset;
                window_moved = true;
            } else if (on_title && hdrbtns[0].btn->is_selected) {
                // BBB-AIRGAP: the mirror image - 'left' on the title bar goes round to the last
                // item, with the window at the end so that it is the item on screen.
                if (offset + num_visible < num_items) {
                    offset = num_items - num_visible;
                    window_moved = true;
                }
                wrap_to = rowbtns[num_visible - 1].btn;
            }
            break;

        default:
            // GUI_SELECT_FIRST_EVENT: nothing to do but the state refresh below
            break;
        }

        if (window_moved) {
            hdrbtns[0].btn->nav_skip = offset > 0;
            for (size_t i = 0; i < num_visible; ++i) {
                gui_update_text(rowbtns[i].content, items[offset + i].txt);
            }
            // The window only ever moves when there is something off screen, so the indicator
            // exists whenever this runs
            update_list_scrollbar(scrollbar_cells, offset, num_visible, num_items);
            if (has_symbols) {
                update_list_symbols(symbol_nodes, items, offset, num_visible);
            }
        }

        if (wrap_to) {
            gui_select_node(wrap_to);
            new_on_row = list_selected_row(rowbtns, num_visible, &new_row);
        }

        // Keep the last row seen: while the title bar is selected there is no row to record.
        if (new_on_row) {
            row = new_row;
        }
        on_row = new_on_row;
        on_title = hdrbtns[0].btn->is_selected;

        // Refresh every event because scrolling changes the absolute item under a selected row.
        if (on_row) {
            last_item = offset + row;
        }
    }
}

// Helper to create an activity to show a message on a single central label
// Can pass title-bar information (optional) and footer buttons (also optional)
gui_activity_t* make_show_message_activity(const char* message[], const size_t message_size, const char* title,
    btn_data_t* hdrbtns, const size_t num_hdrbtns, btn_data_t* ftrbtns, const size_t num_ftrbtns)
{
    JADE_ASSERT(message);
    JADE_ASSERT(message_size);
    JADE_ASSERT(message_size < 5);
    // Header and footer are optional
    JADE_ASSERT(hdrbtns || !num_hdrbtns);
    JADE_ASSERT(ftrbtns || !num_ftrbtns);

    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* parent = act->root_node;

    // Add a titlebar if deisred
    const bool have_hdr = title || num_hdrbtns;
    if (have_hdr) {
        parent = add_title_bar(act, title, hdrbtns, num_hdrbtns, NULL);
    }

    // Message - align center/middle if no carriage returns in message.
    // If multi-line, align top-left and let the caller manage the spacing.
    gui_view_node_t* msgnode;
    size_t toppad = 0;
    if (message_size > 1) {
        // Create a vsplit for the text lines
        const size_t ypct
            = 100 - (have_hdr ? TITLE_BAR_HEIGHT_PCNT : 0) - (num_ftrbtns ? FOOTER_BUTTONS_HEIGHT_PCNT : 0);
        JADE_ASSERT(ypct > 50 && ypct <= 100); // sanity cehck
        const size_t yextent = (ypct * CONFIG_DISPLAY_HEIGHT) / 100;

        const size_t h = MESSAGE_LINE_ROW_HEIGHT; // each text line height, appropriate for the default font height
        const size_t msgextent = message_size * h;
        toppad = msgextent < yextent ? (yextent - msgextent) / 2 : 0; // top padding to centre message
        JADE_LOGD("ypct, yextent, msgextent, toppad: %u, %u, %u, %u", ypct, yextent, msgextent, toppad);
        JADE_ASSERT(toppad < 100); // sanity check

        switch (message_size) {
        case 2:
            gui_make_vsplit(&msgnode, GUI_SPLIT_ABSOLUTE, 2, h, h);
            break;
        case 3:
            gui_make_vsplit(&msgnode, GUI_SPLIT_ABSOLUTE, 3, h, h, h);
            break;
        case 4:
            gui_make_vsplit(&msgnode, GUI_SPLIT_ABSOLUTE, 4, h, h, h, h);
            break;
        default:
            JADE_ASSERT_MSG(false, "Unsupported number of text lines");
        }

        // Create text lines, each one horizontally centered
        for (size_t i = 0; i < message_size; ++i) {
            if (strchr(message[i], '\n')) {
                JADE_LOGW("Multiline message includes explicit \n!!");
            }
            gui_view_node_t* linenode;
            gui_make_text(&linenode, message[i], TFT_WHITE);
            gui_set_align(linenode, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
            gui_set_parent(linenode, msgnode);
        }
    } else {
        // Just create a single text node
        gui_make_text(&msgnode, message[0], TFT_WHITE);

        // Align center/middle if no carriage returns in message, otherwise
        // align top-left and let the caller manage the spacing.
        if (strchr(message[0], '\n')) {
            gui_set_align(msgnode, GUI_ALIGN_LEFT, GUI_ALIGN_TOP);
        } else {
            gui_set_align(msgnode, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
        }
    }

    // Apply any padding above the message, and a small offset from the screen edges
    gui_set_padding(msgnode, GUI_MARGIN_ALL_DIFFERENT, toppad, 2, 0, 2);

    if (!num_ftrbtns) {
        // Just a message, no buttons - just apply straight to the parent
        gui_set_parent(msgnode, parent);
    } else {
        // Relative height of buttons depends on whether there is a header
        gui_view_node_t* vsplit;
        const uint32_t btnheight = (100 * FOOTER_BUTTONS_HEIGHT_PCNT) / (100 - (have_hdr ? TITLE_BAR_HEIGHT_PCNT : 0));
        gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 2, 100 - btnheight, btnheight);
        gui_set_parent(vsplit, parent);

        // Add message to top of vsplit
        gui_set_parent(msgnode, vsplit);

        // Add buttons to below
        add_buttons(vsplit, UI_ROW, ftrbtns, num_ftrbtns);
    }

    return act;
}

// Activity to show a single value
gui_activity_t* make_show_single_value_activity(const char* name, const char* value, const bool show_helpbtn)
{
    JADE_ASSERT(name);
    JADE_ASSERT(value);

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_BACK },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    if (show_helpbtn) {
        hdrbtns[1].txt = "?";
        hdrbtns[1].ev_id = BTN_HELP;
    }

    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* const parent = add_title_bar(act, name, hdrbtns, 2, NULL);

    gui_view_node_t* node;
    gui_make_text_font(&node, value, TFT_WHITE, GUI_DEFAULT_FONT);
    gui_set_align(node, GUI_ALIGN_LEFT, GUI_ALIGN_TOP);
    gui_set_padding(node, GUI_MARGIN_ALL_DIFFERENT, 24, 0, 0, 0);
    gui_set_parent(node, parent);

    return act;
}

// Make activity that displays a simple message - cannot be dismissed by caller
gui_activity_t* display_message_activity(const char* message[], const size_t message_size)
{
    gui_activity_t* const act = make_show_message_activity(message, message_size, NULL, NULL, 0, NULL, 0);
    gui_set_current_activity(act);
    return act;
}

gui_activity_t* display_processing_message_activity()
{
    const char* message[] = { "Processing..." };
    return display_message_activity(message, 1);
}

// Show passed dialog and handle events until a 'yes' or 'no', which is translated into a boolean return
// Destroys the passed activity before returning.
// NOTE: only expect BTN_YES, BTN_NO and BTN_HELP events.
//
// BBB-AIRGAP: escape_result is what KEY3 means on this screen, and it has to be the caller's
// decision because the two families of caller mean opposite things by it.  A question returns
// false: an escape can never be read as 'Yes'.  A message screen returns true: its only button is
// 'Continue', so dismissing it is the only way out, and its caller asserts on the result.
//
// BBB-AIRGAP: 'escaped' is optional, and is how a caller learns that KEY3 - rather than the
// screen's own button - is what closed the screen.  It matters because gui_escape_pending()
// cannot answer that question after the fact: every other press clears the flag
// (gui_escape_clear(), main/gui.c:2627 and its six siblings), so a direction arriving between
// the screen closing and the caller's check would read as consent.  Here the answer is taken
// at the event that closed the screen and no later press can revise it.  Callers whose next
// step is destructive or outward-facing use this; the rest can keep polling the flag, where a
// lost escape costs at most one extra screen.
static bool await_yesno_activity_loop(
    gui_activity_t* const act, const char* help_url, const bool escape_result, bool* const escaped)
{
    JADE_ASSERT(act);
    // help_url is optional (but should be present if a BTN_HELP btn is present)
    // escaped is optional

    if (escaped) {
        *escaped = false;
    }

    gui_activity_t* const prev_act = gui_current_activity(); // Save current activity

    while (true) {
        gui_set_current_activity(act);

        const int32_t ev_id = gui_activity_wait_button(act, BTN_YES);
        // Return true if 'Yes' was pressed, false if 'No'
        switch (ev_id) {
        case BTN_YES:
            return true;

        case BTN_NO:
            return false;

        case BTN_HELP:
            await_qr_help_activity(help_url);
            // BBB-AIRGAP: the escape may have been pressed on the help screen, and that screen
            // consumed the event; there is none left to wake this wait again, so the flag is
            // what carries it.  Without this the question would just be redrawn.
            if (gui_escape_pending()) {
                if (escaped) {
                    *escaped = true;
                }
                return escape_result;
            }
            break;

        case BTN_ESCAPE_HOME:
            if (escaped) {
                *escaped = true;
            }
            return escape_result;

        case BTN_EVENT_TIMEOUT:
            break;

        default:
            JADE_LOGW("Unexpected button event: %ld", ev_id);
            break;
        }
    }
    gui_destroy_current_activity(act, prev_act); // restore previous activity
}

// Run activity that displays a message and awaits an 'ack' button click.
// BBB-AIRGAP: returns true when KEY3 is what dismissed the screen.  Every void wrapper below
// discards that, which is right for a notice that is only a notice; await_message_escaped() is
// for the callers that must not treat an escape as permission to carry on.
static bool await_message_activity(const char* message[], const size_t message_size)
{
    btn_data_t ftrbtn = { .txt = "Continue", .font = GUI_DEFAULT_FONT, .ev_id = BTN_YES, .borders = GUI_BORDER_TOP };

    gui_activity_t* const act = make_show_message_activity(message, message_size, NULL, NULL, 0, &ftrbtn, 1);

    bool escaped = false;
    const bool rslt = await_yesno_activity_loop(act, NULL, true, &escaped);
    JADE_ASSERT(rslt);
    return escaped;
}

// BBB-AIRGAP: the same screen as await_message(), but it reports how the user left it.  Use it
// wherever what follows the notice is destructive, irreversible or outward-facing - a wipe, a
// file overwrite, a reply to the host - because there the difference between 'the user pressed
// Continue' and 'the user asked to leave' is the difference between consent and its opposite.
bool await_message_escaped(const char* message[], const size_t message_size)
{
    JADE_ASSERT(message);
    JADE_ASSERT(message_size);
    return await_message_activity(message, message_size);
}

void await_message(const char* msg)
{
    const char* m[] = { msg };
    await_message_activity(m, 1);
}

// BBB-AIRGAP: a notice whose second line is data the device did not choose - a record name, say.
// await_message_2() cannot carry that safely: the two-line layout gives each line exactly
// MESSAGE_LINE_ROW_HEIGHT, so display_print_in_area() has no second row to wrap into and a wide
// value is cut with nothing to show it was cut (measured 2026-09-08: fifteen 'W' in the default
// font is 299px against 236px of usable width, and a multisig name may be fifteen characters,
// main/multisig.h:11).  Here the caller's fixed words become the title and the value gets the whole
// message area, which is the same shape make_show_single_value_activity() uses for a wallet name.
void await_titled_message(const char* title, const char* msg)
{
    JADE_ASSERT(title);
    JADE_ASSERT(msg);

    btn_data_t ftrbtn = { .txt = "Continue", .font = GUI_DEFAULT_FONT, .ev_id = BTN_YES, .borders = GUI_BORDER_TOP };
    const char* m[] = { msg };

    gui_activity_t* const act = make_show_message_activity(m, 1, title, NULL, 0, &ftrbtn, 1);

    const bool rslt = await_yesno_activity_loop(act, NULL, true, NULL);
    JADE_ASSERT(rslt);
}
void await_message_2(const char* msg1, const char* msg2)
{
    const char* m[] = { msg1, msg2 };
    await_message_activity(m, 2);
}
void await_message_3(const char* msg1, const char* msg2, const char* msg3)
{
    const char* m[] = { msg1, msg2, msg3 };
    await_message_activity(m, 3);
}
void await_message_4(const char* msg1, const char* msg2, const char* msg3, const char* msg4)
{
    const char* m[] = { msg1, msg2, msg3, msg4 };
    await_message_activity(m, 4);
}

void await_error(const char* msg)
{
    const char* m[] = { msg };
    await_message_activity(m, 1);
}
void await_error_2(const char* msg1, const char* msg2)
{
    const char* m[] = { msg1, msg2 };
    await_message_activity(m, 2);
}
void await_error_3(const char* msg1, const char* msg2, const char* msg3)
{
    const char* m[] = { msg1, msg2, msg3 };
    await_message_activity(m, 3);
}

// Generic activity that displays a message and Yes/No buttons, and waits
// for button press.  Function returns true if 'Yes' was pressed.
static bool await_yesno_activity_impl(const char* title, const char* message[], const size_t message_size,
    const char* yes, const char* no, const bool default_selection, const char* help_url)
{
    // title is optional
    JADE_ASSERT(message);
    JADE_ASSERT(message_size);
    JADE_ASSERT(yes);
    JADE_ASSERT(no);
    // help_url is optional - '?' button shown if passed

    btn_data_t hdrbtns[] = { { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE },
        { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_HELP } };

    btn_data_t ftrbtns[] = { { .txt = no, .font = GUI_DEFAULT_FONT, .ev_id = BTN_NO, .borders = GUI_BORDER_TOPRIGHT },
        { .txt = yes, .font = GUI_DEFAULT_FONT, .ev_id = BTN_YES, .borders = GUI_BORDER_TOPLEFT } };

    gui_activity_t* const act
        = make_show_message_activity(message, message_size, title, hdrbtns, help_url ? 2 : 0, ftrbtns, 2);
    gui_set_activity_initial_selection(ftrbtns[default_selection ? 1 : 0].btn);

    return await_yesno_activity_loop(act, help_url, false, NULL);
}

// Generic Yes/No activity
bool await_yesno_activity(const char* title, const char* message[], const size_t message_size,
    const bool default_selection, const char* help_url)
{
    return await_yesno_activity_impl(title, message, message_size, "Yes", "No", default_selection, help_url);
}

// Variant of the Yes/No activity that is instead Skip/Yes
bool await_skipyes_activity(const char* title, const char* message[], const size_t message_size,
    const bool default_selection, const char* help_url)
{
    return await_yesno_activity_impl(title, message, message_size, "Yes", "Skip", default_selection, help_url);
}

// BBB-AIRGAP: variant of the Yes/No activity with caller-supplied labels, for questions that
// are a choice between two named options rather than a confirmation.  Returns true when the
// first label was chosen.
bool await_choice_activity(const char* title, const char* message[], const size_t message_size, const char* yes_txt,
    const char* no_txt, const bool default_selection, const char* help_url)
{
    return await_yesno_activity_impl(title, message, message_size, yes_txt, no_txt, default_selection, help_url);
}

// Variant of the Yes/No activity that is instead Continue/Back (latter in title bar)
bool await_continueback_activity(const char* title, const char* message[], const size_t message_size,
    const bool default_selection, const char* help_url)
{
    // title is optional
    JADE_ASSERT(message);
    JADE_ASSERT(message_size);
    // help_url is optional - '?' button shown if passed

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_NO },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    // Optionally add help btn
    if (help_url) {
        hdrbtns[1].txt = "?";
        hdrbtns[1].font = GUI_TITLE_FONT;
        hdrbtns[1].ev_id = BTN_HELP;
    }

    btn_data_t ftrbtn = { .txt = "Continue", .font = GUI_DEFAULT_FONT, .ev_id = BTN_YES, .borders = GUI_BORDER_TOP };

    gui_activity_t* const act = make_show_message_activity(message, message_size, title, hdrbtns, 2, &ftrbtn, 1);
    gui_set_activity_initial_selection((default_selection ? ftrbtn : hdrbtns[0]).btn);

    return await_yesno_activity_loop(act, help_url, false, NULL);
}

// Updatable label with left/right arrows
gui_activity_t* make_carousel_activity(const char* title, gui_view_node_t** label, gui_view_node_t** item)
{
    JADE_ASSERT(title);
    // label is optional
    JADE_INIT_OUT_PPTR(item);

    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* parent = add_title_bar(act, title, NULL, 0, NULL);
    gui_view_node_t* node;

    gui_view_node_t* vsplit;
    if (label) {
        gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 3, 40, 35, 25);
        gui_set_parent(vsplit, parent);

        // Updateable label
        gui_make_fill(&node, TFT_BLACK, FILL_PLAIN, vsplit);

        gui_make_text(label, "", TFT_WHITE);
        gui_set_padding(*label, GUI_MARGIN_ALL_DIFFERENT, 0, 8, 0, 0);
        gui_set_align(*label, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
        gui_set_parent(*label, node);
    } else {
        gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 3, 25, 35, 40);
        gui_set_parent(vsplit, parent);

        gui_make_fill(&node, TFT_BLACK, FILL_PLAIN, vsplit);
    }

    // Background fill
    gui_make_fill(&node, gui_get_highlight_color(), FILL_HIGHLIGHT, vsplit);

    gui_view_node_t* hsplit;
    gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 3, 10, 80, 10);
    gui_set_parent(hsplit, node);

    // Left arrow
    gui_make_text_font(&node, "H", TFT_WHITE, JADE_SYMBOLS_16x16_FONT);
    gui_set_align(node, GUI_ALIGN_RIGHT, GUI_ALIGN_MIDDLE);
    gui_set_parent(node, hsplit);

    // Updateable carousel item
    gui_make_fill(&node, gui_get_highlight_color(), FILL_HIGHLIGHT, hsplit);

    gui_make_text(item, "", TFT_WHITE);
    gui_set_align(*item, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(*item, node);

    // Right arrow
    gui_make_text_font(&node, "I", TFT_WHITE, JADE_SYMBOLS_16x16_FONT);
    gui_set_align(node, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_parent(node, hsplit);

    return act;
}

// BBB-AIRGAP: see ui.h.  The loop is the one upstream writes in handle_qr_options() (main/qrmode.c)
// and handle_screen_brightness() (main/process/dashboard.c), for a fixed set of labels.
size_t await_carousel_activity(
    const char* title, const char* const* labels, const size_t num_labels, const size_t initial)
{
    JADE_ASSERT(title);
    JADE_ASSERT(labels);
    JADE_ASSERT(num_labels > 1);
    JADE_ASSERT(initial < num_labels);

    gui_view_node_t* item = NULL;
    gui_activity_t* const act = make_carousel_activity(title, NULL, &item);
    JADE_ASSERT(item);

    size_t index = initial;
    gui_update_text(item, labels[index]);

    // One registration held for the whole screen, exactly as the dice entry screen does
    // (main/entropy_sources.c) - the same shape of screen, wheel and click with no clickable node.
    // gui_activity_wait_event() attaches a fresh handler and a fresh semaphore to the activity on
    // every call and never detaches either (main/gui.c). In a loop that costs two things a user can
    // feel: a click arriving between two iterations is signalled to the previous, already abandoned
    // semaphore and lost, and the handler list grows for as long as the screen stays open.
    // ESP_EVENT_ANY_ID is safe here for the reason run_list_activity() above cannot use it: the
    // carousel has no clickable node, so select_action() (main/gui.c) posts no GUI_BUTTON_EVENT for
    // a press here and each press produces exactly one dispatch into event_data.
    wait_event_data_t* const event_data = gui_activity_make_wait_event_data(act);
    JADE_ASSERT(event_data);
    gui_activity_register_event(act, GUI_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);

    // Switch synchronously so this activity's handlers are live before the drain below, then throw
    // away what is already in flight. This screen is reached by clicking a menu row, and that one
    // physical press posts its GUI_BUTTON_EVENT and then its own GUI_EVENT click
    // (gui_wheel_click()/gui_front_click(), main/gui.c). With the asynchronous switch that trailing
    // click could be dispatched to this activity once its handlers went live and be read as
    // confirmation, returning the initial value before the user saw a usable screen. Same drain and
    // the same 10ms idle timeout as run_list_activity().
    gui_set_current_activity_sync(act, false);
    while (sync_wait_event(event_data, NULL, NULL, NULL, 10 / portTICK_PERIOD_MS) == ESP_OK) {
        // discard - see comment above
    }

    while (true) {
        int32_t ev_id;
        // BBB-AIRGAP: KEY3 drained during the synchronous switch is still an escape, not a
        // reason to wait forever for another event or save the currently previewed setting.
        if (gui_escape_pending()) {
            return initial;
        }
        if (sync_wait_event(event_data, NULL, &ev_id, NULL, 0) != ESP_OK) {
            continue;
        }
        if (ev_id == GUI_WHEEL_LEFT_EVENT) {
            index = (index + num_labels - 1) % num_labels;
        } else if (ev_id == GUI_WHEEL_RIGHT_EVENT) {
            index = (index + 1) % num_labels;
        } else if (ev_id == gui_get_click_event()) {
            return index;
        } else if (ev_id == GUI_ALT_EVENT) {
            // BBB-AIRGAP: leave without applying anything - the caller gets the value it came in
            // with, so an escape out of a settings carousel changes no setting.
            return initial;
        } else {
            continue;
        }
        gui_update_text(item, labels[index]);
    }
}

// Function to update the highlight colour used for the selection
void update_carousel_highlight_color(const gui_view_node_t* text_label, const color_t color, const bool repaint)
{
    // Assert is label in carousel as created above
    JADE_ASSERT(text_label);
    JADE_ASSERT(text_label->kind == TEXT);
    JADE_ASSERT(text_label->parent);
    JADE_ASSERT(text_label->parent->kind == FILL);
    JADE_ASSERT(text_label->parent->parent);
    JADE_ASSERT(text_label->parent->parent->kind == HSPLIT);
    JADE_ASSERT(text_label->parent->parent->parent);
    JADE_ASSERT(text_label->parent->parent->parent->kind == FILL);

    // Update the selection colour of the two fill elements
    gui_set_color(text_label->parent, color);
    gui_set_color(text_label->parent->parent->parent, color);

    // Repaint if requested
    if (repaint) {
        gui_repaint(text_label->parent->parent->parent);
    }
}

// The progress-bar structure indicated is populated, and should be used to update the progress
// using the update_progress_bar() function below.
void make_progress_bar(gui_view_node_t* parent, progress_bar_t* progress_bar)
{
    JADE_ASSERT(parent);
    JADE_ASSERT(progress_bar);

    // A progress-bar can be transparent, but should the value decrease the parent would
    // need to be redrawn to reduce the amount of 'fill' in the bar.
    if (progress_bar->transparent) {
        gui_make_vsplit(&progress_bar->container, GUI_SPLIT_RELATIVE, 1, 100);
        gui_make_vsplit(&progress_bar->progress_bar, GUI_SPLIT_RELATIVE, 1, 100);
    } else {
        gui_make_fill(&progress_bar->container, TFT_BLACK, FILL_PLAIN, NULL);
        gui_make_fill(&progress_bar->progress_bar, TFT_BLACK, FILL_PLAIN, NULL);
    }

    gui_set_borders(progress_bar->container, TFT_WHITE, 2, GUI_BORDER_ALL);
    gui_set_margins(progress_bar->container, GUI_MARGIN_TWO_VALUES, 4, 16);
    gui_set_parent(progress_bar->container, parent);

    gui_set_margins(progress_bar->progress_bar, GUI_MARGIN_ALL_EQUAL, 2);
    gui_set_borders(progress_bar->progress_bar, gui_get_highlight_color(), 0, GUI_BORDER_LEFT);
    gui_set_parent(progress_bar->progress_bar, progress_bar->container);
}

// Create a progress bar screen, with the given title.
// The progress-bar structure indicated is populated, and should be used to update the progress
// using the update_progress_bar() function below.
gui_activity_t* make_progress_bar_activity(const char* title, const char* message, progress_bar_t* progress_bar)
{
    JADE_ASSERT(title);
    JADE_ASSERT(message);
    JADE_ASSERT(progress_bar);

    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* parent = add_title_bar(act, title, NULL, 0, NULL);
    gui_view_node_t* node;

    gui_view_node_t* vsplit;
    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 3, 25, 45, 30);
    gui_set_parent(vsplit, parent);

    // First row, message text
    gui_make_text(&node, message, TFT_WHITE);
    gui_set_parent(node, vsplit);
    gui_set_padding(node, GUI_MARGIN_TWO_VALUES, 0, 12);
    gui_set_align(node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);

    // second row, progress bar
    make_progress_bar(vsplit, progress_bar);

    // third row, percentage text
    gui_make_text(&node, "0%", TFT_WHITE);
    gui_set_align(node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    progress_bar->pcnt_txt = node;

    gui_make_fill(&node, TFT_BLACK, FILL_PLAIN, vsplit);

    gui_set_parent(progress_bar->pcnt_txt, node);

    return act;
}

void update_progress_bar(progress_bar_t* progress_bar, const size_t total, const size_t current)
{
    JADE_ASSERT(progress_bar);
    JADE_ASSERT(progress_bar->progress_bar);
    // progress_bar->pcnt_txt is optional

    JADE_ASSERT(current <= total);
    JADE_ASSERT(total > 0);
    JADE_ASSERT(progress_bar->percent_last_value <= 100);

    const uint8_t pcnt = 100 * current / total;
    if (pcnt == progress_bar->percent_last_value) {
        // percentage hasn't changed, skip update
        return;
    }

    if (!progress_bar->progress_bar->render_data.is_first_time) {
        // Can only reliably update the progress bar after its initial rendering
        const uint16_t constraints_x1 = progress_bar->progress_bar->render_data.original_constraints.x1;
        const uint16_t constraints_x2 = progress_bar->progress_bar->render_data.original_constraints.x2;
        const gui_margin_t* const margins = &progress_bar->progress_bar->margins;
        const uint16_t width_bar = constraints_x2 - constraints_x1 - margins->left - margins->right;
        const uint16_t width_shaded = width_bar * current / total;

        gui_set_borders(progress_bar->progress_bar, gui_get_highlight_color(), width_shaded, GUI_BORDER_LEFT);
        gui_repaint(progress_bar->progress_bar);
    }

    // Update the % progress text label if present
    if (progress_bar->pcnt_txt) {
        char text[8];
        const int ret = snprintf(text, sizeof(text), "%u%%", pcnt);
        JADE_ASSERT(ret > 0 && ret < sizeof(text));
        gui_update_text(progress_bar->pcnt_txt, text);
    }

    progress_bar->percent_last_value = pcnt;
}
#endif // AMALGAMATED_BUILD
