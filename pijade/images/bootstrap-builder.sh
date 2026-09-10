#!/bin/sh
# Builds the piJade ARMv6 build environment from the Raspberry Pi OS image, from scratch.
#
# Until 2026-09-04 this step was nowhere written down in runnable form: it was only described in
# prose notes (the build-path decision of 2026-08-25) and the container had been built by
# hand. That was the one link of the chain that could not be reproduced; if the environment that
# produces the binaries going onto the device were lost, there would be no way to prove the same
# one had been rebuilt. This script closes that gap.
#
# Run it from the images directory (OUTSIDE the repository; the images and packages live there):
#   sh <repo>/pijade/images/bootstrap-builder.sh
# Needs: docker, xz, sha256sum. The source image and img.sha256 must be in the working directory.
# Output: two container images and bootstrap-record.txt (a fingerprint of everything produced).
#
# The reproducibility claim: the source image's fingerprint is verified HERE (fail-closed) and the
# fingerprints of the intermediates are recorded. It is NOT claimed that a bit-for-bit identical
# container image comes out; tar ordering and apt package versions can differ from run to run. The
# only thing measured is the record itself; whether two runs gave the same result is seen by
# comparing the records.
set -e

BASE_VERSION=${BASE_VERSION:-2026-06-18}
IMGXZ="$BASE_VERSION-raspios-bookworm-armhf-lite.img.xz"
IMG="$BASE_VERSION-raspios-bookworm-armhf-lite.img"
ROOTFS="raspios-rootfs.tar"
RECORD="bootstrap-record.txt"
DOCKERFILE=$(cd "$(dirname "$0")" && pwd)/Dockerfile.armv6-build

echo "=== 1) SOURCE IMAGE FINGERPRINT ==="
# This is the root of the chain. If it does not match, we stop without touching anything: a build
# environment set up from the wrong base makes every binary it produces quietly suspect.
[ -f "$IMGXZ" ] || { echo "SOURCE IMAGE MISSING: $IMGXZ"; exit 1; }
[ -f img.sha256 ] || { echo "img.sha256 MISSING; the source image cannot be verified"; exit 1; }
sha256sum -c img.sha256 || { echo "SOURCE IMAGE FINGERPRINT MISMATCH"; exit 1; }

echo "=== 2) DECOMPRESS ==="
if [ -f "$IMG" ]; then
    echo "already decompressed: $IMG"
else
    xz -dk -T0 "$IMGXZ"
fi

echo "=== 3) EXTRACT THE ROOT FILESYSTEM ==="
# The second partition (ext4) is tarred. The partition geometry is not hard-coded into the script,
# it is read from the MBR: a differently sized copy of the same release must still be mounted from
# the right place. losetup -P does not produce partition devices in this environment (measured
# inside the Docker VM, 2026-09-04), so it is mounted with offset/sizelimit instead.
docker run --rm --privileged -v "$PWD/$IMG:/img:ro" -v "$PWD:/out" alpine:3.20@sha256:d9e853e87e55526f6b2917df91a2115c36dd7c696a35be12163d44e6e2a4b6bc sh -c '
    apk add --no-cache util-linux tar >/dev/null 2>&1
    GEO=$(sfdisk -d /img | awk -F"[ ,=]+" "/img2/ {print \$4, \$6}")
    OFF=$(echo "$GEO" | cut -d" " -f1); SIZ=$(echo "$GEO" | cut -d" " -f2)
    [ -n "$OFF" ] && [ -n "$SIZ" ] || { echo "COULD NOT READ PARTITION GEOMETRY"; exit 1; }
    echo "partition 2: sector $OFF, $SIZ sectors"
    mkdir -p /m
    mount -o ro,offset=$((OFF*512)),sizelimit=$((SIZ*512)) /img /m || { echo "MOUNT FAILED"; exit 1; }
    [ -f /m/etc/os-release ] || { umount /m; echo "NOT THE EXPECTED ROOT"; exit 1; }
    tar -cpf /out/rootfs-new.tar --numeric-owner -C /m .
    umount /m
' || { echo "ROOT EXTRACTION FAILED"; exit 1; }
mv rootfs-new.tar "$ROOTFS"
ROOTFS_SHA=$(sha256sum "$ROOTFS" | awk '{print $1}')
echo "root filesystem: $ROOTFS ($ROOTFS_SHA)"

echo "=== 4) IMPORT INTO A CONTAINER ==="
docker import --platform linux/arm/v6 "$ROOTFS" "pijade/raspios-bookworm-armv6:$BASE_VERSION"
BASE_ID=$(docker image inspect --format '{{.Id}}' "pijade/raspios-bookworm-armv6:$BASE_VERSION")

echo "=== 5) INSTALL THE COMPILERS ==="
# No build context is given on purpose (the Dockerfile is read from stdin): this Dockerfile copies
# no files, so it needs none. Had "." been passed, the whole images directory - images and packages
# included, tens of gigabytes - would have been sent to docker.
docker build --platform linux/arm/v6 -t "pijade/raspios-build-armv6:$BASE_VERSION" - < "$DOCKERFILE"
BUILD_ID=$(docker image inspect --format '{{.Id}}' "pijade/raspios-build-armv6:$BASE_VERSION")

echo "=== 6) RECORD ==="
# Nothing is compared, a record is kept: the thing to compare against is the previous run's record.
{
    echo "run: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "source: $IMGXZ $(awk '{print $1}' img.sha256)"
    echo "root tar: $ROOTFS $ROOTFS_SHA"
    echo "base image: pijade/raspios-bookworm-armv6:$BASE_VERSION $BASE_ID"
    echo "build image: pijade/raspios-build-armv6:$BASE_VERSION $BUILD_ID"
} > "$RECORD"
cat "$RECORD"
echo "BOOTSTRAP_DONE"
