/*
 * The HAT's eight buttons, read through the GPIO character device.
 *
 * Joystick left/right map to previous/next, up/down keep their own axis, and press selects. KEY1
 * selects the first item without clicking, KEY2 selects, and KEY3 invokes the screen-specific
 * secondary action.
 */
#ifndef PIJADE_BUTTONS_GPIO_H
#define PIJADE_BUTTONS_GPIO_H

#include <stdbool.h>
#include <stdint.h>

typedef struct buttons buttons_t;

typedef enum {
    BUTTON_NONE = 0,
    BUTTON_PREV, // joystick left
    BUTTON_NEXT, // joystick right
    BUTTON_SELECT, // joystick press
    BUTTON_UP, // joystick up
    BUTTON_DOWN, // joystick down
    BUTTON_KEY1, // first selectable item
    BUTTON_KEY2, // select
    BUTTON_KEY3, // screen-specific secondary action
    BUTTON_ERROR, // GPIO input failed; the caller must stop accepting input
} button_event_t;

/*
 * Claims the joystick lines with pull-ups and kernel-side debounce.
 * Returns NULL on failure, after printing the reason.
 */
buttons_t* buttons_open(const char* gpiochip_path);

/*
 * Waits up to `timeout_ms` for an event. A held direction repeats after BUTTONS_REPEAT_DELAY_MS and
 * every BUTTONS_REPEAT_INTERVAL_MS thereafter. BUTTON_KEY1/2/3 and BUTTON_SELECT never repeat. A
 * press or repeat stores its CLOCK_MONOTONIC timestamp in `timestamp_ns_out`; other returns store
 * zero. Returns BUTTON_NONE on timeout, release or interruption, and BUTTON_ERROR after reporting
 * an input failure on stderr.
 */
button_event_t buttons_wait(buttons_t* buttons, int timeout_ms, uint64_t* timestamp_ns_out);

void buttons_close(buttons_t* buttons);

#endif /* PIJADE_BUTTONS_GPIO_H */
