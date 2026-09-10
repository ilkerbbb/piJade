#!/bin/bash
# BBB-AIRGAP: emulator measurement for task 42 (2026-09-08).
#   42 - Info > I/O Test > Buttons: does each input light its own markers and ONLY those?
#
# Run: docker exec jade-dev bash /jade/pijade/tools/t42_buttons.sh
#
# Opening the screen alone is insufficient. Six events cover seven markers; center and KEY2
# produce the same event and light two markers together. Measure each event alone on a CLEAN
# screen, then repeat the six presses cumulatively. Isolated measurements catch extra markers
# (a cumulative check alone can hide a wrong event on a marker lit by an earlier press).
# Cumulative measurement proves that lit markers stay lit. Both also enforce which markers
# must remain off.
#
# The eighth button, KEY3, has a separate test CHANGED by task 47 (2026-09-08): escape now
# propagates outward to the DASHBOARD. Compare the resulting frame byte for byte with the
# dashboard captured before entry. Accordingly, reset_screen() now walks the full route from
# the dashboard to Buttons; previously KEY3 returned to the parent menu and one click reentered.
#
# The second section measures a FLIPPED screen (`Display > Flip Orientation`). Markers are normal
# view nodes and rotate with the image: a marker at the layout bottom appears at the user's top.
# Both directional pairs therefore reverse: `btn:up` presses physical up and lights layout `down`,
# which appears in the pressed direction on the panel. Emulator frames use layout coordinates
# (`get_display_bytes` returns `display_hw_get_buffer()`; mirroring happens on the panel), so
# expected sets also use layout coordinates. Without this section, the pre-fix code that lit
# `up` for an up press would pass.

set -u
D=/jade/build_linux_nci_log/libjade/libjade_daemon
T=/jade/pijade/tools
ERROR=0
report() { echo "ERROR: $*" >&2; ERROR=1; }

stop() {
    local i rc
    pkill -f "^$D"; rc=$?
    if [ "$rc" -gt 1 ]; then
        report "42: could not stop process: pkill exit $rc"
        return 1
    fi
    for ((i = 0; i < 40; i++)); do
        pgrep -f "^$D" >/dev/null; rc=$?
        case $rc in
            0) sleep 0.3 ;;
            1) return 0 ;;
            *) report "42: could not read process status: pgrep exit $rc"; return 1 ;;
        esac
    done
    report "42: process still running after 40 polls: $D"
    return 1
}
stop || exit 1
rm -f /probe/sockT42 /probe/settingsT42.a /probe/settingsT42.b /probe/t42_*.rgb565 \
    || { report "42: could not clear previous measurement files"; exit 1; }
(nohup $D --socketfile /probe/sockT42 --settings /probe/settingsT42 --log-level info \
   > /probe/daemonT42.log 2>&1 &)
for i in $(seq 1 40); do [ -S /probe/sockT42 ] && break; sleep 0.5; done
[ -S /probe/sockT42 ] || { report "42: control socket did not open: /probe/sockT42"; exit 1; }
sleep 2

PREFIX=t42
M="python3 $T/menu_audit.py /probe/sockT42 /probe/daemonT42.log $PREFIX"

# Options > Info > I/O Test > Buttons. Scrollable lists start on the first row, menus on the
# header button; down counts reflect this difference (measured 2026-09-08).
# Keep the dashboard-to-Buttons route in one place for both initial entry and reset_screen().
# Scrollable lists start on the first row, menus on the header button; down counts reflect
# this difference (measured 2026-09-08).
ENTRY="btn:click btn:down btn:down btn:down btn:down btn:down btn:down btn:click \
       btn:down btn:down btn:down btn:click btn:down btn:down btn:click"

# Dashboard anchor with Options selected. KEY3 must return here.
$M btn:right btn:right shot:dashboard || report "42: could not capture dashboard anchor"
$M $ENTRY shot:entry || report "42: could not reach Buttons screen"

# No marker may be lit on entry; a residual menu click leaking in would light the center
# marker for free and invalidate every subsequent measurement.
python3 $T/t42_marks.py /probe/t42_entry.rgb565 - || report "42: marker lit in entry frame"

# After each press measure the WHOLE set: both markers expected on and markers that must stay off.
measure() { # measure <name> <button> <expected>
    $M "btn:$2" "shot:$1" || report "42: $2 press did not change the display"
    python3 $T/t42_marks.py "/probe/${PREFIX}_$1.rgb565" "$3" || report "42: wrong markers after $2"
}

# Reset before isolated measurements: KEY3 to the dashboard, then walk the full route again.
# Check both that no marker is lit on reentry and that the frame matches initial entry exactly,
# proving the route reached Buttons and navigation counts have not drifted. Without the second
# check, measurement could continue on the wrong screen.
reset_screen() {
    $M btn:alt || { report "42: could not reach dashboard with KEY3"; exit 1; }
    $M $ENTRY "shot:$1" || { report "42: could not open clean test screen"; exit 1; }
    python3 $T/t42_marks.py "/probe/${PREFIX}_$1.rgb565" - \
        || { report "42: marker lit on reentry"; exit 1; }
    cmp -s "/probe/${PREFIX}_entry.rgb565" "/probe/${PREFIX}_$1.rgb565" \
        || { report "42: reentry frame differs from initial entry"; exit 1; }
}
isolated() { # isolated <button> <expected>
    reset_screen "i_$1_entry"
    measure "i_$1" "$1" "$2"
}

isolated up up
isolated down down
isolated left left
isolated right right
isolated click "click,key2"
isolated first key1

# Keep the cumulative section: it alone proves that lit markers stay lit.
reset_screen cumulative_entry
measure s_up    up    "up"
measure s_down  down  "up,down"
measure s_left  left  "up,down,left"
measure s_right right "up,down,left,right"
# One input, two buttons: center and KEY2 produce the same event and light both markers.
measure s_click click "up,down,left,right,click,key2"
measure s_first first "up,down,left,right,click,key2,key1"

# KEY3: return to the dashboard as required by task 47. The frame must match the dashboard anchor byte for byte.
$M btn:alt shot:exit || report "42: KEY3 press did not change the display"
if cmp -s /probe/t42_dashboard.rgb565 /probe/t42_exit.rgb565; then
    echo "42: KEY3 exit: returned to dashboard (frame identical)"
else
    report "42: did not return to dashboard after KEY3"
fi

stop || exit 1

# Second section: flipped screen. Start clean with a separate settings file and flip through the menu.
echo "42: --- flipped screen ---"
rm -f /probe/sockT42F /probe/settingsT42F.a /probe/settingsT42F.b /probe/t42f_*.rgb565 \
    || { report "42: could not clear previous flipped measurement files"; exit 1; }
(nohup $D --socketfile /probe/sockT42F --settings /probe/settingsT42F --log-level info \
   > /probe/daemonT42F.log 2>&1 &)
for i in $(seq 1 40); do [ -S /probe/sockT42F ] && break; sleep 0.5; done
[ -S /probe/sockT42F ] || { report "42: control socket did not open: /probe/sockT42F"; exit 1; }
sleep 2

PREFIX=t42f
M="python3 $T/menu_audit.py /probe/sockT42F /probe/daemonT42F.log $PREFIX"

# Options > Display > Flip Orientation. Use `still:click` because flipping does not change the frame.
$M btn:right btn:right btn:click \
   btn:down btn:down btn:down btn:down btn:click \
   btn:down btn:down still:click \
  || report "42: could not enable flipping"

# From here the screen is flipped: every direction reverses. Two `btn:down` presses reach the
# header, also proving flipping is enabled; on an unflipped screen those presses would move down.
# Directions reverse on the dashboard too. Leave Display for the dashboard, capture its anchor,
# then walk the same route with reversed directions.
ENTRY="btn:click btn:up btn:up btn:up btn:up btn:up btn:up btn:click \
       btn:up btn:up btn:up btn:click btn:up btn:up btn:click"

# Use KEY3 to reach the dashboard: the unflipped section already measured this independently,
# so here it is just navigation. No need to recount the route from the flipped menu to the
# dashboard. The anchor is the flipped dashboard itself.
$M btn:alt shot:dashboard || report "42: could not reach dashboard on flipped screen"
$M $ENTRY shot:entry || report "42: could not reach Buttons on flipped screen"

python3 $T/t42_marks.py /probe/t42f_entry.rgb565 - || report "42: marker lit on flipped entry"

# Expectations use layout coordinates and are REVERSED: up must light `down`, left must light `right`.
isolated up down
isolated down up
isolated left right
isolated right left
isolated click "click,key2"
isolated first key1

reset_screen cumulative_entry
measure s_up    up    "down"
measure s_down  down  "down,up"
measure s_left  left  "down,up,right"
measure s_right right "down,up,right,left"
measure s_click click "down,up,right,left,click,key2"
measure s_first first "down,up,right,left,click,key2,key1"

$M btn:alt shot:exit || report "42: on flipped screen: KEY3 press did not change the display"
if cmp -s /probe/t42f_dashboard.rgb565 /probe/t42f_exit.rgb565; then
    echo "42: flipped KEY3 exit: returned to dashboard (frame identical)"
else
    report "42: on flipped screen: did not return to dashboard after KEY3"
fi

stop || exit 1
if [ $ERROR -eq 0 ]; then echo "42: measurement complete"; else echo "42: MEASUREMENT FAILED"; fi
exit $ERROR
