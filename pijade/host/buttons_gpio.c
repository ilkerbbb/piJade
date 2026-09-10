#include "buttons_gpio.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/gpio.h>
#include <sys/ioctl.h>

// Waveshare 1.3 inch LCD HAT buttons, BCM numbering. SeedSigner drives the same board and lists
// these in BOARD numbering (buttons.py: up 31, down 35, left 29, right 37, press 33,
// KEY1 40, KEY2 38, KEY3 36).
#define PIN_UP 6
#define PIN_DOWN 19
#define PIN_LEFT 5
#define PIN_RIGHT 26
#define PIN_PRESS 13
#define PIN_KEY1 21
#define PIN_KEY2 20
#define PIN_KEY3 16

// Mechanical contacts bounce for a few milliseconds; the kernel filters this for us.
#define DEBOUNCE_US 15000

// Initial values approximate Jade Plus long-press repeat and will be reviewed on the device.
#define BUTTONS_REPEAT_DELAY_MS 500
#define BUTTONS_REPEAT_INTERVAL_MS 150

struct buttons {
    int fd;
    unsigned int held_pin;
    button_event_t held_event;
    uint64_t next_repeat_ns;
};

static const struct {
    unsigned int pin;
    button_event_t event;
} LINES[] = {
    { PIN_LEFT, BUTTON_PREV },
    { PIN_RIGHT, BUTTON_NEXT },
    { PIN_UP, BUTTON_UP },
    { PIN_DOWN, BUTTON_DOWN },
    { PIN_PRESS, BUTTON_SELECT },
    { PIN_KEY1, BUTTON_KEY1 },
    { PIN_KEY2, BUTTON_KEY2 },
    { PIN_KEY3, BUTTON_KEY3 },
};

#define LINE_COUNT (sizeof(LINES) / sizeof(LINES[0]))
_Static_assert(LINE_COUNT <= GPIO_V2_LINES_MAX, "HAT button count exceeds one GPIO line request");

static bool is_direction(const button_event_t event)
{
    return event == BUTTON_PREV || event == BUTTON_NEXT || event == BUTTON_UP || event == BUTTON_DOWN;
}

static bool monotonic_now_ns(uint64_t* const now_ns)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        fprintf(stderr, "pijade: cannot read monotonic clock: %s\n", strerror(errno));
        return false;
    }
    *now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    return true;
}

buttons_t* buttons_open(const char* gpiochip_path)
{
    if (!gpiochip_path) {
        return NULL;
    }

    const int chip_fd = open(gpiochip_path, O_RDWR | O_CLOEXEC);
    if (chip_fd < 0) {
        fprintf(stderr, "pijade: cannot open %s: %s\n", gpiochip_path, strerror(errno));
        return NULL;
    }

    struct gpio_v2_line_request request = { 0 };
    for (size_t i = 0; i < LINE_COUNT; ++i) {
        request.offsets[i] = LINES[i].pin;
    }
    request.num_lines = LINE_COUNT;
    // The buttons pull the line to ground, so falling means press and rising means release.
    // Keep the default CLOCK_MONOTONIC event clock; do not add EVENT_CLOCK_REALTIME or EVENT_CLOCK_HTE.
    request.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP
        | GPIO_V2_LINE_FLAG_EDGE_FALLING | GPIO_V2_LINE_FLAG_EDGE_RISING;
    request.config.num_attrs = 1;
    request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
    request.config.attrs[0].attr.debounce_period_us = DEBOUNCE_US;
    request.config.attrs[0].mask = (1ULL << LINE_COUNT) - 1;
    snprintf(request.consumer, sizeof(request.consumer), "pijade-buttons");

    const int rc = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request);
    close(chip_fd);
    if (rc < 0 || request.fd < 0) {
        fprintf(stderr, "pijade: cannot claim joystick gpio lines: %s\n", strerror(errno));
        return NULL;
    }

    buttons_t* const buttons = calloc(1, sizeof(*buttons));
    if (!buttons) {
        close(request.fd);
        return NULL;
    }
    buttons->fd = request.fd;
    return buttons;
}

button_event_t buttons_wait(
    buttons_t* const buttons, const int timeout_ms, uint64_t* const timestamp_ns_out)
{
    *timestamp_ns_out = 0;
    if (!buttons) {
        return BUTTON_NONE;
    }

    int poll_timeout_ms = timeout_ms;
    if (buttons->held_event != BUTTON_NONE) {
        uint64_t now_ns;
        if (!monotonic_now_ns(&now_ns)) {
            return BUTTON_ERROR;
        }
        uint64_t remaining_ms = 0;
        if (buttons->next_repeat_ns > now_ns) {
            remaining_ms = (buttons->next_repeat_ns - now_ns + 999999ULL) / 1000000ULL;
        }
        const int repeat_timeout_ms = remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
        if (poll_timeout_ms < 0 || repeat_timeout_ms < poll_timeout_ms) {
            poll_timeout_ms = repeat_timeout_ms;
        }
    }

    struct pollfd pfd = { .fd = buttons->fd, .events = POLLIN };
    const int ready = poll(&pfd, 1, poll_timeout_ms);
    if (ready < 0) {
        if (errno == EINTR) {
            return BUTTON_NONE;
        }
        fprintf(stderr, "pijade: joystick poll failed: %s\n", strerror(errno));
        return BUTTON_ERROR;
    }
    if (!ready) {
        if (buttons->held_event != BUTTON_NONE) {
            uint64_t now_ns;
            if (!monotonic_now_ns(&now_ns)) {
                return BUTTON_ERROR;
            }
            if (now_ns >= buttons->next_repeat_ns) {
                *timestamp_ns_out = now_ns;
                buttons->next_repeat_ns
                    = now_ns + (uint64_t)BUTTONS_REPEAT_INTERVAL_MS * 1000000ULL;
                return buttons->held_event;
            }
        }
        return BUTTON_NONE;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        fprintf(stderr, "pijade: joystick poll reported device failure (revents 0x%x)\n",
            (unsigned int)pfd.revents);
        return BUTTON_ERROR;
    }
    if (!(pfd.revents & POLLIN)) {
        return BUTTON_NONE;
    }

    struct gpio_v2_line_event event;
    const ssize_t got = read(buttons->fd, &event, sizeof(event));
    if (got < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return BUTTON_NONE;
        }
        fprintf(stderr, "pijade: joystick read failed: %s\n", strerror(errno));
        return BUTTON_ERROR;
    }
    if (got != (ssize_t)sizeof(event)) {
        fprintf(stderr, "pijade: short read from joystick: got %zd of %zu bytes\n", got,
            sizeof(event));
        return BUTTON_ERROR;
    }

    // offset is the line's offset on the chip - the BCM pin number - not its index in the request
    // (linux/gpio.h: "the offset of the line that triggered the event").
    for (size_t i = 0; i < LINE_COUNT; ++i) {
        if (LINES[i].pin == event.offset) {
            if (event.id == GPIO_V2_LINE_EVENT_RISING_EDGE) {
                if (buttons->held_event != BUTTON_NONE && buttons->held_pin == event.offset) {
                    buttons->held_pin = 0;
                    buttons->held_event = BUTTON_NONE;
                    buttons->next_repeat_ns = 0;
                }
                return BUTTON_NONE;
            }
            if (event.id != GPIO_V2_LINE_EVENT_FALLING_EDGE) {
                return BUTTON_NONE;
            }

            if (is_direction(LINES[i].event)) {
                buttons->held_pin = event.offset;
                buttons->held_event = LINES[i].event;
                buttons->next_repeat_ns
                    = event.timestamp_ns + (uint64_t)BUTTONS_REPEAT_DELAY_MS * 1000000ULL;
            } else if (LINES[i].event == BUTTON_KEY1 || LINES[i].event == BUTTON_KEY2
                || LINES[i].event == BUTTON_KEY3) {
                buttons->held_pin = 0;
                buttons->held_event = BUTTON_NONE;
                buttons->next_repeat_ns = 0;
            }
            *timestamp_ns_out = event.timestamp_ns;
            return LINES[i].event;
        }
    }
    return BUTTON_NONE;
}

void buttons_close(buttons_t* const buttons)
{
    if (!buttons) {
        return;
    }
    close(buttons->fd);
    free(buttons);
}
