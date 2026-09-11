/* BBB-AIRGAP: report WHICH stage of quirc a frame dies at, and whether halving it revives one.
 *
 * quirc answers "how many codes did you decode", which collapses three very different failures
 * into one number: it never found the finder patterns, it found them but could not build the
 * grid, or it built the grid and the error correction rejected the payload. Item 65 (the camera
 * not reading a QR code held close) was diagnosed with this tool: on the device's own 460x460
 * window the finders were being found and the grid was not, which is what pointed at scale
 * rather than at focus, exposure or the decoder.
 *
 * It also runs the device's own halving (main/qr_downscale.h) so an offline measurement and the
 * device do the same arithmetic instead of two copies that can drift apart.
 *
 * Usage:
 *   qr_stage_probe [--gray] [--half] <file> <width> <height> [expected text]
 *
 *   --gray   input is raw 8-bit grayscale, one byte per pixel (a camera frame)
 *            default is RGB565, two bytes per pixel, high byte first (a display dump)
 *   --half   halve the image before scanning; <width> and <height> still describe the FILE
 *
 * Exit code is 0 when a payload decoded (and matched the expected text, if one was given), 1
 * when nothing decoded, 2 on a usage or I/O error. The stage counters are printed either way.
 *
 * Build it inside the jade-dev container, from the repo root mounted at /jade:
 *
 *   docker exec jade-dev sh -lc 'gcc -O2 -o /tmp/qr_stage_probe \
 *     -include stdlib.h -include string.h /jade/pijade/tools/qr_stage_probe.c \
 *     /jade/components/esp32-quirc/lib/*.c /jade/components/esp32-quirc/openmv/collections.c \
 *     -I/jade/components/esp32-quirc/lib -I/jade/components/esp32-quirc \
 *     -I/jade/libjade/include -I/jade/main -lm'
 *
 * The two -include flags are not decoration: quirc_internal.h reaches for esp_heap_caps.h, which
 * the host build satisfies from libjade/include, and that header expects the C library basics to
 * have been declared already.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quirc.h"
#include "quirc_internal.h"

#include "qr_downscale.h"

int main(int argc, char** argv)
{
    bool gray = false;
    bool half = false;
    int arg = 1;

    for (; arg < argc && argv[arg][0] == '-'; ++arg) {
        if (!strcmp(argv[arg], "--gray")) {
            gray = true;
        } else if (!strcmp(argv[arg], "--half")) {
            half = true;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[arg]);
            return 2;
        }
    }

    if (argc - arg < 3) {
        fprintf(stderr, "usage: %s [--gray] [--half] <file> <width> <height> [expected]\n", argv[0]);
        return 2;
    }

    const char* const path = argv[arg];
    const int w = atoi(argv[arg + 1]);
    const int h = atoi(argv[arg + 2]);
    const char* const expected = (argc - arg > 3) ? argv[arg + 3] : NULL;

    if (w <= 0 || h <= 0 || (half && ((w % 2) || (h % 2)))) {
        fprintf(stderr, "bad dimensions: %dx%d%s\n", w, h, half ? " (--half needs both even)" : "");
        return 2;
    }

    const size_t pixels = (size_t)w * (size_t)h;
    const size_t bytes = gray ? pixels : pixels * 2;

    FILE* const f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 2;
    }
    uint8_t* const raw = malloc(bytes);
    if (!raw) {
        fprintf(stderr, "out of memory\n");
        fclose(f);
        return 2;
    }
    if (fread(raw, 1, bytes, f) != bytes) {
        fprintf(stderr, "short read: %s wanted %zu bytes\n", path, bytes);
        free(raw);
        fclose(f);
        return 2;
    }
    fclose(f);

    /* Whatever the input format, reduce it to one grayscale byte per pixel first, so the
       halving below is fed the same thing the device feeds it. */
    uint8_t* const luma = malloc(pixels);
    if (!luma) {
        fprintf(stderr, "out of memory\n");
        free(raw);
        return 2;
    }
    if (gray) {
        memcpy(luma, raw, pixels);
    } else {
        for (size_t i = 0; i < pixels; ++i) {
            /* Jade keeps colours in panel wire order, high byte first (pijade/tools/rgb2png.py) */
            const uint16_t px = (uint16_t)((raw[i * 2] << 8) | raw[(i * 2) + 1]);
            const int r = ((px >> 11) & 0x1f) << 3;
            const int g = ((px >> 5) & 0x3f) << 2;
            const int b = (px & 0x1f) << 3;
            luma[i] = (uint8_t)(((r * 77) + (g * 151) + (b * 28)) >> 8);
        }
    }
    free(raw);

    int scan_w = w;
    int scan_h = h;
    uint8_t* scan = luma;
    uint8_t* halved = NULL;
    if (half) {
        scan_w = w / 2;
        scan_h = h / 2;
        halved = malloc((size_t)scan_w * (size_t)scan_h);
        if (!halved) {
            fprintf(stderr, "out of memory\n");
            free(luma);
            return 2;
        }
        qr_downscale_half(luma, (size_t)w, (size_t)h, (size_t)w, halved);
        scan = halved;
    }

    struct quirc* const q = quirc_new();
    if (!q || quirc_resize(q, scan_w, scan_h) < 0) {
        fprintf(stderr, "quirc allocation failed for %dx%d\n", scan_w, scan_h);
        free(halved);
        free(luma);
        return 2;
    }

    int qw = 0, qh = 0;
    uint8_t* const image = quirc_begin(q, &qw, &qh);
    memcpy(image, scan, (size_t)qw * (size_t)qh);
    quirc_end(q);

    const int count = quirc_count(q);
    printf("%dx%d regions=%d capstones=%d grids=%d count=%d", scan_w, scan_h, q->num_regions, q->num_capstones,
        q->num_grids, count);

    /* The fork's quirc takes the decoder scratch from the caller so it can choose the memory it
       comes from (main/qrscan.c does the same), and ds->data is a separate allocation. */
    struct datastream* const ds = malloc(sizeof(struct datastream));
    uint8_t* const ds_data = malloc(QUIRC_MAX_PAYLOAD);
    if (!ds || !ds_data) {
        fprintf(stderr, "out of memory\n");
        free(ds_data);
        free(ds);
        quirc_destroy(q);
        free(halved);
        free(luma);
        return 2;
    }
    ds->data = ds_data;

    int rc = 1;
    for (int i = 0; i < count; ++i) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(q, i, &code);
        const quirc_decode_error_t err = quirc_decode(&code, &data, ds);
        if (err != QUIRC_SUCCESS) {
            printf(" decode=%s", quirc_strerror(err));
            continue;
        }
        printf(" payload_len=%u", (unsigned)data.payload_len);
        if (expected) {
            const bool match
                = (strlen(expected) == data.payload_len) && !memcmp(expected, data.payload, data.payload_len);
            printf(" expected=%s", match ? "MATCH" : "MISMATCH");
            if (!match) {
                continue;
            }
        }
        rc = 0;
        break;
    }
    printf(" result=%s\n", rc ? "FAIL" : "OK");

    free(ds_data);
    free(ds);
    quirc_destroy(q);
    free(halved);
    free(luma);
    return rc;
}
