#!/bin/sh
# T7 verification chain: twin fidelity -> fresh copy -> prepare-image.sh -> independent check -> sabotage
# -> boot-verify line counts. Any failed step produces a nonzero exit code.
# Usage: sh pijade/images/tests/t7/t7_chain.sh
#   IMAGES=<directory> overrides the image directory; default is ../images beside the repo root.
# Input : $IMAGES/pijade-t33a.img (master image, UNTOUCHABLE) and $IMAGES/armv6-out (package directory).
# Output: $IMAGES/pijade-t7-test.img (hardened copy), logs /tmp/t7-*.log,
#         final line "CHAIN_DONE P=<prepare> V=<verify> Z=<sabotage> B=<boot-verify>" (all four must be 0).
set -u
T=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$T/../../../.." && pwd)
PREP="$REPO/pijade/images/prepare-image.sh"
IMAGES=${IMAGES:-$REPO/../images}
# Invoke Alpine by digest, not tag: "3.20" is a branch tag that moves to a new image with
# point releases (3.20.1, 3.20.2, ...); measured on 2026-09-04, when it pointed to 3.20.10.
# A verification chain using a tag would use different tools on identical inputs over time.
ALPINE=alpine:3.20@sha256:d9e853e87e55526f6b2917df91a2115c36dd7c696a35be12163d44e6e2a4b6bc

echo "=== 0a) twin fidelity: is t7_gates_only.sh derived from the main script ==="
# Twin = its own 12-line header + main script MASKED_UNITS list + main script gate block
# (between `--- T7 offline gates ---` and `=== 9) UNMOUNT`) + one closing line. If the main script
# changes without refreshing the twin, sabotage tests stale code; this step rejects that mechanically.
MS=$(grep -n '^MASKED_UNITS="$' "$PREP" | head -1 | cut -d: -f1)
ME=$(awk -v s="$MS" 'NR>s && /^"$/ {print NR; exit}' "$PREP")
GS=$(grep -n '^echo "--- T7 offline gates ---"$' "$PREP" | cut -d: -f1)
GE=$(grep -n '^echo "=== 9) UNMOUNT ==="$' "$PREP" | cut -d: -f1)
if [ -z "$MS" ] || [ -z "$ME" ] || [ -z "$GS" ] || [ -z "$GE" ]; then
    echo "TWIN ANCHORS NOT FOUND (MS=$MS ME=$ME GS=$GS GE=$GE)"; exit 1
fi
{ sed -n '1,12p' "$T/t7_gates_only.sh"; sed -n "${MS},${ME}p" "$PREP"; sed -n "${GS},$((GE-1))p" "$PREP"; echo 'echo "ALL GATES PASSED"'; } > /tmp/t7-gates-expected.sh
if ! cmp -s /tmp/t7-gates-expected.sh "$T/t7_gates_only.sh"; then
    echo "STALE TWIN: t7_gates_only.sh differs from main script; diff:"
    diff /tmp/t7-gates-expected.sh "$T/t7_gates_only.sh"
    exit 1
fi
echo "twin faithful to main script (MASKED_UNITS $MS-$ME, gate block $GS-$((GE-1)))"

cd "$IMAGES" || { echo "IMAGE DIRECTORY MISSING: $IMAGES"; exit 1; }
echo "=== 0b) fresh copy + initial hashes ==="
cp pijade-t33a.img pijade-t7-test.img || { echo "COPY ERROR"; exit 1; }
# prepare-image.sh cannot pin its input hash (it modifies IN PLACE, so the second run consumes
# the first run's output). This is the only moment to pin it, before the run touches anything.
# Two initial hashes: the master image copied and the package. Here they are RECORDED,
# not COMPARED; the previous run's log is the comparison reference. Verifying the package
# against its own .sha256 is a separate gate in prepare-image.sh step 3.
INITIAL=/tmp/t7-initial-hash.txt
{ sha256sum pijade-t33a.img && sha256sum armv6-out/pijade-armv6.tar.gz; } > "$INITIAL" \
    || { echo "INITIAL HASH ERROR"; exit 1; }
cat "$INITIAL"
echo "=== 1) prepare-image.sh (fresh copy) ==="
docker run --rm -i --privileged -v "$PWD/pijade-t7-test.img:/img" -v "$PWD/armv6-out:/pkg" "$ALPINE" sh -s < "$PREP" > /tmp/t7-prepare.log 2>&1
P1=$?; echo "PREPARE_EXIT=$P1"; grep -c "PASSED\|OK" /tmp/t7-prepare.log; tail -3 /tmp/t7-prepare.log
echo "=== 2) independent verification ==="
docker run --rm -i --privileged -v "$PWD/pijade-t7-test.img:/img" -v "$T:/s" "$ALPINE" sh /s/t7_verify.sh > /tmp/t7-verify.log 2>&1
V=$?; echo "VERIFY_EXIT=$V"; tail -2 /tmp/t7-verify.log
echo "=== 3) sabotage ==="
docker run --rm -i --privileged -v "$PWD/pijade-t7-test.img:/img" -v "$T:/s" "$ALPINE" sh /s/t7_sabotage.sh > /tmp/t7-sabotage.log 2>&1
Z=$?; echo "SABOTAGE_EXIT=$Z"; tail -3 /tmp/t7-sabotage.log
echo "=== 4) boot-verify: info lines and syntax ==="
docker run --rm -i --privileged -v "$PWD/pijade-t7-test.img:/img" "$ALPINE" sh -c '
    apk add --no-cache util-linux >/dev/null 2>&1; mkdir -p /m
    mount -o ro,offset=$((1064960*512)),sizelimit=$((3817472*512)) /img /m || exit 1
    V=/m/usr/local/sbin/pijade-boot-verify
    N=$(grep -c "rng (info)" $V); NB=$(grep -c "boot_id (info)" $V)
    NP=$(grep -c "power (info)" $V); NT=$(grep -c "temperature (info)" $V)
    echo "info lines: rng=$N boot_id=$NB power=$NP temperature=$NT"
    sh -n $V && [ "$N" = 4 ] && [ "$NB" = 2 ] && [ "$NP" = 4 ] && [ "$NT" = 1 ]; rc=$?
    umount /m; exit $rc'
B=$?; echo "BOOTVERIFY_EXIT=$B"
echo "CHAIN_DONE P=$P1 V=$V Z=$Z B=$B"
[ "$P1" = 0 ] && [ "$V" = 0 ] && [ "$Z" = 0 ] && [ "$B" = 0 ]
