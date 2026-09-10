#ifndef AMALGAMATED_BUILD
#include "idletimer.h"
#include "gui.h"
#include "jade_assert.h"
#include "jade_tasks.h"
#include "keychain.h"
#include "power.h"
#include "storage.h"
#include "ui.h"
#include "utils/event.h"
#include <esp_system.h>
#ifdef CONFIG_LIBJADE
#include <pthread.h> // BBB-AIRGAP: cleanup handlers keep teardown observable after pthread_exit().
#include <stdatomic.h> // BBB-AIRGAP: atomics synchronize idle-task lifecycle state between threads.
#endif

#define DEFAULT_IDLE_TIMEOUT_SECS 600
// BBB-AIRGAP: the dimming threshold used to be this fixed constant; it is now a stored
// preference (Preferences > Screen Timeout) and this value is only the initial default.
#define DEFAULT_SCREEN_TIMEOUT_SECS 60
#define TIMEOUT_SLEEP_PERIOD_SECS 60
#define KEEP_AWAKE_WARNING_SECS 10

#define SECS_TO_TICKS(secs) (secs * 1000 / portTICK_PERIOD_MS)

// The 'last activity' counters, protected by a mutex
static TickType_t last_activity_registered = 0;
static TickType_t last_ui_activity_registered = 0;
static SemaphoreHandle_t last_activity_mutex = NULL;
static uint16_t min_timeout_override_secs = 0;
static bool screen_dimmed = false;

// Set if unit reboots idle/screen powered off
// NOTE: we do not use an 'is_idle' bool here, as the first time the unit boots with
// this variable being read (eg. after the initial OTA to this version) the value will
// be uninitialised (and very unlikely to be 0x00/false).
typedef enum { NORMAL, IDLE } idle_state_t;
static __NOINIT_ATTR idle_state_t idle_state;

#ifdef CONFIG_LIBJADE
// BBB-AIRGAP: on a Jade the idle task runs until the power goes away, so upstream never needs to
// take it down. libjade can be stopped and started repeatedly inside one process (see libjade.h),
// and pijade-host stops it on every clean exit, so here the task has to be able to end: left alive
// it would keep polling a gui and an event loop that libjade_stop() has already torn down, and the
// next start would add a second task alongside it. Same shape as gui_stop() in main/gui.c.
static _Atomic bool idletimer_task_should_run = false;
static _Atomic bool idletimer_task_running = false;
// BBB-AIRGAP: the loop works out how long to sleep from a snapshot taken before the sleep, so
// anything that changes the answer during the sleep stays invisible until the sleep ends. Two
// things change it: the user editing a timeout, and a keypress that wakes a dimmed screen and so
// starts a fresh dimming countdown. Measured on the emulator 2026-09-03 with the screen timeout
// at 30 seconds: waking the panel and then leaving the device alone dimmed it again 60 seconds
// later, a whole period, not 30. This flag ends the current sleep so the next one is computed
// from current values.
static _Atomic bool idletimer_recheck_requested = false;
#define IDLETIMER_TASK_SHOULD_RUN idletimer_task_should_run

// BBB-AIRGAP: pthread_exit() can bypass the task's normal return path during teardown; this is the
// single exit path that releases idletimer_stop().
static void idletimer_task_cleanup(void* ignore) { idletimer_task_running = false; }

// Sleeps in short steps so a stop request is noticed within a tenth of a second rather than at the
// end of a period that can be a full minute long.
static void idletimer_delay(const TickType_t delay)
{
    const TickType_t slice = 100 / portTICK_PERIOD_MS;
    TickType_t remaining = delay;
    while (remaining && idletimer_task_should_run) {
        if (idletimer_recheck_requested) {
            idletimer_recheck_requested = false;
            return;
        }
        const TickType_t step = remaining < slice ? remaining : slice;
        vTaskDelay(step);
        remaining -= step;
    }
}
#else
#define IDLETIMER_TASK_SHOULD_RUN true
#define idletimer_delay(delay) vTaskDelay(delay)
#endif // CONFIG_LIBJADE

static void set_screen_dimmed(const bool dimmed)
{
#ifdef CONFIG_LIBJADE
    // BBB-AIRGAP: turn the panel off rather than down. On the boards upstream dims, a level between
    // off and full is a value written to a power management chip and costs nothing. piJade has no
    // such chip: the backlight is a plain gpio line and every intermediate level runs a 200 Hz
    // software pwm thread (pijade/host/panel_st7789.c:329-367, one gpio ioctl and one sleep per
    // edge). On a single core busy with something else - mining saturates it - that thread's period
    // drifts and the drift is visible as flicker. Measured on the device 2026-09-02: dimmed
    // flickers, and a keypress that restores full brightness stops it, because full drives the line
    // steadily and runs no thread.
    //
    // So the idle screen goes dark here instead of dim, and any press brings it back at the stored
    // level. What this does not cover: a user who picks brightness 2, 3 or 4 from the menu is
    // asking for the pwm thread and may see the same flicker at that level. The default is 5
    // (display.c:297-299), which drives the line steadily.
    if (dimmed) {
        power_backlight_off();
    } else {
        power_backlight_on(storage_get_brightness());
    }
#else
    power_backlight_on(dimmed ? BACKLIGHT_MIN : storage_get_brightness());
#endif
    screen_dimmed = dimmed;
}

// Function to (temporarily?) set a minimum timeout value
// eg. to set temporarilty while doing a 'slow' operation where
// you don't want the hw hitting the idle timeout and shutting down.
// eg. using the camera to scan large qrs or similar.
// BBB-AIRGAP: the idle task may already be sleeping on a deadline computed with the previous
// override, so changing the field alone is not enough.  Leaving the camera or a QR display drops
// the override back to zero, and without a recheck the ordinary screen keeps the extended
// deadline until that sleep ends: with Screen Timeout at 30s it could stay lit for the whole
// checking period.  Entering those screens takes the same path, where the recheck only makes the
// longer timeout apply at once instead of one period later.
void idletimer_set_min_timeout_secs(const uint16_t min_timeout_secs)
{
    min_timeout_override_secs = min_timeout_secs;
    idletimer_recheck();
}

// BBB-AIRGAP: ends the idle task's current sleep so that it recomputes from current values; see
// the flag above for what goes wrong without it. On esp32 the sleep is one uninterruptible
// vTaskDelay() and piJade builds no esp32 image, so that path keeps upstream's behaviour rather
// than paying a wakeup every 100 ms on battery.
void idletimer_recheck(void)
{
#ifdef CONFIG_LIBJADE
    idletimer_recheck_requested = true;
#endif
}

// Function to register activity
bool idletimer_register_activity(const bool is_ui)
{
#ifdef CONFIG_LIBJADE
    // BBB-AIRGAP: input can arrive before the idle-timer mutex exists during libjade startup.
    if (!idletimer_task_running) {
        return false;
    }
#endif
    JADE_ASSERT(last_activity_mutex);

    // Take the semaphore and put the tick time in the counter
    while (xSemaphoreTake(last_activity_mutex, portMAX_DELAY) != pdTRUE) {
        // wait for the mutex
    }

    // Register activity, and optionally 'ui activity'
    idle_state = NORMAL;
    last_activity_registered = xTaskGetTickCount();
    if (is_ui) {
        last_ui_activity_registered = last_activity_registered;
    }

    xSemaphoreGive(last_activity_mutex);

    // UI activity ensures screen fully on
    if (is_ui && screen_dimmed) {
        JADE_LOGI("Activity while screen disabled - powering screen");
        set_screen_dimmed(false);
        // BBB-AIRGAP: the screen is lit again, so a fresh dimming countdown starts now.  The idle
        // task is part way through a sleep sized for the old projection and would otherwise not
        // look at the clock again until that sleep ended.
        idletimer_recheck();
        return true;
    }
    return false;
}

// Function to get last registered activity time
static TickType_t get_last_registered_activity(const bool ui)
{
    JADE_ASSERT(last_activity_mutex);

    // Get the last activity time
    while (xSemaphoreTake(last_activity_mutex, portMAX_DELAY) != pdTRUE) {
        // wait for the mutex
    }
    const TickType_t last_activity = ui ? last_ui_activity_registered : last_activity_registered;
    xSemaphoreGive(last_activity_mutex);
    return last_activity;
}

static bool show_timeout_warning_screen(void)
{
    gui_activity_t* const prior_activity = gui_current_activity();

    const char* message[] = { "Jade preparing to sleep", "", "Press button to", "keep awake." };
    gui_activity_t* const act = display_message_activity(message, 4);
    const bool ret = gui_activity_wait_event(
        act, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, NULL, NULL, SECS_TO_TICKS(KEEP_AWAKE_WARNING_SECS));

    // Replace prior activity if we're still current
    if (gui_current_activity() == act) {
        gui_set_current_activity(prior_activity);
    }

    return ret;
}

// The idle timer task - loops, waking periodically to check the time since
// the last registered user activity.  If sufficiently long ago, deactivates
// the device, after having diplayed a warning/cancel screen for a few seconds.
static void idletimer_task(void* ignore)
{
#ifdef CONFIG_LIBJADE
    pthread_cleanup_push(idletimer_task_cleanup, NULL);
#endif
    const TickType_t period = SECS_TO_TICKS(TIMEOUT_SLEEP_PERIOD_SECS);
    while (IDLETIMER_TASK_SHOULD_RUN) {
        // Always fetch the timeout period, in case the user has changed it
        uint16_t timeout_secs = storage_get_idle_timeout();
        if (timeout_secs < min_timeout_override_secs) {
            timeout_secs = min_timeout_override_secs;
        }

        // NOTE: timeout secs set to UINT16_MAX means 'never time-out'
        const bool idle_timeout_disabled = (timeout_secs == UINT16_MAX);
        const TickType_t timeout = SECS_TO_TICKS(timeout_secs);

        const TickType_t last_activity = get_last_registered_activity(false);
        const TickType_t checktime = xTaskGetTickCount();

        // See if the last activity was sufficiently long ago
        const TickType_t projected_timeout_time = last_activity + timeout;
        JADE_LOGI(
            "Idle-timeout check - last-activity: %lu, timeout period: %lu, projected-timeout: %lu, checktime: %lu",
            last_activity, timeout, projected_timeout_time, checktime);
        JADE_LOGI("Idle task stack HWM: %u free", uxTaskGetStackHighWaterMark(NULL));

        // If we are already flagged as idle, or the idle-timeout is explicitly disabled, we skip these checks
        if (!idle_timeout_disabled && (projected_timeout_time <= checktime)) {
            // If usb is connected instead of deactivating we can reboot (if wallet loaded) and dim the screen
            typedef enum { SCREEN_DIMMED, REBOOT, POWER_OFF } reset_action_t;
            reset_action_t action = !usb_is_powered() ? POWER_OFF : (keychain_get() ? REBOOT : SCREEN_DIMMED);
            JADE_LOGW("Idle-timeout elapsed - action: %u", action);

            if (action != SCREEN_DIMMED) {
                // reboot/power-off device - give user last chance ...
                const bool acted = show_timeout_warning_screen();

                // Check the activity time again, if it was recent we can cancel the power-off
                if (acted || get_last_registered_activity(false) > checktime) {
                    // User pressed something or message arrived - recheck from the top.
                    // BBB-AIRGAP: upstream slept a whole period here, which discards the screen
                    // timeout for that period.  Every press path registers activity before it
                    // posts the gui event (gui.c:2665, 2722, 2749), so the next pass always sees
                    // fresh activity and cannot come straight back into this warning.
                    JADE_LOGI("Cancelling idle-timeout, rechecking");
                    continue;
                }

                // BBB-AIRGAP: this cuts the stop-to-power-action window from ten seconds to instants;
                // the theoretical race from a stop arriving just after this check cannot be closed by any
                // flag protocol.
                if (!IDLETIMER_TASK_SHOULD_RUN) {
                    continue;
                }

#if defined(CONFIG_DEBUG_UNATTENDED_CI) || defined(CONFIG_ETH_USE_OPENETH)
                // Don't reboot or power-off in unattended/ci build or in emulator
                action = SCREEN_DIMMED;
#endif
            }

            // Sometimes we can reboot and/or dim the screen rather than power-off
            // eg. if connected via usb this may be a more sensible option.
            idle_state = IDLE;
            switch (action) {
            case POWER_OFF:
                power_backlight_off();
                keychain_clear();
                power_shutdown();
                break;
            case REBOOT:
                power_backlight_off();
                keychain_clear();
                esp_restart();
                break;
            default:
                if (!screen_dimmed) {
                    set_screen_dimmed(true);
                }
            }
        }

        // If we did not idle time-out entirely we may still dim the screen if no physical interaction.
        // BBB-AIRGAP: always fetch the screen timeout too, in case the user has changed it.
        // NOTE: screen timeout set to UINT16_MAX means 'never dim the screen'
        uint16_t screen_timeout_secs = storage_get_screen_timeout();
        const bool screen_timeout_disabled = (screen_timeout_secs == UINT16_MAX);

        // BBB-AIRGAP: the camera and the QR-display screens ask to be left alone for longer
        // (idletimer_set_min_timeout_secs(); camera.c, qrmode.c), but that request only reached
        // the power-off timeout above - never this one.  So the panel went dark 60s in while the
        // viewfinder was the thing being looked at, and the press that woke it was discarded
        // (register_activity() returns true, and gui.c returns on it) - which reads as a joystick
        // that drops presses.  qrscan.c only registers activity on a DECODED code, so lining a QR
        // up registers nothing at all: the camera screen is the easiest one of all to dim.
        // UINT16_MAX ('never dim') survives the comparison untouched.
        if (screen_timeout_secs < min_timeout_override_secs) {
            screen_timeout_secs = min_timeout_override_secs;
        }
        const TickType_t projected_ui_timeout_time
            = get_last_registered_activity(true) + SECS_TO_TICKS(screen_timeout_secs);
        if (!screen_dimmed && !screen_timeout_disabled && projected_ui_timeout_time <= checktime) {
            // deactivate the screen
            JADE_LOGW("Idle-timeout - dimming screen");
            set_screen_dimmed(true);
        }

        // If projected timeout is imminent, only sleep until then.
        // Otherwise sleep for our regular checking period.
        // (We have to wake up before the projected timeout in case the user
        // reduces the timeout period of the device in the interim.)
        // BBB-AIRGAP: the screen-timeout projection counts here as well. It is user-configurable
        // now, so waking only for the power-off projection would round every dimming value up to
        // the next checking period and make the shorter settings indistinguishable.
        TickType_t next_check_time = projected_timeout_time;
        if (!screen_dimmed && !screen_timeout_disabled && projected_ui_timeout_time < next_check_time) {
            next_check_time = projected_ui_timeout_time;
        }
        const TickType_t delay = next_check_time > checktime && next_check_time < checktime + period
            ? next_check_time - checktime
            : period;
        JADE_LOGI("Next check in %lu", delay);
        idletimer_delay(delay);
    }
#ifdef CONFIG_LIBJADE
    pthread_cleanup_pop(1);
#endif
}

void idletimer_init(void)
{
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    JADE_LOGI("esp_reset_reason: %u", reset_reason);

    // Reset idle_state to NORMAL if this is not a software restart.
    // (If this *is* a sw restart, then 'idle_state' retains its value)
    if (reset_reason != ESP_RST_SW) {
        JADE_LOGI("Resetting idle-state flag");
        idle_state = NORMAL;
    }
    JADE_LOGI("idle_state: %u", idle_state);

    // If this is a soft-reset due to inactivity, do not power the screen.
    // In most cases - we power the screen backlight.
    const bool start_dimmed = (idle_state == IDLE);
    JADE_LOGI("powering screen, dimmed mode: %u", start_dimmed);
    set_screen_dimmed(start_dimmed);

    // Create mutext semaphore.
    last_activity_mutex = xSemaphoreCreateMutex();
    JADE_ASSERT(last_activity_mutex);

    // Default timeout time if not set
    const uint16_t timeout_secs = storage_get_idle_timeout();
    if (timeout_secs == 0) {
        storage_set_idle_timeout(DEFAULT_IDLE_TIMEOUT_SECS);
    }

    // BBB-AIRGAP: same for the screen (dimming) timeout
    const uint16_t screen_timeout_secs = storage_get_screen_timeout();
    if (screen_timeout_secs == 0) {
        storage_set_screen_timeout(DEFAULT_SCREEN_TIMEOUT_SECS);
    }

    // Kick off the idletimer task
#ifdef CONFIG_LIBJADE
    // Raised before the task exists so a stop arriving immediately after still waits for it.
    idletimer_task_should_run = true;
    idletimer_task_running = true;
#endif
#ifdef CONFIG_IDF_TARGET_ESP32S3
    const size_t stack_size = (2 * 1024) + 512;
#else
    const size_t stack_size = 2 * 1024;
#endif
    const BaseType_t retval = xTaskCreatePinnedToCore(
        idletimer_task, "idle_timeout", stack_size, NULL, JADE_TASK_PRIO_IDLETIMER, NULL, JADE_CORE_PRIMARY);
    JADE_ASSERT_MSG(
        retval == pdPASS, "Failed to create idle_timeout task, xTaskCreatePinnedToCore() returned %d", retval);
}

// BBB-AIRGAP: request idle-task shutdown before libjade teardown can block, without freeing resources.
void idletimer_request_stop(void)
{
#ifdef CONFIG_LIBJADE
    idletimer_task_should_run = false;
#endif // CONFIG_LIBJADE
}

// BBB-AIRGAP: ends the idle task and returns the module to the state idletimer_init() expects, so
// the next libjade_start() begins with no leftover activity times and a single task. A no-op on
// real hardware, where nothing takes the firmware down while the machine is still on.
void idletimer_stop(void)
{
#ifdef CONFIG_LIBJADE
    JADE_ASSERT(last_activity_mutex);

    // Stop the idle task and wait for it to fully exit. The wait is normally under a tenth of a
    // second; it can reach ten if the stop lands while the 'preparing to sleep' warning is on
    // screen, which needs the full idle timeout to have elapsed first.
    idletimer_task_should_run = false;
    while (idletimer_task_running) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    vSemaphoreDelete(last_activity_mutex);
    last_activity_mutex = NULL;
    last_activity_registered = 0;
    last_ui_activity_registered = 0;
    min_timeout_override_secs = 0;
    screen_dimmed = false;
#endif // CONFIG_LIBJADE
}
#endif // AMALGAMATED_BUILD
