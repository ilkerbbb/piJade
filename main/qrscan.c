#ifndef AMALGAMATED_BUILD
#include <quirc.h>
#include <string.h>

#include "camera.h"
#include "idletimer.h"
#include "jade_assert.h"
#include "jade_wally_verify.h"
#include "qr_downscale.h"
#include "qrscan.h"
#include "sensitive.h"
#include "utils/malloc_ext.h"
#include "utils/util.h"

#define SCAN_MARGIN 20

// BBB-AIRGAP: both entry points below need the same pair of quirc instances and the same decoder
// scratch, and both have to release them.  Kept in one place because there are now two instances
// to keep in step: the half-scale one has to be sized from the same scan window as the full-scale
// one, or the fallback pass in qr_recognize() reads the wrong number of pixels.
static void qr_scanner_init(qr_data_t* qr_data)
{
    JADE_ASSERT(qr_data);
    JADE_ASSERT(!qr_data->q);
    JADE_ASSERT(!qr_data->q_half);
    JADE_ASSERT(!qr_data->ds);

    // Size the internal image buffers since we know the size of the camera images.
    // These are then reused for every camera image frame processed.
    const uint16_t scan_width = min_u16(CAMERA_IMAGE_WIDTH, CAMERA_IMAGE_HEIGHT) - SCAN_MARGIN;
    JADE_ASSERT(scan_width % 2 == 0); // qr_downscale_half() halves this window

    qr_data->q = quirc_new();
    JADE_ASSERT(qr_data->q);
    int qret = quirc_resize(qr_data->q, scan_width, scan_width);
    JADE_ASSERT(qret == 0);

    qr_data->q_half = quirc_new();
    JADE_ASSERT(qr_data->q_half);
    qret = quirc_resize(qr_data->q_half, scan_width / 2, scan_width / 2);
    JADE_ASSERT(qret == 0);

    qr_data->len = 0;

    // BBB-AIRGAP: upstream logs these at ERROR level (719fa40c), but they are not errors - the
    // scan box is a fixed calculation from the camera size. On this device the log is a product
    // feature (T3.13 added --log-level), so a line that reads ERROR has to BE an error; two
    // fake ones on every QR scan both bury real failures and make the audit tool report a
    // finding where nothing is wrong. Kept at debug level rather than deleted: the value is
    // worth having when the scan box is being tuned for a different panel.
    JADE_LOGD("SCAN WIDTH: %u", scan_width);
    JADE_LOGD("SCAN HEIGHT: %u", scan_width);

    qr_data->ds = JADE_MALLOC_PREFER_DRAM(sizeof(struct datastream));
    qr_data->ds->data = JADE_MALLOC_PREFER_DRAM(QUIRC_MAX_PAYLOAD * sizeof(uint8_t));
}

// BBB-AIRGAP: quirc_destroy() releases the image buffer without clearing it, and in a SeedQR
// scan that buffer holds the frame the mnemonic was read from.  quirc_begin() is the public way
// to reach the buffer and its dimensions, so the component's internal header stays out of this
// file; on this path its only side effect is resetting three counters on a struct that is freed
// on the next line.  The buffer is single: QUIRC_MAX_REGIONS is 254, so quirc_pixel_t is uint8_t
// and identify.c aliases q->pixels onto q->image rather than allocating a second one.
static void quirc_wipe_and_destroy(struct quirc* q)
{
    JADE_ASSERT(q);

    int width = 0;
    int height = 0;
    uint8_t* const image = quirc_begin(q, &width, &height);
    JADE_ASSERT(image);
    JADE_ASSERT(width > 0);
    JADE_ASSERT(height > 0);
    JADE_WALLY_VERIFY(wally_bzero(image, (size_t)width * (size_t)height));

    quirc_destroy(q);
}

static void qr_scanner_destroy(qr_data_t* qr_data)
{
    JADE_ASSERT(qr_data);

    // BBB-AIRGAP: every buffer a scan allocates is wiped before it is released.  In a SeedQR
    // scan the two image buffers hold the frame the mnemonic was read from, and the decoder
    // scratch holds the mnemonic itself in two places: the struct's own raw[QUIRC_MAX_PAYLOAD]
    // carries the codewords read off the symbol, and the separately allocated ds->data carries
    // the payload they decode to.  The struct was measured with 27 non-zero bytes at free() on
    // a 12-word scan, which is why it is wiped as well and not just the buffer hanging off it.
    // This closes the camera-buffer gap tracked as phase 1.3 of the hardening round, and it
    // lands here, once, for both instances.
    JADE_WALLY_VERIFY(wally_bzero(qr_data->ds->data, QUIRC_MAX_PAYLOAD * sizeof(uint8_t)));
    free(qr_data->ds->data);
    qr_data->ds->data = NULL;
    JADE_WALLY_VERIFY(wally_bzero(qr_data->ds, sizeof(struct datastream)));
    free(qr_data->ds);
    qr_data->ds = NULL;
    quirc_wipe_and_destroy(qr_data->q);
    qr_data->q = NULL;
    quirc_wipe_and_destroy(qr_data->q_half);
    qr_data->q_half = NULL;
}

// Inspect qrcodes and try to extract payload - whether any were seen and any
// string data extracted are stored in the qr_data struct passed.
// BBB-AIRGAP: 'q' is passed rather than taken from qr_data because there are now two instances
// to extract from - the full-scale window and the halved one.
static bool qr_extract_payload(qr_data_t* qr_data, struct quirc* const q)
{
    JADE_ASSERT(qr_data);
    JADE_ASSERT(q);
    JADE_ASSERT(qr_data->ds);

    qr_data->data[0] = '\0';
    qr_data->len = 0;

    const int count = quirc_count(q);
    if (count <= 0) {
        return false;
    }
    JADE_LOGI("Detected %d QR codes in image.", count);

    // Store the first string we manage to extract - initialise to empty string.
    struct quirc_data data;
    SENSITIVE_PUSH(&data, sizeof(data));

    // Look for a string
    for (int i = 0; i < count; ++i) {
        struct quirc_code code;
        quirc_extract(q, i, &code);

        const quirc_decode_error_t error_status = quirc_decode(&code, &data, qr_data->ds);
        if (error_status != QUIRC_SUCCESS) {
            JADE_LOGW("QUIRC error %s", quirc_strerror(error_status));
        } else if (data.data_type == QUIRC_DATA_TYPE_KANJI) {
            JADE_LOGW("QUIRC unexpected data type: %d", data.data_type);
        } else if (!data.payload_len) {
            JADE_LOGW("QUIRC empty string");
        } else if (data.payload_len >= sizeof(qr_data->data)) {
            JADE_LOGW("QUIRC data too long to handle: %u", data.payload_len);
            JADE_ASSERT(data.payload_len <= sizeof(data.payload));
        } else {
            // The payload appears to be a nul terminated string, but the
            // 'payload_len' seems to be the string length not including that
            // terminator.
            // To avoid any confusion or grey areas, we copy the bytes,
            // and then explicitly add the nul terminator ourselves.
            memcpy(qr_data->data, data.payload, data.payload_len);
            qr_data->data[data.payload_len] = '\0';
            qr_data->len = data.payload_len;
            SENSITIVE_POP(&data);
            return true;
        }
    }
    SENSITIVE_POP(&data);
    return false;
}

// Look for qr-codes, and if found extract any string data into the camera_data passed
static bool qr_recognize(
    const size_t width, const size_t height, const uint8_t* data, const size_t len, void* ctx_qr_data)
{
    JADE_ASSERT(data);
    JADE_ASSERT(ctx_qr_data);
    JADE_ASSERT(len == width * height);

    qr_data_t* const qr_data = (qr_data_t*)ctx_qr_data;
    JADE_ASSERT(qr_data);
    JADE_ASSERT(qr_data->q);

    // Checked qr image buffer exists and is an acceptable size
    int quirc_width = 0, quirc_height = 0;
    uint8_t* const quirc_image = quirc_begin(qr_data->q, &quirc_width, &quirc_height);
    JADE_ASSERT(quirc_image);
    JADE_ASSERT(quirc_width <= width);
    JADE_ASSERT(quirc_height <= height);

    // Crop to central area of image (zero offsets when the whole image is the scan window)
    const uint16_t xoffset = (width - quirc_width) / 2;
    const uint16_t yoffset = (height - quirc_height) / 2;
    if (quirc_width == width && quirc_height == height) {
        // Whole image optimisation
        memcpy(quirc_image, data, len);
    } else {
        for (uint16_t y = 0; y < quirc_height; ++y) {
            memcpy(quirc_image + (y * quirc_width), data + ((y + yoffset) * width) + xoffset, quirc_width);
        }
    }
    quirc_end(qr_data->q);

    bool found = qr_extract_payload(qr_data, qr_data->q) && qr_data->len;

    // BBB-AIRGAP: second pass at half scale.  A code held close enough to fill the frame has
    // modules wider than about 7 pixels, and quirc then finds the finder patterns but cannot
    // build the grid - measured 0/19 on held device frames at this scale, 11/19 halved
    // (main/qr_downscale.h carries the measurement and the choice of kernel).
    //
    // Full scale is tried FIRST so that nothing which works today gets slower: a frame that
    // decodes at 460 never reaches this branch, and the halving reads the camera frame directly,
    // so a successful scan pays nothing at all for this.  The reverse order would have made the
    // dense descriptor codes of item 44 pay on every frame.
    //
    // The ordering inside this function matters too: quirc_end() thresholds its own buffer in
    // place, so the source here is the camera frame rather than qr_data->q's image, which is no
    // longer grayscale by this point.
    if (!found && qr_data->q_half) {
        int half_width = 0, half_height = 0;
        uint8_t* const half_image = quirc_begin(qr_data->q_half, &half_width, &half_height);
        JADE_ASSERT(half_image);
        JADE_ASSERT(half_width == quirc_width / 2);
        JADE_ASSERT(half_height == quirc_height / 2);

        qr_downscale_half(data + ((size_t)yoffset * width) + xoffset, quirc_width, quirc_height, width, half_image);
        quirc_end(qr_data->q_half);

        found = qr_extract_payload(qr_data, qr_data->q_half) && qr_data->len;
    }

    // If no QR data can be recognised/extracted, return false
    if (!found) {
        qr_data->len = 0;
        return false;
    }

    // If we have extracted data and we have an additional validation
    // function, run that function now - clear the data and return false
    // if it fails.  Otherwise all good.
    if (qr_data->is_valid && !qr_data->is_valid(qr_data)) {
        qr_data->len = 0;
        return false;
    }

    // Make the completed QR image capture count as 'activity' against the idle timer
    idletimer_register_activity(true);

    // QR data was extracted and validated - return true
    return true;
}

#ifdef CONFIG_DEBUG_MODE
// Function to scan single image - may be useful for testing
bool scan_qr(const size_t width, const size_t height, const uint8_t* data, const size_t len, qr_data_t* qr_data)
{
    JADE_ASSERT(qr_data);

    // Create the quirc structs - destroyed below
    qr_scanner_init(qr_data);

    const bool ret = qr_recognize(width, height, data, len, qr_data);

    qr_scanner_destroy(qr_data);

    // Any scanned qr code will be in the qr_data passed
    return ret && qr_data->len > 0;
}
#endif // CONFIG_DEBUG_MODE

// Main entry point to run camera task to capture frames and scan each
// image until a valid qr-code is found ('valid' as defined by the caller).
bool jade_camera_scan_qr(
    qr_data_t* qr_data, const char* text_label, const qr_guide_type_t qr_guide_type, const char* help_url)
{
    JADE_ASSERT(qr_data);
    // text_label is optional
    JADE_ASSERT(qr_guide_type != QR_GUIDE_HIDE);
    // help_url is optional

#ifdef CONFIG_HAS_CAMERA
    // Remember the activity that was showing before the camera
    gui_activity_t* const prev_act = gui_current_activity();

    // Create the quirc structs (reused for each frame) - destroyed below
    qr_scanner_init(qr_data);

    // Run the camera task trying to interpet frames as qr-codes
    const bool show_camera_ui = true;
    const bool show_click_button = false;
    gui_activity_t* camera_act = NULL;
    jade_camera_process_images(qr_recognize, qr_data, show_camera_ui, text_label, show_click_button, qr_guide_type,
        help_url, qr_data->progress_bar, &camera_act, NULL);

    // Destroy the camera activity that was created by the camera task
    // and restore the previous activity.
    if (camera_act) {
        gui_destroy_current_activity(camera_act, prev_act);
    }

    // Destroy the quirc structs created above
    qr_scanner_destroy(qr_data);

    // Any scanned qr code will be in the qr_data passed
    return qr_data->len > 0;
#else // CONFIG_HAS_CAMERA
    JADE_LOGW("No camera supported for this device");
    await_error("No camera detected");
    return false;
#endif
}
#endif // AMALGAMATED_BUILD
