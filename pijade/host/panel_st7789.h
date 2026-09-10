/*
 * ST7789 panel over spidev, for the Waveshare 1.3 inch LCD HAT (240x240).
 *
 * The panel is driven from user space rather than through a kernel framebuffer overlay: fewer
 * moving parts in the image, and the initialisation sequence stays visible in this repository
 * where it can be read and changed. Jade's own ESP32 build leaves this to the IDF panel driver
 * (esp_lcd_new_panel_st7789), which has no Linux counterpart.
 */
#ifndef PIJADE_PANEL_ST7789_H
#define PIJADE_PANEL_ST7789_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct panel panel_t;

/*
 * Opens the SPI device and the control lines, resets the panel and runs the init sequence.
 * `spidev_path` is typically /dev/spidev0.0, `gpiochip_path` /dev/gpiochip0.
 * Returns NULL on failure, after printing the reason.
 */
panel_t* panel_open(const char* spidev_path, const char* gpiochip_path, unsigned int width,
    unsigned int height, uint32_t speed_hz);

/*
 * Writes one full frame. `len` must be width * height * sizeof(uint16_t).
 * The buffer goes out unchanged: Jade already stores colours in the panel's wire order (see
 * main/display.c, where TFT_RED is 0x00F8).
 */
bool panel_write_frame(panel_t* panel, const uint16_t* pixels, size_t len);

/*
 * Turns the display over, matching Jade's Options > Display > Flip Orientation. Rewrites the scan
 * direction and moves the memory window that follows from it.
 * Returns false, after printing the reason, if the panel could not be told - in which case what is
 * on screen no longer matches what Jade thinks is on screen.
 */
bool panel_set_flipped(panel_t* panel, bool flipped);

/*
 * Backlight level, 0 (off) to PANEL_BACKLIGHT_MAX (full), matching Jade's BACKLIGHT_MIN..MAX scale.
 *
 * The HAT wires the backlight to a plain gpio, and that gpio has no hardware PWM behind it on this
 * chip, so intermediate levels are produced by toggling the line from a thread. Off and full are
 * driven directly and run no thread.
 * Returns false, after printing the reason, if the line could not be driven.
 */
#define PANEL_BACKLIGHT_MAX 5
bool panel_set_backlight(panel_t* panel, unsigned int level);

void panel_close(panel_t* panel);

#endif /* PIJADE_PANEL_ST7789_H */
