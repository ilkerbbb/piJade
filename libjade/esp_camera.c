#include "esp_camera.h"
#include "camera.h"
#include "jade_assert.h"
#include "jade_log.h"
#include "jade_wally_verify.h"
#include "libjade.h"
#include "sdkconfig.h"
#include <string.h>
#include <time.h>
#include <wally_core.h>

// BBB-AIRGAP: a host builds its frames from the libjade.h values and cannot see camera.h; these
// two lines are what keeps the copy honest.
_Static_assert(LIBJADE_CAMERA_FRAME_WIDTH == CAMERA_IMAGE_WIDTH,
    "camera frame width drifted; update LIBJADE_CAMERA_FRAME_WIDTH in libjade/libjade.h");
_Static_assert(LIBJADE_CAMERA_FRAME_HEIGHT == CAMERA_IMAGE_HEIGHT,
    "camera frame height drifted; update LIBJADE_CAMERA_FRAME_HEIGHT in libjade/libjade.h");

#ifdef CONFIG_LIBJADE_CAMERA

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>

static uint8_t _cam_frame_buffer[CAMERA_IMAGE_WIDTH * CAMERA_IMAGE_HEIGHT];
// BBB-AIRGAP: esp_camera_fb_get() releases the mutex before handing the buffer back, so a host
// that feeds frames continuously would overwrite the frame while Jade is still decoding a QR code
// or hashing it for entropy. Torn frames are the visible symptom; the dangerous one is that a
// repeating cycle of sensor frames can look unique to the entropy duplicate check. The consumer
// therefore gets its own copy, untouched until it asks for the next frame.
static uint8_t _cam_delivered_buffer[CAMERA_IMAGE_WIDTH * CAMERA_IMAGE_HEIGHT];
static uint64_t _cam_frame_count = 0;
static bool _cam_stopped = false;
// BBB-AIRGAP: on the Pi the host owns the camera device and opens it on demand, so it needs to
// know when Jade actually wants frames. _cam_stopped cannot answer that: it starts out false,
// meaning "not stopped", which is not the same as "wanted".
static bool _cam_wanted = false;
static pthread_mutex_t _cam_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t _cam_cond = PTHREAD_COND_INITIALIZER;

// Called by the host to push a grayscale camera frame.
// data must be CAMERA_IMAGE_WIDTH * CAMERA_IMAGE_HEIGHT bytes of 8-bit grayscale.
bool libjade_push_camera_frame(const uint8_t* data, const size_t len)
{
    if (!data || len != sizeof(_cam_frame_buffer)) {
        // BBB-AIRGAP: say why. The frame size is a build-time contract (main/camera.h), and it
        // changed once already (QVGA to VGA), which leaves every tool that hardcoded the old size
        // pushing frames that are silently dropped. Without this line the symptom is "the camera
        // never delivered a frame", which points at the plumbing instead of at the size.
        JADE_LOGW("camera frame rejected: %u bytes, expected %u", (unsigned)len, (unsigned)sizeof(_cam_frame_buffer));
        return false;
    }
    pthread_mutex_lock(&_cam_mutex);
    if (!_cam_stopped) {
        memcpy(_cam_frame_buffer, data, len);
        _cam_frame_count++;
        pthread_cond_signal(&_cam_cond);
    }
    pthread_mutex_unlock(&_cam_mutex);
    return true;
}

esp_err_t esp_camera_init(const camera_config_t* config)
{
    JADE_ASSERT(config);
    JADE_ASSERT(config->pixel_format == PIXFORMAT_GRAYSCALE);
    JADE_ASSERT(config->frame_size == FRAMESIZE_VGA); // BBB-AIRGAP: see main/camera.h
    pthread_mutex_lock(&_cam_mutex);
    _cam_frame_count = 0;
    _cam_stopped = false;
    _cam_wanted = true; // BBB-AIRGAP
    pthread_mutex_unlock(&_cam_mutex);
    return ESP_OK;
}

esp_err_t esp_camera_deinit(void)
{
    pthread_mutex_lock(&_cam_mutex);
    _cam_stopped = true;
    _cam_wanted = false; // BBB-AIRGAP
    // BBB-AIRGAP: wipe both frame buffers on the way out. They are static, so on the Pi they live
    // for the whole run in ordinary heap that is never encrypted, and the last frames through them
    // are the ones that matter most: a scanned SeedQR carries the mnemonic itself. Leaving them
    // behind would mean "the wallet was closed" is true of the keychain but not of this file.
    // Only the camera's own copies are cleared here; a decoded QR payload belongs to its caller.
    JADE_WALLY_VERIFY(wally_bzero(_cam_frame_buffer, sizeof(_cam_frame_buffer)));
    JADE_WALLY_VERIFY(wally_bzero(_cam_delivered_buffer, sizeof(_cam_delivered_buffer)));
    pthread_cond_broadcast(&_cam_cond);
    pthread_mutex_unlock(&_cam_mutex);
    return ESP_OK;
}

camera_fb_t* esp_camera_fb_get(void)
{
    static camera_fb_t fb = {
        .buf = _cam_delivered_buffer, // BBB-AIRGAP: see _cam_delivered_buffer above
        .len = sizeof(_cam_delivered_buffer),
        .width = CAMERA_IMAGE_WIDTH,
        .height = CAMERA_IMAGE_HEIGHT,
        .format = PIXFORMAT_GRAYSCALE,
    };
    gettimeofday(&fb.timestamp, NULL);
    pthread_mutex_lock(&_cam_mutex);
    if (_cam_stopped) {
        pthread_mutex_unlock(&_cam_mutex);
        return NULL;
    }
    const uint64_t count_before = _cam_frame_count;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 100 * 1000000; // 100 ms
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    while (_cam_frame_count == count_before && !_cam_stopped) {
        const int rc = pthread_cond_timedwait(&_cam_cond, &_cam_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            break;
        }
    }
    // BBB-AIRGAP: copied while the producer is still locked out.
    memcpy(_cam_delivered_buffer, _cam_frame_buffer, sizeof(_cam_delivered_buffer));
    pthread_mutex_unlock(&_cam_mutex);
    // Always return the buffer (possibly stale/zero if no frame arrived yet),
    // matching the original v4l2 behaviour of never returning NULL on timeout.
    return &fb;
}

void esp_camera_fb_return(camera_fb_t* fb) { /* static buffer, nothing to free */ }

// BBB-AIRGAP: lets the host open the camera device only while Jade is using it.
bool libjade_camera_active(void)
{
    pthread_mutex_lock(&_cam_mutex);
    const bool wanted = _cam_wanted;
    pthread_mutex_unlock(&_cam_mutex);
    return wanted;
}

#else

esp_err_t esp_camera_init(const camera_config_t* config) { return ESP_FAIL; }
esp_err_t esp_camera_deinit() { return ESP_FAIL; }
camera_fb_t* esp_camera_fb_get(void) { return NULL; }
void esp_camera_fb_return(camera_fb_t* fb) {}
bool libjade_push_camera_frame(const uint8_t* data, const size_t len) { return false; }
bool libjade_camera_active(void) { return false; } // BBB-AIRGAP

static const uint8_t* debug_image_data = NULL;
void camera_set_debug_image(const uint8_t* data, const size_t len)
{
    JADE_ASSERT(!data == !len);
    JADE_ASSERT(!len || len == CAMERA_IMAGE_WIDTH * CAMERA_IMAGE_HEIGHT);
    debug_image_data = data;
}

void jade_camera_process_images(camera_process_fn_t fn, void* ctx, const bool show_ui, const char* text_label,
    const bool show_click_button, const qr_guide_type_t qr_guide_type, const char* help_url,
    progress_bar_t* progress_bar, gui_activity_t** act_out, gui_view_node_t** label_out)
{
    if (act_out) {
        *act_out = NULL;
    }
    if (label_out) {
        *label_out = NULL;
    }
    if (debug_image_data) {
        if (!fn(CAMERA_IMAGE_WIDTH, CAMERA_IMAGE_HEIGHT, debug_image_data, CAMERA_IMAGE_WIDTH * CAMERA_IMAGE_HEIGHT,
                ctx)) {
            JADE_LOGW("User callback returned false for fixed debug image - exiting camera regardless");
        }
        return;
    }

    // BBB-AIRGAP: with no camera and no injected image there is nothing to hand the callback, and
    // returning quietly makes every camera entry point the user can reach (the home 'Scan SeedQR'
    // tile, 'Scan QR' on the restore menu, QR Mode) look like a button that does nothing. Jade
    // already has a message for a device without a camera - main/camera.c and main/qrscan.c show it
    // when CONFIG_HAS_CAMERA is undefined - so say the same thing here rather than inventing a
    // second wording. Only where the caller asked for camera UI, though: main.c harvests startup
    // entropy with show_ui false (main/main.c:258), and an error screen there would block the boot
    // of every camera-less build on a button press.
    JADE_LOGW("No camera and no debug image - nothing to scan");
    if (show_ui) {
        await_error("No camera detected");
    }
}

void camera_stop(void) {}

#endif // CONFIG_LIBJADE_CAMERA
