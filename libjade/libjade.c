// A single source file containing a local implementation of the Jade Firmware
//
// This hack is designed for local development, debugging and testing.
//
// WARNING: THIS CODE IS NOT SUITABLE FOR PROCESSING REAL DATA.
//          DO NOT USE THIS CODE FOR ANY PURPOSE WITH NON-TEST DATA.
//          DOING SO IS INSECURE AND MAY RESULT IN THE LOSS OF FUNDS!
//
// Includes the entire Jade firmware code, replacing the GUI and most
// of the OS support code.
// This file can be compiled to a shared library which implements an
// in-processes software Jade emulator/virtual Jade device.
// The exposed API allows passing and fetching messages using the same
// binary format that would be passed to a real device by serial/bluetooth.
//
#define _GNU_SOURCE 1 // needed for pthread extra funcs e.g. pthread_setname_np
#include "sdkconfig.h"

#include "libjade.h"
#include "libjade_port.h"

#include "pijade_settings.h" // BBB-AIRGAP: persisted settings

#include "icons.inc"

// Prevent secp symbols being externally visible in our final shared library
#define SECP256K1_API

// Address sanitizer doesn't like calling sha256 into a non-aligned
// buffer, even though its technically legal (but slower). Force
// wally to handle unaligned destination buffers internally to work
// around this.
#define HAVE_UNALIGNED_ACCESS 0

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/time.h> // Must be included before we redefine settimeofday()

// Include the tinycbor sources we need for CBOR processing
#include "managed_components/espressif__cbor/tinycbor/src/cborencoder.c"
#include "managed_components/espressif__cbor/tinycbor/src/cborparser.c"
#include "managed_components/espressif__cbor/tinycbor/src/cborparser_dup_string.c"
#include "managed_components/espressif__cbor/tinycbor/src/cborpretty.c"
#include "managed_components/espressif__cbor/tinycbor/src/cborpretty_stdio.c"
#include "managed_components/espressif__cbor/tinycbor/src/cbortojson.c"
// Include the asset snapshot component sources
#include "components/assets/assets_snapshot.c"
// Include the miniz compression code.
// This is a manually shortened version of the amalgamation from
// https://github.com/richgel999/miniz with a couple of additional
// patches for memory safety.
#include "miniz.c"
// Include the emulation of the o/s task/event/camera functions
#include "esp_camera.c"
#include "esp_event.c"
#include "task.c"
// Include the esp32_deflate component
#define ESP_PLATFORM 1
#define ESP_IDF_VERSION 1
#define ESP_IDF_VERSION_VAL(x, y, z) 1
#include "components/esp32_deflate/deflate.h"
#undef ESP_IDF_VERSION_VAL
#undef ESP_IDF_VERSION
#undef ESP_PLATFORM
#include "components/esp32_deflate/deflate.c"

// Prevent "components/esp32-quirc/lib/identify.c" to include OpenMV's "fmath.h"
// because we are redefining its functions below.
#define __FMATH_H
static inline int fast_roundf(float x) { return (int)(x); }
static inline float fast_fabsf(float d) { return fabsf(d); }

// qrCode encoding/decoding
#include "components/esp32-quirc/lib/decode.c"
#include "components/esp32-quirc/lib/identify.c"
#include "components/esp32-quirc/lib/quirc.c"
#include "components/esp32-quirc/lib/version_db.c"
#include "components/esp32-quirc/openmv/collections.c"
// bspatch
#include "components/esp32_bsdiff/bspatch.c"
// BBB-AIRGAP: the miner is built into libjade so the emulator can measure the hash loop the same
// way the device runs it. Included here rather than compiled separately because libjade is a unity
// build (see main/amalgamated.c below); it needs no jade internals, only the freertos and esp_timer
// shims, so it sits with the other components ahead of the firmware body.
#include "components/miner/miner.c"

// abort is mapped to __wrap_abort in the firmware. This calls jade_abort,
// which calls __real_abort, which we implement as calling (the real) abort
static void __real_abort(void) { abort(); }

// Route firmware clock requests through the optional in-process host handler below.
static int settimeofday_host(const struct timeval* tv, const void* tz);
#define settimeofday settimeofday_host

#include "main/camera.h"
#include "main/display.h"
#include "main/gui.h"

typedef void* locale_multilang_string_t;
const locale_multilang_string_t* locale_get(const char* key) { return NULL; }
const char* locale_lang_with_fallback(const locale_multilang_string_t* str, jlocale_t lang) { return NULL; }

// Include the core Jade firmware core, including wally/secp.
#define AMALGAMATED_BUILD
#include "main/amalgamated.c"
#undef settimeofday

// Include the NVS emulation code
#include "nvs_flash.c"

// BBB-AIRGAP: the persisted subset of it
#include "pijade_settings.c"

//
// Stubs for code that does not apply to libjade or is not yet implemented
//

// main/logging.c
#ifndef CONFIG_LOG_DEFAULT_LEVEL_NONE
esp_log_level_t _libjade_log_level = ESP_LOG_NONE;
#endif

#ifndef SELFCHECK
// Include the default selfcheck impl
#include "main/selfcheck.c"
#else
// Include a user-defined selfcheck impl
// clang-format off
#define HSTR(x) #x
#define XSTR(x) HSTR(x)
#define SELFCHECK_FILE(dir, base) XSTR(dir/base.c)
#include SELFCHECK_FILE(selfcheck, SELFCHECK)
// clang-format on
#endif

// BBB-AIRGAP: upstream leaves a filler string here, and it is not invisible - the home screen
// prints it next to the wallet status, where it showed up as "Uninitialized 12345678901 2345678901"
// and overflowed the line. It also feeds the entropy hasher (main/random.c:249) and the
// 'get_version_info' reply (main/versioninfo.c:44), so a companion app reads it too. Carries the
// upstream tag this fork is based on plus the fork's own name; update it when rebasing onto a newer
// Jade tag (git describe --tags $(git merge-base HEAD upstream/master)).
esp_app_desc_t running_app_info = { "1.0.41-pijade" };
esp_chip_info_t chip_info = { 0 };

// BBB-AIRGAP: power requests handed to the host, see libjade.h. Upstream aborts in both stubs
// below - fine for an emulator, but on piJade the sleep menu entry is a real feature, and a
// factory reset has to come back up to be of any use.
// Locked like the other handlers: the host installs it at startup and clears it at exit, and the
// task asking to shut down may be neither of those.
static libjade_power_fn _power_fn = NULL;
static void* _power_ctx = NULL;
static pthread_mutex_t _power_mutex = PTHREAD_MUTEX_INITIALIZER;

void libjade_set_power_handler(const libjade_power_fn fn, void* ctx)
{
    pthread_mutex_lock(&_power_mutex);
    _power_fn = fn;
    _power_ctx = ctx;
    pthread_mutex_unlock(&_power_mutex);
}

// BBB-AIRGAP: backlight level handed to the host, see libjade.h. Upstream's minimal power layer
// returns ESP_OK and does nothing, which is why the brightness menu had no effect here.
static libjade_backlight_fn _backlight_fn = NULL;
static void* _backlight_ctx = NULL;
static pthread_mutex_t _backlight_mutex = PTHREAD_MUTEX_INITIALIZER;

void libjade_set_backlight_handler(const libjade_backlight_fn fn, void* ctx)
{
    pthread_mutex_lock(&_backlight_mutex);
    _backlight_fn = fn;
    _backlight_ctx = ctx;
    pthread_mutex_unlock(&_backlight_mutex);
}

// Called from main/power/minimal.inc, ie. from whichever task changed the level.
void libjade_backlight_request(const uint8_t level)
{
    pthread_mutex_lock(&_backlight_mutex);
    if (_backlight_fn) {
        _backlight_fn(level, _backlight_ctx);
    }
    pthread_mutex_unlock(&_backlight_mutex);
}

// BBB-AIRGAP: clock setting handed to the host, see libjade.h. With no handler this deliberately
// remains the no-op an in-process emulator expects.
static libjade_clock_fn _clock_fn = NULL;
static void* _clock_ctx = NULL;
static pthread_mutex_t _clock_mutex = PTHREAD_MUTEX_INITIALIZER;

void libjade_set_clock_handler(const libjade_clock_fn fn, void* ctx)
{
    pthread_mutex_lock(&_clock_mutex);
    _clock_fn = fn;
    _clock_ctx = ctx;
    pthread_mutex_unlock(&_clock_mutex);
}

static int settimeofday_host(const struct timeval* tv, const void* tz)
{
    (void)tz;
    pthread_mutex_lock(&_clock_mutex);
    const int rc = _clock_fn ? _clock_fn((int64_t)tv->tv_sec, _clock_ctx) : 0;
    pthread_mutex_unlock(&_clock_mutex);
    return rc;
}

__attribute__((noreturn)) static void _power_request(const libjade_power_action_t action)
{
    // Called under the lock, as the display handlers are: removal then waits for a call already in
    // progress, so a host may free whatever ctx pointed at once the setter has returned. The other
    // side of that guarantee is the same as theirs - the handler must not call the setter.
    pthread_mutex_lock(&_power_mutex);
    if (_power_fn) {
        _power_fn(action, _power_ctx);
    }
    pthread_mutex_unlock(&_power_mutex);
    // Either no host took the request or the host could not carry it out. Upstream's behaviour is
    // the honest answer to both: stop, rather than carry on as a device the user thinks is off.
    abort();
}

// BBB-AIRGAP: main/idletimer.c asks this to tell an idle-timeout restart from a cold start, so it
// knows whether to bring the screen up dimmed. The answer here is always a cold start: the flag it
// consults lives in memory an ESP32 preserves across esp_restart() and a Linux process does not
// (see __NOINIT_ATTR in include/sdkconfig.h). The screen therefore comes back lit after an
// idle-timeout restart - the wallet is still cleared, only the dimming is lost.
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }

void esp_restart() { _power_request(LIBJADE_POWER_RESTART); }

const char* esp_get_idf_version(void) { return "9.9.99-99-fake_hack"; }

void esp_chip_info(esp_chip_info_t* out)
{
    out->features = 0; // FIXME
}

// BBB-AIRGAP: give the port a stable device identity.
// Upstream libjade zeroes this, so 'macid' - which Jade shows on the home screen,
// feeds into its entropy pool (main/random.c) and now seeds the camera entropy
// (main/entropy_sources.c) - was all zeroes on this port.
// The machine-id itself is confidential (see machine-id(5)), so a hash of it is
// used rather than the raw value. Falls back to zeroes when it cannot be read.
esp_err_t esp_efuse_mac_get_default(uint8_t* out)
{
    memset(out, 0, 6);

    FILE* const f = fopen("/etc/machine-id", "r");
    if (!f) {
        JADE_LOGW("Could not read /etc/machine-id - device id left as zeroes");
        return ESP_OK;
    }

    char id[64];
    const size_t read = fread(id, 1, sizeof(id), f);
    const bool read_failed = ferror(f) != 0;
    fclose(f);

    if (read_failed) {
        JADE_LOGW("Could not read complete /etc/machine-id - device id left as zeroes");
        JADE_WALLY_VERIFY(wally_bzero(id, sizeof(id)));
        return ESP_OK;
    }

    size_t begin = 0;
    size_t end = read;
    while (begin < end && (id[begin] == ' ' || id[begin] == '\t' || id[begin] == '\r' || id[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (id[end - 1] == ' ' || id[end - 1] == '\t' || id[end - 1] == '\r'
                              || id[end - 1] == '\n')) {
        --end;
    }

    if (begin < end) {
        uint8_t hash[SHA256_LEN];
        JADE_WALLY_VERIFY(wally_sha256((const uint8_t*)id + begin, end - begin, hash, sizeof(hash)));
        memcpy(out, hash, 6);
        JADE_WALLY_VERIFY(wally_bzero(hash, sizeof(hash)));
    } else {
        JADE_LOGW("Empty /etc/machine-id - device id left as zeroes");
    }
    JADE_WALLY_VERIFY(wally_bzero(id, sizeof(id)));
    return ESP_OK;
}

void esp_deep_sleep_start(void) { _power_request(LIBJADE_POWER_OFF); }

// No physical buttons
void input_init(void) {}

// Serial
bool serial_init(TaskHandle_t* task) { return true; }

#ifdef __APPLE__
const uint8_t binary_pinserver_public_key_pub_start[33]
#else
const uint8_t _binary_pinserver_public_key_pub_start[33]
#endif
    = { 0x03, 0x32, 0xb7, 0xb1, 0x34, 0x8b, 0xde, 0x8c, 0xa4, 0xb4, 0x6b, 0x9d, 0xcc, 0x30, 0x32, 0x0e, 0x14, 0x0c,
          0xa2, 0x64, 0x28, 0x16, 0x0a, 0x27, 0xbd, 0xbf, 0xc3, 0x0b, 0x34, 0xec, 0x87, 0xc5, 0x47 };

// Events
volatile bool _libjade_stop_requested = false; // Used to stop the firmware

// HW: Task API
bool run_on_temporary_stack(size_t stack_size, temporary_stack_function_t fn, void* ctx) { return fn(ctx); }

bool run_in_temporary_task(const size_t stack_size, temporary_stack_function_t fn, void* ctx) { return fn(ctx); }

void temp_stack_init(void) {}

// BBB-AIRGAP: real implementation of the sensitive-memory stack.
// Upstream libjade stubs these out, so mnemonics, entropy buffers and passphrases
// were never zeroed on this port (main/sensitive.c is excluded from the
// amalgamation when CONFIG_LIBJADE is set, see main/amalgamated.c).
// Mirrors main/sensitive.c, using pthread TLS instead of FreeRTOS TLS.
#define SENS_STACK_SIZE 32

struct sens_elem {
    const char* file;
    int line;
    void* addr;
    size_t size;
};

struct sens_stack {
    struct sens_elem* top;
    struct sens_elem elems[SENS_STACK_SIZE];
};

static pthread_key_t _sens_stack_key;
static pthread_once_t _sens_stack_once = PTHREAD_ONCE_INIT;

// Clear any items on the passed stack.
// Returns true if any items were present and needed clearing.
static bool sensitive_clear_stack_impl(struct sens_stack* stack)
{
    bool had_items = false;
    if (stack) {
        while (stack->top > stack->elems) {
            stack->top--;
            JADE_LOGW("sensitive: clearing %p %u bytes pushed from %s:%d", stack->top->addr,
                (unsigned)stack->top->size, stack->top->file, stack->top->line);
            JADE_WALLY_VERIFY(wally_bzero(stack->top->addr, stack->top->size));
            had_items = true;
        }
    }
    return had_items;
}

// Called by pthread when a thread exits. As with the FreeRTOS delete callback in
// main/sensitive.c, avoid asserting/aborting from this context.
static void _sens_stack_free(void* ptr)
{
    if (ptr) {
        sensitive_clear_stack_impl((struct sens_stack*)ptr);
        free(ptr);
    }
}

static void _sens_stack_key_init(void)
{
    const int rc = pthread_key_create(&_sens_stack_key, _sens_stack_free);
    if (rc != 0) {
        JADE_LOGE("pthread_key_create failed: %d", rc);
        __real_abort();
    }
}

// NOTE: unlike main/sensitive.c this allocates the stack on demand rather than
// asserting that sensitive_init() has run for this thread - libjade spawns threads
// via its own task shim (libjade/task.c), which has no per-task init hook.
static struct sens_stack* get_sens_stack(void)
{
    const int once_rc = pthread_once(&_sens_stack_once, _sens_stack_key_init);
    if (once_rc != 0) {
        JADE_LOGE("pthread_once failed: %d", once_rc);
        __real_abort();
    }

    struct sens_stack* stack = pthread_getspecific(_sens_stack_key);
    if (!stack) {
        stack = calloc(1, sizeof(struct sens_stack));
        JADE_ASSERT(stack);
        stack->top = stack->elems;
        const int set_rc = pthread_setspecific(_sens_stack_key, stack);
        if (set_rc != 0) {
            JADE_LOGE("pthread_setspecific failed: %d", set_rc);
            free(stack);
            __real_abort();
        }
    }
    return stack;
}

void sensitive_init(void) { get_sens_stack(); }

void sensitive_push(const char* file, int line, void* addr, const size_t size)
{
    JADE_LOGD("sensitive_push %s:%d %p %u bytes", file, line, addr, (unsigned)size);
    JADE_ASSERT(addr);
    JADE_ASSERT(size);

    struct sens_stack* const stack = get_sens_stack();
    JADE_ASSERT_MSG(stack->top < stack->elems + SENS_STACK_SIZE, "sensitive_push() exhausted sensitive stack");

    stack->top->file = file;
    stack->top->line = line;
    stack->top->addr = addr;
    stack->top->size = size;
    stack->top++;
}

void sensitive_pop(const char* file, int line, void* addr)
{
    JADE_LOGD("sensitive_pop  %s:%d %p", file, line, addr);
    struct sens_stack* const stack = get_sens_stack();
    JADE_ASSERT(stack->top > stack->elems);

    stack->top--;
    JADE_WALLY_VERIFY(wally_bzero(stack->top->addr, stack->top->size));

    if (addr != stack->top->addr) {
        JADE_LOGE("sensitive_pop %s:%d unexpectedly popping addr %p", file, line, addr);
        JADE_LOGE("sensitive_pop expected addr %p (%u bytes pushed from %s:%d)", stack->top->addr,
            (unsigned)stack->top->size, stack->top->file, stack->top->line);
        JADE_ABORT();
    }
}

void sensitive_assert_empty(void)
{
    if (sensitive_clear_stack_impl(get_sens_stack())) {
        JADE_LOGE("Sensitive stack not empty!");
        JADE_ABORT();
    }
}

void sensitive_clear_stack(void) { sensitive_clear_stack_impl(get_sens_stack()); }

// HW: Random
void get_random(void* bytes_out, size_t len)
{
    if (!bytes_out || !len) {
        abort();
    }

    uint8_t* current_ptr = (uint8_t*)bytes_out;
    size_t remaining = len;
    int getrandom_enosys = 0;

    while (remaining > 0) {
        const ssize_t bytes_read = libjade_getrandom(current_ptr, remaining);

        if (bytes_read == -1) {
            if (errno == EINTR) {
                continue;
            } else if (errno == ENOSYS) {
                getrandom_enosys = 1;
                break;
            } else {
                abort();
            }
        } else if (bytes_read == 0) {
            abort();
        } else {
            current_ptr += bytes_read;
            remaining -= bytes_read;
        }
    }

    if (remaining == 0 && !getrandom_enosys) {
        // happy path
        return;
    }

    if (getrandom_enosys) {
        // FIXME: find another source of entropy of cryptographic strength or abort()
    }
    abort();
}

void refeed_entropy(const void* additional, size_t len)
{
    // Unused since our get_random has no state.
    // FIXME: change main/random.c to use getrandom, pids etc for libjade, then use it
}

uint8_t get_uniform_random_byte(uint8_t upper_bound)
{
    uint8_t ret;
    get_random(&ret, sizeof(ret));
    return ret % upper_bound; // Not used for crypto, so return a biased byte
}

void random_start_collecting(void) {}

void random_full_initialization(void) {}

int random_mbedtls_cb(void* ctx, uint8_t* buf, const size_t len)
{
    // ctx is ignored (should be NULL)
    get_random(buf, len);
    return 0;
}

// BBB-AIRGAP: TLS identifies the sole owner of the stop-wakeup slot without assuming pthread_t is
// numeric or that zero is invalid, and without racing with libjade_stop() as it writes the stored
// thread ID. A restarted firmware thread establishes its own marker, so no prior identity leaks.
static _Thread_local bool _libjade_fw_thread = false;
bool _libjade_is_firmware_thread(void) { return _libjade_fw_thread; }

static void* jade_fw_thread_fn(void* arg)
{
    libjade_thread_setname("libjade_fw");
    _libjade_fw_thread = true;
    start_dashboard();
    return NULL; // Never reached
}

// External API:
static pthread_t _libjade_thread_id = 0; // Thread ID of the FW thread

// BBB-AIRGAP: records which ring buffer the item THIS caller is holding came from, so its
// libjade_release() returns it to the right one. Thread-local because it is per-caller state; no
// routing state is kept, see libjade_receive() for why a count of pending replies was not enough.
static __thread bool _libjade_received_internal_msg = false;

// BBB-AIRGAP: upstream declares these next to libjade_send(), but libjade_start() below resets
// them when a session opens, so they have to be declared before it.
static uint8_t _libjade_serial_data_in[MAX_INPUT_MSG_SIZE + 1] = { 0 };
static size_t _libjade_serial_read_ptr = 0;
static TickType_t _libjade_last_processing_time = 0;

// BBB-AIRGAP: bridge to cxx_terminate.cpp.  jade_abort() is local to this translation unit
// (main/amalgamated.c is included above), so the C++ side takes it as a pointer.
void jade_install_terminate_handler(void (*on_terminate)(void));

static void abort_on_cxx_terminate(void) { jade_abort("CXX-TERMINATE", 0); }

void libjade_start(void)
{
    // Before anything that can parse untrusted input: an uncaught C++ throw must clear the
    // keychain and the sensitive stack rather than dropping the process where it stands.
    jade_install_terminate_handler(abort_on_cxx_terminate);

    // BBB-AIRGAP: start the tick count from here rather than from whenever this process began, so
    // a session opens at tick zero the way a freshly powered Jade does. Everything holding a tick
    // from the previous session has to go with it, or the next reading will look like it went
    // backwards - main/wire.c asserts that it never does.
    libjade_tick_epoch_reset();
    _libjade_serial_read_ptr = 0;
    _libjade_last_processing_time = 0;
    _libjade_received_internal_msg = false;

    ensure_boot_flags();
    random_start_collecting();
    validate_running_image();
    boot_process();
    sensitive_assert_empty();
    pthread_create(&_libjade_thread_id, NULL, &jade_fw_thread_fn, NULL);
}

void libjade_stop(void)
{
    // BBB-AIRGAP: stop idle power actions first because even camera_stop() can block during teardown.
    idletimer_request_stop();
    // stop camera task (if running)
    camera_stop();
    // Request the firmware to stop
    // the main firmware thread branches down various code paths depending on what activity the user
    // is doing, and in some of those code paths it may be waiting for an event with no timeout.
    _libjade_stop_requested = true;
    _trigger_last_wait_handle();
    pthread_join(_libjade_thread_id, NULL);
    _libjade_thread_id = 0;
    _libjade_stop_requested = false;
    // BBB-AIRGAP: stop the idle task before the gui it draws on and the event loop it waits on
    idletimer_stop();
    // stop the gui task
    gui_stop();
    // clean up remaining resources
    esp_event_loop_delete_default();
    vRingbufferDelete(shared_in);
    shared_in = NULL;
    vRingbufferDelete(serial_out);
    serial_out = NULL;
    vRingbufferDelete(internal_out);
    internal_out = NULL;
    vRingbufferDelete(libjade_out);
    libjade_out = NULL;
    _libjade_received_internal_msg = false;
    // clear keychain
    keychain_clear();
}

// Mutex to serialize calls to libjade_send() / handle_data()
static pthread_mutex_t _libjade_send_mutex = PTHREAD_MUTEX_INITIALIZER;

bool libjade_send(const uint8_t* data, size_t len)
{
    pthread_mutex_lock(&_libjade_send_mutex);

    // BBB-AIRGAP: the daemon is a serial transport. Internal routing is selected only after
    // main/wire.c has parsed the complete top-level method, never from bytes in a socket chunk.
    _libjade_serial_data_in[0] = SOURCE_SERIAL;

    while (len) {
        const size_t remaining_bytes = MAX_INPUT_MSG_SIZE - _libjade_serial_read_ptr;
        const size_t copy_len = len > remaining_bytes ? remaining_bytes : len;
        JADE_ASSERT(_libjade_serial_read_ptr + copy_len <= MAX_INPUT_MSG_SIZE);
        memcpy(_libjade_serial_data_in + 1 + _libjade_serial_read_ptr, data, copy_len);
        handle_data(_libjade_serial_data_in, &_libjade_serial_read_ptr, copy_len, &_libjade_last_processing_time);
        data += copy_len;
        len -= copy_len;
    }

    pthread_mutex_unlock(&_libjade_send_mutex);
    return true;
}

uint8_t* libjade_receive(const unsigned int timeout, size_t* len_out)
{
    // BBB-AIRGAP: the deadlock fix gives a synchronously dispatched libjade_request reply its own
    // ring (main/process.c:323) so it cannot block behind a full serial_out. Draining two rings
    // through this single-ring API first ran off a count of internal replies expected to be
    // pending, but a caller that sends and receives on different threads can read that count
    // before the sending thread raises it: the reader then commits to serial_out while the reply
    // waits in libjade_out. A count predicts which ring will have the item; this looks at both.
    // Cost is nil because the shim's own wait is already a 1ms poll loop, not a blocking park
    // (libjade/include/freertos/ringbuf.h:99-124), so slicing the wait here adds no wakeups.
    // portTICK_PERIOD_MS is statically asserted to be 1 (freertos/timecvt.h:15), so ticks are ms.
    unsigned int ms_left = timeout * 1000;
    for (;;) {
        // Both rings are created by jade_process_init(), which runs on the firmware thread that
        // libjade_start() spawns rather than before it returns, so a receive can arrive before
        // either exists. libjade_out is created last (main/process.c:189), so it gates both.
        if (libjade_out) {
            void* item = xRingbufferReceive(libjade_out, len_out, 0);
            if (item) {
                _libjade_received_internal_msg = true;
                return item;
            }
            item = xRingbufferReceive(serial_out, len_out, 0);
            if (item) {
                _libjade_received_internal_msg = false;
                return item;
            }
        }
        if (!ms_left) {
            break;
        }
        --ms_left;
        const struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&ts, NULL);
    }

    *len_out = 0; // No message available
    _libjade_received_internal_msg = false;
    return NULL;
}

void libjade_release(uint8_t* data)
{
    RingbufHandle_t ringbuf = _libjade_received_internal_msg ? libjade_out : serial_out;
    vRingbufferReturnItem(ringbuf, (void*)data);
    _libjade_received_internal_msg = false;
}

void libjade_set_log_level(int level)
{
#ifndef CONFIG_LOG_DEFAULT_LEVEL_NONE
    // Note we don't bother about thread safety for _libjade_log_level
    if (level < 0) {
        _libjade_log_level = ESP_LOG_VERBOSE;
    } else if (level >= ESP_LOG_NONE) {
        _libjade_log_level = ESP_LOG_NONE;
    } else {
        _libjade_log_level = (esp_log_level_t)level;
    }
#endif
}

// BBB-AIRGAP: host integration, see libjade.h. Kept next to the RPC handlers below because both
// reach the same gui/display entry points; the difference is that these need no debug build.
static libjade_display_flush_fn _display_flush_fn = NULL;
static void* _display_flush_ctx = NULL;
static pthread_mutex_t _display_flush_mutex = PTHREAD_MUTEX_INITIALIZER;

void libjade_set_display_flush_handler(const libjade_display_flush_fn fn, void* ctx)
{
    pthread_mutex_lock(&_display_flush_mutex);
    _display_flush_fn = fn;
    _display_flush_ctx = ctx;
    pthread_mutex_unlock(&_display_flush_mutex);
}

// Called from display_hw_flush() with the finished frame.
// The handler runs with the lock held, so removing a handler waits for a call already in progress
// and cannot return while the gui task is still inside the host's callback. The cost is that the
// handler must not call libjade_set_display_flush_handler itself; see libjade.h.
void libjade_display_flushed(const uint16_t* buffer)
{
    pthread_mutex_lock(&_display_flush_mutex);
    if (_display_flush_fn) {
        _display_flush_fn(buffer, CONFIG_DISPLAY_WIDTH * CONFIG_DISPLAY_HEIGHT * sizeof(uint16_t),
            _display_flush_ctx);
    }
    pthread_mutex_unlock(&_display_flush_mutex);
}

// BBB-AIRGAP: display orientation, sharing the flush handler's lock.
// One lock rather than two because both callbacks end up at the same panel: holding it means a
// host cannot be told to mirror the panel while it is part way through writing a frame to it.
static libjade_display_orientation_fn _display_orientation_fn = NULL;
static void* _display_orientation_ctx = NULL;

void libjade_set_display_orientation_handler(const libjade_display_orientation_fn fn, void* ctx)
{
    pthread_mutex_lock(&_display_flush_mutex);
    _display_orientation_fn = fn;
    _display_orientation_ctx = ctx;
    pthread_mutex_unlock(&_display_flush_mutex);
}

// Called from display_hw_flip_orientation().
void libjade_display_orientation_changed(const bool flipped)
{
    pthread_mutex_lock(&_display_flush_mutex);
    if (_display_orientation_fn) {
        _display_orientation_fn(flipped, _display_orientation_ctx);
    }
    pthread_mutex_unlock(&_display_flush_mutex);
}

// BBB-AIRGAP: persisted settings, see libjade.h.
// A lock of its own rather than the display one: this fires from whichever task wrote the setting,
// which may well be the gui task mid-way through an activity, and it has nothing to do with the
// panel. Sharing the display lock would make a slow write to storage block frames.
static libjade_settings_fn _settings_fn = NULL;
static void* _settings_ctx = NULL;
static pthread_mutex_t _settings_mutex = PTHREAD_MUTEX_INITIALIZER;

void libjade_set_settings_handler(const libjade_settings_fn fn, void* ctx)
{
    pthread_mutex_lock(&_settings_mutex);
    _settings_fn = fn;
    _settings_ctx = ctx;
    pthread_mutex_unlock(&_settings_mutex);
}

bool libjade_load_settings(const uint8_t* const data, const size_t len)
{
    return data && pijade_settings_deserialize(pijade_settings_storage(0), data, len);
}

// Called from nvs_commit() once the change is in the store.
// Serialising here rather than in the host keeps the format in one place and means the host never
// sees the store itself. As with the display handlers, the handler must not call the setter.
// No handler registered is not a failure: running without --settings is a deliberate mode (the
// emulator, and any run meant to leave nothing behind), and there the writes have nowhere to go by
// design. Returning false there would break PIN setup in exactly the mode built for testing it.
bool libjade_settings_changed(void)
{
    pthread_mutex_lock(&_settings_mutex);
    bool ok = true;
    if (_settings_fn) {
        uint8_t* data = NULL;
        size_t len = 0;
        if (pijade_settings_serialize(pijade_settings_storage(0), &data, &len)) {
            ok = _settings_fn(data, len, _settings_ctx);
            // BBB-AIRGAP: the serialised settings now contain PIN wallet key material.
            wally_bzero(data, len);
            free(data);
        } else {
            JADE_LOGE("Failed to serialise settings; this change will not persist");
            ok = false;
        }
    }
    pthread_mutex_unlock(&_settings_mutex);
    return ok;
}

// Called from nvs_flash_erase() after the in-memory maps have been cleared. A zero-length callback
// is an erase request rather than a serialised empty store, so the host can remove every copy.
bool libjade_settings_erased(void)
{
    pthread_mutex_lock(&_settings_mutex);
    const bool ok = !_settings_fn || _settings_fn(NULL, 0, _settings_ctx);
    pthread_mutex_unlock(&_settings_mutex);
    return ok;
}

void libjade_display_size(unsigned int* width_out, unsigned int* height_out)
{
    if (width_out) {
        *width_out = CONFIG_DISPLAY_WIDTH;
    }
    if (height_out) {
        *height_out = CONFIG_DISPLAY_HEIGHT;
    }
}

void libjade_input(const libjade_input_t event)
{
    switch (event) {
    case LIBJADE_INPUT_PREV:
        gui_prev();
        break;
    case LIBJADE_INPUT_NEXT:
        gui_next();
        break;
    case LIBJADE_INPUT_CLICK:
        gui_front_click();
        break;
    case LIBJADE_INPUT_UP:
        gui_up();
        break;
    case LIBJADE_INPUT_DOWN:
        gui_down();
        break;
    case LIBJADE_INPUT_FIRST:
        gui_select_first();
        break;
    case LIBJADE_INPUT_ALT:
        gui_alt_click();
        break;
    }
}

uint32_t libjade_activity_generation(void) { return gui_get_activity_generation(); }

uint32_t libjade_jobs_posted(void) { return gui_get_jobs_posted(); }

uint32_t libjade_jobs_drained(void) { return gui_get_jobs_drained(); }

static void build_display_size_reply(const void* ctx, CborEncoder* container)
{
    JADE_ASSERT(ctx && container);
    CborEncoder map_encoder;
    JADE_ASSERT(cbor_encoder_create_map(container, &map_encoder, 2) == CborNoError);
    add_uint_to_map(&map_encoder, "width", CONFIG_DISPLAY_WIDTH);
    add_uint_to_map(&map_encoder, "height", CONFIG_DISPLAY_HEIGHT);
    JADE_ASSERT(cbor_encoder_close_container(container, &map_encoder) == CborNoError);
}

// libjade RPC handlers
#define CONST_STRNCMP(str, str_len, cmp_to) str_len == sizeof(cmp_to) - 1 && !strncmp(str, cmp_to, str_len)
#define IS_JADE_REQUEST(name) CONST_STRNCMP(request, request_len, name)

void process_libjade_request(const cbor_msg_t* const ctx)
{
    CborValue params;
    if (!rpc_get_map("params", &ctx->value, &params)) {
        goto cleanup;
    }

    const char* request;
    size_t request_len = 0;
    rpc_get_string_ptr("request", &params, &request, &request_len);
    JADE_ASSERT(request_len != 0);

    if (IS_JADE_REQUEST("send_input")) {
        const char* event;
        size_t event_len = 0;
        rpc_get_string_ptr("event", &params, &event, &event_len);
        if (CONST_STRNCMP(event, event_len, "left")) {
            jade_process_reply_to_message_ok_ex(ctx);
            gui_prev();
        } else if (CONST_STRNCMP(event, event_len, "right")) {
            jade_process_reply_to_message_ok_ex(ctx);
            gui_next();
        } else if (CONST_STRNCMP(event, event_len, "click")) {
            jade_process_reply_to_message_ok_ex(ctx);
            gui_front_click();
        } else if (CONST_STRNCMP(event, event_len, "up")) {
            jade_process_reply_to_message_ok_ex(ctx);
            gui_up();
        } else if (CONST_STRNCMP(event, event_len, "down")) {
            jade_process_reply_to_message_ok_ex(ctx);
            gui_down();
        } else if (CONST_STRNCMP(event, event_len, "first")) {
            jade_process_reply_to_message_ok_ex(ctx);
            gui_select_first();
        } else if (CONST_STRNCMP(event, event_len, "alt")) {
            jade_process_reply_to_message_ok_ex(ctx);
            gui_alt_click();
        } else {
            goto cleanup;
        }
        return;
    } else if (IS_JADE_REQUEST("get_display_bytes")) {
        const uint8_t* output = (uint8_t*)display_hw_get_buffer();
        const size_t output_len = CONFIG_DISPLAY_WIDTH * CONFIG_DISPLAY_HEIGHT * sizeof(color_t);
        jade_process_reply_to_message_bytes(ctx, output, output_len);
        return;
    } else if (IS_JADE_REQUEST("get_display_size")) {
        uint8_t buf[128]; // sufficient
        jade_process_reply_to_message_result(ctx, buf, sizeof(buf), &ctx->source, build_display_size_reply);
        return;
    } else if (IS_JADE_REQUEST("set_camera_bytes")) {
        const uint8_t* bytes = NULL;
        size_t bytes_len = 0;
        rpc_get_bytes_ptr("bytes", &params, &bytes, &bytes_len);
        if (libjade_push_camera_frame(bytes, bytes_len)) {
            jade_process_reply_to_message_ok_ex(ctx);
            return;
        }
    } else if (IS_JADE_REQUEST("get_nvs")) {
        uint8_t* output;
        size_t output_len;
        if (libjade_save_nvs(&output, &output_len) == ESP_OK) {
            jade_process_reply_to_message_bytes(ctx, output, output_len);
            JADE_WALLY_VERIFY(wally_bzero(output, output_len));
            free(output);
            return;
        }
    } else if (IS_JADE_REQUEST("set_nvs")) {
        const uint8_t* bytes = NULL;
        size_t bytes_len = 0;
        rpc_get_bytes_ptr("bytes", &params, &bytes, &bytes_len);
        if (bytes_len && libjade_load_nvs(bytes, bytes_len) == ESP_OK) {
            jade_process_reply_to_message_ok_ex(ctx);
            return;
        }
    }

cleanup:
    uint8_t buf[JADE_MSG_REPLY_LEN];
    jade_process_reject_message_ex(ctx, CBOR_RPC_BAD_PARAMETERS, "Unhandled error", NULL, 0, buf, sizeof(buf));
}
