#!/bin/bash
# BBB-AIRGAP: emulator measurement for task 46 (2026-09-08).
#   46 - Are USB and Bluetooth icons aligned to the right edge of the home status bar?
#
# Run: docker exec jade-dev bash /jade/pijade/tools/t46_status_bar.sh <tag>
# The tag becomes part of the dump name, so old and new builds can be measured consecutively
# in the same container and compared.
#
# Measure the COUNT of white pixel clusters in the status bar icon strip and the distance
# from the rightmost cluster to the panel right edge. Both are printed numerically for the
# caller to compare; drawing the icons alone is insufficient.

set -u
TAG=${1:-measurement}
D=/jade/build_linux_nci_log/libjade/libjade_daemon
ERROR=0
report() { echo "ERROR: $*"; ERROR=1; }

DUMP=/probe/t46_${TAG}_home.rgb565
rm -f $DUMP

stop() { pkill -f "^$D"; for i in $(seq 1 40); do pgrep -f "^$D" >/dev/null || break; sleep 0.3; done; }
stop
rm -f /probe/sockT46 /probe/settingsT46.a /probe/settingsT46.b
(nohup $D --socketfile /probe/sockT46 --settings /probe/settingsT46 --log-level info \
   > /probe/daemonT46.log 2>&1 &)
for i in $(seq 1 40); do [ -S /probe/sockT46 ] && break; sleep 0.5; done
sleep 2

python3 /jade/pijade/tools/menu_audit.py /probe/sockT46 /probe/daemonT46.log t46_${TAG} \
  shot:home || report "46: could not capture home display dump"
[ -s $DUMP ] || report "46: dump file missing or empty"

python3 - "$DUMP" "$TAG" <<'PY'
import sys
path, tag = sys.argv[1], sys.argv[2]
W = H = 240
raw = open(path, "rb").read()
assert len(raw) == W * H * 2, f"expected {W*H*2}, got {len(raw)}"

def pixel(x, y):
    i = (y * W + x) * 2
    v = (raw[i] << 8) | raw[i + 1]
    return (((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31)

# The status bar is 44 pixels high (main/display.c: GUI_STATUS_BAR_HEIGHT). Icons occupy its
# upper half; the lower half contains the white device name, so restrict the band to the upper half.
# The left 58% holds the logo, icons occupy the right 42%; start scanning there.
TOP, BOTTOM, LEFT = 0, 22, int(W * 0.58)
columns = set()
for x in range(LEFT, W):
    for y in range(TOP, BOTTOM):
        r, g, b = pixel(x, y)
        if r > 180 and g > 180 and b > 180:
            columns.add(x)
            break

if not columns:
    print(f"ERROR: 46: {tag}: no white pixels in icon strip (positive control failed)")
    raise SystemExit(1)

# Group adjacent columns into clusters; there must be at least one empty column between icons.
ordered = sorted(columns)
clusters, start, previous = [], ordered[0], ordered[0]
for x in ordered[1:]:
    if x > previous + 1:
        clusters.append((start, previous))
        start = x
    previous = x
clusters.append((start, previous))

right_edge = W - 1
distance = right_edge - clusters[-1][1]
print(f"46: {tag}: cluster count {len(clusters)}")
for i, (a, b) in enumerate(clusters, 1):
    print(f"46: {tag}: cluster {i}: x {a}..{b} (width {b - a + 1})")
print(f"46: {tag}: rightmost pixel x={clusters[-1][1]}, distance to right edge "
      f"{distance} pixels")

# Acceptance has two conditions, both enforced here; printing measurements and exiting zero
# would silently show a pass to a caller trusting the script.
failed = False
if len(clusters) != 2:
    print(f"ERROR: 46: {tag}: expected two icon clusters, found {len(clusters)}")
    failed = True
# With correct layout, only padding remains, measured at 8 pixels. The limit is 12, allowing
# four pixels for rounding and glyph spacing; before the fix it was 53, and an intermediate
# version that only reordered columns gave 13. Both exceed this limit.
LIMIT = 12
if distance > LIMIT:
    print(f"ERROR: 46: {tag}: rightmost icon is {distance} pixels inside the edge, "
          f"limit {LIMIT}")
    failed = True
if failed:
    raise SystemExit(1)
PY
[ $? -eq 0 ] || report "46: analysis failed"

stop
if [ $ERROR -eq 0 ]; then echo "46: measurement complete ($TAG)"; else echo "46: MEASUREMENT FAILED"; fi
exit $ERROR
