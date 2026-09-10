// No-op/minimal implementation for devices with no power-management chip
//
#include <esp_sleep.h>

esp_err_t power_init(void) { return ESP_OK; }

esp_err_t power_shutdown(void)
{
    // If we don't have a PMU, use esp_deep_sleep()
    esp_deep_sleep_start();
    return ESP_OK;
}

esp_err_t power_screen_on(void) { return ESP_OK; }

#ifdef CONFIG_LIBJADE
// BBB-AIRGAP: defined in libjade/libjade.c; hands the backlight level to the embedding host, which
// drives the panel's backlight line. Same shape as the display and power handlers in libjade.h.
// Without this the brightness menu ran, stored the level and changed nothing on screen.
void libjade_backlight_request(uint8_t level);

esp_err_t power_backlight_on(const uint8_t brightness)
{
    libjade_backlight_request(brightness);
    return ESP_OK;
}
esp_err_t power_backlight_off(void)
{
    libjade_backlight_request(0);
    return ESP_OK;
}
#else
esp_err_t power_backlight_on(const uint8_t brightness) { return ESP_OK; }
esp_err_t power_backlight_off(void) { return ESP_OK; }
#endif

esp_err_t power_camera_on(void) { return ESP_OK; }
esp_err_t power_camera_off(void) { return ESP_OK; }

uint16_t power_get_vbat(void) { return 0; }
uint8_t power_get_battery_status(void) { return 0; }
bool power_get_battery_charging(void) { return false; }
uint16_t power_get_ibat_charge(void) { return 0; }
uint16_t power_get_ibat_discharge(void) { return 0; }
uint16_t power_get_vusb(void) { return 0; }
uint16_t power_get_iusb(void) { return 0; }
uint16_t power_get_temp(void) { return 0; }

void disable_usb_host(void) {}
void enable_usb_host(void) {}

bool usb_is_powered(void) { return true; }
