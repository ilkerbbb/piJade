/* T3.19B preliminary measurement: does the generated QR carry the rendered digit string?
   Prove it by reading back with quirc, independently of the library return value. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../main/qrcode.h"
#include "../../components/esp32-quirc/lib/quirc.h"
#include "../../components/esp32-quirc/lib/quirc_internal.h"

#define PX 6      /* pixels per module */
#define QZ 4      /* quiet zone, modules */

static unsigned failures;

static void check(const bool ok, const char* const what)
{
    printf("%-70s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) { ++failures; }
}

/* Render the QR module grid as a grayscale image and decode it with quirc.
   Write decoded text to out; on failure, out stays empty. */
static bool decode_qr(const QRCode* const qr, char* const out, const size_t out_len)
{
    const int side = (qr->size + 2 * QZ) * PX;
    struct quirc* const q = quirc_new();
    if (!q) { return false; }
    if (quirc_resize(q, side, side) < 0) { quirc_destroy(q); return false; }

    int w = 0, h = 0;
    uint8_t* const img = quirc_begin(q, &w, &h);
    memset(img, 0xff, (size_t)w * h);
    for (uint8_t my = 0; my < qr->size; ++my) {
        for (uint8_t mx = 0; mx < qr->size; ++mx) {
            if (!qrcode_getModule((QRCode*)qr, mx, my)) { continue; }
            for (int dy = 0; dy < PX; ++dy) {
                const int py = (my + QZ) * PX + dy;
                memset(img + (size_t)py * w + (size_t)(mx + QZ) * PX, 0x00, PX);
            }
        }
    }
    quirc_end(q);

    bool ok = false;
    if (quirc_count(q) == 1) {
        struct quirc_code code;
        struct quirc_data data;
        struct datastream* const ds = calloc(1, sizeof(struct datastream));
        /* quirc.h:169 - the caller allocates this pointer (as in qrscan.c:148) */
        if (ds) { ds->data = calloc(QUIRC_MAX_PAYLOAD, sizeof(uint8_t)); }
        quirc_extract(q, 0, &code);
        if (ds && quirc_decode(&code, &data, ds) == QUIRC_SUCCESS
            && (size_t)data.payload_len < out_len) {
            memcpy(out, data.payload, data.payload_len);
            out[data.payload_len] = '\0';
            ok = true;
        }
        if (ds) { free(ds->data); }
        free(ds);
    }
    quirc_destroy(q);
    return ok;
}

static void roundtrip(const uint8_t version, const char* const digits, const bool expect_match,
                      const char* const what)
{
    uint8_t buf[256];
    QRCode qr;
    const int r = qrcode_initText(&qr, buf, version, ECC_LOW, (char*)digits);
    if (r != 0) { check(false, what); printf("    initText failed (%d)\n", r); return; }

    char got[256] = { 0 };
    if (!decode_qr(&qr, got, sizeof(got))) {
        printf("    [%s] could not decode (code size %u)\n", what, qr.size);
        check(!expect_match, what);
        return;
    }
    const bool same = strcmp(got, digits) == 0;
    printf("    [%s] v%u, decoded %zu digits, %s\n", what, version, strlen(got),
           same ? "SAME" : "DIFFERENT");
    if (!same) { printf("      expected: %s\n      decoded : %s\n", digits, got); }
    check(same == expect_match, what);
}

int main(void)
{
    /* Digit strings derived from public BIP39 test vectors */
    const char* d12 = "000000000000000000000000000000000000000000000003";
    const char* d24 = "0000000000000000000000000000000000000000000000000"
                      "00000000000000000000000000000000000000000000102";
    const char* d12b = "101920151790203919831533203119191019201517902040";

    roundtrip(2, d12, true, "12-word digit string encodes to v2 and reads back");
    roundtrip(3, d24, true, "24-word digit string encodes to v3 and reads back");
    roundtrip(2, d12b, true, "third vector (0x7f x16) reads back in v2");
    roundtrip(2, d24, false, "96 digits forced into v2 read back DIFFERENT from the string");

    printf("\n%u failure(s)\n", failures);
    return failures != 0;
}
