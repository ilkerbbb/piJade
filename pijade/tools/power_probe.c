/*
 * BBB-AIRGAP: proves that a power request reaches the host instead of aborting inside libjade.
 *
 * Jade's sleep menu entry comes out at esp_deep_sleep_start(), and a factory reset at
 * esp_restart(); upstream's libjade stubs both to abort(), which on the device shows
 * "Internal error WRAPPED:0" and freezes. See libjade_set_power_handler() in libjade.h.
 *
 * What this checks is the libjade half: the handler is called, with the right action, and a handler
 * that returns still ends in abort() - the honest answer when nothing could carry the request out.
 * The host half (`systemctl --no-block --job-mode=replace-irreversibly start poweroff.target`) is
 * not exercised here: it replaces the process, and whether the service may do so is a question
 * about the image, answered on the device.
 *
 * Build against a libjade static build; the command is in pijade/UPSTREAM.md.
 */
#include <stdio.h>
#include <stdlib.h>

#include "libjade.h"

// Declared here rather than pulled from an ESP header: this is the ESP-IDF surface libjade fills in.
void esp_deep_sleep_start(void);
void esp_restart(void);

static int _calls = 0;

static void on_power(const libjade_power_action_t action, void* ctx)
{
    (void)ctx;
    ++_calls;
    printf("probe: handler called action=%d\n", (int)action);
    if (action != LIBJADE_POWER_OFF) {
        printf("probe: FAILED - expected LIBJADE_POWER_OFF (%d)\n", (int)LIBJADE_POWER_OFF);
        exit(EXIT_FAILURE);
    }
    // Returning on purpose: a real host does not come back, and the abort that follows is what a
    // device with no permission to power off would do.
}

int main(void)
{
    // Line buffered so the trace survives the abort below.
    setvbuf(stdout, NULL, _IOLBF, 0);

    libjade_set_power_handler(on_power, NULL);
    esp_deep_sleep_start();

    printf("probe: FAILED - esp_deep_sleep_start() returned, calls=%d\n", _calls);
    return EXIT_FAILURE;
}
