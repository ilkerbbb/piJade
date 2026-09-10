#ifndef _LIBJADE_H_
#define _LIBJADE_H_ 1

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef LIBJADE_API
#if defined(_WIN32)
#ifdef LIBJADE_BUILD
#define LIBJADE_API __declspec(dllexport)
#else
#define LIBJADE_API
#endif
#elif defined(__GNUC__) && defined(LIBJADE_BUILD)
#define LIBJADE_API __attribute__((visibility("default")))
#else
#define LIBJADE_API
#endif
#endif

#define LIBJADE_REQUEST_METHOD "libjade_request"

/*
 * Start the global libjade instance.
 * Only one instance may be running at at time, however it can be stopped
 * and restarted as many times as required.
 */
LIBJADE_API void libjade_start(void);

/*
 * Stop the global libjade instance.
 */
LIBJADE_API void libjade_stop(void);

/*
 * Send a CBOR message to the global libjade instance.
 */
LIBJADE_API bool libjade_send(const uint8_t* data, size_t len);

/*
 * Receive a CBOR reply message from the global libjade instance.
 * `libjade_release` must be used to free any returned message.
 */
LIBJADE_API uint8_t* libjade_receive(unsigned int timeout, size_t* len_out);

/*
 * Free a CBOR message returned from `libjade_receive`.
 */
LIBJADE_API void libjade_release(uint8_t* data);

/*
 * Set the logging verbosity level for the global libjade instance.
 * levels are 0-4 in decreasing verbosity, or 5 to disable logging
 */
LIBJADE_API void libjade_set_log_level(int level);

/*
 * BBB-AIRGAP: in-process host integration.
 *
 * Upstream drives libjade over CBOR messages, and its display/input/camera hooks are reachable
 * only through the libjade_request debug message. A device that holds keys is built without that
 * surface (see libjade/CMakeLists.txt, DEBUG_MODE), so the program that owns the real panel,
 * buttons and camera needs a way in that does not depend on it. These calls are that way in: they
 * are direct C calls inside one process, with no socket and nothing listening.
 */

/*
 * Called with each finished frame, from the gui task, while the buffer is stable.
 * `len` is width * height * sizeof(uint16_t); pixels are RGB565.
 * The buffer belongs to libjade and is valid only for the duration of the call; copy any data the
 * host needs to retain.
 * Handler must return promptly - the gui task is blocked while it runs.
 *
 * Passing NULL as `fn` removes the current handler. Removal waits for a callback already in
 * progress, so once it returns the gui task is no longer inside the old handler and the host may
 * free whatever `ctx` pointed at. The other side of that guarantee: the handler itself must not
 * call this function, which would deadlock.
 */
typedef void (*libjade_display_flush_fn)(const uint16_t* buffer, size_t len, void* ctx);
LIBJADE_API void libjade_set_display_flush_handler(libjade_display_flush_fn fn, void* ctx);

/*
 * Called with the display orientation whenever Jade sets it: once during startup with the stored
 * setting, and again on each change from Options > Display > Flip Orientation. `flipped` is true
 * for the upside-down orientation.
 *
 * Jade's own build leaves this to the panel driver (esp_lcd_panel_mirror in main/display_hw.c),
 * which libjade does not have. Without a handler the setting still changes which way the buttons
 * move the highlight, but the picture stays as it was - that mismatch is what this exists to fix.
 *
 * Runs under the same lock as the flush handler, so a host driving one panel from both never has
 * the two overlap and needs no serialisation of its own. The other rules match that handler:
 * return promptly, and do not call this setter from inside the callback.
 */
typedef void (*libjade_display_orientation_fn)(bool flipped, void* ctx);
LIBJADE_API void libjade_set_display_orientation_handler(libjade_display_orientation_fn fn, void* ctx);

/*
 * Called whenever persisted NVS changes, with the blob the host should store, or with NULL and a
 * zero length when every persisted copy must be destroyed.
 *
 * Upstream keeps NVS in memory and offers it only through the get_nvs/set_nvs debug messages, so a
 * device built without that surface has no way to keep NVS across a power cut. This handler carries
 * the allowlisted default-namespace fields, multisig and descriptor registrations, AES-encrypted
 * OTP records, and HOTP counters; see libjade/pijade_settings.c for their bounds and format.
 *
 * `data` belongs to libjade and is valid only for the duration of the call. It CONTAINS KEY
 * MATERIAL: the PIN client key and the encrypted wallet blob. A host must treat it the way it would
 * treat a private key: store it where only the device owner can read it, and wipe its own copy
 * rather than just freeing it. pijade/host/settings_store.c is the in-tree example.
 *
 * Runs on whichever task changed the setting, and that task waits: a handler that writes to slow
 * storage holds up the UI for as long as the write takes. HOTP generation commits every counter
 * bump at otpauth.c:603 and storage.c:773, so that path also writes the whole blob.
 *
 * For a non-empty blob, returns true if it was persisted. For NULL and zero length, returns true
 * only if no persisted copy remains. The settings now carry the PIN wallet, so unlike a display
 * preference this is not best effort: the caller (nvs_commit()/nvs_flash_erase() in
 * libjade/nvs_flash.c) passes false up through Jade's own storage error path, the same one a failed
 * on-device flash write already goes through.
 */
typedef bool (*libjade_settings_fn)(const uint8_t* data, size_t len, void* ctx);
LIBJADE_API void libjade_set_settings_handler(libjade_settings_fn fn, void* ctx);

/*
 * Applies a blob previously handed to the settings handler. Returns false if it was not one, or was
 * damaged, in which case Jade keeps its defaults.
 *
 * Call before libjade_start(): the settings are read during startup, so one applied afterwards
 * takes effect only where Jade happens to read it again.
 */
LIBJADE_API bool libjade_load_settings(const uint8_t* data, size_t len);

/*
 * BBB-AIRGAP: what to do when Jade wants the machine off or restarted.
 *
 * On an ESP32 these are single instructions - the sleep menu entry and the idle timeout reach
 * esp_deep_sleep_start(), and a factory reset or an OTA reaches esp_restart(). A Linux host has to
 * ask its init system instead, so libjade hands the request over rather than pretending to do it.
 * (The idle timeout does not arrive here in a libjade build: main/idletimer.c is left out of the
 * amalgamation and stubbed below, so that timer never fires.)
 *
 * The handler is not expected to return: on success the process is gone. If it does return -
 * no permission, no such binary - libjade aborts, which is what it did before there was a handler
 * at all. With no handler installed the behaviour is unchanged.
 *
 * Runs on whichever task asked to shut down - the gui task for the menu entry - and that task is
 * not coming back either way. Passing NULL as `fn` removes the handler and, as with the display
 * handlers, waits for a call already in progress, so the host may then free whatever `ctx` pointed
 * at; the handler itself must not call this setter.
 */
typedef enum {
    LIBJADE_POWER_OFF = 0,
    LIBJADE_POWER_RESTART = 1,
} libjade_power_action_t;
typedef void (*libjade_power_fn)(libjade_power_action_t action, void* ctx);
LIBJADE_API void libjade_set_power_handler(libjade_power_fn fn, void* ctx);

/*
 * Screen backlight level, 1..5, matching Jade's own BACKLIGHT_MIN..BACKLIGHT_MAX. Called whenever
 * the user moves the 'Display Brightness' slider and once at startup from the stored setting; also
 * called with the dim level by the idle timer.
 *
 * On Jade's own boards brightness is a voltage the power management chip supplies. piJade has no
 * PMU, so the level is handed to the host, which drives the panel's backlight gpio. Unlike the
 * power handler, no handler installed is not an error - the level is dropped and the screen stays
 * as it is, which is what an emulator wants. Locked and cleared the same way as the handlers
 * above: passing NULL waits for a call already in progress, and the handler must not call this
 * setter.
 */
typedef void (*libjade_backlight_fn)(uint8_t level, void* ctx);
LIBJADE_API void libjade_set_backlight_handler(libjade_backlight_fn fn, void* ctx);

/*
 * BBB-AIRGAP: called when Jade wants to set its clock, through the set_epoch RPC, a QR epoch, or a
 * companion handshake. Return 0 on success; any other value is reported to the caller as "Failed
 * to set time".
 *
 * With no handler installed the request succeeds without changing the clock. This preserves
 * upstream's in-process no-op behaviour, which an emulator needs. Passing NULL as `fn` removes the
 * current handler and waits for a call already in progress; the handler itself must not call this
 * setter.
 */
typedef int (*libjade_clock_fn)(int64_t epoch_seconds, void* ctx);
LIBJADE_API void libjade_set_clock_handler(libjade_clock_fn fn, void* ctx);

/*
 * The compiled-in panel size (build option DISPLAY_WIDTH/DISPLAY_HEIGHT).
 */
LIBJADE_API void libjade_display_size(unsigned int* width_out, unsigned int* height_out);

/*
 * Navigation input. PREV/NEXT follow Jade's one-dimensional control; UP/DOWN select the nearest
 * vertical neighbour and fall back to PREV/NEXT when none exists. FIRST selects, but does not
 * click, the screen's first selectable item. ALT is a screen-specific secondary action, currently
 * only the next keyboard page on keyboards. Safe to call from the host's own thread: on real
 * hardware these same gui calls come from the button task, not the gui task.
 */
typedef enum {
    LIBJADE_INPUT_PREV = 0,
    LIBJADE_INPUT_NEXT = 1,
    LIBJADE_INPUT_CLICK = 2,
    LIBJADE_INPUT_UP = 3,
    LIBJADE_INPUT_DOWN = 4,
    LIBJADE_INPUT_FIRST = 5,
    LIBJADE_INPUT_ALT = 6,
} libjade_input_t;
LIBJADE_API void libjade_input(libjade_input_t event);

/*
 * Push one 8-bit greyscale camera frame (CAMERA_IMAGE_WIDTH * CAMERA_IMAGE_HEIGHT bytes).
 *
 * Returns false when the frame is rejected outright (null data, wrong size) and in every call if
 * libjade was built without camera support, where the whole camera layer is a stub. It returns
 * true for a well formed frame even when the camera is not currently running, in which case the
 * frame is simply dropped - so true means "well formed", not "consumed". Feed at a steady rate
 * rather than trying to infer readiness from the return value. (This mirrors upstream behaviour
 * in libjade/esp_camera.c; changing it would also change what the RPC path reports.)
 */
LIBJADE_API bool libjade_push_camera_frame(const uint8_t* data, size_t len);

/*
 * True while Jade has the camera open, i.e. between esp_camera_init() and esp_camera_deinit().
 *
 * Exists because libjade_push_camera_frame() cannot answer this: it reports whether a frame was
 * well formed, not whether anyone wanted it. A host that owns a real camera device uses this to
 * open the sensor only while it is being used, instead of holding it open for the whole session.
 * Always false in a build without camera support.
 */
LIBJADE_API bool libjade_camera_active(void);

#endif /* _LIBJADE_H_ */
