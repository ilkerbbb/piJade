/* T3.19B: decode a raw RGB565 display dump with quirc and compare it to the expected text.
   Verify FROM THE DEVICE DISPLAY that the generated SeedQR carries the correct digit string.
   The library return value is not proof of fit (v3_capacity_probe.c).

   Usage: screen_qr_decode <file.rgb565> <width> <height> [expected_text] */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../components/esp32-quirc/lib/quirc.h"
#include "../../components/esp32-quirc/lib/quirc_internal.h"

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <file.rgb565> <w> <h> [expected]\n", argv[0]);
        return 2;
    }
    const int w = atoi(argv[2]);
    const int h = atoi(argv[3]);
    const char* const expected = argc > 4 ? argv[4] : NULL;

    FILE* const f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    uint8_t* const raw = malloc((size_t)w * h * 2);
    if (fread(raw, 1, (size_t)w * h * 2, f) != (size_t)w * h * 2) {
        fprintf(stderr, "could not read the expected %d bytes\n", w * h * 2);
        return 2;
    }
    fclose(f);

    struct quirc* const q = quirc_new();
    if (!q || quirc_resize(q, w, h) < 0) { fprintf(stderr, "quirc_resize\n"); return 2; }
    int qw = 0, qh = 0;
    uint8_t* const img = quirc_begin(q, &qw, &qh);

    /* RGB565 -> grayscale. Jade keeps colors in panel wire order (rgb2png.py note:
       the first byte is the high byte); approximate weights suffice for luma. */
    int dark_min_x = w, dark_max_x = -1, dark_min_y = h, dark_max_y = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = ((size_t)y * w + x) * 2;
            const uint16_t px = (uint16_t)((raw[i] << 8) | raw[i + 1]);
            const int r = ((px >> 11) & 0x1f) << 3;
            const int g = ((px >> 5) & 0x3f) << 2;
            const int b = (px & 0x1f) << 3;
            const int luma = (r * 77 + g * 151 + b * 28) >> 8;
            img[(size_t)y * qw + x] = (uint8_t)luma;
            if (luma < 128) {
                if (x < dark_min_x) dark_min_x = x;
                if (x > dark_max_x) dark_max_x = x;
                if (y < dark_min_y) dark_min_y = y;
                if (y > dark_max_y) dark_max_y = y;
            }
        }
    }
    quirc_end(q);

    printf("dark pixel bounding box: x %d..%d (%d px), y %d..%d (%d px)\n", dark_min_x, dark_max_x,
        dark_max_x - dark_min_x + 1, dark_min_y, dark_max_y, dark_max_y - dark_min_y + 1);

    if (quirc_count(q) != 1) {
        printf("RESULT: code not found (quirc_count=%d)\n", quirc_count(q));
        return 1;
    }

    struct quirc_code code;
    struct quirc_data data;
    struct datastream* const ds = calloc(1, sizeof(struct datastream));
    /* quirc.h:169 - the caller allocates this pointer */
    if (ds) { ds->data = calloc(QUIRC_MAX_PAYLOAD, sizeof(uint8_t)); }
    quirc_extract(q, 0, &code);
    if (!ds || !ds->data || quirc_decode(&code, &data, ds) != QUIRC_SUCCESS) {
        printf("RESULT: could not decode\n");
        return 1;
    }
    char decoded[QUIRC_MAX_PAYLOAD + 1];
    const size_t len = (size_t)data.payload_len < sizeof(decoded) ? (size_t)data.payload_len : 0;
    memcpy(decoded, data.payload, len);
    decoded[len] = '\0';
    printf("decoded: %zu characters, QR version %d, %d modules\n", len, data.version, code.size);
    printf("content : %s\n", decoded);
    if (expected) {
        const bool same = strcmp(decoded, expected) == 0;
        printf("RESULT: %s as expected string\n", same ? "SAME" : "NOT THE SAME");
        return same ? 0 : 1;
    }
    return 0;
}
