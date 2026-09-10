#!/bin/bash
# Builds the pijade host against an existing libjade build.
# Usage: pijade/host/build.sh [build directory, default build_linux]
#
# The build directory may be absolute, which is what a cross build needs: the ARMv6 image builder
# mounts the repository read-only at /src and writes to /out, so its output lives nowhere under the
# repository. CC and CFLAGS are read from the environment for the same reason - the cross build has
# to pass -mcpu/-mfpu/-mfloat-abi, and the source list must not be copied into a second script to
# do it. It was copied once, into images/build-armv6.sh, and that copy went stale the day
# settings_store.c was added: the cross build failed to link while the host build was fine.
set -e
BUILD_DIR="${1:-build_linux}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
case "$BUILD_DIR" in
    /*) OUT_DIR="$BUILD_DIR" ;;
    *)  OUT_DIR="${REPO}/${BUILD_DIR}" ;;
esac
CC="${CC:-gcc}"
# BBB-AIRGAP: hardening for the binary that actually runs on the card. Measured on the image's
# own compiler before being added; see libjade/CMakeLists.txt for the same list on the library
# side. -pie matters here and not there: a shared library is position independent already, an
# executable is not unless it is asked to be, and without it the load address is fixed.
HARDENING="-D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fPIE -pie -Wl,-z,relro,-z,now"
"$CC" $CFLAGS -Wall -Wextra -O2 $HARDENING -o "${OUT_DIR}/pijade-host" \
    "${REPO}/pijade/host/pijade_host.c" \
    "${REPO}/pijade/host/panel_st7789.c" \
    "${REPO}/pijade/host/buttons_gpio.c" \
    "${REPO}/pijade/host/camera_v4l2.c" \
    "${REPO}/pijade/host/settings_store.c" \
    -I"${REPO}/libjade" -L"${OUT_DIR}/libjade" -ljade -lpthread -lz -lm
echo "built ${OUT_DIR}/pijade-host"
echo "run with LD_LIBRARY_PATH=${OUT_DIR}/libjade"
