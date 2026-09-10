#!/bin/bash
# Produces the pijade-host and libjade.so binaries for ARMv6 (Pi Zero W) and builds the package
# that goes into the card image.
# Runs inside a container: pijade/raspios-build-armv6 (the target's own Raspbian root filesystem).
# Mounts: /src = the Jade repository, /idf = two components taken from ESP-IDF, /out = output.
#
# The command it is run with (from the images/ directory):
#   docker run --rm -i \
#       -v "$HOME/Projects/piJade/Jade:/src" -v "$PWD/idf-min:/idf" -v "$PWD/armv6-out:/out" \
#       pijade/raspios-build-armv6:2026-06-18 bash -s < ../Jade/pijade/images/build-armv6.sh
# Its output is /out/pijade-armv6.tar.gz; prepare-image.sh looks for it in the directory mounted
# as /pkg, so the armv6-out directory is mounted directly as /pkg.
set -e

export IDF_PATH=/idf
# Raspbian gcc already defaults to ARMv6 + VFPv2; these are given as an explicit contract.
# T8: on armhf Bookworm (glibc 2.36) time_t defaults to 4 bytes, so an epoch past 2038 enters
# settimeofday silently wrapped and TOTP produces the wrong code. Measured in this container with
# _TIME_BITS=64 (which requires _FILE_OFFSET_BITS=64): sizeof(time_t) 4 -> 8. The flag is given to
# the whole build (the libjade amalgamation, wally, mbedtls, pijade-host) at once; the
# _Static_assert in pijade_host.c breaks the build if the flag is ever dropped.
# The v4l2 ABI (Codex r2 P1, rejected against the source): struct v4l2_buffer carries a timeval;
# measured in this container, with 32-bit time_t sizeof=68 and VIDIOC_QBUF=0xc044560f, with time64
# 80 and 0xc050560f. The device kernel (rpi-6.12.y) defines struct v4l2_buffer with
# __kernel_v4l2_timeval (two long longs, 16 bytes) in include/uapi/linux/videodev2.h, the
# "#ifdef __KERNEL__" branch, so the kernel's NATIVE number is the 80-byte 0xc050560f; the 68-byte
# old layout is the COMPATIBILITY path through VIDIOC_QBUF_TIME32 / v4l2_buffer_time32
# (old_timeval32) in include/media/v4l2-ioctl.h. A time64 build moves onto the kernel's native ABI,
# whereas the camera that has worked on the device so far was on the compatibility path. The camera
# is measured again with this build on a device round; that measurement is still outstanding.
ARCHFLAGS='-mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64'

echo "=== ENVIRONMENT ==="
gcc --version | head -1
cmake --version | head -1
echo "IDF_PATH=$IDF_PATH"
ls "$IDF_PATH/components" | tr '\n' ' '; echo

echo "=== 1) CMAKE CONFIGURE ==="
# These flags produce exactly the values libjade/make_libjade.sh's
# "Release --camera --no-ci --no-debug --display=240x240" call produces (pijade/UPSTREAM.md, the
# production build command).
# LOG=1 (BBB-AIRGAP): the logging machinery stays in the binary but the default level is NONE
# (libjade/libjade.c:111), so not one line is printed unless pijade-host is given --log-level.
# LOG=0 deleted the macros outright at compile time (libjade/include/esp_log.h:16-19), which meant
# the only way to get evidence for a fault seen on the device was to build a new package and write
# it to the card.
cmake -S /src -B /out/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLOG=1 \
  -DCOVERAGE= \
  -DCAMERA=CAMERA \
  -DCI=0 \
  -DDEBUG_MODE=0 \
  -DDISPLAY_WIDTH=240 \
  -DDISPLAY_HEIGHT=240 \
  -DCMAKE_C_FLAGS="$ARCHFLAGS" \
  -DCMAKE_CXX_FLAGS="$ARCHFLAGS"

echo "=== 2) BUILD LIBJADE (the jade target only) ==="
/usr/bin/time -v cmake --build /out/build --target jade -j4 2> /out/time-jade.log || {
    echo "BUILD_FAILED"; tail -40 /out/time-jade.log; exit 1;
}
grep -E "Elapsed \(wall clock\)|Maximum resident set size" /out/time-jade.log || true

echo "=== 3) BUILD PIJADE-HOST ==="
# The source list is NOT repeated here. It was repeated once and went stale the day
# settings_store.c was added: the cross build failed to link while the host build was fine. The
# list lives in one place, in build.sh; the architecture flags and the output directory are passed
# to it from outside.
CC=gcc CFLAGS="$ARCHFLAGS" /src/pijade/host/build.sh /out/build

echo "=== 3b) T44 MEASUREMENT TOOL ==="
# Dense QR codes are not read because of quirc's window (220x220). That widening
# the window fixes decodability was shown on x86; the one open question was the per-frame cost on
# ARMv6. This binary produces that number and nothing else. Without
# /boot/firmware/pijade-t44.enable it exits doing nothing, so its presence in the image does not
# change boot behaviour.
# It is NOT linked against libjade.so: the quirc symbols are not exported, so the five source files
# (the same list as libjade.c:69-73) are compiled directly here. The flags have to match the
# shipped library exactly, otherwise the number measured does not belong to the code running on the
# device: ARCHFLAGS + CMAKE_C_FLAGS_RELEASE (libjade/CMakeLists.txt:51). The object is placed under
# /out/build so that step 4's ARMv6 gate scans it too.
T44_FLAGS="-O2 -DNDEBUG -D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS -fstack-protector-strong -fstack-clash-protection"
mkdir -p /out/build/t44
gcc $ARCHFLAGS $T44_FLAGS \
    -I/src/components/esp32-quirc/lib -I/src/components/esp32-quirc \
    -I/src/libjade/include -I/src/pijade/tools \
    -c /src/pijade/tools/t44_bench.c -o /out/build/t44/t44_bench.o
gcc $ARCHFLAGS -Wl,-z,relro,-z,now -o /out/build/pijade-t44-bench /out/build/t44/t44_bench.o -lm
ls -la /out/build/pijade-t44-bench

echo "=== 4) ARMv6 GATE: every object file ==="
FAIL=0
while IFS= read -r -d '' obj; do
    attrs="$(readelf -A "$obj" 2>/dev/null || true)"
    if printf '%s\n' "$attrs" | grep -Eq 'Tag_CPU_arch: v7|VFPv3|VFPv4|Advanced_SIMD|Tag_THUMB_ISA_use: Thumb-2'; then
        echo "GATE FAILED: $obj"
        printf '%s\n' "$attrs" | grep -E 'Tag_CPU_arch|Tag_FP_arch|Advanced_SIMD|THUMB'
        FAIL=1
    fi
done < <(find /out/build -type f -name '*.o' -print0)
OBJ_COUNT=$(find /out/build -type f -name '*.o' | wc -l)
echo "objects scanned: $OBJ_COUNT"
[ "$FAIL" -eq 0 ] && echo "OBJECT_GATE=PASS" || { echo "OBJECT_GATE=FAIL"; exit 1; }

echo "=== 5) ARMv6 GATE: the final binaries ==="
for f in /out/build/pijade-host /out/build/libjade/libjade.so /out/build/pijade-t44-bench; do
    echo "--- $f ---"
    readelf -hW "$f" | grep -E "Class:|Machine:|Flags:"
    readelf -AW "$f" | grep -E "Tag_CPU_name|Tag_CPU_arch:|Tag_FP_arch|Tag_ABI_VFP_args"
    readelf -lW "$f" | grep -i "program interpreter" || echo "(no INTERP; normal for a shared library)"
    readelf -dW "$f" | grep NEEDED
done

echo "=== 6) GLIBC / GLIBCXX VERSION REQUIREMENTS ==="
for f in /out/build/pijade-host /out/build/libjade/libjade.so; do
    echo "--- $f ---"
    readelf --version-info -W "$f" | grep -oE "(GLIBC|GLIBCXX|GCC)_[0-9.]+" | sort -u | tr '\n' ' '; echo
done

echo "=== 7) SIZES ==="
ls -la /out/build/pijade-host /out/build/libjade/libjade.so

echo "=== 8) PACKAGE ==="
# Packaging is in the same script as the build: when the two come from separate hands the package
# goes stale silently and the card image is prepared with a binary that does not work (which is
# exactly what happened on 2026-08-25).
mkdir -p /out/stage/opt/pijade/bin /out/stage/opt/pijade/lib
cp /out/build/pijade-host        /out/stage/opt/pijade/bin/pijade-host
cp /out/build/libjade/libjade.so /out/stage/opt/pijade/lib/libjade.so
cp /out/build/pijade-t44-bench   /out/stage/opt/pijade/bin/pijade-t44-bench
chmod 0755 /out/stage/opt/pijade/bin/pijade-host /out/stage/opt/pijade/lib/libjade.so \
           /out/stage/opt/pijade/bin/pijade-t44-bench
# The package is produced deterministically: run twice from the same source it must give the same
# SHA256 (the round's acceptance criterion). Four normalisations are needed for that: --sort=name
# (directory read order comes from the filesystem and is not stable), --mtime=@0 (the build time
# leaked into the tar headers; measured 2026-09-04, two packages differed by mtime ALONE),
# --owner/--group=0 and --numeric-owner (name resolution depends on the container's passwd file).
# No extra flag is needed for the timestamp in the gzip header: tar -z calls gzip over a pipe, and
# gzip reading from stdin writes zero into the MTIME field (measured: 1f 8b 08 00 00 00 00 00).
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    -czf /out/pijade-armv6.tar.gz -C /out/stage ./opt
( cd /out && sha256sum pijade-armv6.tar.gz > pijade-armv6.tar.gz.sha256 )
tar -tvzf /out/pijade-armv6.tar.gz
cat /out/pijade-armv6.tar.gz.sha256

echo "DONE"
