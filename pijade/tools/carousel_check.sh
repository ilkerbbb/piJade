#!/bin/bash
# BBB-AIRGAP: carousel screen regression measurement; the driver uses Options > Display > Camera
# Rotation because its result is written to the card, so acceptance can be verified through a
# persistent effect as well as a display comparison.
#
# Why: await_carousel_activity() (main/ui/dialogs.c) had two findings at Codex Gate G:
# the opening click leaked into the carousel, and each iteration registered another event waiter.
# The fix follows the dice screen pattern (main/entropy_sources.c): one registration,
# synchronous transitions and draining stale input on entry. The race itself cannot be
# triggered in the emulator; this checks that the new event path works end to end.
#
# Usage (in the container): bash /jade/pijade/tools/carousel_check.sh
# Requires: build_linux_nci_log build and /probe mount.
set -u
# BBB-AIRGAP: failures propagate to the EXIT CODE. The previous version echoed each finding,
# continued and returned 0 with an unconditional "done", misleading the caller into assuming a pass.
# Also, `cmp` returns nonzero for MISSING files, which some checks read as "different" and thus
# SUCCESS; two missing files gave a false pass. compare_same()/compare_different() first require
# both files to exist.
ERROR=0
report() { echo "ERROR: $*" >&2; ERROR=1; }
_files_exist() { # _files_exist <name> <a> <b>
    if [ ! -s "$2" ] || [ ! -s "$3" ]; then report "$1: missing frame for comparison ($2 / $3)"; return 1; fi
    return 0
}
compare_same() { # compare_same <name> <a> <b>  : must be SAME
    _files_exist "$1" "$2" "$3" || return
    if cmp -s "$2" "$3"; then echo "OK: $1"; else report "$1"; fi
}
compare_different() { # compare_different <name> <a> <b> : must be DIFFERENT
    _files_exist "$1" "$2" "$3" || return
    if cmp -s "$2" "$3"; then report "$1"; else echo "OK: $1"; fi
}
D=/jade/build_linux_nci_log/libjade/libjade_daemon
SOCK=/probe/sockC1; LOG=/probe/daemonC1.log; SET=/probe/settingsC1
pkill -f "^$D" 2>/dev/null; sleep 1
rm -f $SET.a $SET.b $SOCK $LOG /probe/c1_*.rgb565 /probe/car3.rgb565
(nohup $D --socketfile $SOCK --settings $SET --log-level info > $LOG 2>&1 &)
for i in $(seq 1 40); do [ -S $SOCK ] && break; sleep 0.5; done
[ -S $SOCK ] || { report "daemon did not start"; exit 1; }
sleep 2
cd /probe
J="python3 /jade/pijade/tools/jadectl.py $SOCK"
M="python3 /jade/pijade/tools/menu_audit.py $SOCK $LOG c1"
SEED="seed:abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
# Options starts on Add Wallet (measured); five down presses select Display.
$M "$SEED" shot:home \
  btn:right btn:right btn:click shot:options \
  btn:down btn:down btn:down btn:down btn:down btn:click shot:display || report "menu audit 1 failed"
# Card state BEFORE the carousel: this proves the persistent effect of the click below.
BEFORE=$(cat $SET.a $SET.b 2>/dev/null | md5sum | awk '{print $1}')
# Display starts on the back arrow; four down presses select Camera Rotation. btn: verifies
# a display change after each press, so clicking the row MUST actually open the carousel.
$M btn:down btn:down btn:down btn:down btn:click shot:car1 \
  btn:right shot:car2 \
  btn:click shot:back || report "menu audit 2 failed"
AFTER=$(cat $SET.a $SET.b 2>/dev/null | md5sum | awk '{print $1}')
echo "=== was the card written (after click) ==="
[ "$BEFORE" != "$AFTER" ] && echo "OK: setting written to card" || report "card unchanged (selection not accepted)"
# Reopen the carousel: the rebuilt menu starts on the back arrow, so use btn:first to return
# to the beginning and count four down presses.
$J btn:first wait:0.5 btn:down wait:0.3 btn:down wait:0.3 btn:down wait:0.3 btn:down wait:0.3 btn:click wait:1 shot:car3 > /dev/null  # jadectl adds no prefix: /probe/car3.rgb565
echo "=== comparisons ==="
compare_different "carousel screen opened" c1_display.rgb565 c1_car1.rgb565
compare_different "right press changed the label" c1_car1.rgb565 c1_car2.rgb565
compare_same "selection accepted, carousel reopened with the new value" c1_car2.rgb565 car3.rgb565
compare_different "different from the initial opening" c1_car1.rgb565 car3.rgb565
echo "=== log ==="
N=$(grep -ciE "assert|abort|Failed to store" $LOG || true)
echo "log error lines: $N"
[ "$N" = "0" ] || report "log contains $N error lines"
pgrep -f "^$D" >/dev/null && echo "DAEMON RUNNING" || report "daemon died"
if [ $ERROR -eq 0 ]; then echo "CAROUSEL_CHECK_DONE"; else echo "CAROUSEL MEASUREMENT FAILED"; fi
exit $ERROR
