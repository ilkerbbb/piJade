#ifndef _LIBJADE_ESP_SYSTEM_H_
#define _LIBJADE_ESP_SYSTEM_H_ 1

// BBB-AIRGAP: needed by main/idletimer.c, which upstream keeps out of libjade builds and this fork
// builds in. Member names and order copied from esp-idf 5.5
// (components/esp_system/include/esp_system.h) so the upstream file compiles untouched.
typedef enum {
    ESP_RST_UNKNOWN,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
    ESP_RST_SDIO,
    ESP_RST_USB,
    ESP_RST_JTAG,
    ESP_RST_EFUSE,
    ESP_RST_PWR_GLITCH,
    ESP_RST_CPU_LOCKUP,
} esp_reset_reason_t;

esp_reset_reason_t esp_reset_reason(void);

void esp_restart();

#endif // _LIBJADE_ESP_SYSTEM_H_
