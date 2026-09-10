#ifndef _LIBJADE_ESP_TIMER_H_
#define _LIBJADE_ESP_TIMER_H_ 1

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

// BBB-AIRGAP: needed by components/miner/miner.c, which times its hash loop with this call.
// esp-idf returns microseconds since boot; the miner only ever subtracts two readings
// (components/miner/miner.c:755 start, :818 difference), so any monotonic origin serves. Same
// clock source as xTaskGetTickCount() (libjade/task.c:280), at microsecond resolution instead of
// milliseconds. Deliberately NOT tied to libjade_tick_epoch_reset() (libjade/task.c:270): that
// epoch exists so a second session's idle timer does not inherit the first one's uptime, and
// hanging an elapsed-time measurement off a resettable origin would let a reset land between the
// two readings and turn a duration negative. Only the one function the miner uses is shimmed.
static inline int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        abort();
    }
    return ((int64_t)ts.tv_sec * 1000000) + (ts.tv_nsec / 1000);
}

#endif // _LIBJADE_ESP_TIMER_H_
