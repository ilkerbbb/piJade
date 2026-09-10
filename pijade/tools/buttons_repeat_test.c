/*
 * BBB-AIRGAP acceptance test: auto-repeat ownership on a diagonal push.
 *
 * A diagonal push drops a second direction pin while the first is still under the thumb.  The
 * repeat used to follow the last pin to fall, so brushing UP while pushing RIGHT repeated the
 * opposite way and the held key stopped repeating.  This drives buttons_wait() from a pipe
 * standing in for the GPIO event fd, so the state machine is exercised without the HAT.
 *
 * Build and run:  gcc -Wall -Wextra -Werror -o /tmp/bt pijade/tools/buttons_repeat_test.c \
 *                     -Ipijade/host && /tmp/bt
 * Takes a couple of seconds: three real repeat delays are waited out.
 */
#include "buttons_gpio.c"

#include <stdio.h>

static int write_fd = -1;
static int failures = 0;

static void emit(const unsigned int pin, const unsigned int id, const uint64_t timestamp_ns)
{
    struct gpio_v2_line_event event = { 0 };
    event.timestamp_ns = timestamp_ns;
    event.id = id;
    event.offset = pin;
    if (write(write_fd, &event, sizeof(event)) != (ssize_t)sizeof(event)) {
        fprintf(stderr, "test: cannot queue event for pin %u\n", pin);
        exit(2);
    }
}

static void expect(const char* const what, const button_event_t got, const button_event_t want)
{
    const bool ok = got == want;
    printf("%-46s got %d want %d  %s\n", what, (int)got, (int)want, ok ? "OK" : "FAIL");
    if (!ok) {
        ++failures;
    }
}

int main(void)
{
    int fds[2];
    if (pipe(fds) != 0) {
        perror("pipe");
        return 2;
    }
    write_fd = fds[1];

    buttons_t buttons = { 0 };
    buttons.fd = fds[0];

    uint64_t now_ns;
    if (!monotonic_now_ns(&now_ns)) {
        return 2;
    }

    uint64_t timestamp_ns = 0;

    // RIGHT goes down and takes the repeat.
    emit(PIN_RIGHT, GPIO_V2_LINE_EVENT_FALLING_EDGE, now_ns);
    expect("right press", buttons_wait(&buttons, 100, &timestamp_ns), BUTTON_NEXT);

    // UP is brushed while RIGHT is still held: delivered once, but the repeat stays with RIGHT.
    emit(PIN_UP, GPIO_V2_LINE_EVENT_FALLING_EDGE, now_ns);
    expect("up brushed while right held", buttons_wait(&buttons, 100, &timestamp_ns), BUTTON_UP);
    printf("%-46s pin %u %s\n", "repeat owner after the brush", buttons.held_pin,
        buttons.held_pin == PIN_RIGHT ? "OK" : "FAIL");
    if (buttons.held_pin != PIN_RIGHT) {
        ++failures;
    }

    // The repeat that follows must still be RIGHT, not the pin that fell last.
    expect("first repeat", buttons_wait(&buttons, 2000, &timestamp_ns), BUTTON_NEXT);

    // Releasing the brushed pin must not stop the held key repeating.
    emit(PIN_UP, GPIO_V2_LINE_EVENT_RISING_EDGE, now_ns);
    expect("up released", buttons_wait(&buttons, 100, &timestamp_ns), BUTTON_NONE);
    expect("repeat continues after the brush ends", buttons_wait(&buttons, 2000, &timestamp_ns),
        BUTTON_NEXT);

    // Leaving the diagonal the other way round: the owner goes up while UP is still held, so the
    // repeat has to move to UP.  Keeping the repeat pinned to the first direction would leave the
    // held key silent, which is the same complaint from the other side.
    emit(PIN_UP, GPIO_V2_LINE_EVENT_FALLING_EDGE, now_ns);
    expect("up pressed again while right held", buttons_wait(&buttons, 100, &timestamp_ns),
        BUTTON_UP);
    emit(PIN_RIGHT, GPIO_V2_LINE_EVENT_RISING_EDGE, now_ns);
    expect("right released out of the diagonal", buttons_wait(&buttons, 100, &timestamp_ns),
        BUTTON_NONE);
    printf("%-46s pin %u %s\n", "repeat owner after handover", buttons.held_pin,
        buttons.held_pin == PIN_UP ? "OK" : "FAIL");
    if (buttons.held_pin != PIN_UP) {
        ++failures;
    }
    expect("up repeats once it owns the repeat", buttons_wait(&buttons, 2000, &timestamp_ns),
        BUTTON_UP);

    // Releasing the held key stops the repeat.
    emit(PIN_UP, GPIO_V2_LINE_EVENT_RISING_EDGE, now_ns);
    expect("up released", buttons_wait(&buttons, 100, &timestamp_ns), BUTTON_NONE);
    expect("no repeat after release", buttons_wait(&buttons, 800, &timestamp_ns), BUTTON_NONE);

    close(fds[0]);
    close(fds[1]);
    printf("\nFAILURES: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
