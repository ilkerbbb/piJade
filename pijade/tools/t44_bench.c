/* t44: measure quirc cost per frame on the device.
 *
 * WHY: measurements showed dense QR codes could not be read, suggesting a larger
 * quirc window (320x240/220 -> 640x480/460). Decodability was demonstrated through
 * simulation on x86; the only UNANSWERED question is the microsecond cost on ARMv6.
 * This binary only produces that number; it makes no decision, opens no camera, and reads no settings.
 *
 * HOW: compile quirc directly using the same five source files (the list in
 * libjade/libjade.c:69-73) and optimization level as the device. Do not link libjade.so;
 * its symbols are not exported.
 *
 * FIXTURES: embedded module matrices (t44_matrix.h, generator t44_matrix.py). Render
 * the frame from the matrix; commit no binary image and use no real wallet QR code.
 * Optical distortion (blur, noise, tilt) is DELIBERATELY absent: round 6 measurements
 * on x86 put the identification-time difference between clean and distorted synthetic
 * frames below 3% (846/1885 real frame, 817/1861 clean, 837/1894 distorted us). Clean
 * rendering therefore suffices for timing; distortion only affects DECODABILITY,
 * which is outside this binary's responsibility.
 *
 * GATE: exit without action unless /boot/firmware/pijade-t44.enable exists (same pattern
 * as t40, pijade/host/pijade_host.c:471). The first argument can override the marker
 * path, allowing the acceptance path itself to run in the emulator. */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "quirc.h"
#include "quirc_internal.h"

/* The five files used by the device, in the same order as libjade.c. */
#include "lib/decode.c"
#include "lib/identify.c"
#include "lib/quirc.c"
#include "lib/version_db.c"
#include "openmv/collections.c"

#include "t44_matrix.h"

/* Same value as main/qrscan.c:13; the window is derived from it. */
#define SCAN_MARGIN 20
/* Quiet zone, in modules. Same as round 6 fixtures; the frame is already white outside the QR square. */
#define QUIET 2
#define ROUNDS 15

struct capture {
    int w, h; /* capture size */
};

static const struct capture CAPTURES[] = {
    { 320, 240 }, /* current path; window 220 */
    { 640, 480 }, /* path proposed by item 44; window 460 */
};

static uint64_t now_us(clockid_t clock)
{
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int compare(const void* a, const void* b)
{
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static bool module_dark(const struct t44_matrix* m, int mx, int my)
{
    if (mx < 0 || my < 0 || mx >= m->n || my >= m->n) {
        return false; /* quiet zone */
    }
    const int row_bytes = (m->n + 7) / 8;
    return (m->bit[my * row_bytes + mx / 8] >> (7 - (mx % 8))) & 1;
}

/* Draw the matrix in the center of a white WxH frame. The target side preserves the
 * real scene ratio: QR + quiet zone is 91.7% of frame height (220 at 240, 440 at 480),
 * as when the user fills the guide box with the QR. Average each target pixel over
 * 4x4 subsamples to produce soft edges like optical sampling even with noninteger
 * pixels per module, avoiding artificial nearest-neighbour stair steps. */
static void draw_frame(const struct t44_matrix* m, int W, int H, uint8_t* frame, int* target_out, double* module_px)
{
    const int target = (int)lround((double)H * 220.0 / 240.0);
    const int total_modules = m->n + 2 * QUIET;
    const double scale = (double)total_modules / (double)target;

    memset(frame, 255, (size_t)W * (size_t)H);
    const int x0 = (W - target) / 2, y0 = (H - target) / 2;

    for (int py = 0; py < target; ++py) {
        for (int px = 0; px < target; ++px) {
            int dark = 0;
            for (int sy = 0; sy < 4; ++sy) {
                const double fy = ((double)py + ((double)sy + 0.5) / 4.0) * scale - QUIET;
                for (int sx = 0; sx < 4; ++sx) {
                    const double fx = ((double)px + ((double)sx + 0.5) / 4.0) * scale - QUIET;
                    dark += module_dark(m, (int)floor(fx), (int)floor(fy)) ? 1 : 0;
                }
            }
            frame[(size_t)(y0 + py) * (size_t)W + (size_t)(x0 + px)] = (uint8_t)(255 - (dark * 255) / 16);
        }
    }
    *target_out = target;
    *module_px = (double)target / (double)total_modules;
}

/* Same crop as main/qrscan.c:69-115: the quirc window in the center of the frame. */
static void crop(const uint8_t* frame, int W, int H, uint8_t* window, int qw, int qh)
{
    const int xo = (W - qw) / 2, yo = (H - qh) / 2;
    for (int y = 0; y < qh; ++y) {
        memcpy(window + (size_t)y * (size_t)qw, frame + (size_t)(y + yo) * (size_t)W + (size_t)xo, (size_t)qw);
    }
}

static bool measure(const struct t44_matrix* m, int W, int H)
{
    const int window = (W < H ? W : H) - SCAN_MARGIN;

    uint8_t* const frame = malloc((size_t)W * (size_t)H);
    uint8_t* const cropped = malloc((size_t)window * (size_t)window);
    struct quirc* const q = quirc_new();
    struct datastream* const ds = malloc(sizeof(struct datastream));
    uint8_t* const ds_data = malloc(QUIRC_MAX_PAYLOAD);
    if (!frame || !cropped || !q || !ds || !ds_data || quirc_resize(q, window, window) < 0) {
        fprintf(stderr, "pijade: t44 memory/setup error (%s %dx%d)\n", m->name, W, H);
        free(frame);
        free(cropped);
        free(ds);
        free(ds_data);
        if (q) {
            quirc_destroy(q);
        }
        return false;
    }
    ds->data = ds_data;

    int target = 0;
    double module_px = 0.0;
    draw_frame(m, W, H, frame, &target, &module_px);
    crop(frame, W, H, cropped, window, window);

    uint64_t identify[ROUNDS], decode[ROUNDS];
    int found = 0;
    bool decoded = false;
    const char* last_error = "(none)";
    const uint64_t cpu0 = now_us(CLOCK_THREAD_CPUTIME_ID);

    for (int t = 0; t < ROUNDS; ++t) {
        int qw = 0, qh = 0;
        const uint64_t a = now_us(CLOCK_MONOTONIC);
        uint8_t* const image = quirc_begin(q, &qw, &qh);
        memcpy(image, cropped, (size_t)qw * (size_t)qh);
        quirc_end(q);
        const uint64_t b = now_us(CLOCK_MONOTONIC);

        found = quirc_count(q);
        decoded = false;
        for (int i = 0; i < found; ++i) {
            struct quirc_code code;
            struct quirc_data data;
            quirc_extract(q, i, &code);
            const quirc_decode_error_t error = quirc_decode(&code, &data, ds);
            if (error == QUIRC_SUCCESS) {
                decoded = true;
            } else {
                last_error = quirc_strerror(error);
            }
        }
        const uint64_t c = now_us(CLOCK_MONOTONIC);
        identify[t] = b - a;
        decode[t] = c - b;
    }
    const uint64_t cpu = now_us(CLOCK_THREAD_CPUTIME_ID) - cpu0;

    qsort(identify, ROUNDS, sizeof(identify[0]), compare);
    qsort(decode, ROUNDS, sizeof(decode[0]), compare);

    printf("pijade: t44 %s version=%d modules=%d frame=%dx%d window=%d qr_px=%d module_px=%.2f\n", m->name, m->version, m->n,
        W, H, window, target, module_px);
    printf("pijade: t44   identify_us=%" PRIu64 "/%" PRIu64 "/%" PRIu64 " decode_us=%" PRIu64 "/%" PRIu64 "/%" PRIu64
           " frame_us=%" PRIu64 " cpu_us=%" PRIu64 " found=%d decoded=%d error=%s\n",
        identify[0], identify[ROUNDS / 2], identify[ROUNDS - 1], decode[0], decode[ROUNDS / 2], decode[ROUNDS - 1],
        identify[ROUNDS / 2] + decode[ROUNDS / 2], cpu / ROUNDS, found, decoded ? 1 : 0, decoded ? "(none)" : last_error);

    free(frame);
    free(cropped);
    free(ds);
    free(ds_data);
    quirc_destroy(q);
    return true;
}

int main(int argc, char** argv)
{
    const char* const marker = (argc > 1) ? argv[1] : "/boot/firmware/pijade-t44.enable";
    if (access(marker, F_OK) != 0) {
        return 0; /* measurement disabled; exit silently */
    }

    printf("pijade: t44 measurement started (marker %s, rounds %d)\n", marker, ROUNDS);
    bool ok = true;
    for (size_t i = 0; i < sizeof(t44_matrices) / sizeof(t44_matrices[0]); ++i) {
        for (size_t g = 0; g < sizeof(CAPTURES) / sizeof(CAPTURES[0]); ++g) {
            ok = measure(&t44_matrices[i], CAPTURES[g].w, CAPTURES[g].h) && ok;
        }
    }
    printf("pijade: t44 measurement complete (%s)\n", ok ? "ok" : "INCOMPLETE");
    fflush(stdout);
    return ok ? 0 : 1;
}
