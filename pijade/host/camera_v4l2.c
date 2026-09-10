#include "camera_v4l2.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>

// Four buffers is what the driver is happy with and leaves room for one in flight while the
// caller works on another. More would only add latency: a stale frame is worse than no frame
// when the user is aiming the camera at a QR code.
#define BUFFER_COUNT 4

struct mapped_buffer {
    void* start;
    size_t length;
};

struct camera {
    int fd;
    unsigned int width;
    unsigned int height;
    // The driver may pad each luma row; this is the real row stride, which can exceed width.
    unsigned int bytesperline;
    struct mapped_buffer buffers[BUFFER_COUNT];
    unsigned int buffer_count;
    bool streaming;
};

/* ioctl restarted on EINTR; a signal arriving mid-call is not an error. */
static int xioctl(const int fd, const unsigned long request, void* const arg)
{
    int rc;
    do {
        rc = ioctl(fd, request, arg);
    } while (rc == -1 && errno == EINTR);
    return rc;
}

static void unmap_buffers(camera_t* const camera)
{
    for (unsigned int i = 0; i < camera->buffer_count; ++i) {
        if (camera->buffers[i].start && camera->buffers[i].start != MAP_FAILED) {
            munmap(camera->buffers[i].start, camera->buffers[i].length);
        }
        camera->buffers[i].start = NULL;
        camera->buffers[i].length = 0;
    }
    camera->buffer_count = 0;
}

camera_t* camera_open(const char* const path, const unsigned int width, const unsigned int height)
{
    if (!path || !width || !height) {
        fprintf(stderr, "pijade: camera_open called with invalid arguments\n");
        return NULL;
    }

    camera_t* const camera = calloc(1, sizeof(*camera));
    if (!camera) {
        fprintf(stderr, "pijade: cannot allocate camera state\n");
        return NULL;
    }
    camera->fd = -1;

    camera->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (camera->fd == -1) {
        fprintf(stderr, "pijade: cannot open %s: %s\n", path, strerror(errno));
        goto fail;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(camera->fd, VIDIOC_QUERYCAP, &cap) == -1) {
        fprintf(stderr, "pijade: %s is not a V4L2 device: %s\n", path, strerror(errno));
        goto fail;
    }
    /* capabilities describes the whole physical device; device_caps describes this node. A
     * multifunction device can advertise capture on another node and still fail here, so the
     * per-node field is the one to test when the driver provides it. */
    const uint32_t caps
        = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "pijade: %s cannot capture video with streaming I/O\n", path);
        goto fail;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420; // YU12
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(camera->fd, VIDIOC_S_FMT, &fmt) == -1) {
        fprintf(stderr, "pijade: cannot set %ux%u YU12: %s\n", width, height, strerror(errno));
        goto fail;
    }

    /* S_FMT negotiates rather than commands: the driver may quietly return something else and
     * still report success. Jade's buffer is a fixed size, so anything but an exact match is a
     * failure we must catch here rather than discover as a garbled frame. */
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420 || fmt.fmt.pix.width != width
        || fmt.fmt.pix.height != height) {
        fprintf(stderr, "pijade: driver returned %ux%u fourcc %.4s instead of %ux%u YU12\n",
            fmt.fmt.pix.width, fmt.fmt.pix.height, (const char*)&fmt.fmt.pix.pixelformat, width,
            height);
        goto fail;
    }

    camera->width = width;
    camera->height = height;
    camera->bytesperline = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : width;
    if (camera->bytesperline < width) {
        fprintf(stderr, "pijade: driver reports row stride %u below width %u\n",
            camera->bytesperline, width);
        goto fail;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(camera->fd, VIDIOC_REQBUFS, &req) == -1) {
        fprintf(stderr, "pijade: cannot request buffers: %s\n", strerror(errno));
        goto fail;
    }
    if (req.count < 2) {
        fprintf(stderr, "pijade: driver granted only %u buffers\n", req.count);
        goto fail;
    }
    camera->buffer_count = req.count < BUFFER_COUNT ? req.count : BUFFER_COUNT;

    for (unsigned int i = 0; i < camera->buffer_count; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(camera->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            fprintf(stderr, "pijade: cannot query buffer %u: %s\n", i, strerror(errno));
            goto fail;
        }
        // The luma plane is read straight out of this mapping, so a short buffer would mean
        // reading past its end.
        if (buf.length < (size_t)camera->bytesperline * height) {
            fprintf(stderr, "pijade: buffer %u is %u bytes, too small for the luma plane\n", i,
                buf.length);
            goto fail;
        }
        camera->buffers[i].length = buf.length;
        camera->buffers[i].start
            = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, camera->fd, buf.m.offset);
        if (camera->buffers[i].start == MAP_FAILED) {
            fprintf(stderr, "pijade: cannot map buffer %u: %s\n", i, strerror(errno));
            camera->buffers[i].start = NULL;
            goto fail;
        }
    }

    for (unsigned int i = 0; i < camera->buffer_count; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(camera->fd, VIDIOC_QBUF, &buf) == -1) {
            fprintf(stderr, "pijade: cannot queue buffer %u: %s\n", i, strerror(errno));
            goto fail;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(camera->fd, VIDIOC_STREAMON, &type) == -1) {
        fprintf(stderr, "pijade: cannot start streaming: %s\n", strerror(errno));
        goto fail;
    }
    camera->streaming = true;

    return camera;

fail:
    camera_close(camera);
    return NULL;
}

void camera_close(camera_t* const camera)
{
    if (!camera) {
        return;
    }
    if (camera->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(camera->fd, VIDIOC_STREAMOFF, &type);
        camera->streaming = false;
    }
    unmap_buffers(camera);
    if (camera->fd != -1) {
        close(camera->fd);
        camera->fd = -1;
    }
    free(camera);
}

camera_result_t camera_read_gray(
    camera_t* const camera, uint8_t* const out, const size_t out_len, const unsigned int timeout_ms)
{
    if (!camera || !out || out_len != (size_t)camera->width * camera->height) {
        return CAMERA_FRAME_ERROR;
    }

    struct pollfd pfd = { .fd = camera->fd, .events = POLLIN, .revents = 0 };
    int rc;
    do {
        rc = poll(&pfd, 1, (int)timeout_ms);
    } while (rc == -1 && errno == EINTR);
    if (rc == -1) {
        fprintf(stderr, "pijade: camera poll failed: %s\n", strerror(errno));
        return CAMERA_FRAME_ERROR;
    }
    if (rc == 0) {
        return CAMERA_FRAME_NONE; // sensor has not produced a frame yet
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(camera->fd, VIDIOC_DQBUF, &buf) == -1) {
        // POLLIN can be reported without a buffer actually being ready; that is a non-event.
        if (errno == EAGAIN) {
            return CAMERA_FRAME_NONE;
        }
        fprintf(stderr, "pijade: cannot dequeue frame: %s\n", strerror(errno));
        return CAMERA_FRAME_ERROR;
    }
    if (buf.index >= camera->buffer_count || !camera->buffers[buf.index].start) {
        fprintf(stderr, "pijade: driver returned buffer index %u out of range\n", buf.index);
        return CAMERA_FRAME_ERROR;
    }

    /* The driver reports how much of the buffer it actually filled and whether the capture went
     * wrong. Ignoring either would let stale or corrupted luma reach QR decoding and, worse, the
     * seed entropy pool - the mapping is large enough to read from regardless. */
    const size_t needed = (size_t)camera->bytesperline * camera->height;
    const bool usable = !(buf.flags & V4L2_BUF_FLAG_ERROR) && buf.bytesused >= needed;
    if (usable) {
        /* The luma plane sits at the start of a YU12 buffer, one row every bytesperline bytes.
         * With no padding that is a single contiguous copy, which is the common case here. */
        const uint8_t* const src = camera->buffers[buf.index].start;
        if (camera->bytesperline == camera->width) {
            memcpy(out, src, out_len);
        } else {
            for (unsigned int row = 0; row < camera->height; ++row) {
                memcpy(out + (size_t)row * camera->width, src + (size_t)row * camera->bytesperline,
                    camera->width);
            }
        }
    }

    if (xioctl(camera->fd, VIDIOC_QBUF, &buf) == -1) {
        /* The buffer is now lost to the driver's queue. Four of them means four such failures
         * starve the stream, after which polling would block forever with nothing to dequeue, so
         * this is reported as fatal even though this particular frame may have been copied. */
        fprintf(stderr, "pijade: cannot requeue buffer %u: %s\n", buf.index, strerror(errno));
        return CAMERA_FRAME_ERROR;
    }
    return usable ? CAMERA_FRAME_OK : CAMERA_FRAME_NONE;
}
