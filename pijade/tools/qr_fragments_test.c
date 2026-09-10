// BBB-AIRGAP: dumps the QR icons bcur_create_qr_icons() actually builds, one PBM per icon, so an
// external decoder can read exactly what the panel paints. Written during T3.8's second round to
// test whether the fragment set was incomplete - it was not, every pure fragment was present and
// decodable, which is what moved the search onto module size and found the root cause. Kept as the
// way to check what a density setting really produces: icon dimensions, fragment count, and
// whether an outside decoder can read each frame.
// Link against a libjade build, same LIBS line as qr_scale_test (see pijade/UPSTREAM.md).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint16_t width, height;
    uint32_t* data;
} Icon;

extern void bcur_create_qr_icons(
    const uint8_t* payload, size_t len, const char* bcur_type, uint8_t qr_version, Icon** icons, size_t* num_icons);

// Matches QR_VER_XPUB_LOW in main/qrmode.c - the version an untouched device now exports at.
// Pass a third argument to dump another version (4 = Medium, 6 = High on the xpub ladder).
#define QR_VERSION_DEFAULT 3
#define BCUR_TYPE "crypto-account"

static void write_pbm(const Icon* icon, const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P1\n%u %u\n", icon->width, icon->height);
    for (uint32_t y = 0; y < icon->height; ++y) {
        for (uint32_t x = 0; x < icon->width; ++x) {
            const uint32_t val = (uint32_t)y * icon->width + x;
            const uint32_t bit = (icon->data[val / 32] >> (val % 32)) & 1u;
            fputc(bit ? '1' : '0', f);
            fputc(x + 1 == icon->width ? '\n' : ' ', f);
        }
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    const size_t payload_len = argc > 1 ? (size_t)atoi(argv[1]) : 100;
    const char* const outdir = argc > 2 ? argv[2] : ".";
    const uint8_t qr_version = argc > 3 ? (uint8_t)atoi(argv[3]) : QR_VERSION_DEFAULT;

    // A fourth argument names a file to read the payload from. Filler bytes are fine for measuring
    // icon size and count, but a real cbor payload is what a decoder has to reconstruct, so the
    // regression check for the locale fix feeds real crypto-account payloads through here.
    const char* const payload_file = argc > 4 ? argv[4] : NULL;

    uint8_t payload[512];
    if (payload_len > sizeof(payload)) { fprintf(stderr, "payload too big\n"); return 1; }
    if (payload_file) {
        FILE* pf = fopen(payload_file, "rb");
        if (!pf) { perror(payload_file); return 1; }
        const size_t got = fread(payload, 1, payload_len, pf);
        fclose(pf);
        if (got != payload_len) { fprintf(stderr, "short read: %zu of %zu\n", got, payload_len); return 1; }
    } else {
        for (size_t i = 0; i < payload_len; ++i) {
            payload[i] = (uint8_t)(i * 7 + 13); // deterministic filler; bcur does not inspect content
        }
    }

    Icon* icons = NULL;
    size_t num_icons = 0;
    bcur_create_qr_icons(payload, payload_len, BCUR_TYPE, qr_version, &icons, &num_icons);

    // The icon count is decided at generation time (bcur_create_qr_icons keeps going until its own
    // decoder confirms the set is sufficient), so it cannot be derived from a multiplier here.
    printf("payload %zu bytes -> %zu icons\n", payload_len, num_icons);
    if (num_icons) {
        printf("icon size: %ux%u px\n", icons[0].width, icons[0].height);
    }

    for (size_t i = 0; i < num_icons; ++i) {
        char path[512];
        snprintf(path, sizeof(path), "%s/frag_%02zu.pbm", outdir, i);
        write_pbm(icons + i, path);
        printf("  written: %s (%ux%u)\n", path, icons[i].width, icons[i].height);
    }
    return 0;
}
