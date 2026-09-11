#ifndef QR_DOWNSCALE_H_
#define QR_DOWNSCALE_H_

#include <stddef.h>
#include <stdint.h>

// BBB-AIRGAP: halve a grayscale image for the second QR scanning pass (main/qrscan.c).
//
// Why this exists at all: with the code filling the frame, a module (the smallest square of a QR
// code) grows past about 7 pixels and quirc finds the finder patterns but cannot build the grid.
// Measured on 19 device frames held from the round 9 camera dump: at the device's own 460x460
// window 0/19 decoded; halved, 11/19 did. Smoothing is not what rescues them - the same frames
// were run through four Gaussian sigmas at full scale and all four stayed at 0/19. Scale is.
//
// Why a tent kernel rather than a 2x2 box: both were measured on those 19 frames with the exact
// integer arithmetic below, on raw grayscale, which is what the camera hands over. The box
// average decoded 10/19, the tent 11/19. The tent does not contain the box - it wins frames 06,
// 08 and 16 and loses 17 and 19 - so this is a measured choice between two whole sets, not a
// strict improvement. Their union is 13/19, which would need a third pass; one kernel was kept
// instead. The extra cost is four more additions per output pixel, against the 165 ms quirc
// itself spends on a pass (pijade/tools/t44_bench.c on the device), so it does not show up.
//
// An earlier version of this measurement went through an RGB565 round trip, because the two
// offline tools that existed both read display dumps. It read 13/19, not 11: quantising to five
// bits is itself a contrast operation and rescued two frames the camera's own bytes do not.
// The numbers above are the raw-grayscale ones. This is why qr_stage_probe.c grew a --gray mode.
//
// Why a header rather than a function in qrscan.c: pijade/tools/qr_stage_probe.c measures this
// offline and must run the arithmetic the device runs, not a second copy of it that can drift.
// A copied constant that drifts is exactly how an offline measurement starts certifying
// something the device never does.
//
// The kernel is [1,3,3,1]/8 on each axis, applied as one separable 2D pass so no intermediate
// buffer is needed. Output pixel j reads input 2j-1 .. 2j+2, clamped at the edges.
//
// src_stride lets the source be a window inside a larger frame: qrscan.c reads the central crop
// straight out of the camera frame, so the second pass costs nothing on frames the first one
// already decoded, and needs no copy of its own.

static inline size_t qr_downscale_clamp(const size_t value, const size_t limit)
{
    return value < limit ? value : limit - 1;
}

// Halve the src_w x src_h window into dst. dst must hold (src_w / 2) * (src_h / 2) bytes, both
// source dimensions must be even, and src_stride is the pixel pitch of the frame src points into.
static inline void qr_downscale_half(
    const uint8_t* src, const size_t src_w, const size_t src_h, const size_t src_stride, uint8_t* dst)
{
    const size_t dst_w = src_w / 2;
    const size_t dst_h = src_h / 2;

    for (size_t i = 0; i < dst_h; ++i) {
        // 2i - 1 would wrap on the unsigned first row, so the first tap is clamped by construction
        const size_t y0 = i ? (2 * i) - 1 : 0;
        const size_t y1 = 2 * i;
        const size_t y2 = qr_downscale_clamp((2 * i) + 1, src_h);
        const size_t y3 = qr_downscale_clamp((2 * i) + 2, src_h);

        const uint8_t* const row0 = src + (y0 * src_stride);
        const uint8_t* const row1 = src + (y1 * src_stride);
        const uint8_t* const row2 = src + (y2 * src_stride);
        const uint8_t* const row3 = src + (y3 * src_stride);

        for (size_t j = 0; j < dst_w; ++j) {
            const size_t x0 = j ? (2 * j) - 1 : 0;
            const size_t x1 = 2 * j;
            const size_t x2 = qr_downscale_clamp((2 * j) + 1, src_w);
            const size_t x3 = qr_downscale_clamp((2 * j) + 2, src_w);

            const uint32_t h0 = row0[x0] + (3 * row0[x1]) + (3 * row0[x2]) + row0[x3];
            const uint32_t h1 = row1[x0] + (3 * row1[x1]) + (3 * row1[x2]) + row1[x3];
            const uint32_t h2 = row2[x0] + (3 * row2[x1]) + (3 * row2[x2]) + row2[x3];
            const uint32_t h3 = row3[x0] + (3 * row3[x1]) + (3 * row3[x2]) + row3[x3];

            // Weights sum to 64; round to nearest rather than truncating
            dst[(i * dst_w) + j] = (uint8_t)((h0 + (3 * h1) + (3 * h2) + h3 + 32) >> 6);
        }
    }
}

#endif /* QR_DOWNSCALE_H_ */
