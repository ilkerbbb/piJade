#!/bin/bash
# BBB-AIRGAP: emulator measurements for tasks 41 and 48 (2026-09-08).
#   48 - Does Options > OTP > Set Clock show the clock page URL as a QR BEFORE opening the camera?
#   41 - Which URL does '?' on Session > fingerprint > Sign Message show, and does the device
#        accept the QR drawn by that page?
# Both follow the 2026-09-08 decision to publish pages at a URL and display its QR on the device,
# so they are measured in one script.
#
# Run in two steps; the first is outside the emulator because the page JavaScript runs with node,
# which is not installed in the container:
#   node pijade/tools/sign_qr_frame.js docs/sign/index.html <scratchpad>/sign_frame.gray
#   docker exec jade-dev bash /jade/pijade/tools/t4148_help.sh
# Requires: build_linux_nci_log daemon (camera and logging enabled) and compiled screen_qr_decode
# (build command in pijade/UPSTREAM.md section 19).
#
# Acceptance requires decoded text IDENTICAL to the URL; merely drawing a QR is insufficient.
# Likewise the signing path requires the device to produce the signature, beyond camera detection.
# Count every failure and exit nonzero so a caller can see the result without inspecting the screen.

D=/jade/build_linux_nci_log/libjade/libjade_daemon
SQR=/tmp/screenqr
# Public BIP39 test vector; real seeds never enter this machine.
SEED="abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
FRAME=/probe/sign_frame.gray
# The same seed, path and text produce the same signature on every run (deterministic nonce),
# so a constant can verify the EXPECTED signature. It was not copied from the device:
# pijade/tools/verify_signature.py recovers the public key and compares it with the key derived
# from the BIP39 words, providing independent verification. The path and the text are arguments
# of that script now, so if the page self-test path or text changes, rerun it as
# `verify_signature.py <signature> <path> <message>` and update this constant to what it
# confirms; do not copy the signature from the display.
EXPECTED_SIGNATURE="IA4824BgeUflJ7q/N0w5Lis+7hQa00HZQIrA5AzM4JG5c3Rm7zQKiWKG9ltzhKUyO+E4qoBpb1KbIQI84Gq4mCc="

ERROR=0
report() { echo "ERROR: $*"; ERROR=1; }

# Measured dumps have fixed names. A stale file could appear to decode a frame never captured
# in this run; delete them at startup and check every jadectl exit status below (Codex r4 P2).
rm -f /probe/s1_clock_page.rgb565 /probe/clock_cam_help.rgb565 /probe/sign_help.rgb565 \
      /probe/sign_sig.rgb565 /probe/otp_help.rgb565

stop() { pkill -f "^$D"; for i in $(seq 1 40); do pgrep -f "^$D" >/dev/null || break; sleep 0.3; done; }
start() { stop; rm -f /probe/$1 /probe/$2.a /probe/$2.b; (nohup $D --socketfile /probe/$1 --settings /probe/$2 --log-level info > $3 2>&1 &); for i in $(seq 1 40); do [ -S /probe/$1 ] && break; sleep 0.5; done; sleep 2; }

# 48: Set Clock, URL screen before the camera.
# The screen now uses the upstream back/continue flow (decision 2026-09-08). The old help screen
# said "Learn more:" and its sole back button still opened the camera. Measure the QR URL,
# that back does NOT open the camera, and that Continue does. The camera '?' must also show
# this flow's page.
L=/probe/daemonT48.log
start sockT48 settingsT48 $L
J="python3 /jade/pijade/tools/jadectl.py /probe/sockT48"
M="python3 /jade/pijade/tools/menu_audit.py /probe/sockT48 $L"
$M s0 "seed:$SEED" shot:home || report "48: could not load seed"
# Session is selected on home; two right presses select Options. Its list opens on Add Wallet;
# one down selects OTP. OTP opens with header '=' selected, requiring TWO down presses for
# Set Clock (measured 2026-09-08; one down clicked View OTP).
$M s1 btn:right shot:tile_scan btn:right shot:tile_options btn:click shot:opts \
      btn:down shot:opts_otp btn:click shot:otpmenu btn:down shot:otp_view \
      btn:down shot:otp_setclock btn:click shot:clock_page || report "48: menu navigation"
$SQR /probe/s1_clock_page.rgb565 240 240 "ilkerbbb.github.io/piJade/clock" \
  || report "48: display QR does not carry the expected URL"
# Back path: the screen opens on 'Continue'; up selects the header back arrow.
# The log must STILL lack 'Camera init done', proving back returned to the menu.
$J btn:up wait:0.5 shot:clock_back_sel btn:click wait:1.5 shot:clock_back \
  || report "48: could not run back path"
if grep -q "Camera init done" $L; then report "48: back arrow opened the camera"; \
  else echo "48: back arrow returned to menu, camera did not open"; fi
# Continue path: Set Clock remains selected in the menu; click again, then Continue.
$M s2 btn:click shot:clock_page2 btn:click shot:clock_camera || report "48: could not run continue path"
if grep -q "Camera init done" $L; then echo "48: Continue opened the camera"; \
  else report "48: Continue did not open the camera"; fi
# The camera header starts on '='; right selects '?'. This must show the flow page;
# handle_scan_qr() takes the help URL as a parameter.
$J btn:right wait:0.6 shot:cam_q_sel btn:click wait:1.5 shot:clock_cam_help \
  || report "48: could not capture camera help frame"
$SQR /probe/clock_cam_help.rgb565 240 240 "ilkerbbb.github.io/piJade/clock" \
  || report "48: camera '?' does not show the expected URL"

# 41: help URL on the Sign Message screen.
L=/probe/daemonT41.log
start sockT41 settingsT41 $L
J="python3 /jade/pijade/tools/jadectl.py /probe/sockT41"
M="python3 /jade/pijade/tools/menu_audit.py /probe/sockT41 $L"
$M m0 "seed:$SEED" shot:home41 || report "41: could not load seed"
# Session selected: click > Session list (fingerprint first) > click > wallet menu
# (Scan QR first) > three down presses = Sign Message (measured 2026-09-08).
$M m1 btn:click shot:session btn:click shot:wmenu btn:down btn:down btn:down shot:signrow \
      btn:click shot:signcam || report "41: menu navigation"
# The camera header starts on =; right selects ?, click opens help.
$J btn:right wait:0.6 shot:cam_help_sel btn:click wait:1.2 shot:sign_help \
  || report "41: could not capture help screen frame"
$SQR /probe/sign_help.rgb565 240 240 "ilkerbbb.github.io/piJade/sign" \
  || report "41: display QR does not carry the expected URL"

# 41 continued: does the code drawn by the page work on the device?
# Displaying a help URL does not prove it works. Feed the frame generated by the page's OWN
# code to the camera and require the device to produce the signature.
L=/probe/daemonT41B.log
if [ ! -f $FRAME ]; then
  report "41: $FRAME missing; run sign_qr_frame.js first (two-step instructions above)"
else
  start sockT41B settingsT41B $L
  J="python3 /jade/pijade/tools/jadectl.py /probe/sockT41B"
  M="python3 /jade/pijade/tools/menu_audit.py /probe/sockT41B $L"
  $M p0 "seed:$SEED" shot:home41b || report "41 payload: could not load seed"
  $M p1 btn:click btn:click btn:down btn:down btn:down shot:signrow_b btn:click shot:signcam_b \
    || report "41 payload: menu navigation"
  # The scanner retries on every frame; send the same frame several times. Transition from
  # camera to confirmation proves the payload was parsed.
  $J camfile:$FRAME:25 wait:1.5 shot:sign_confirm || report "41 payload: could not feed frame"
  grep -q "Not a message to sign" $L && report "41 payload: device rejected payload"
  grep -q "Detected 1 QR codes in image" $L || report "41 payload: camera never detected the code"
  # Confirmation starts on the header back arrow; right selects the check mark, click signs.
  # Then one down selects Show QR.
  $J btn:right wait:0.5 shot:sign_accept_sel btn:click wait:1.5 shot:sign_done \
     btn:down wait:0.5 btn:click wait:1.5 shot:sign_sig || report "41 payload: could not reach signature screen"
  $SQR /probe/sign_sig.rgb565 240 240 "$EXPECTED_SIGNATURE" \
    || report "41 payload: display signature QR does not carry the expected signature"
fi

# Comparison baseline: an existing upstream help screen.
# Measure how the same screen renders with a short URL to determine whether a long URL disrupts layout.
L=/probe/daemonTB.log
start sockTB settingsTB $L
J="python3 /jade/pijade/tools/jadectl.py /probe/sockTB"
M="python3 /jade/pijade/tools/menu_audit.py /probe/sockTB $L"
$M b0 "seed:$SEED" shot:homeb || report "baseline: could not load seed"
$M b1 btn:right btn:right btn:click shot:optsb btn:down btn:click shot:otpmenub \
  || report "baseline: menu navigation"
$J btn:right wait:0.6 shot:otp_help_sel btn:click wait:1.2 shot:otp_help \
  || report "baseline: could not capture help screen frame"
$SQR /probe/otp_help.rgb565 240 240 "blkstrm.com/otp" \
  || report "baseline: display QR does not carry the expected URL"

# Layout measurement: does the code frame fit on the screen?
# A cropped QR cannot decode, but successful decoding alone does not measure its clearance
# from the right edge. That is where layout trouble from a long URL would appear first.
# All three frames are expected to give the same bounding box.
python3 - <<'PYEOF'
import io, struct, sys
W = H = 240
def dark(v):
    r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
    return r < 8 and g < 16 and b < 8
failure = 0
for name in ("s1_clock_page", "clock_cam_help", "sign_help", "otp_help"):
    try:
        raw = io.open("/probe/%s.rgb565" % name, "rb").read()
    except OSError as e:
        print("ERROR: could not read %s: %s" % (name, e)); failure = 1; continue
    if len(raw) != W * H * 2:
        print("ERROR: %s %d bytes; expected %d" % (name, len(raw), W * H * 2)); failure = 1; continue
    px = struct.unpack("<%dH" % (W * H), raw)
    xs = []; ys = []
    for y in range(28, 210):
        for x in range(134, 239):
            if dark(px[y * W + x]):
                xs.append(x); ys.append(y)
    if not xs:
        print("ERROR: no dark modules in %s" % name); failure = 1; continue
    print("%-14s QR bounding box: x %d..%d (%d px), y %d..%d (%d px)"
          % (name, min(xs), max(xs), max(xs) - min(xs) + 1, min(ys), max(ys), max(ys) - min(ys) + 1))
sys.exit(failure)
PYEOF
LAYOUT=$?
[ $LAYOUT -eq 0 ] || report "layout measurement failed"
stop
[ $ERROR -eq 0 ] && echo "ALL MEASUREMENTS PASSED" || echo "AT LEAST ONE MEASUREMENT FAILED"
exit $ERROR
