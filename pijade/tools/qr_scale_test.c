// BBB-AIRGAP: checks the claims behind the full-screen QR layout - that a wallet code fits the
// panel, never scans worse than it did in the split layout, keeps a quiet zone where the panel has
// room for one, and is the largest scale those constraints allow. The scale claims are checked for
// every version the ui can ask for, starting at v2 (bytes_to_qr_icon in main/qrmode.c picks v2 for
// a short payload); the icons themselves are only built from v4 up, as bcur_create_qr_icons asserts
// that lower bound.
// Link against a libjade build and tell the test which panel that build used:
//   -DPANEL_W=240 -DPANEL_H=240        (defaults, the piJade target)
// see pijade/UPSTREAM.md for the full command.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// main/display.h:29-32 - declared here rather than included, as display.h pulls in the esp headers
typedef struct {
    uint16_t width, height;
    uint32_t* data;
} Icon;

extern void bcur_create_qr_icons(
    const uint8_t* payload, size_t len, const char* bcur_type, uint8_t qr_version, Icon** icons, size_t* num_icons);
extern uint32_t qr_fullscreen_scale_factor(uint8_t qr_version);

// The panel the libjade build under test was configured for
#ifndef PANEL_W
#define PANEL_W 240
#endif
#ifndef PANEL_H
#define PANEL_H 240
#endif
#define PANEL_SHORT (PANEL_W < PANEL_H ? PANEL_W : PANEL_H)

// What the same code measures in the 44/56 split layout - the QR_SCALE_FACTOR table in main/bcur.c,
// which the full-screen scale must never scan worse than. Index is the qr version.
#if PANEL_W >= 480 && PANEL_H >= 220
static const uint32_t SPLIT_SCALE_FACTOR[] = { 0, 10, 8, 7, 6, 5, 5, 4, 4, 4, 3, 3, 3 };
#elif PANEL_W >= 320 && PANEL_H >= 170
static const uint32_t SPLIT_SCALE_FACTOR[] = { 0, 8, 6, 5, 5, 4, 4, 3, 3, 3, 2, 2, 2 };
#else
static const uint32_t SPLIT_SCALE_FACTOR[] = { 0, 6, 5, 4, 4, 3, 3, 2, 2, 2, 2, 2, 2 };
#endif

static int failures = 0;

static void check(const int passed, const char* what)
{
    printf("%-66s %s\n", what, passed ? "ok" : "FAILED");
    if (!passed) {
        ++failures;
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0); // so output survives an assert inside the library
    printf("panel %ux%u\n\n", (unsigned)PANEL_W, (unsigned)PANEL_H);

    uint8_t payload[64];
    memset(payload, 0x42, sizeof(payload));

    unsigned larger_than_split = 0;

    for (uint8_t version = 2; version <= 12; ++version) {
        const uint32_t scale = qr_fullscreen_scale_factor(version);
        const uint32_t modules = 17 + (4 * version); // a version-N code is 17+4N modules square
        const uint32_t px = modules * scale;
        // The largest scale that still leaves two modules of quiet zone per side
        const uint32_t quiet_scale = PANEL_SHORT / (modules + 4);
        char what[160];

        snprintf(what, sizeof(what), "v%u is %ux%u px at scale %u (split layout: %u px)", version, (unsigned)px,
            (unsigned)px, scale, (unsigned)(modules * SPLIT_SCALE_FACTOR[version]));
        check(scale > 0, what);

        // display_icon() asserts an icon is no larger than the screen (main/display.c), so this one
        // is not cosmetic: an oversized code aborts the device rather than drawing badly.
        snprintf(what, sizeof(what), "v%u fits the %u px panel", version, (unsigned)PANEL_SHORT);
        check(px <= PANEL_SHORT, what);

        // No regression against the split layout, except where that layout itself overflows
        const uint32_t no_worse_than = SPLIT_SCALE_FACTOR[version] < PANEL_SHORT / modules
            ? SPLIT_SCALE_FACTOR[version]
            : PANEL_SHORT / modules;
        snprintf(what, sizeof(what), "v%u scans no worse than the split layout (scale >= %u)", version,
            (unsigned)no_worse_than);
        check(scale >= no_worse_than, what);

        // A quiet zone is only owed where one fits: on a panel too short for the floor scale, the
        // floor wins, because a code that shrinks below the split layout stops scanning outright.
        if (quiet_scale >= no_worse_than) {
            snprintf(what, sizeof(what), "v%u keeps two modules of quiet zone per side", version);
            check(px + (4 * scale) <= PANEL_SHORT, what);
        } else {
            snprintf(what, sizeof(what), "v%u panel too short for a quiet zone, floor wins", version);
            check(scale == no_worse_than, what);
        }

        // The other checks admit smaller values too. Pin the result to the largest quiet-zone
        // scale when one preserves the floor, and otherwise to the largest floor that fits.
        const uint32_t expected_scale = quiet_scale > no_worse_than ? quiet_scale : no_worse_than;
        snprintf(what, sizeof(what), "v%u uses the largest allowed scale (%u)", version, (unsigned)expected_scale);
        check(scale == expected_scale, what);

        // From v4 up the scale is not just arithmetic: check the icons bcur actually builds
        if (version >= 4) {
            Icon* icons = NULL;
            size_t num_icons = 0;
            bcur_create_qr_icons(payload, sizeof(payload), "test-type", version, &icons, &num_icons);
            snprintf(what, sizeof(what), "v%u icons are drawn at that scale", version);
            check(icons && num_icons && icons[0].width == px && icons[0].height == px, what);
            for (size_t i = 0; i < num_icons; ++i) {
                free(icons[i].data);
            }
            free(icons);
        }

        if (scale > SPLIT_SCALE_FACTOR[version]) {
            ++larger_than_split;
        }
    }

    // The point of the change on the piJade panel: codes there must actually get bigger
#if PANEL_W == 240 && PANEL_H == 240
    char what[160];
    snprintf(what, sizeof(what), "every version is larger than the split layout (%u/11)", larger_than_split);
    check(larger_than_split == 11, what);
#else
    printf("\n%u/11 versions larger than the split layout\n", larger_than_split);
#endif

    printf("\n%s\n", failures ? "FAILURES" : "all passed");
    return failures ? 1 : 0;
}
