#ifndef PIJADE_CAMERA_V4L2_H
#define PIJADE_CAMERA_V4L2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Grayscale camera frames from a V4L2 device.
 *
 * The Pi's bcm2835_v4l2 driver does not offer V4L2_PIX_FMT_GREY (measured on the device:
 * fourteen formats, none of them GREY). It does offer YU12 - planar YUV 4:2:0 - whose first
 * plane is the luma plane, which is exactly an 8-bit grayscale image. So we ask for YU12 and
 * hand Jade the luma plane; no conversion, no scaling, no extra library.
 */

typedef struct camera camera_t;

typedef enum {
    CAMERA_FRAME_OK, // out holds a fresh frame
    // No frame this time round: timeout, or the driver handed back a buffer it had marked bad.
    // The device is still healthy; ask again.
    CAMERA_FRAME_NONE,
    // The device can no longer be trusted to deliver frames - a buffer was lost to the driver's
    // queue, or an ioctl failed for a reason retrying cannot fix. Close and reopen.
    CAMERA_FRAME_ERROR,
} camera_result_t;

/* Opens the device and starts streaming at width x height. Returns NULL on any failure,
 * having reported the reason on stderr and released whatever it had already acquired. */
camera_t* camera_open(const char* path, unsigned int width, unsigned int height);

void camera_close(camera_t* camera);

/*
 * Waits for the next frame and writes its luma plane to out.
 *
 * out_len must be exactly width * height. Blocks for at most timeout_ms. The three outcomes are
 * kept apart because they call for different responses: a timeout is normal while the sensor
 * settles, whereas a lost buffer silently shrinks the queue until the stream starves.
 */
camera_result_t camera_read_gray(
    camera_t* camera, uint8_t* out, size_t out_len, unsigned int timeout_ms);

#endif // PIJADE_CAMERA_V4L2_H
