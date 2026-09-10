// Verify that the libjade clock handler is called and real time is unchanged without a handler.
// The probe handler does not change the development machine clock.
//
// Build and run (jade-dev container): static recipe in pijade/UPSTREAM.md section 24.
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libjade.h"

// CBOR decoding:
//   a3                                      three-field map
//   62 69 64 61 31                         "id": "1"
//   66 6d 65 74 68 6f 64 69 73 65 74 5f
//      65 70 6f 63 68                       "method": "set_epoch"
//   66 70 61 72 61 6d 73 a1                "params": single-field map
//   65 65 70 6f 63 68 1a 65 53 f1 00       "epoch": 1700000000
static const uint8_t SET_EPOCH_1[] = { 0xa3, 0x62, 0x69, 0x64, 0x61, 0x31, 0x66, 0x6d, 0x65,
    0x74, 0x68, 0x6f, 0x64, 0x69, 0x73, 0x65, 0x74, 0x5f, 0x65, 0x70, 0x6f, 0x63, 0x68, 0x66, 0x70,
    0x61, 0x72, 0x61, 0x6d, 0x73, 0xa1, 0x65, 0x65, 0x70, 0x6f, 0x63, 0x68, 0x1a, 0x65, 0x53, 0xf1,
    0x00 };

// The second request differs only in its "id": "2" field.
static const uint8_t SET_EPOCH_2[] = { 0xa3, 0x62, 0x69, 0x64, 0x61, 0x32, 0x66, 0x6d, 0x65,
    0x74, 0x68, 0x6f, 0x64, 0x69, 0x73, 0x65, 0x74, 0x5f, 0x65, 0x70, 0x6f, 0x63, 0x68, 0x66, 0x70,
    0x61, 0x72, 0x61, 0x6d, 0x73, 0xa1, 0x65, 0x65, 0x70, 0x6f, 0x63, 0x68, 0x1a, 0x65, 0x53, 0xf1,
    0x00 };

static atomic_int g_calls = 0;
static atomic_int_fast64_t g_epoch = 0;

static int on_clock(const int64_t epoch_seconds, void* ctx)
{
    (void)ctx;
    atomic_store(&g_epoch, epoch_seconds);
    atomic_fetch_add(&g_calls, 1);
    return 0;
}

static void nap(void)
{
    const struct timespec ts = { 1, 0 };
    nanosleep(&ts, NULL);
}

static bool contains_error(const uint8_t* const data, const size_t len)
{
    static const uint8_t error_key[] = { 'e', 'r', 'r', 'o', 'r' };
    if (len < sizeof(error_key)) {
        return false;
    }
    for (size_t i = 0; i <= len - sizeof(error_key); ++i) {
        if (memcmp(data + i, error_key, sizeof(error_key)) == 0) {
            return true;
        }
    }
    return false;
}

static bool receive_and_print(const char* const id)
{
    size_t len = 0;
    uint8_t* const response = libjade_receive(1, &len);
    if (!response || !len) {
        printf("PROBE FAILED id=%s no response received\n", id);
        if (response) {
            libjade_release(response);
        }
        return false;
    }

    printf("response id=%s hex=", id);
    for (size_t i = 0; i < len; ++i) {
        printf("%02x", response[i]);
    }
    printf("\n");

    const bool error = contains_error(response, len);
    libjade_release(response);
    if (error) {
        printf("PROBE FAILED id=%s error response received\n", id);
        return false;
    }
    return true;
}

int main(void)
{
    bool ok = true;
    libjade_set_clock_handler(on_clock, NULL);
    libjade_start();
    nap();

    if (!libjade_send(SET_EPOCH_1, sizeof(SET_EPOCH_1))) {
        printf("PROBE FAILED id=1 could not send message\n");
        ok = false;
    }
    nap();
    ok = receive_and_print("1") && ok;
    if (atomic_load(&g_calls) != 1 || atomic_load(&g_epoch) != INT64_C(1700000000)) {
        printf("PROBE FAILED expectation A: calls=%d epoch=%lld\n", atomic_load(&g_calls),
            (long long)atomic_load(&g_epoch));
        ok = false;
    }

    libjade_set_clock_handler(NULL, NULL);
    const time_t before = time(NULL);
    if (!libjade_send(SET_EPOCH_2, sizeof(SET_EPOCH_2))) {
        printf("PROBE FAILED id=2 could not send message\n");
        ok = false;
    }
    nap();
    ok = receive_and_print("2") && ok;
    const time_t after = time(NULL);
    const double elapsed = difftime(after, before);
    if (atomic_load(&g_calls) != 1 || elapsed < 0.0 || elapsed >= 5.0) {
        printf("PROBE FAILED expectation B: calls=%d real clock difference=%.0f s\n", atomic_load(&g_calls), elapsed);
        ok = false;
    }

    libjade_stop();
    if (!ok) {
        return EXIT_FAILURE;
    }
    printf("PROBE OK\n");
    return EXIT_SUCCESS;
}
