#include "panel_st7789.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>

// Waveshare 1.3 inch LCD HAT, BCM numbering. Verified against the same board's wiring as used by
// SeedSigner (its buttons.py/ST7789.py use BOARD numbering: DC 22, RST 13, BL 18).
#define PIN_DC 25
#define PIN_RST 27
#define PIN_BACKLIGHT 24

#define PANEL_WIDTH 240
#define PANEL_HEIGHT 240

// ST7789 commands used here; see the datasheet for the full set.
#define CMD_SWRESET 0x01
#define CMD_SLPOUT 0x11
#define CMD_NORON 0x13
#define CMD_INVON 0x21
#define CMD_DISPON 0x29
#define CMD_CASET 0x2A
#define CMD_RASET 0x2B
#define CMD_RAMWR 0x2C
#define CMD_MADCTL 0x36
#define CMD_COLMOD 0x3A

// MADCTL scan direction. 0x70 is MX | MV | ML: the value this board's driver uses, and the one the
// init sequence starts from. Turning the display over is the same scan with both address orders
// reversed, i.e. MY and MX toggled.
#define MADCTL_BASE 0x70
#define MADCTL_MIRROR_BOTH 0xC0

// The controller addresses 240x320 of memory while this panel shows 240x240 of it. The window sits
// at whichever end of the longer axis the scan starts from, so reversing the scan moves it by the
// difference; without following it, a mirrored picture comes out shifted with a band of stale
// memory along one edge.
#define ST7789_RAM_LONG_AXIS 320

// Software PWM for the backlight. The Waveshare HAT wires the panel's backlight to BCM 24, and the
// BCM2835 offers hardware PWM only on gpios 12/18/40/52 (PWM0) and 13/19/41/45/53 (PWM1), so the
// line is toggled from a thread instead. 200 Hz is above the flicker most people can see while
// keeping the toggle rate - and the ioctl per edge - low enough not to matter on a Pi Zero.
#define BACKLIGHT_PWM_HZ 200
#define BACKLIGHT_PERIOD_US (1000000 / BACKLIGHT_PWM_HZ)

// The kernel's spidev caps a single transfer; larger frames are sent in chunks.
#define SPI_CHUNK_BYTES 4096

struct panel {
    int spi_fd;
    int gpio_fd; // GPIO_V2 line request covering DC, RST and backlight
    unsigned int width;
    unsigned int height;
    bool flipped; // display turned over, i.e. both address orders reversed
    // Backlight dimming, see panel_set_backlight(). The thread only exists while a level between
    // off and full is set; full and off are driven straight onto the line and cost nothing.
    pthread_t bl_thread;
    bool bl_thread_running;
    pthread_mutex_t bl_mutex;
    unsigned int bl_level; // 0 = off, PANEL_BACKLIGHT_MAX = full
    bool bl_stop;
};

enum { LINE_DC = 0, LINE_RST = 1, LINE_BACKLIGHT = 2, LINE_COUNT = 3 };

static void sleep_ms(const unsigned int ms)
{
    const struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static bool gpio_set(const panel_t* panel, const unsigned int line, const bool high)
{
    struct gpio_v2_line_values values = { .bits = high ? (1ULL << line) : 0, .mask = 1ULL << line };
    if (ioctl(panel->gpio_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0) {
        fprintf(stderr, "pijade: cannot drive gpio line %u: %s\n", line, strerror(errno));
        return false;
    }
    return true;
}

static bool spi_write(const panel_t* panel, const uint8_t* data, const size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        const size_t chunk = len - sent > SPI_CHUNK_BYTES ? SPI_CHUNK_BYTES : len - sent;
        const ssize_t written = write(panel->spi_fd, data + sent, chunk);
        if (written <= 0) {
            fprintf(stderr, "pijade: spi write failed: %s\n", strerror(errno));
            return false;
        }
        sent += (size_t)written;
    }
    return true;
}

static bool send_command(const panel_t* panel, const uint8_t cmd, const uint8_t* args, const size_t nargs)
{
    if (!gpio_set(panel, LINE_DC, false) || !spi_write(panel, &cmd, 1)) {
        return false;
    }
    if (!nargs) {
        return true;
    }
    return gpio_set(panel, LINE_DC, true) && spi_write(panel, args, nargs);
}

// Standard ST7789 bring-up. The porch, gate and gamma values are the panel vendor's defaults for
// this module; they are not tuned here.
static bool run_init_sequence(const panel_t* panel)
{
    static const struct {
        uint8_t cmd;
        uint8_t nargs;
        uint8_t args[14];
    } sequence[] = {
        // The driver for this board uses 0x70; 0x00 is the alternative seen on other revisions.
        // If the assembled device shows the image rotated or mirrored, this is the byte to change.
        { CMD_MADCTL, 1, { MADCTL_BASE } },
        { CMD_COLMOD, 1, { 0x05 } }, // 16 bit per pixel, RGB565
        { 0xB2, 5, { 0x0C, 0x0C, 0x00, 0x33, 0x33 } }, // porch control
        { 0xB7, 1, { 0x35 } }, // gate control
        { 0xBB, 1, { 0x19 } }, // vcom
        { 0xC0, 1, { 0x2C } }, // lcm control
        { 0xC2, 1, { 0x01 } }, // vdv/vrh enable
        { 0xC3, 1, { 0x12 } }, // vrh
        { 0xC4, 1, { 0x20 } }, // vdv
        { 0xC6, 1, { 0x0F } }, // frame rate, 60 Hz
        { 0xD0, 2, { 0xA4, 0xA1 } }, // power control
        { 0xE0, 14, { 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F,
                        0x23 } }, // positive gamma
        { 0xE1, 14, { 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20,
                        0x23 } }, // negative gamma
        { CMD_INVON, 0, { 0 } }, // this module is an inverted panel
        { CMD_NORON, 0, { 0 } },
        { CMD_DISPON, 0, { 0 } },
    };

    if (!send_command(panel, CMD_SWRESET, NULL, 0)) {
        return false;
    }
    sleep_ms(150);
    if (!send_command(panel, CMD_SLPOUT, NULL, 0)) {
        return false;
    }
    sleep_ms(120);

    for (size_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i) {
        if (!send_command(panel, sequence[i].cmd, sequence[i].args, sequence[i].nargs)) {
            return false;
        }
    }
    return true;
}

static bool set_window(const panel_t* panel)
{
    // MV is set, so the column range addresses the controller's long axis; that is the one whose
    // window has to move when the scan is reversed. See ST7789_RAM_LONG_AXIS above.
    const uint16_t xs = panel->flipped ? (uint16_t)(ST7789_RAM_LONG_AXIS - panel->width) : 0;
    const uint16_t xe = (uint16_t)(xs + panel->width - 1);
    const uint16_t ye = (uint16_t)(panel->height - 1);
    const uint8_t caset[] = { (uint8_t)(xs >> 8), (uint8_t)(xs & 0xFF), (uint8_t)(xe >> 8), (uint8_t)(xe & 0xFF) };
    const uint8_t raset[] = { 0x00, 0x00, (uint8_t)(ye >> 8), (uint8_t)(ye & 0xFF) };
    return send_command(panel, CMD_CASET, caset, sizeof(caset))
        && send_command(panel, CMD_RASET, raset, sizeof(raset));
}

bool panel_set_flipped(panel_t* const panel, const bool flipped)
{
    if (!panel) {
        return false;
    }
    if (panel->flipped == flipped) {
        return true;
    }
    panel->flipped = flipped;

    const uint8_t madctl = flipped ? (uint8_t)(MADCTL_BASE ^ MADCTL_MIRROR_BOTH) : (uint8_t)MADCTL_BASE;
    if (!send_command(panel, CMD_MADCTL, &madctl, 1) || !set_window(panel)) {
        fprintf(stderr, "pijade: cannot set panel orientation\n");
        // The panel is now in an unknown state; say so rather than let the caller assume it took.
        return false;
    }
    return true;
}

static int open_gpio_lines(const char* gpiochip_path)
{
    const int chip_fd = open(gpiochip_path, O_RDWR | O_CLOEXEC);
    if (chip_fd < 0) {
        fprintf(stderr, "pijade: cannot open %s: %s\n", gpiochip_path, strerror(errno));
        return -1;
    }

    struct gpio_v2_line_request request = { 0 };
    request.offsets[LINE_DC] = PIN_DC;
    request.offsets[LINE_RST] = PIN_RST;
    request.offsets[LINE_BACKLIGHT] = PIN_BACKLIGHT;
    request.num_lines = LINE_COUNT;
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    snprintf(request.consumer, sizeof(request.consumer), "pijade-panel");

    const int rc = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request);
    close(chip_fd);
    if (rc < 0 || request.fd < 0) {
        fprintf(stderr, "pijade: cannot claim panel gpio lines: %s\n", strerror(errno));
        return -1;
    }
    return request.fd;
}

panel_t* panel_open(const char* spidev_path, const char* gpiochip_path, const unsigned int width,
    const unsigned int height, const uint32_t speed_hz)
{
    if (width != PANEL_WIDTH || height != PANEL_HEIGHT) {
        fprintf(stderr,
            "pijade: panel requires a 240x240 libjade build; rebuild with: "
            "./libjade/make_libjade.sh Release --camera --no-ci --no-debug --display=240x240\n");
        return NULL;
    }
    if (!spidev_path || !gpiochip_path) {
        fprintf(stderr, "pijade: invalid panel parameters\n");
        return NULL;
    }

    panel_t* const panel = calloc(1, sizeof(*panel));
    if (!panel) {
        return NULL;
    }
    panel->width = width;
    panel->height = height;
    panel->spi_fd = -1;
    panel->gpio_fd = -1;
    // The init sequence below turns the backlight fully on, so the level starts there; a stored
    // setting arrives later, once libjade has read it.
    panel->bl_level = PANEL_BACKLIGHT_MAX;
    // pthread_* return the error number; they do not set errno, so strerror(errno) would print
    // an unrelated error and send a reader of pijade.log after the wrong fault.
    const int mutex_rc = pthread_mutex_init(&panel->bl_mutex, NULL);
    if (mutex_rc != 0) {
        fprintf(stderr, "pijade: cannot init backlight mutex: %s\n", strerror(mutex_rc));
        free(panel);
        return NULL;
    }

    panel->spi_fd = open(spidev_path, O_RDWR | O_CLOEXEC);
    if (panel->spi_fd < 0) {
        fprintf(stderr, "pijade: cannot open %s: %s\n", spidev_path, strerror(errno));
        panel_close(panel);
        return NULL;
    }

    const uint8_t mode = SPI_MODE_0;
    const uint8_t bits = 8;
    if (ioctl(panel->spi_fd, SPI_IOC_WR_MODE, &mode) < 0
        || ioctl(panel->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0
        || ioctl(panel->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0) {
        fprintf(stderr, "pijade: cannot configure spi: %s\n", strerror(errno));
        panel_close(panel);
        return NULL;
    }

    panel->gpio_fd = open_gpio_lines(gpiochip_path);
    if (panel->gpio_fd < 0) {
        panel_close(panel);
        return NULL;
    }

    // Hardware reset, then bring the panel up and turn the backlight on.
    if (!gpio_set(panel, LINE_RST, true)) {
        panel_close(panel);
        return NULL;
    }
    sleep_ms(10);
    if (!gpio_set(panel, LINE_RST, false)) {
        panel_close(panel);
        return NULL;
    }
    sleep_ms(10);
    if (!gpio_set(panel, LINE_RST, true)) {
        panel_close(panel);
        return NULL;
    }
    sleep_ms(120);

    if (!run_init_sequence(panel) || !set_window(panel) || !gpio_set(panel, LINE_BACKLIGHT, true)) {
        panel_close(panel);
        return NULL;
    }
    return panel;
}

bool panel_write_frame(panel_t* const panel, const uint16_t* const pixels, const size_t len)
{
    if (!panel || !pixels || len != (size_t)panel->width * panel->height * sizeof(uint16_t)) {
        return false;
    }

    // The window never changes, so only RAMWR is reissued per frame.
    if (!send_command(panel, CMD_RAMWR, NULL, 0) || !gpio_set(panel, LINE_DC, true)) {
        return false;
    }

    // No byte swap here: Jade already stores colours in the panel's wire order, because on real
    // hardware the ESP LCD driver ships the buffer as is. main/display.c defines TFT_RED as 0x00F8,
    // which is 0xF800 - red - once the bytes reach the panel high byte first.
    return spi_write(panel, (const uint8_t*)pixels, len);
}

static void sleep_us(const unsigned int us)
{
    const struct timespec ts = { .tv_sec = us / 1000000u, .tv_nsec = (long)(us % 1000000u) * 1000L };
    nanosleep(&ts, NULL);
}

// One duty cycle per pass. Reads the level under the lock each time round, so a level change takes
// effect within a single period rather than needing the thread to be torn down and restarted.
static void* backlight_thread(void* arg)
{
    panel_t* const panel = arg;
    unsigned int level = PANEL_BACKLIGHT_MAX;
    for (;;) {
        pthread_mutex_lock(&panel->bl_mutex);
        const bool stop = panel->bl_stop;
        level = panel->bl_level;
        pthread_mutex_unlock(&panel->bl_mutex);
        if (stop) {
            break;
        }

        const unsigned int on_us = BACKLIGHT_PERIOD_US * level / PANEL_BACKLIGHT_MAX;
        bool driven = true;
        if (on_us) {
            driven = gpio_set(panel, LINE_BACKLIGHT, true);
            sleep_us(on_us);
        }
        if (driven && on_us < BACKLIGHT_PERIOD_US) {
            driven = gpio_set(panel, LINE_BACKLIGHT, false);
            sleep_us(BACKLIGHT_PERIOD_US - on_us);
        }
        // A gpiochip that has stopped accepting writes will not start again on its own, and this
        // loop runs at 200 Hz with gpio_set() printing on every failure - carrying on would put
        // hundreds of identical lines per second on stderr while the backlight sat at whichever
        // level the last successful write left. Give up dimming instead: the line is driven full
        // on below, which is the state a user can still read the screen in.
        if (!driven) {
            fprintf(stderr, "pijade: backlight pwm stopped after a gpio failure, going full on\n");
            level = PANEL_BACKLIGHT_MAX;
            break;
        }
    }
    // Leave the line in a defined state rather than wherever the last duty cycle left it. 'level'
    // is the value read under the lock above, so this does not touch panel state unlocked.
    gpio_set(panel, LINE_BACKLIGHT, level >= PANEL_BACKLIGHT_MAX);
    return NULL;
}

static void backlight_thread_stop(panel_t* const panel)
{
    if (!panel->bl_thread_running) {
        return;
    }
    pthread_mutex_lock(&panel->bl_mutex);
    panel->bl_stop = true;
    pthread_mutex_unlock(&panel->bl_mutex);
    pthread_join(panel->bl_thread, NULL);
    panel->bl_thread_running = false;
    panel->bl_stop = false;
}

bool panel_set_backlight(panel_t* const panel, const unsigned int level)
{
    if (!panel) {
        return false;
    }
    const unsigned int clamped = level > PANEL_BACKLIGHT_MAX ? PANEL_BACKLIGHT_MAX : level;

    pthread_mutex_lock(&panel->bl_mutex);
    panel->bl_level = clamped;
    pthread_mutex_unlock(&panel->bl_mutex);

    // Full and off are steady states: no toggling, so no thread and no flicker to get wrong.
    if (clamped == 0 || clamped >= PANEL_BACKLIGHT_MAX) {
        backlight_thread_stop(panel);
        return gpio_set(panel, LINE_BACKLIGHT, clamped >= PANEL_BACKLIGHT_MAX);
    }

    if (panel->bl_thread_running) {
        return true; // running thread picks the new level up on its next pass
    }
    const int create_rc = pthread_create(&panel->bl_thread, NULL, backlight_thread, panel);
    if (create_rc != 0) {
        fprintf(stderr, "pijade: cannot start backlight thread: %s\n", strerror(create_rc));
        // Better a lit screen at full brightness than a dark one the user cannot read.
        return gpio_set(panel, LINE_BACKLIGHT, true);
    }
    panel->bl_thread_running = true;
    return true;
}

void panel_close(panel_t* const panel)
{
    if (!panel) {
        return;
    }
    backlight_thread_stop(panel);
    if (panel->gpio_fd >= 0) {
        gpio_set(panel, LINE_BACKLIGHT, false);
        close(panel->gpio_fd);
    }
    if (panel->spi_fd >= 0) {
        close(panel->spi_fd);
    }
    pthread_mutex_destroy(&panel->bl_mutex);
    free(panel);
}
