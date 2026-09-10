#ifndef AMALGAMATED_BUILD
#include "../button_events.h"
#include "../jade_assert.h"
#include "../ui.h"

#ifdef HAVE_CAMERA_ROTATION_SETTING
// BBB-AIRGAP: how far the header buttons are inset from the screen corner; see the comment on the
// gui_set_margins calls below for where the number comes from.
#define CAMERA_HEADER_BTN_INSET 11
#endif

gui_activity_t* make_camera_activity(gui_view_node_t** image_node, gui_view_node_t** label_node,
    const bool show_click_btn, const qr_guide_type_t qr_guide_type, progress_bar_t* progress_bar,
    const bool show_help_btn)
{
    // progress bar is optional
    JADE_INIT_OUT_PPTR(image_node);
    JADE_INIT_OUT_PPTR(label_node);

    // NOTE: atm show_click_btn and help_url are mutually exclusive
    JADE_ASSERT(!show_click_btn || !show_help_btn);

    gui_activity_t* const act = gui_make_activity();

    // Whole screen image
    gui_make_picture(image_node, NULL);
    gui_set_align(*image_node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(*image_node, act->root_node);
    gui_view_node_t* parent = *image_node;

    // QR frame guide if applicable
    if (qr_guide_type == QR_GUIDE_SHOW) {
        gui_make_qrguide(&parent, TFT_WHITE);
        gui_set_parent(parent, *image_node);
    }

    gui_view_node_t* vsplit;
    gui_make_vsplit(
        &vsplit, GUI_SPLIT_RELATIVE, 3, CAMERA_HEADER_PCNT, 100 - (2 * CAMERA_HEADER_PCNT), CAMERA_HEADER_PCNT);
    gui_set_parent(vsplit, parent);

    // Header row buttons - back and either help or 'click'
    btn_data_t hdrbtns[]
        = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_CAMERA_EXIT, .borders = GUI_BORDER_ALL },
              { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_CAMERA_HELP, .borders = GUI_BORDER_ALL } };

    if (show_click_btn) {
        hdrbtns[1].txt = "S";
        hdrbtns[1].font = VARIOUS_SYMBOLS_FONT;
        hdrbtns[1].ev_id = BTN_CAMERA_CLICK;
    }

    gui_view_node_t* hsplit;
#ifdef HAVE_CAMERA_ROTATION_SETTING
    // BBB-AIRGAP: square cells the height of the header row, so the buttons in them can be inset
    // into the corners rather than filling them.  Upstream's 15/70/15 works because upstream draws
    // the camera image 70% of the screen wide (UI_DISPLAY_WIDTH, main/camera.c) and those edge
    // cells land in the black margins either side of it.  This fork draws the image across the
    // whole screen, so the same cells landed on the image, on top of the QR guide's two upper
    // corner marks; the opaque button fill covered 114 of each mark's 120 pixels.
    gui_make_hsplit(
        &hsplit, GUI_SPLIT_RELATIVE, 3, CAMERA_HEADER_PCNT, 100 - (2 * CAMERA_HEADER_PCNT), CAMERA_HEADER_PCNT);
#else
    gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 3, 15, 70, 15);
#endif
    gui_set_parent(hsplit, vsplit);

    // Back/cancel button
    add_button(hsplit, &hdrbtns[0]);

    gui_make_text(label_node, "Initializing...", TFT_WHITE);
    gui_set_align(*label_node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);

#ifdef HAVE_CAMERA_ROTATION_SETTING
    // BBB-AIRGAP: the label is the header's middle cell here, not the middle of the screen, where
    // it was printed straight across the centre of the code being scanned.  It also becomes the
    // only thing between the two buttons, so no spacer is needed to push the second one to the end.
    gui_set_parent(*label_node, hsplit);

    // Any help or 'click' button, if required
    if (show_help_btn || show_click_btn) {
        add_button(hsplit, &hdrbtns[1]);
    }

    // BBB-AIRGAP: shrink each button to a 26x26 box inset from the screen corner.  The guide's
    // corner mark ends its arms at x 8-9 and y 8-9 (render_qrguide, main/gui.c), so a box starting
    // at 10 clears it; 11 leaves a pixel between the two so they do not read as one thick frame.
    // The cell is 48x48 (HAVE_CAMERA_ROTATION_SETTING is only defined for a 240x240 panel), which
    // is what makes the margin square.  Colours are untouched: the fill is still whatever
    // Display > Theme is set to and the border is still GUI_BLOCKSTREAM_BUTTONBORDER_GREY.
    gui_set_margins(hdrbtns[0].btn, GUI_MARGIN_ALL_EQUAL, CAMERA_HEADER_BTN_INSET);
    if (show_help_btn || show_click_btn) {
        gui_set_margins(hdrbtns[1].btn, GUI_MARGIN_ALL_EQUAL, CAMERA_HEADER_BTN_INSET);
    }
#else
    // Any help or 'click' button, if required
    if (show_help_btn || show_click_btn) {
        gui_view_node_t* spacer;
        gui_make_vsplit(&spacer, GUI_SPLIT_RELATIVE, 1, 100); // no-op transparent spacer
        gui_set_parent(spacer, hsplit);
        add_button(hsplit, &hdrbtns[1]);
    }

    // Text label across the centre
    gui_set_parent(*label_node, vsplit);
#endif

    // Bottom part, any progress bar if applicable (transparent)
    if (progress_bar) {
#ifdef HAVE_CAMERA_ROTATION_SETTING
        // BBB-AIRGAP: the label no longer takes the middle row, so a transparent spacer takes it
        // and the progress bar still lands in the bottom one.
        gui_view_node_t* spacer;
        gui_make_vsplit(&spacer, GUI_SPLIT_RELATIVE, 1, 100); // no-op transparent spacer
        gui_set_parent(spacer, vsplit);
#endif
        progress_bar->transparent = true;
        make_progress_bar(vsplit, progress_bar);
        gui_set_borders(progress_bar->container, GUI_BLOCKSTREAM_BUTTONBORDER_GREY, 1, GUI_BORDER_ALL);
        gui_set_margins(progress_bar->container, GUI_MARGIN_ALL_DIFFERENT, 12, 12, 4, 12);
    }

    return act;
}
#endif // AMALGAMATED_BUILD
