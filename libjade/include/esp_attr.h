#ifndef _LIBJADE_ESP_ATTR_H_
#define _LIBJADE_ESP_ATTR_H_ 1

// BBB-AIRGAP: needed by components/miner/miner.c, which uses DRAM_ATTR without including this
// header - on esp-idf the definition arrives transitively through freertos/FreeRTOS.h, so
// libjade's FreeRTOS.h shim includes this one for the same reason. DRAM_ATTR asks the linker to
// keep the sha256 round constant table (:47 K) in internal memory rather than memory-mapped
// flash; a hosted build has no flash to avoid, so it expands to nothing. IRAM_ATTR is shimmed
// beside it because the miner carried it on its two hot functions upstream; this fork took those
// two markers out instead (components/miner/miner.c:229-234). Only these two attributes are
// shimmed; esp_attr.h proper carries a few dozen more.
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif

#endif // _LIBJADE_ESP_ATTR_H_
