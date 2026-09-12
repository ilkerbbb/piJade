#!/bin/bash
#
# Build the Jade firmware into a shared library for in-process debugging
#
# ./libjade/make_libjade.sh [Debug|Release|RelWithDebInfo|MinSizeRel|Sanitize] [--log] [--camera] [--no-ci] [--no-debug] [--display=WxH] [--coverage]
#
set -e

BUILD_TYPE="Debug"
LOG="0"
CI="CI"
# BBB-AIRGAP: defaults ON to match upstream; --no-debug drops the debug handlers and RPC surface
DEBUG_MODE="DEBUG_MODE"
# BBB-AIRGAP: 320x200 is libjade's placeholder; the Waveshare 1.3" HAT is 240x240
DISPLAY_WIDTH="320"
DISPLAY_HEIGHT="200"
CAMERA="0"

usage() {
    echo "Usage: $0 [Debug|Release|RelWithDebInfo|MinSizeRel|Sanitize] [--log] [--camera] [--no-ci] [--no-debug] [--display=WxH] [--coverage]"
    exit 1
}

# iterate through optional arguments and set variables accordingly
for arg in "$@"; do
    case $arg in
        --help)
            usage
            ;;
        Debug|Release|RelWithDebInfo|MinSizeRel|Sanitize)
            BUILD_TYPE="$arg"
            shift
            ;;
        --coverage)
            COVERAGE="COVERAGE"
            shift
            ;;
        --log)
            LOG="LOG"
            shift
            ;;
        --no-ci)
            CI="0"
            shift
            ;;
        --camera)
            CAMERA="CAMERA"
            shift
            ;;
        --no-debug)
            DEBUG_MODE="0"
            shift
            ;;
        # BBB-AIRGAP: panel size, e.g. --display=240x240. Written as a single argument because
        # this loop is 'for arg in "$@"', where shift does not consume a following argument.
        --display=*)
            DISPLAY_WIDTH="${arg#*=}"
            if [[ "${DISPLAY_WIDTH}" != *x* ]]; then
                echo "Invalid display size: ${DISPLAY_WIDTH} (expected WxH)"
                usage
            fi
            DISPLAY_HEIGHT="${DISPLAY_WIDTH#*x}"
            DISPLAY_WIDTH="${DISPLAY_WIDTH%%x*}"
            shift
            ;;
        *)
            echo "Unknown argument: $arg"
            usage
            ;;
    esac
done

# BBB-AIRGAP: this script writes build_linux, which the emulator container bind-mounts from this
# same tree; running it on macOS would overwrite the container's build with host objects.  The
# macOS host build lives in build_macos instead - the configure line is in .gitignore.
if [ "$(uname -s)" = "Darwin" ]; then
    echo "make_libjade.sh builds build_linux, which the emulator container shares; on macOS" >&2
    echo "configure build_macos instead (see the build_macos entry in .gitignore)." >&2
    exit 1
fi

mkdir -p build_linux
cd build_linux
EXTRA_ARGS=''
if [ "${BUILD_TYPE}" == "Sanitize" ]; then
    EXTRA_ARGS='-DCMAKE_C_FLAGS"-fsanitize=undefined" -DCMAKE_CXX_FLAGS"-fsanitize=undefined"'
fi
cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} ${EXTRA_ARGS} -DLOG=${LOG} -DCOVERAGE=${COVERAGE} -DCAMERA=${CAMERA} -DCI=${CI} -DDEBUG_MODE=${DEBUG_MODE} -DDISPLAY_WIDTH=${DISPLAY_WIDTH} -DDISPLAY_HEIGHT=${DISPLAY_HEIGHT} ..
make -j8
cd ..

echo "to use libjade set LD_LIBRARY_PATH=$PWD/build_linux/libjade"
if [ "${BUILD_TYPE}" == "Sanitize" ]; then
    echo "and ASAN_OPTIONS=symbolize=1,detect_leaks=0 LD_PRELOAD=$(ls /usr/lib/gcc/x86_64-linux-gnu/*/libasan.so) UBSAN_OPTIONS=print_stacktrace=1"
fi
