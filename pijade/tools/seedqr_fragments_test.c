#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wally_core.h>
#include <wally_crypto.h>

#include "../../main/qrcode.h"
#include "../../main/seedqr.h"
#include "../../components/esp32-quirc/lib/quirc.h"
#include "../../components/esp32-quirc/lib/quirc_internal.h"

#define PANEL_WIDTH 240
#define PANEL_HEIGHT 240
#define CONTEXT_MODULES 2

extern uint32_t qr_fullscreen_scale_factor(uint8_t qr_version);

static unsigned failures;

static void check(const bool passed, const char* const what)
{
    printf("%-72s %s\n", what, passed ? "ok" : "FAILED");
    if (!passed) {
        ++failures;
    }
}

static bool make_fragments(QRCode* const qrcode, const uint16_t target_size, const bool show_grid,
    const uint8_t context_modules, Icon** const icons_out, size_t* const num_icons_out)
{
#ifdef SEEDQR_LEGACY_API
    if (context_modules) {
        return false;
    }
    return qrcode_toFragmentsIcons(qrcode, target_size, show_grid, icons_out, num_icons_out);
#else
    return qrcode_toFragmentsIcons(qrcode, target_size, show_grid, context_modules, icons_out, num_icons_out);
#endif
}

static void free_icons(Icon* const icons, const size_t num_icons)
{
    for (size_t i = 0; i < num_icons; ++i) {
        qrcode_freeIconData(&icons[i]);
    }
    free(icons);
}

static bool icon_pixel(const Icon* const icon, const uint16_t x, const uint16_t y)
{
    const uint32_t pixel = (uint32_t)y * icon->width + x;
    return ((icon->data[pixel / 32] >> (pixel % 32)) & 1u) != 0;
}

static void icon_sha256(const Icon* const icon, char hex[65])
{
    uint8_t digest[SHA256_LEN];
    const size_t data_size = qrcode_get_icon_data_size(icon->width, icon->height);
    if (wally_sha256((const uint8_t*)icon->data, data_size, digest, sizeof(digest)) != WALLY_OK) {
        fprintf(stderr, "wally_sha256 failed\n");
        exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(hex + (2 * i), 3, "%02x", digest[i]);
    }
    hex[64] = '\0';
}

static void init_qrcode(const uint8_t version, QRCode* const qrcode, uint8_t buffer[112])
{
    uint8_t entropy[32];
    const size_t entropy_len = version == 1 ? 16 : 32;
    for (size_t i = 0; i < entropy_len; ++i) {
        entropy[i] = (uint8_t)(0x31u + (i * 17u) + version);
    }
    if (qrcode_initBytes(qrcode, buffer, version, ECC_LOW, entropy, entropy_len) != 0) {
        fprintf(stderr, "qrcode_initBytes failed for version %u\n", version);
        exit(EXIT_FAILURE);
    }
}

static void golden_test(const char* const path, const bool write_golden)
{
    FILE* const fixture = fopen(path, write_golden ? "w" : "r");
    if (!fixture) {
        perror(path);
        exit(EXIT_FAILURE);
    }

    for (uint8_t version = 1; version <= 2; ++version) {
        uint8_t buffer[112];
        QRCode qrcode;
        init_qrcode(version, &qrcode, buffer);

        Icon* icons = NULL;
        size_t num_icons = 0;
        if (!make_fragments(&qrcode, PANEL_HEIGHT * QRCODE_FRAGMENT_ROW_PERCENT / 100, true, 0, &icons, &num_icons)) {
            fprintf(stderr, "plain fragment generation failed for version %u\n", version);
            exit(EXIT_FAILURE);
        }

        for (size_t i = 0; i < num_icons; ++i) {
            char actual_hash[65];
            icon_sha256(&icons[i], actual_hash);
            if (write_golden) {
                fprintf(fixture, "v%u icon%02zu %ux%u %s\n", version, i, icons[i].width, icons[i].height, actual_hash);
            } else {
                unsigned expected_version;
                size_t expected_index;
                unsigned expected_width;
                unsigned expected_height;
                char expected_hash[65];
                const int fields = fscanf(fixture, "v%u icon%zu %ux%u %64s\n", &expected_version, &expected_index,
                    &expected_width, &expected_height, expected_hash);
                char what[128];
                snprintf(what, sizeof(what), "v%u plain icon %zu matches the pre-change SHA256 fixture", version, i);
                check(fields == 5 && expected_version == version && expected_index == i
                        && expected_width == icons[i].width && expected_height == icons[i].height
                        && strcmp(expected_hash, actual_hash) == 0,
                    what);
            }
        }
        free_icons(icons, num_icons);
    }

    if (!write_golden) {
        char trailing;
        check(fscanf(fixture, " %c", &trailing) == EOF, "golden fixture has no unexpected trailing records");
    }
    fclose(fixture);
}

#ifndef SEEDQR_LEGACY_API
static bool is_center_pixel(const uint16_t px, const uint16_t py, const uint16_t cx0, const uint16_t cy0,
    const uint16_t cx1, const uint16_t cy1)
{
    return px >= cx0 && px <= cx1 && py >= cy0 && py <= cy1;
}

static bool is_frame_pixel(const uint16_t px, const uint16_t py, const uint16_t cx0, const uint16_t cy0,
    const uint16_t cx1, const uint16_t cy1)
{
    return is_center_pixel(px, py, cx0, cy0, cx1, cy1)
        && (px == cx0 || px == cx1 || py == cy0 || py == cy1);
}

static void context_contract_test(const uint8_t version)
{
    const uint16_t target_size = PANEL_HEIGHT * QRCODE_FRAGMENT_ROW_PERCENT / 100;
    const uint8_t fragments_per_side = version == 1 ? 3 : (version == 2 ? 5 : 6);
    const uint8_t fragment_size = version == 1 ? 7 : 5;
    const uint8_t total_modules = fragment_size + (2 * CONTEXT_MODULES);
    const uint8_t scale = target_size / total_modules;
    const uint16_t expected_size = total_modules * scale;
    const uint16_t cx0 = CONTEXT_MODULES * scale;
    const uint16_t cy0 = CONTEXT_MODULES * scale;
    const uint16_t cx1 = cx0 + (fragment_size * scale) - 1;
    const uint16_t cy1 = cy0 + (fragment_size * scale) - 1;

    uint8_t buffer[112];
    QRCode qrcode;
    init_qrcode(version, &qrcode, buffer);

    Icon* icons = NULL;
    size_t num_icons = 0;
    char label[128];
    snprintf(label, sizeof(label), "v%u context fit is known before UI creation", version);
    check(qrcode_fragmentsContextFits(version, target_size, CONTEXT_MODULES), label);
    if (!make_fragments(&qrcode, target_size, true, CONTEXT_MODULES, &icons, &num_icons)) {
        snprintf(label, sizeof(label), "v%u context fragments are generated", version);
        check(false, label);
        return;
    }

    bool outside_clear = true;
    bool context_thinned = true;
    bool center_grid = true;
    bool context_has_no_grid = true;
    bool frame_dark = true;
    bool inner_neighbor_not_frame = false;

    for (size_t i = 0; i < num_icons; ++i) {
        const Icon* const icon = &icons[i];
        if (icon->width != expected_size || icon->height != expected_size) {
            outside_clear = context_thinned = center_grid = context_has_no_grid = frame_dark = false;
            continue;
        }
        const int fragment_orig_x = (int)(i % fragments_per_side) * fragment_size;
        const int fragment_orig_y = (int)(i / fragments_per_side) * fragment_size;

        for (uint16_t py = 0; py < icon->height; ++py) {
            for (uint16_t px = 0; px < icon->width; ++px) {
                const int src_x = fragment_orig_x - CONTEXT_MODULES + (px / scale);
                const int src_y = fragment_orig_y - CONTEXT_MODULES + (py / scale);
                const bool in_bounds = src_x >= 0 && src_x < qrcode.size && src_y >= 0 && src_y < qrcode.size;
                const bool center = is_center_pixel(px, py, cx0, cy0, cx1, cy1);
                const bool frame = is_frame_pixel(px, py, cx0, cy0, cx1, cy1);
                const bool actual = icon_pixel(icon, px, py);

                // BBB-AIRGAP: for v3 the mask can extend one module past the code (the empty
                // strip); there the frame and grid still apply, so only pixels outside BOTH the
                // code and the mask are required to be clear.  For v1/v2 that set is identical
                // to "outside the code", since those tile evenly.
                if (!in_bounds && !center) {
                    outside_clear = outside_clear && !actual;
                    continue;
                }

                const bool base = in_bounds ? qrcode_getModule(&qrcode, (uint8_t)src_x, (uint8_t)src_y) : false;
                if (!center) {
                    const bool thinned = base || !(px % 2 == 0 && py % 2 == 0);
                    context_thinned = context_thinned && actual == thinned;
                    context_has_no_grid = context_has_no_grid && actual == thinned;
                } else if (frame) {
                    frame_dark = frame_dark && actual;
                } else {
                    const bool gridline = px % scale == 0 || py % scale == 0;
                    const bool gridded = base != gridline;
                    center_grid = center_grid && actual == gridded;
                    if ((px == cx0 + 1 || px == cx1 - 1 || py == cy0 + 1 || py == cy1 - 1) && !actual) {
                        inner_neighbor_not_frame = true;
                    }
                }
            }
        }
    }

    char what[128];
    snprintf(what, sizeof(what), "v%u context icon size is exactly %u px", version, expected_size);
    check(icons[0].width == expected_size && icons[0].height == expected_size, what);
    snprintf(what, sizeof(what), "v%u pixels outside the QR and outside the mask are all zero", version);
    check(outside_clear, what);
    snprintf(what, sizeof(what), "v%u light context pixels follow the even-even thinning rule", version);
    check(context_thinned, what);
    snprintf(what, sizeof(what), "v%u center block uses the grid rule", version);
    check(center_grid, what);
    snprintf(what, sizeof(what), "v%u context area has no grid overlay", version);
    check(context_has_no_grid, what);
    snprintf(what, sizeof(what), "v%u four frame edges are dark and exactly one pixel wide", version);
    check(frame_dark && inner_neighbor_not_frame, what);

    free_icons(icons, num_icons);
}

// BBB-AIRGAP: spec T3.19B section 6.2, checks 11a to 11d - the v3 empty module strip.
// Label semantics (main/process/mnemonic.c:202-203): the letter is the ROW and the number is
// the COLUMN, so with a 6x6 grid the last column is A6..F6 (index i % 6 == 5) and the last
// row is F1..F6 (index i / 6 == 5).  F6 (index 35) is short on both axes.
// BBB-AIRGAP: spec T3.19B section 6.2, checks 14 to 17 - the actual correctness proof.
// A library return code of 0 is not evidence that the payload fits (measured: see
// pijade/tools/v3_capacity_probe.c), so the generated code is read back with quirc and the
// decoded text is compared against the digit string it was built from.
#define RT_QUIET_ZONE 4
#define RT_PIXELS_PER_MODULE 6

typedef enum { RT_SAME, RT_DIFFERENT, RT_UNDECODABLE } rt_result_t;

// 'decoded_len' reports quirc's payload_len, which is what the device compares against
// digits_len (main/process/mnemonic.c:266-268 via main/qrscan.c:59).  strcmp() alone would
// NOT catch a length that included the terminator: the copy is nul terminated either way.
static rt_result_t roundtrip(const uint8_t version, const char* const digits, size_t* const decoded_len)
{
    if (decoded_len) {
        *decoded_len = 0;
    }
    uint8_t buffer[112];
    QRCode qrcode;
    if (qrcode_initText(&qrcode, buffer, version, ECC_LOW, (char*)digits) != 0) {
        return RT_UNDECODABLE;
    }

    const int side = (qrcode.size + 2 * RT_QUIET_ZONE) * RT_PIXELS_PER_MODULE;
    struct quirc* const q = quirc_new();
    if (!q) {
        return RT_UNDECODABLE;
    }
    if (quirc_resize(q, side, side) < 0) {
        quirc_destroy(q);
        return RT_UNDECODABLE;
    }

    int w = 0;
    int h = 0;
    uint8_t* const img = quirc_begin(q, &w, &h);
    memset(img, 0xff, (size_t)w * h);
    for (uint8_t my = 0; my < qrcode.size; ++my) {
        for (uint8_t mx = 0; mx < qrcode.size; ++mx) {
            if (!qrcode_getModule(&qrcode, mx, my)) {
                continue;
            }
            for (int dy = 0; dy < RT_PIXELS_PER_MODULE; ++dy) {
                const int py = (my + RT_QUIET_ZONE) * RT_PIXELS_PER_MODULE + dy;
                memset(img + (size_t)py * w + (size_t)(mx + RT_QUIET_ZONE) * RT_PIXELS_PER_MODULE, 0x00,
                    RT_PIXELS_PER_MODULE);
            }
        }
    }
    quirc_end(q);

    rt_result_t result = RT_UNDECODABLE;
    if (quirc_count(q) == 1) {
        struct quirc_code code;
        struct quirc_data data;
        // NOTE: quirc.h:169 - the caller allocates this pointer (main/qrscan.c:148 does too);
        // leaving it NULL makes quirc_decode memset address zero and crash.
        struct datastream* const ds = calloc(1, sizeof(struct datastream));
        if (ds) {
            ds->data = calloc(QUIRC_MAX_PAYLOAD, sizeof(uint8_t));
        }
        quirc_extract(q, 0, &code);
        if (ds && ds->data && quirc_decode(&code, &data, ds) == QUIRC_SUCCESS) {
            char decoded[QUIRC_MAX_PAYLOAD + 1];
            if ((size_t)data.payload_len < sizeof(decoded)) {
                memcpy(decoded, data.payload, data.payload_len);
                decoded[data.payload_len] = '\0';
                if (decoded_len) {
                    *decoded_len = data.payload_len;
                }
                result = strcmp(decoded, digits) == 0 ? RT_SAME : RT_DIFFERENT;
            }
        }
        if (ds) {
            free(ds->data);
        }
        free(ds);
    }
    quirc_destroy(q);
    return result;
}

static void roundtrip_test(void)
{
    char digits[97];
    size_t written = 0;

    const uint8_t zero16[16] = { 0 };
    size_t decoded_len = 0;
    check(seedqr_digits_from_entropy(zero16, sizeof(zero16), digits, sizeof(digits), &written)
            && roundtrip(2, digits, &decoded_len) == RT_SAME,
        "12 word digit string survives v2 encode and quirc decode unchanged");
    check(decoded_len == 48, "12 word code decodes to exactly 48 characters (the device compares lengths)");

    // The 96 digit payload does not fit a v2 code, but qrcode_initText() still returns 0.
    // Report the two failure shapes separately: today the result is undecodable, and a
    // truncated payload that happened to decode to something else would be just as wrong.
    char digits24[97];
    const uint8_t zero32[32] = { 0 };
    check(seedqr_digits_from_entropy(zero32, sizeof(zero32), digits24, sizeof(digits24), &written)
            && roundtrip(3, digits24, &decoded_len) == RT_SAME,
        "24 word digit string survives v3 encode and quirc decode unchanged");
    check(decoded_len == 96, "24 word code decodes to exactly 96 characters (the device compares lengths)");

    const rt_result_t forced = roundtrip(2, digits24, NULL);
    check(forced != RT_SAME, "96 digits forced into a v2 code do NOT read back as the digit string");
    check(forced == RT_UNDECODABLE,
        "96 digits forced into a v2 code are undecodable (documents today's library behaviour)");

    uint8_t vector3[16];
    memset(vector3, 0x7f, sizeof(vector3));
    check(seedqr_digits_from_entropy(vector3, sizeof(vector3), digits, sizeof(digits), &written)
            && roundtrip(2, digits, NULL) == RT_SAME,
        "a third vector (0x7f x16) survives the v2 round trip unchanged");
}

static void v3_strip_test(void)
{
    const uint16_t target_size = PANEL_HEIGHT * QRCODE_FRAGMENT_ROW_PERCENT / 100;
    const uint8_t fragments_per_side = 6;
    const uint8_t fragment_size = 5;

    uint8_t buffer[112];
    QRCode qrcode;
    init_qrcode(3, &qrcode, buffer);
    check(qrcode.size == 29, "v3 code is 29 modules across");

    Icon* icons = NULL;
    size_t num_icons = 0;
    if (!make_fragments(&qrcode, target_size, true, 0, &icons, &num_icons) || num_icons != 36) {
        check(false, "v3 plain fragments are generated for the strip test");
        return;
    }

    // 11a - every icon is the same size and square, despite the code not tiling evenly
    const uint8_t plain_scale = target_size / fragment_size;
    const uint16_t expected_plain = fragment_size * plain_scale;
    bool uniform = true;
    for (size_t i = 0; i < num_icons; ++i) {
        uniform = uniform && icons[i].width == expected_plain && icons[i].height == expected_plain;
    }
    char what[128];
    snprintf(what, sizeof(what), "v3 all 36 plain icons are exactly %ux%u px", expected_plain, expected_plain);
    check(uniform, what);

    // 11b and 11c - in the strip there is no data, only grid lines; and the line that closes
    // the last real cell is present.  Checked on the last column (A6, index 5), the last row
    // (F1, index 30) and the corner (F6, index 35).
    const size_t edge_icons[] = { 5, 30, 35 };
    bool strip_has_no_data = true;
    bool closing_line_x = true;
    bool closing_line_y = true;

    for (size_t e = 0; e < sizeof(edge_icons) / sizeof(edge_icons[0]); ++e) {
        const size_t i = edge_icons[e];
        const Icon* const icon = &icons[i];
        const int fragment_orig_x = (int)(i % fragments_per_side) * fragment_size;
        const int fragment_orig_y = (int)(i / fragments_per_side) * fragment_size;
        const bool short_x = fragment_orig_x + fragment_size > qrcode.size;
        const bool short_y = fragment_orig_y + fragment_size > qrcode.size;

        for (uint16_t py = 0; py < icon->height; ++py) {
            for (uint16_t px = 0; px < icon->width; ++px) {
                const int src_x = fragment_orig_x + (px / plain_scale);
                const int src_y = fragment_orig_y + (py / plain_scale);
                if (src_x < qrcode.size && src_y < qrcode.size) {
                    continue; // real module, covered by the golden fixture rules
                }
                // Strip pixel: base data must be light, so only the grid line shows.
                // The grid rule also draws the block's far edge (main/qrcode.c: dest_x == cx1
                // and dest_y == cy1); in plain mode there is no frame branch to take over, so
                // those two lines are part of the expected pattern.
                const uint16_t cx1 = (uint16_t)(fragment_size * plain_scale) - 1;
                const uint16_t cy1 = cx1;
                const bool gridline
                    = px == cx1 || px % plain_scale == 0 || py == cy1 || py % plain_scale == 0;
                strip_has_no_data = strip_has_no_data && icon_pixel(icon, px, py) == gridline;
            }
        }

        if (short_x) {
            // The first pixel column of the strip closes the last real cell column
            const uint16_t px = (uint16_t)(qrcode.size - fragment_orig_x) * plain_scale;
            for (uint16_t py = 0; py < icon->height; ++py) {
                closing_line_x = closing_line_x && icon_pixel(icon, px, py);
            }
        }
        if (short_y) {
            const uint16_t py = (uint16_t)(qrcode.size - fragment_orig_y) * plain_scale;
            for (uint16_t px = 0; px < icon->width; ++px) {
                closing_line_y = closing_line_y && icon_pixel(icon, px, py);
            }
        }
    }

    check(strip_has_no_data, "v3 empty strip carries no data modules, only grid lines");
    check(closing_line_x, "v3 last real cell column is closed in the last column blocks (A6, F6)");
    check(closing_line_y, "v3 last real cell row is closed in the last row blocks (F1, F6)");

    free_icons(icons, num_icons);
}

static void scale_test(void)
{
    const uint16_t target_size = PANEL_HEIGHT * QRCODE_FRAGMENT_ROW_PERCENT / 100;
    check(target_size == 187, "240x240 fragment target size is exactly 187 px");
    check(qrcode_fragmentsContextFits(1, target_size, CONTEXT_MODULES), "v1 context scale is nonzero at 187 px");
    check(qrcode_fragmentsContextFits(2, target_size, CONTEXT_MODULES), "v2 context scale is nonzero at 187 px");
    check(qrcode_fragmentsContextFits(3, target_size, CONTEXT_MODULES), "v3 context scale is nonzero at 187 px");
    check(!qrcode_fragmentsContextFits(1, 10, CONTEXT_MODULES), "v1 context rejects a zero scale target");
    check(!qrcode_fragmentsContextFits(2, 8, CONTEXT_MODULES), "v2 context rejects a zero scale target");

    for (uint8_t version = 1; version <= 3; ++version) {
        uint8_t buffer[112];
        QRCode qrcode;
        init_qrcode(version, &qrcode, buffer);

        Icon* icons = NULL;
        size_t num_icons = 0;
        char generated[128];
        snprintf(generated, sizeof(generated), "v%u plain fragments are generated", version);
        check(make_fragments(&qrcode, target_size, true, 0, &icons, &num_icons), generated);
        const uint8_t expected_grid = version == 1 ? 3 : (version == 2 ? 5 : 6);
        snprintf(generated, sizeof(generated), "v%u produces %u fragments", version, expected_grid * expected_grid);
        check(num_icons == (size_t)(expected_grid * expected_grid), generated);
        const uint16_t expected_plain = version == 1 ? 182 : 185;
        char what[128];
        snprintf(what, sizeof(what), "v%u plain icon size is exactly %u px", version, expected_plain);
        check(icons[0].width == expected_plain && icons[0].height == expected_plain, what);
        free_icons(icons, num_icons);

        const uint8_t fullscreen_scale = qr_fullscreen_scale_factor(version);
        Icon fullscreen;
        qrcode_toIcon(&qrcode, &fullscreen, fullscreen_scale);
        const uint16_t expected_fullscreen = version == 1 ? 189 : (version == 2 ? 200 : 203);
        snprintf(what, sizeof(what), "v%u fullscreen icon size is exactly %u px", version, expected_fullscreen);
        check(fullscreen.width == expected_fullscreen && fullscreen.height == expected_fullscreen, what);
        snprintf(what, sizeof(what), "v%u fullscreen icon fits the panel", version);
        check(fullscreen.width <= PANEL_WIDTH && fullscreen.height <= PANEL_HEIGHT, what);
        qrcode_freeIconData(&fullscreen);
    }
}
#endif

// BBB-AIRGAP: spec T3.19B section 6.2, checks 1 to 5 - Standard SeedQR digit derivation.
// The two vectors are the public BIP39 all-zero test vectors, not seeds.
static void digits_test(void)
{
    char digits[97];
    size_t written = 0;

    const uint8_t zero16[16] = { 0 };
    memset(digits, 'x', sizeof(digits));
    check(seedqr_digits_from_entropy(zero16, sizeof(zero16), digits, sizeof(digits), &written),
        "12 word digit string is derived");
    check(written == 48, "12 word digit string is 48 digits");
    check(strcmp(digits, "000000000000000000000000000000000000000000000003") == 0,
        "12 word digit string matches the BIP39 zero vector (abandon x11 + about)");

    const uint8_t zero32[32] = { 0 };
    memset(digits, 'x', sizeof(digits));
    check(seedqr_digits_from_entropy(zero32, sizeof(zero32), digits, sizeof(digits), &written),
        "24 word digit string is derived");
    check(written == 96, "24 word digit string is 96 digits");
    check(strlen(digits) == 96 && strncmp(digits, "000000000000000000000000000000000000000000000000", 48) == 0
            && strncmp(digits + 48, "00000000000000000000000000000000000000000000", 44) == 0
            && strcmp(digits + 92, "0102") == 0,
        "24 word digit string matches the BIP39 zero vector (abandon x23 + art)");

    // Rejected inputs: the buffer must be left alone
    memset(digits, 'x', sizeof(digits));
    check(!seedqr_digits_from_entropy(zero16, 24, digits, sizeof(digits), &written),
        "entropy length other than 16 or 32 is rejected");
    check(!seedqr_digits_from_entropy(zero32, sizeof(zero32), digits, 96, &written),
        "digits buffer with no room for the NUL is rejected");
    check(digits[0] == 'x', "a rejected call does not write to the buffer");
}

int main(int argc, char** argv)
{
    const bool write_golden = argc == 3 && strcmp(argv[1], "--write-golden") == 0;
    const char* const fixture_path = write_golden ? argv[2]
                                                  : argc == 2 ? argv[1] : "pijade/tools/seedqr_fragments_golden.txt";
    golden_test(fixture_path, write_golden);

#ifndef SEEDQR_LEGACY_API
    if (!write_golden) {
        digits_test();
        scale_test();
        context_contract_test(1);
        context_contract_test(2);
        context_contract_test(3);
        v3_strip_test();
        roundtrip_test();
    }
#endif

    if (write_golden) {
        printf("wrote %s\n", fixture_path);
    } else {
        printf("\n%u failure(s)\n", failures);
    }
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
