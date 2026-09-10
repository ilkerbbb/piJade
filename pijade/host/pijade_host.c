/*
 * pijade host.
 *
 * This program owns the hardware on the Pi: the ST7789 panel over SPI and the HAT's joystick over
 * GPIO. libjade is linked into this process, so nothing listens on a socket and the debug/RPC
 * surface stays compiled out (see pijade/UPSTREAM.md).
 *
 * Two modes:
 *
 *   device    --panel /dev/spidev0.0 --gpio /dev/gpiochip0 [--camera /dev/video0]
 *             [--settings /boot/firmware/pijade-settings.bin]
 *             Frames go straight to the panel, the joystick drives navigation. With --camera the
 *             device is opened while Jade is scanning and closed again afterwards. Settings use
 *             two slots, <path>.a and <path>.b. Runs until SIGINT or SIGTERM.
 *
 *   headless  no --panel [--settings <path>]
 *             Frames are kept in memory and can be written to a file, input comes from stdin.
 *             This is how the production build (--no-debug) is driven in the emulator, which the
 *             RPC-based tooling cannot do.
 *
 * Headless commands on stdin, one per line:
 *   l | r | u | d  navigate previous / next / up / down
 *   c | 1 | 3      select / first item / screen-specific secondary action
 *   s <path>       write the latest frame to <path> as raw RGB565
 *   f              print how many frames have been flushed so far
 *   q              stop libjade and exit
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "buttons_gpio.h"
#include "camera_v4l2.h"
#include "input_gate.h"
#include "libjade.h"
#include "panel_st7789.h"
#include "settings_store.h"

// The HAT's panel runs happily at 40 MHz, which is what SeedSigner uses on the same board. One
// 240x240 RGB565 frame is 115200 bytes, so a full redraw costs roughly 25 ms on the wire.
#define DEFAULT_SPI_HZ 40000000

/* Jade's camera buffer is fixed at main/camera.h's CAMERA_IMAGE_WIDTH x CAMERA_IMAGE_HEIGHT and
 * libjade_push_camera_frame() rejects anything else. libjade does not export those values, so they
 * are repeated here; a mismatch shows up on the first frame as a rejected push, not as silence. */
#define CAMERA_WIDTH 320
#define CAMERA_HEIGHT 240
#define CAMERA_FRAME_BYTES ((size_t)CAMERA_WIDTH * CAMERA_HEIGHT)

// Long enough that a stalled sensor does not spin this thread, short enough that stopping the
// program does not visibly hang on the last frame.
#define CAMERA_FRAME_TIMEOUT_MS 200
// Idle poll while Jade is not asking for frames, and the pause after a failed open.
#define CAMERA_IDLE_SLEEP_US 100000
#define CAMERA_RETRY_SLEEP_US 500000

/* BBB-AIRGAP: the card log carried a trace of how the device is used even with measurement off.
 * Three lines written on the normal path (panel size, shutdown request, exit on SIGTERM) piled up
 * in /boot/firmware/pijade.log on the unencrypted vfat partition, so whoever holds the card could
 * read how many times the device was opened and how many shutdowns were asked for. Those three
 * lines now sit behind the same operator marker (pijade-t40.enable). Failure branches are NOT
 * behind it: no fork, no waitpid, no systemctl, death by signal and an invalid panel size are
 * still written with no marker present; what is gone is only the trace of a successful run. The
 * marker used to be sampled again inside run_device(); sampling moved here so there is one source
 * and so main()'s first line goes through the same gate.
 * Thread visibility: main() writes this once before ANY thread exists and nobody writes it after,
 * and the readers (the camera thread, the libjade callbacks) start only after that, so unlike
 * g_stop this needs no extra ordering. */
static bool g_flow_trace = false;

static volatile sig_atomic_t g_stop = 0;
/* g_stop is the signal-handler flag and says nothing about visibility between threads: the main
 * thread's write at shutdown is not ordered against the camera thread's reads. This one is. */
static atomic_bool g_camera_stop = false;
static pthread_mutex_t g_panel_state_mutex = PTHREAD_MUTEX_INITIALIZER;
/* BBB-AIRGAP: the input gate's whole state, guarded by g_panel_state_mutex. The rule it applies
 * and the reasoning behind it are in pijade/host/input_gate.h. */
static input_gate_t g_input_gate = { 0 };
static bool g_panel_failed = false;

static void on_signal(int signum)
{
    (void)signum;
    g_stop = 1;
}

static bool now_ns(uint64_t* const out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return false;
    }
    *out = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    return true;
}

static bool panel_failed(void)
{
    pthread_mutex_lock(&g_panel_state_mutex);
    const bool failed = g_panel_failed;
    pthread_mutex_unlock(&g_panel_state_mutex);
    return failed;
}

static pthread_mutex_t g_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint16_t* g_frame = NULL;
static size_t g_frame_len = 0;
static unsigned long g_frame_count = 0;

/* Runs on the gui task with the finished frame; on the Pi this is where the SPI write goes.
 * Copy and return - the gui task is blocked while we are in here. */
static void on_display_flush(const uint16_t* buffer, const size_t len, void* ctx)
{
    (void)ctx;
    if (len != g_frame_len) {
        return;
    }
    pthread_mutex_lock(&g_frame_mutex);
    memcpy(g_frame, buffer, len);
    ++g_frame_count;
    pthread_mutex_unlock(&g_frame_mutex);
}

/* Device mode: Jade turned the display over, so the panel has to follow. libjade runs this under
 * the same lock as the flush handler, so it never lands mid-frame and needs no locking here.
 *
 * A failure stops the device, the same as a failed frame write. panel_set_flipped() leaves the
 * panel in a state nobody knows, and later frames can still be written into it successfully - so
 * the screen keeps updating while showing something other than what Jade believes it shows. That
 * is not a display glitch on a device whose whole job is to let someone read a transaction before
 * approving it. */
static void on_display_orientation_panel(const bool flipped, void* ctx)
{
    if (!panel_set_flipped(ctx, flipped)) {
        pthread_mutex_lock(&g_panel_state_mutex);
        g_panel_failed = true;
        pthread_mutex_unlock(&g_panel_state_mutex);
    }
}

/* BBB-AIRGAP: where Jade's Options > Display > Display Brightness ends up (see
 * libjade_set_backlight_handler in libjade.h). Jade's own boards ask a power management chip for a
 * backlight voltage; here the panel's backlight gpio is dimmed by toggling, as that pin has no
 * hardware PWM behind it.
 *
 * A failure is not treated the way a failed orientation change is. There the screen would show
 * something other than what Jade believes it shows, which is a correctness problem on a signing
 * device; a backlight that did not change is only a brightness the user did not get, and
 * panel_set_backlight() has already fallen back to a lit screen. It prints the reason and carries
 * on rather than stopping the device. */
static void on_backlight_request(const uint8_t level, void* ctx)
{
    panel_set_backlight(ctx, level);
}

/* Device mode: the gui task hands us the frame and we push it out over SPI. Writing here rather
 * than handing off to another thread keeps the frame the panel shows in step with what Jade
 * believes is on screen; the cost is that the gui task waits for the transfer. */
static void on_display_flush_panel(const uint16_t* buffer, const size_t len, void* ctx)
{
    panel_t* const panel = ctx;
    uint64_t start;
    if (!now_ns(&start)) {
        pthread_mutex_lock(&g_panel_state_mutex);
        g_panel_failed = true;
        pthread_mutex_unlock(&g_panel_state_mutex);
        fprintf(stderr, "pijade: cannot read monotonic clock, stopping\n");
        return;
    }

    /* BBB-AIRGAP: which screen this frame carries, and how much of the gui task's queue had been
     * applied when it was composed. Jade updates both on the same gui task that renders and calls
     * us, and it drains before it renders, so both readings are exact for this buffer. Read before
     * the lock is taken: libjade must never be called back into with the panel lock held. */
    const uint32_t screen_gen = libjade_activity_generation();
    const uint32_t drained = libjade_jobs_drained();

    pthread_mutex_lock(&g_panel_state_mutex);
    input_gate_write_begin(&g_input_gate, start, screen_gen, drained);

    /* Keep completion and its publication indivisible to input snapshots. Otherwise SPI can
     * finish, the GUI can be descheduled before write_end, and input can reject a press on the
     * new screen using the still-active state. panel_write_frame calls no libjade functions.
     * Input may wait one transfer here; its original timestamp still decides which screen it saw. */
    const bool frame_written = panel_write_frame(panel, buffer, len);
    uint64_t end;
    const bool have_end = now_ns(&end);

    input_gate_write_end(&g_input_gate, have_end ? end : UINT64_MAX);
    if (!frame_written || !have_end) {
        g_panel_failed = true;
    }
    pthread_mutex_unlock(&g_panel_state_mutex);

    if (!frame_written) {
        // What the panel shows and what Jade thinks is on screen have diverged. On a signing
        // device that pair must hold: a click always applies to the prompt the user is reading.
        // There is no way to re-establish it from here, so stop accepting input.
        fprintf(stderr, "pijade: frame not written to panel, stopping\n");
    }
    if (!have_end) {
        fprintf(stderr, "pijade: cannot read monotonic clock, stopping\n");
    }
}

/*
 * Owns the camera device for as long as Jade wants frames.
 *
 * Jade opens and closes its camera as the user moves in and out of the scanning screens;
 * libjade_camera_active() reports that, so the sensor is only powered while it is in use. On the
 * Pi Zero W the module draws about twice what the board itself does, and the case has no airflow.
 */
static void* camera_thread(void* const ctx)
{
    const char* const device_path = ctx;
    uint8_t* const frame = malloc(CAMERA_FRAME_BYTES);
    if (!frame) {
        fprintf(stderr, "pijade: cannot allocate camera frame buffer\n");
        return NULL;
    }

    camera_t* camera = NULL;
    bool open_failure_reported = false;

    while (!atomic_load(&g_camera_stop)) {
        if (!libjade_camera_active()) {
            if (camera) {
                camera_close(camera);
                camera = NULL;
                open_failure_reported = false;
            }
            /* BBB-AIRGAP: the scan screen is gone, so the last frame - which may be a SeedQR, i.e.
             * the mnemonic - has no reason to sit in this buffer for the rest of the service
             * lifetime.  Wiping only on shutdown left it there through every normal scan, and
             * hanging the wipe off `camera` skipped the case where CAMERA_FRAME_ERROR had already
             * closed the handle - exactly when a captured frame is still in the buffer.  So it is
             * unconditional here (Codex review r1 and r2, 2026-09-03). */
            explicit_bzero(frame, CAMERA_FRAME_BYTES);
            usleep(CAMERA_IDLE_SLEEP_US);
            continue;
        }

        if (!camera) {
            camera = camera_open(device_path, CAMERA_WIDTH, CAMERA_HEIGHT);
            if (!camera) {
                // camera_open() has already said why. Saying it again every half second would
                // bury the rest of the log, so the reason is reported once per attempt series.
                if (!open_failure_reported) {
                    fprintf(stderr, "pijade: camera unavailable, retrying while Jade wants it\n");
                    open_failure_reported = true;
                }
                usleep(CAMERA_RETRY_SLEEP_US);
                continue;
            }
            open_failure_reported = false;
        }

        const camera_result_t result
            = camera_read_gray(camera, frame, CAMERA_FRAME_BYTES, CAMERA_FRAME_TIMEOUT_MS);
        if (result == CAMERA_FRAME_ERROR) {
            // The device is no longer trustworthy - a buffer was lost or an ioctl failed for good.
            // Closing and reopening is the only way back; Jade sees a short gap in frames.
            fprintf(stderr, "pijade: camera error, reopening\n");
            camera_close(camera);
            camera = NULL;
            usleep(CAMERA_RETRY_SLEEP_US);
            continue;
        }
        if (result == CAMERA_FRAME_NONE) {
            continue; // no frame this time; the sensor is still fine
        }

        if (!libjade_push_camera_frame(frame, CAMERA_FRAME_BYTES)) {
            // Only a malformed frame is rejected, and ours is a fixed size, so this means the
            // sizes above no longer match Jade's. Retrying cannot fix that.
            fprintf(stderr, "pijade: libjade rejected a %zu byte frame, stopping camera\n",
                CAMERA_FRAME_BYTES);
            break;
        }
    }

    if (camera) {
        camera_close(camera);
    }
    /* BBB-AIRGAP: the same reasoning as esp_camera_deinit() in libjade: this buffer holds the last
     * camera frame, and a scanned SeedQR frame is the mnemonic. free() only returns the pages to
     * the allocator, so wipe before releasing. */
    explicit_bzero(frame, CAMERA_FRAME_BYTES);
    free(frame);
    return NULL;
}

/* Jade changed or erased a setting. The two slots carry the PIN wallet, so a failed write or erase
 * is not the host's to swallow: the store has already logged why, and the result goes back to Jade
 * through the same storage error path as a failed on-device flash write. */
static bool on_settings_changed(const uint8_t* const data, const size_t len, void* const ctx)
{
    return len == 0 ? settings_store_erase(ctx) : settings_store_write(ctx, data, len);
}

/* Applies the newest valid slot when one exists. Invalid slots have already been reported and are
 * left in place; alternating writes keep the other valid generation. The host always installs
 * the normal write handler, including when libjade rejects the selected payload. */
static void load_settings(settings_store_t* const store)
{
    uint8_t* data = NULL;
    size_t len = 0;
    if (!settings_store_read(store, &data, &len)) {
        return;
    }

    const bool loaded = libjade_load_settings(data, len);
    // BBB-AIRGAP: the slot holds the encrypted wallet and the PIN key, and libjade_load_settings()
    // has copied what it needs into the store, so this buffer is wiped rather than just freed.
    explicit_bzero(data, len);
    free(data);

    if (!loaded) {
        fprintf(stderr, "pijade: stored settings rejected by libjade, starting with defaults\n");
    }
}

/* BBB-AIRGAP: the service runs as root because pijade.service has no User= entry. The board has no
 * RTC and no network, so Jade's epoch input is the only clock source. Nothing is persisted: after a
 * power cut the clock returns to the image creation time, matching an RTC-less Jade. */
/* The epoch is narrowed to time_t inside Jade's own params_set_epoch_time() before this handler
 * sees it, so a 32-bit time_t would wrap a post-2038 epoch silently and still report success. The
 * ARMv6 build therefore compiles everything with _TIME_BITS=64 (pijade/images/build-armv6.sh);
 * this assertion breaks the build if that flag is ever dropped. */
_Static_assert(sizeof(time_t) >= 8, "time_t must be 64-bit; build with -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64");

static int on_clock_set(const int64_t epoch_seconds, void* ctx)
{
    (void)ctx;
    const struct timeval tv = { .tv_sec = (time_t)epoch_seconds, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        const int error = errno;
        fprintf(stderr, "pijade: settimeofday failed: %s\n", strerror(error));
        return error ? error : -1;
    }
    return 0;
}

/* BBB-AIRGAP: where Jade's sleep menu entry and a factory reset end up. T7 masks logind, so the
 * compatibility poweroff and reboot commands each spend two measured 25-second timeouts trying
 * logind before falling back. `systemctl reboot` also tries logind first. Starting the target
 * directly reaches systemd's orderly shutdown path without that delay; force mode is not used
 * because it invokes reboot(2) without unmounting filesystems.
 *
 * Forked rather than exec'd over the top: the binary is there and executable even where this
 * service is not allowed to use it, so an exec would succeed, the tool would exit with an error,
 * and pijade-host would already be gone - a device left on, showing a frozen screen, with nobody
 * left to write the reason down. Forking keeps the failure observable as an exit status.
 *
 * --no-block makes systemctl return after queueing the irreversible replacement job. Returning
 * from here is not an option: libjade would abort over a normal shutdown. Waiting for the child
 * used to make the host report a spurious status 15 when systemd terminated this service first;
 * the SIGTERM branch below is what handles that race now. */
static void on_power_request(const libjade_power_action_t action, void* ctx)
{
    (void)ctx;
    const char* const target = action == LIBJADE_POWER_OFF ? "poweroff.target" : "reboot.target";
    if (g_flow_trace) {
        fprintf(stderr, "power: systemctl start %s\n", target);
    }
    /* fflush(NULL) is independent of tracing: without flushing buffers before fork(), the child
     * writes the same output a second time. Keep this outside the guard. */
    fflush(NULL);

    const pid_t pid = fork();
    if (pid == 0) {
        // Child of a multi-threaded process: only async-signal-safe calls until exec.
        execl("/bin/systemctl", "systemctl", "--no-block", "--job-mode=replace-irreversibly",
            "start", target, (char*)NULL);
        _exit(127);
    }
    if (pid < 0) {
        fprintf(stderr, "power: fork failed: %s\n", strerror(errno));
        return;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "power: waitpid failed: %s\n", strerror(errno));
            return;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        // Accepted; init is taking the machine down. _exit rather than exit: libjade's tasks are
        // still running and atexit handlers have no business firing under them. Flush first,
        // because _exit does not - the log is on the card and is the only account of this.
        fflush(NULL);
        _exit(EXIT_SUCCESS);
    }
    // BBB-AIRGAP: report what waitpid actually said instead of the raw wait status the old line
    // printed, which is what the 2026-09-02 device round logged as "did not shut the machine down,
    // status 15" - 15 being SIGTERM, not an exit code - with the next boot record sitting directly
    // underneath it: the machine had gone down and the log called it a failure. Returning from here
    // reaches libjade's _power_request() (libjade/libjade.c:204), which aborts at :216. Every branch
    // returns and lets that abort happen, except the SIGTERM branch, which exits without abort.
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        // The child's own exit code for "exec did not happen": no such file, not executable, or
        // the image never installed it. A different reason from the tool refusing to do the job.
        fprintf(stderr, "power: could not run systemctl - missing or not executable\n");
    } else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM) {
        // BBB-AIRGAP: this branch does not claim that the machine went down; a signal cannot show
        // that. This handler runs only on a power request, so reaching it already means this process
        // asked for the shutdown. Of the code paths the image actually ships, none was measured
        // sending signals to this unit besides systemd: getty@, serial-getty@, and autovt@ are
        // masked (pijade/images/prepare-image.sh:315-317); ssh and its helpers are masked
        // (:292-295); cron's wants link is removed (:359); and the image installs no watchdog, no
        // dropbear, and no UART console. The enabled units are pinned by an allowlist (:808-810):
        // pijade's own three, dbus, getty.target, systemd-logind, systemd-user-sessions,
        // systemd-update-utmp-runlevel, and systemd-ask-password-wall.path. This treatment rests on
        // that measured set rather than on a proof; a sender outside it is outside the device's
        // threat model.
        // KillMode is left at its default of control-group, so the child in the cgroup is signalled
        // along with the service. --no-block means the child normally exits 0 right after handing
        // the job to systemd; this branch is that race going the other way. Returning would reach
        // the abort described above and paint a second "Internal error / WRAPPED:0" screen after the
        // duress screen. When systemd is already stopping the unit, Restart=on-abort is not applied;
        // the second screen is the defect because it distinguishes a duress wipe from a genuine
        // internal fault.
        if (g_flow_trace) {
            fprintf(stderr, "power: systemctl start %s: child terminated by SIGTERM; exiting without abort\n", target);
        }
        /* Flush before _exit(); done whatever the trace flag says, never inside its guard. */
        fflush(NULL);
        _exit(EXIT_SUCCESS);
    } else if (WIFSIGNALED(status)) {
        // Any other signalled exit looks identical to the child being killed for an unrelated
        // reason: it proves neither that the shutdown is under way nor that it failed, so this
        // records the fact and claims neither.
        // A SIGKILL seen here is not systemd's stop escalation after TimeoutStopSec - that kills
        // the whole control group, this process with it, leaving nobody to observe the child.
        fprintf(stderr, "power: systemctl start %s: child terminated by signal %d\n", target,
            WTERMSIG(status));
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "power: systemctl start %s did not shut the machine down, exit code %d\n", target,
            WEXITSTATUS(status));
    } else {
        // Neither exited nor signalled: waitpid returned something this code does not model (a
        // stopped child, with no WUNTRACED asked for). Report it raw rather than claiming a cause.
        fprintf(stderr, "power: systemctl start %s did not shut the machine down, wait status 0x%x\n", target,
            (unsigned)status);
    }
}

/* Headless mode drives the emulator on a workstation, not the card. Carrying the request out here
 * would take a developer's machine down with it, so it is reported and not acted on; libjade then
 * aborts, which is visible in a test and is also what a device with no permission does. */
static void on_power_request_headless(const libjade_power_action_t action, void* ctx)
{
    (void)ctx;
    printf("power: %s requested (headless, not carried out)\n",
        action == LIBJADE_POWER_OFF ? "off" : "restart");
    fflush(NULL);
}

static int run_device(const char* panel_path, const char* gpio_path, const char* camera_path,
    settings_store_t* const settings_store, const uint32_t spi_hz, const unsigned int width,
    const unsigned int height)
{
    panel_t* const panel = panel_open(panel_path, gpio_path, width, height, spi_hz);
    if (!panel) {
        settings_store_close(settings_store);
        return EXIT_FAILURE;
    }

    buttons_t* const buttons = buttons_open(gpio_path);
    if (!buttons) {
        panel_close(panel);
        settings_store_close(settings_store);
        return EXIT_FAILURE;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* BBB-AIRGAP: operator opt-in on the card, sampled once per run in main() before the first
     * line is written; g_flow_trace is that single sample and the same marker now also gates the
     * three normal-flow log lines. No CLI/image variant and no per-event filesystem polling.
     * Without it this instrument writes nothing: even anonymous drop counts and camera activation
     * anchors would preserve activity history. The leading newline separates this run from a
     * previously interrupted log line. This is a process/test boundary, NOT a camera-screen
     * boundary: the marker is sampled once per run, so the trace spans the whole process rather
     * than one screen. */
    const bool measure_input = g_flow_trace;
    if (measure_input) {
        fprintf(stderr, "\npijade: t40 test begin\n");
    }

    libjade_set_display_flush_handler(on_display_flush_panel, panel);
    libjade_set_power_handler(on_power_request, NULL);
    libjade_set_backlight_handler(on_backlight_request, panel);
    libjade_set_clock_handler(on_clock_set, NULL);
    /* Registered before libjade_start() because gui_init() applies the stored setting on the way
     * up; register it after and a device saved in the flipped orientation comes up unflipped. */
    libjade_set_display_orientation_handler(on_display_orientation_panel, panel);
    /* Both before libjade_start(), and in this order: the settings have to be in the store before
     * startup reads them, and the handler has to be registered before startup writes anything. */
    if (settings_store) {
        load_settings(settings_store);
        libjade_set_settings_handler(on_settings_changed, settings_store);
    }
    libjade_start();

    /* Started after libjade so the first camera_active() query sees real state. A failure here is
     * not fatal: the device still signs, it just cannot scan. */
    pthread_t camera_tid;
    bool camera_running = false;
    if (camera_path) {
        if (pthread_create(&camera_tid, NULL, camera_thread, (void*)camera_path) == 0) {
            camera_running = true;
        } else {
            fprintf(stderr, "pijade: cannot start camera thread, continuing without camera\n");
        }
    }

    bool buttons_failed = false;
    while (!g_stop && !panel_failed()) {
        uint64_t timestamp_ns = 0;
        const button_event_t event = buttons_wait(buttons, 200, &timestamp_ns);
        if (event == BUTTON_ERROR) {
            buttons_failed = true;
            break;
        }
        if (event == BUTTON_NONE) {
            continue;
        }

        /* BBB-AIRGAP: only a press that ACTS is worth discarding. These three are the ones that
         * commit something: the joystick press and KEY2 both send a click, which dispatches
         * whatever button is selected, and KEY3 sends the alt press a screen may treat as cancel
         * (see the switch below). Everything else moves a selection or an index; measured in
         * main/entropy_sources.c and main/qrmode.c, a wheel event only changes what is highlighted
         * or shown and the click is what acts on it. Sending navigation through the gate as well
         * would discard presses for tens of milliseconds after every navigation press, which is
         * the fault this gate was rewritten to remove. */
        const bool is_action = (event == BUTTON_SELECT || event == BUTTON_KEY2 || event == BUTTON_KEY3);

        pthread_mutex_lock(&g_panel_state_mutex);
        const bool failed = g_panel_failed;
        const input_gate_t gate = g_input_gate;
        pthread_mutex_unlock(&g_panel_state_mutex);

        if (failed) {
            break;
        }

        /* See input_gate.h for the rule and what it cannot see. The screen is read here rather
         * than taken from the copy on purpose: it is what tells us Jade has made a new screen
         * current whose frame has not been written yet, which is the window between a swap and
         * the flush that follows it. Safe from this thread, as the only use is an equality test.
         * What this still cannot cover is a swap landing after the decision and before
         * libjade_input; the panel lock cannot be held across a call into Jade. */
        const uint32_t current_gen = libjade_activity_generation();
        const input_gate_verdict_t verdict
            = is_action ? input_gate_judge(&gate, timestamp_ns, current_gen) : INPUT_GATE_ALLOW;
        if (verdict != INPUT_GATE_ALLOW) {
            /* BBB-AIRGAP: this line establishes only that the host gate dropped an input event.
             * frame_requested samples camera init/deinit state AFTER dequeue and the gate's
             * decision, not at the physical press. It does not identify a screen: boot entropy
             * capture and camera help also request frames, and deinit precedes screen restoration.
             * Attributing a drop to an exit attempt requires an isolated operator test; ambiguous
             * runs are inconclusive. Sample only when enabled, with g_panel_state_mutex released.
             *
             * The key's identity is deliberately NOT written. stderr is appended to
             * /boot/firmware/pijade.log on an unencrypted vfat partition
             * (pijade/images/prepare-image.sh, StandardError=append:), so a key sequence written
             * here would hand the PIN and the duress PIN to anyone who reads the card.
             *
             * SINCE 2026-09-09 A LINE MEANS SOMETHING NARROWER than it did in the runs already
             * recorded, in two ways. Only a press that ACTS can be dropped at all now; navigation
             * never reaches this gate (see is_action above). And what is dropped is a press made
             * before the panel showed what it would act on, rather than any press that happened to
             * land inside a panel write (pijade/host/input_gate.h): an ordinary repaint no longer
             * causes a drop. Counts before and after that date are not comparable, and a run of
             * this test now measures a different thing.
             *
             * why= says which clause of the rule dropped it, which is the field to read when a
             * count looks wrong: "pending" means an earlier press had not reached the panel,
             * "screen" means the screen had already been replaced. last_write_us is the duration
             * of the last COMPLETED panel write, 0 before any write or after an unusable end-clock
             * reading (input_gate_last_write_us()). */
            if (measure_input) {
                const bool frame_requested = libjade_camera_active();
                fprintf(stderr, "pijade: t40 drop frame_requested=%u why=%s last_write_us=%" PRIu64 "\n",
                    frame_requested ? 1u : 0u, input_gate_verdict_name(verdict),
                    input_gate_last_write_us(&gate));
            }
            continue;
        }

        /* BBB-AIRGAP: arm the gate on EVERY press we hand over that actually posted work,
         * navigation included. Navigation is not judged by the gate, but it changes the panel, and
         * an acting press made before that change is drawn has to wait for it.
         *
         * Read the job count on both sides of the dispatch. Reading it AFTER is what makes the
         * mark exact: libjade_input posts the press's repaint or screen swap before it returns, so
         * the count then names every job the press produced, and a frame composed earlier cannot
         * meet it. Equal readings mean the press changed nothing - a wheel press at a carousel
         * limit - so nothing is armed and the device cannot stop answering its buttons.
         *
         * libjade_input runs outside the panel lock on purpose: holding it across a call into Jade
         * would break the no-libjade-under-panel-lock rule and could deadlock when input waits for
         * GUI work. A panel failure or screen swap landing in that window is picked up by the next
         * iteration, which re-reads the latched state. */
        const uint32_t jobs_before = libjade_jobs_posted();

        switch (event) {
        case BUTTON_PREV:
            libjade_input(LIBJADE_INPUT_PREV);
            break;
        case BUTTON_NEXT:
            libjade_input(LIBJADE_INPUT_NEXT);
            break;
        case BUTTON_SELECT:
            libjade_input(LIBJADE_INPUT_CLICK);
            break;
        case BUTTON_UP:
            libjade_input(LIBJADE_INPUT_UP);
            break;
        case BUTTON_DOWN:
            libjade_input(LIBJADE_INPUT_DOWN);
            break;
        case BUTTON_KEY1:
            libjade_input(LIBJADE_INPUT_FIRST);
            break;
        case BUTTON_KEY2:
            libjade_input(LIBJADE_INPUT_CLICK);
            break;
        case BUTTON_KEY3:
            libjade_input(LIBJADE_INPUT_ALT);
            break;
        case BUTTON_NONE:
            break;
        case BUTTON_ERROR:
            break;
        }

        const uint32_t jobs_after = libjade_jobs_posted();
        if (jobs_after != jobs_before) {
            /* BBB-AIRGAP: a qualifying flush can finish before we acquire the panel lock below.
             * The gate retains drain-advancing completions and repairs settlement if this mark
             * is already met, without requiring another frame or holding this lock in Jade. */
            uint64_t dispatch_ns;
            const bool have_dispatch = now_ns(&dispatch_ns);
            pthread_mutex_lock(&g_panel_state_mutex);
            input_gate_input_dispatched(
                &g_input_gate, have_dispatch ? dispatch_ns : timestamp_ns, jobs_after);
            pthread_mutex_unlock(&g_panel_state_mutex);
        }
    }

    /* The camera thread watches g_stop; the loop above can also exit on panel or button failure,
     * so it is set here rather than relying on the signal handler having run. Joining before
     * libjade_stop() keeps the thread from pushing into a stopped libjade. */
    g_stop = 1;
    atomic_store(&g_camera_stop, true);
    if (camera_running) {
        pthread_join(camera_tid, NULL);
    }

    libjade_stop();
    // Removal waits for a callback in progress, so the panel is not in use after this returns.
    libjade_set_display_flush_handler(NULL, NULL);
    libjade_set_display_orientation_handler(NULL, NULL);
    libjade_set_settings_handler(NULL, NULL);
    libjade_set_power_handler(NULL, NULL);
    libjade_set_backlight_handler(NULL, NULL);
    libjade_set_clock_handler(NULL, NULL);
    settings_store_close(settings_store);
    const bool failed = panel_failed();
    buttons_close(buttons);
    panel_close(panel);
    return buttons_failed || failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

static bool write_frame(const char* path)
{
    uint16_t* const frame_copy = malloc(g_frame_len);
    if (!frame_copy) {
        return false;
    }

    bool have_frame = false;
    pthread_mutex_lock(&g_frame_mutex);
    if (g_frame_count) {
        memcpy(frame_copy, g_frame, g_frame_len);
        have_frame = true;
    }
    pthread_mutex_unlock(&g_frame_mutex);

    bool ok = false;
    if (have_frame) {
        FILE* const f = fopen(path, "wb");
        if (f) {
            ok = fwrite(frame_copy, 1, g_frame_len, f) == g_frame_len;
            if (fclose(f) != 0) {
                ok = false;
            }
        }
    }
    free(frame_copy);
    return ok;
}

static int run_headless(void)
{
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strcmp(line, "q")) {
            break;
        } else if (!strcmp(line, "l")) {
            libjade_input(LIBJADE_INPUT_PREV);
            printf("ok\n");
        } else if (!strcmp(line, "r")) {
            libjade_input(LIBJADE_INPUT_NEXT);
            printf("ok\n");
        } else if (!strcmp(line, "u")) {
            libjade_input(LIBJADE_INPUT_UP);
            printf("ok\n");
        } else if (!strcmp(line, "d")) {
            libjade_input(LIBJADE_INPUT_DOWN);
            printf("ok\n");
        } else if (!strcmp(line, "c")) {
            libjade_input(LIBJADE_INPUT_CLICK);
            printf("ok\n");
        } else if (!strcmp(line, "1")) {
            libjade_input(LIBJADE_INPUT_FIRST);
            printf("ok\n");
        } else if (!strcmp(line, "3")) {
            libjade_input(LIBJADE_INPUT_ALT);
            printf("ok\n");
        } else if (!strncmp(line, "s ", 2)) {
            printf(write_frame(line + 2) ? "ok %s\n" : "FAILED %s\n", line + 2);
        } else if (!strcmp(line, "f")) {
            pthread_mutex_lock(&g_frame_mutex);
            printf("frames %lu (%zu bytes each)\n", g_frame_count, g_frame_len);
            pthread_mutex_unlock(&g_frame_mutex);
        } else if (line[0]) {
            printf("unknown command: %s\n", line);
        }
        fflush(stdout);
    }

    libjade_stop();
    libjade_set_display_flush_handler(NULL, NULL);
    libjade_set_settings_handler(NULL, NULL);
    libjade_set_power_handler(NULL, NULL);
    return EXIT_SUCCESS;
}

/* BBB-AIRGAP: the level names are kept identical to libjade_daemon's (libjade/daemon.c:410-429), so
 * a flag learned on the emulator means the same thing on the device. The numbers are libjade.h:52's
 * contract: 0 to 4 is decreasing detail, 5 is off. */
#define PIJADE_LOG_NONE 5

static int parse_log_level(const char* name)
{
    static const struct {
        const char* name;
        int level;
    } levels[] = {
        { "verbose", 0 },
        { "debug", 1 },
        { "info", 2 },
        { "warn", 3 },
        { "error", 4 },
        { "none", PIJADE_LOG_NONE },
    };
    for (size_t i = 0; i < sizeof(levels) / sizeof(*levels); ++i) {
        if (!strcmp(name, levels[i].name)) {
            return levels[i].level;
        }
    }
    return -1;
}

static void usage(const char* argv0)
{
    fprintf(stderr,
        "Usage: %s [--panel /dev/spidev0.0 --gpio /dev/gpiochip0] [--camera /dev/video0]\n"
        "          [--settings /boot/firmware/pijade-settings.bin] [--spi-hz N]\n"
        "          [--log-level none|error|warn|info|debug|verbose]\n"
        "Without --panel the program runs headless: frames in memory, commands on stdin.\n"
        "--camera is ignored in headless mode.\n"
        "--settings uses two slots: <path>.a and <path>.b.\n"
        "Without --settings nothing is persisted: every run starts on Jade's defaults.\n"
        "--log-level defaults to none: without it libjade prints nothing at all.\n",
        argv0);
}

int main(int argc, char** argv)
{
    const char* panel_path = NULL;
    const char* gpio_path = "/dev/gpiochip0";
    const char* camera_path = NULL;
    const char* settings_path = NULL;
    uint32_t spi_hz = DEFAULT_SPI_HZ;
    int log_level = PIJADE_LOG_NONE;

    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (!strcmp(argv[i], "--panel") && has_value) {
            panel_path = argv[++i];
        } else if (!strcmp(argv[i], "--gpio") && has_value) {
            gpio_path = argv[++i];
        } else if (!strcmp(argv[i], "--camera") && has_value) {
            camera_path = argv[++i];
        } else if (!strcmp(argv[i], "--settings") && has_value) {
            settings_path = argv[++i];
        } else if (!strcmp(argv[i], "--spi-hz") && has_value) {
            const long value = strtol(argv[++i], NULL, 10);
            if (value <= 0 || value > 80000000) {
                fprintf(stderr, "pijade: --spi-hz out of range\n");
                return EXIT_FAILURE;
            }
            spi_hz = (uint32_t)value;
        } else if (!strcmp(argv[i], "--log-level") && has_value) {
            log_level = parse_log_level(argv[++i]);
            if (log_level < 0) {
                fprintf(stderr, "pijade: --log-level must be one of: none error warn info debug verbose\n");
                return EXIT_FAILURE;
            }
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* BBB-AIRGAP: the daemon sets this after libjade_start(); here it is set before, so the lines
     * written during start-up are captured too. The ordering is safe: the level lives in the global
     * at libjade.c:111 and libjade_start() does not touch it. Setting it in one place is also
     * required, because libjade_start() is called from two paths in this file (device and
     * headless). */
    libjade_set_log_level(log_level);

    /* BBB-AIRGAP: sampled once per run, before the first line is written. A file or a directory
     * will do; the content is never read. Removing the marker while the device is off turns the
     * next run's trace back off. */
    g_flow_trace = access("/boot/firmware/pijade-t40.enable", F_OK) == 0;

    unsigned int width = 0, height = 0;
    libjade_display_size(&width, &height);
    if (g_flow_trace) {
        printf("panel %ux%u\n", width, height);
        fflush(stdout);
    }

    if (!width || !height || width > SIZE_MAX / height / sizeof(*g_frame)) {
        fprintf(stderr, "invalid panel dimensions\n");
        return EXIT_FAILURE;
    }

    settings_store_t* settings_store = NULL;
    if (settings_path) {
        settings_store = settings_store_open(settings_path);
        if (!settings_store) {
            fprintf(stderr, "pijade: cannot open settings store\n");
            return EXIT_FAILURE;
        }
    }

    if (panel_path) {
        return run_device(panel_path, gpio_path, camera_path, settings_store, spi_hz, width, height);
    }

    g_frame_len = (size_t)width * height * sizeof(*g_frame);
    g_frame = malloc(g_frame_len);
    if (!g_frame) {
        fprintf(stderr, "failed to allocate frame buffer\n");
        settings_store_close(settings_store);
        return EXIT_FAILURE;
    }

    libjade_set_display_flush_handler(on_display_flush, NULL);
    libjade_set_power_handler(on_power_request_headless, NULL);
    /* No clock handler in headless mode: it runs on a workstation, whose clock a Jade emulator must
     * not change. The handler-free libjade no-op is the intended behaviour here. */
    // Same order as run_device(), and for the same reasons.
    if (settings_store) {
        load_settings(settings_store);
        libjade_set_settings_handler(on_settings_changed, settings_store);
    }
    libjade_start();

    const int rc = run_headless();
    settings_store_close(settings_store);
    free(g_frame);
    return rc;
}
