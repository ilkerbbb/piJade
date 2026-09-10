// Verify that libjade can stop and restart in the same process.
// Check that the idle task exits on every stop, the tick base resets on each start,
// and the main/wire.c assertion that ticks never go backwards survives a restart.
//
// Build and run (jade-dev container):
//   gcc -O0 -g -o restart_probe restart_probe.c -L<build>/libjade -ljade \
//       -Wl,-rpath,<build>/libjade -lpthread -lz
//   ./restart_probe
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

void libjade_start(void);
void libjade_stop(void);
void libjade_set_log_level(int level);
void libjade_input(int event); // LIBJADE_INPUT_NEXT = 1
bool libjade_send(const uint8_t* data, size_t len);

// CBOR: {"id":"0","method":"get_version_info"} - main/wire.c processes this and
// prints "at tick N"; the same function also asserts that ticks never go backwards.
static const uint8_t GET_VERSION[] = { 0xa2, 0x62, 0x69, 0x64, 0x61, 0x30, 0x66, 0x6d, 0x65, 0x74,
    0x68, 0x6f, 0x64, 0x70, 0x67, 0x65, 0x74, 0x5f, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x5f,
    0x69, 0x6e, 0x66, 0x6f };

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void nap(int secs)
{
    struct timespec ts = { secs, 0 };
    nanosleep(&ts, NULL);
}

static void nap_ms(const long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(void)
{
    for (int i = 1; i <= 3; ++i) {
        printf("=== PROBE cycle %d: start ===\n", i);
        fflush(stdout);
        libjade_start();
        // Startup window measurement: input after gui setup but before idletimer_init()
        // (main/main.c:206-220) used to trigger the assertion in idletimer_register_activity().
        // With the guard, this call must be silently rejected.
        nap_ms(50);
        libjade_input(1); // LIBJADE_INPUT_NEXT
        libjade_set_log_level(2); // ESP_LOG_INFO
        nap(1);
        printf("=== PROBE cycle %d: ran for 1 s, sending message ===\n", i);
        fflush(stdout);
        libjade_send(GET_VERSION, sizeof(GET_VERSION));
        nap(1);

        const double t0 = now_s();
        libjade_stop();
        printf("=== PROBE cycle %d: stop took %.3f s ===\n", i, now_s() - t0);
        fflush(stdout);

        nap(5);
        printf("=== PROBE cycle %d: waited 5 s while stopped ===\n", i);
        fflush(stdout);
    }
    printf("=== PROBE OK: 3 start/stop cycles completed without abort ===\n");
    return 0;
}
