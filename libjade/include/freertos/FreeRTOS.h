#ifndef _LIBJADE_FREERTOS_FREERTOS_H
#define _LIBJADE_FREERTOS_FREERTOS_H 1

// BBB-AIRGAP: esp-idf's FreeRTOS.h pulls in esp_attr.h, and components/miner/miner.c relies on
// that to get IRAM_ATTR without naming the header. Mirrored here so the upstream file compiles
// untouched.
#include <esp_attr.h>

#define CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS 3

#endif // _LIBJADE_FREERTOS_FREERTOS_H
