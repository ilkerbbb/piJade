#ifndef _LIBJADE_SDKCONFIG_H_
#define _LIBJADE_SDKCONFIG_H_ 1

// Config defines for a software Jade device

// Export debug mode functions for testing
// BBB-AIRGAP: was hardwired, which left the debug handlers and the libjade RPC surface
// enabled in every build, including Release. Built with -DDEBUG_MODE=0 the flag is absent
// and main/wire.c stops routing libjade_request. See libjade/CMakeLists.txt.
#ifndef CONFIG_LIBJADE_NO_DEBUG_MODE
#define CONFIG_DEBUG_MODE 1
#endif

// In CI mode, auto "press" OK buttons after 1 millisecond
#define CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS 1

// Tell the firmware code we are building libjade
#define CONFIG_LIBJADE 1

// Users can define CONFIG_LIBJADE_NO_SPIRAM to disable SPIRAM emulation
// (e.g. to allow testing DIY devices)
#ifndef CONFIG_LIBJADE_NO_SPIRAM
#define CONFIG_SPIRAM 1
#endif // CONFIG_LIBJADE_NO_SPIRAM

// Provide values in order to compile (we don't actually have a screen)
// FIXME: Allow defaulting to the values for Jade v1 and v2
// BBB-AIRGAP: made overridable, which is what the FIXME above asks for. The panel we drive is a
// 240x240 ST7789; the build passes -DCONFIG_DISPLAY_WIDTH/HEIGHT. See libjade/CMakeLists.txt.
#ifndef CONFIG_DISPLAY_WIDTH
#define CONFIG_DISPLAY_WIDTH 320
#endif
#ifndef CONFIG_DISPLAY_HEIGHT
#define CONFIG_DISPLAY_HEIGHT 200
#endif
#define CONFIG_DISPLAY_OFFSET_X 0
#define CONFIG_DISPLAY_OFFSET_Y 0
#define CONFIG_DISPLAY_FULL_FRAME_BUFFER 1
#define CONFIG_DISPLAY_FULL_FRAME_BUFFER_DOUBLE 1

// libjade may have no camera, but supports the debug scan_qr message
#define CONFIG_HAS_CAMERA 1

#define CONFIG_IDF_FIRMWARE_CHIP_ID 0 // Needed to build

// BBB-AIRGAP: on an ESP32 this marks a variable as living in the RAM segment the bootloader leaves
// alone, so its value survives a software restart. A Linux process has no such memory - every start
// is a cold one - so the attribute goes away and the variable is an ordinary static, zero at start.
// The one user is main/idletimer.c's 'idle_state', which therefore reads NORMAL after the
// idle-timeout restart rather than IDLE, and the screen comes back lit instead of dimmed.
#define __NOINIT_ATTR

#endif // _LIBJADE_SDKCONFIG_H_
