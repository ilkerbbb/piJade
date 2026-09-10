#!/bin/bash
# BBB-AIRGAP: emulator version of task 51 (2026-09-08).
#   Camera screen top strip: buttons no longer cover guide corners, the title moved from
#   the image center to the strip, and the strip is dimmed rather than opaque.
#
# Run (in the container):
#   docker exec jade-dev bash /jade/pijade/tools/t51_strip.sh
# Requires: build_linux_nci_log daemon (camera enabled). Measurements are in t51_measure.py.
#
# Drive the camera with FLAT tones to measure dimming against a known base: for a 0x80 frame,
# the top strip base should be 0x40. Two tones provide a positive control: does dimming follow
# the input or produce a fixed value?
# The third frame is a scene because flat tones cannot show whether the image is preserved.
set -u
D=/jade/build_linux_nci_log/libjade/libjade_daemon
# Public BIP39 test vector; real seeds never enter this machine.
SEED="abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
SCENE=/probe/t51_scene.gray
L=/probe/daemonT51.log
ERROR=0
report() { echo "ERROR: $*"; ERROR=1; }

# A stale dump could make a frame never captured in this run appear to have been measured.
rm -f /probe/t51_flat80.rgb565 /probe/t51_flat40.rgb565 /probe/t51_scene.rgb565

# Generate frames here to avoid relying on an external file. The scene resembles a code but
# cannot be scanned: a real QR decodes on the first frame, closes the camera and causes measurement
# on another screen (happened once on 2026-09-08). The pattern has no finder squares, so quirc
# cannot lock onto it; the two-axis gradient leaves tonal variation UNDER the strip, where
# preservation of the image can be measured.
python3 - <<'FRAME'
import hashlib
W, H = 640, 480
open('/probe/t51_flat80.gray','wb').write(bytes([0x80])*W*H)
open('/probe/t51_flat40.gray','wb').write(bytes([0x40])*W*H)
frame = bytearray(W*H)
for y in range(H):
    for x in range(W):
        frame[y*W+x] = 90 + (x*80)//W + (y*70)//H
seed = hashlib.sha256(b't51').digest()
module, count = 8, 21
x0, y0 = (W - module*count)//2, (H - module*count)//2
for my in range(count):
    for mx in range(count):
        if not (seed[(my*count+mx) % len(seed)] >> (mx % 8)) & 1:
            continue
        for y in range(y0+my*module, y0+(my+1)*module):
            for x in range(x0+mx*module, x0+(mx+1)*module):
                frame[y*W+x] = 20
open('/probe/t51_scene.gray','wb').write(bytes(frame))
FRAME
[ -s "$SCENE" ] || report "could not generate scene frame: $SCENE"

stop() { pkill -f "^$D"; for i in $(seq 1 40); do pgrep -f "^$D" >/dev/null || break; sleep 0.3; done; }
stop
rm -f /probe/sockT51 /probe/settingsT51.a /probe/settingsT51.b
(nohup $D --socketfile /probe/sockT51 --settings /probe/settingsT51 --log-level info > $L 2>&1 &)
for i in $(seq 1 40); do [ -S /probe/sockT51 ] && break; sleep 0.5; done
sleep 2

J="python3 /jade/pijade/tools/jadectl.py /probe/sockT51"
M="python3 /jade/pijade/tools/menu_audit.py /probe/sockT51 $L"

# Options > OTP > Set Clock > Continue enters the camera through handle_scan_qr() (dashboard.c),
# one of its three call sites; its title is now "Clock QR".
$M s0 "seed:$SEED" shot:home || report "could not load seed"
$M s1 btn:right btn:right btn:click btn:down btn:click btn:down btn:down btn:click shot:clock_page \
  || report "menu navigation"
$M s2 btn:click shot:camera || report "camera did not open"
grep -q "Camera init done" $L || report "camera startup line missing from log"

for tone in 80 40; do
  $J camfile:/probe/t51_flat${tone}.gray:14 wait:0.4 shot:t51_flat${tone} \
    || report "could not feed flat 0x${tone} frame"
done
$J camfile:${SCENE}:14 wait:0.4 shot:t51_scene || report "could not feed scene frame"

for f in t51_flat80 t51_flat40 t51_scene; do
  [ -s /probe/$f.rgb565 ] || report "dump not written: $f"
done

stop
if [ $ERROR -eq 0 ]; then
  python3 /jade/pijade/tools/t51_measure.py /probe/t51_flat80.rgb565 /probe/t51_flat40.rgb565 \
      /probe/t51_scene.rgb565 || ERROR=1
fi
echo "T51_RC=$ERROR"
exit $ERROR
