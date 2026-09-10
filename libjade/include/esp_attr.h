#ifndef _LIBJADE_ESP_ATTR_H_
#define _LIBJADE_ESP_ATTR_H_ 1

// BBB-AIRGAP: needed by components/miner/miner.c, which uses these placement attributes without
// including this header - on esp-idf the definitions arrive transitively through
// freertos/FreeRTOS.h, so libjade's FreeRTOS.h shim includes this one for the same reason. Both
// ask the linker to keep something in internal memory rather than memory-mapped flash: IRAM_ATTR
// for the two hot functions (:189 calc_midstate, :290 verify_nonce), DRAM_ATTR for the sha256
// round constant table (:39 K). Neither has meaning in a hosted build, where there is no flash to
// avoid, so both expand to nothing. Defined empty rather than edited out of miner.c so that file
// stays as upstream wrote it. Only the two attributes the miner uses are shimmed; esp_attr.h
// proper carries a few dozen more.
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif

#endif // _LIBJADE_ESP_ATTR_H_
