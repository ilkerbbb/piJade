#!/bin/bash
# BBB-AIRGAP: emulator measurement for item 47 (2026-09-08).
#   47 - KEY3 returns to the dashboard from every screen (escape flag, main/gui.h gui_escape_request()).
#
# Run: docker exec jade-dev bash /jade/pijade/tools/t47_escape.sh
#
# Every positive case uses the same criterion: after KEY3, is the frame byte-identical
# to the dashboard frame captured BEFORE entering the screen? "Screen changed" is
# insufficient; it can change without escape reaching the dashboard.
#
# Negative controls verify escape does not trigger in the WRONG place. Without them,
# positive measurements could pass a bug where every key exits every screen:
#   N1 - KEY3 does NOT CHANGE the recovery-word entry screen (exempt from escape,
#        main/process/mnemonic.c enter_word_activity: one press must not lose a partial word).
#   N5 - On the REAL keyboard (main/ui/keyboard.c make_keyboard_screen), KEY3 remains SHIFT:
#        the screen changes but does NOT EXIT; four presses cycle through four keyboards
#        and return to the start. N1 uses a separate word-entry screen without shift.
#        Ilker's first constraint.
#   N6 - KEY3 produces NO SIGNATURE on signing confirmation (pijade/tools/t47_signature.py).
#        The criterion is the RPC response, not the frame: require CBOR_RPC_USER_CANCELLED;
#        a response with 'result' fails. Ilker's second constraint; a frame alone cannot
#        prove that no signature was produced.
#   N7 - At both gates of factory reset, the device's MOST DESTRUCTIVE action, KEY3 neither
#        confirms nor silently cancels. The second gate is a DIGIT_ENTRY_PIN confirmation-code
#        screen (main/process/dashboard.c:697), the strictest instance of "PIN entry" in
#        Ilker's second constraint. Three criteria: reached dashboard, card file UNCHANGED,
#        and cancellation logged (dashboard.c:704), making cancellation visible rather than dropped.
#   N8 - On a DIMMED screen, the first KEY3 only wakes; escape starts on the second press.
#        A key pressed on a screen the user CANNOT SEE must not act. KEY3 behaves like
#        the other seven keys (gui_alt_click() first calls idletimer_register_activity(true)
#        and returns early on a dimmed screen).
#   N9 - KEY3 returns to the dashboard from SEED PAGES. This screen waited on only two
#        GUI_BUTTON_EVENTs, but KEY3 arrives as GUI_EVENT, so escape was dead precisely
#        where recovery words were visible (comprehensive handoff finding, 2026-09-08).
#        The UNINITIALIZED dashboard anchor proves both return to the dashboard and no
#        wallet setup. Do NOT inspect frame contents; words only enter byte comparisons.
#   N10 - In the I/O test camera, ONE KEY3 press returns to the dashboard. Escape formerly
#         opened "Frames seen" after the camera, requiring a second press to reach home.
#   N11-N13 - Measure the ESCAPE DECISION ITSELF (pijade/tools/t47_destructive.py). In the closing
#         round, three decisions began reading the PRESS that closed the screen rather than
#         the flag (await_message_escaped(), main/ui/dialogs.c). No t47 measurement exercised
#         that branch, since each path here closed the screen with "Continue". Use N6's RPC
#         response criterion: N11 does NOT SET UP the debug wallet, N12 does NOT RUN debug wipe,
#         N13 does NOT GIVE THE HOST an address. Each has a positive control using Continue
#         to actually perform the action. Mutation testing also proved the checks were not
#         blind (2026-09-08: disabling the decision made all three fail while positive controls passed).
#   N2 - KEY3 never means "yes" on a yes/no screen: escape from Advanced Setup returns to
#        the dashboard, not forward (main/ui/dialogs.c await_yesno_activity_loop escape_result).
#   N3 - A direction key stays in the camera, so escape is specific to KEY3.
#   N4 - The dashboard clears the flag: normal reentry immediately after escape is possible
#        without a ghost escape.
#
# The deepest path (Options > Info > I/O Test > Buttons) is measured in t42_buttons.sh,
# not here; its KEY3 criterion was updated with item 47 to require dashboard return.

set -u
D=/jade/build_linux_nci_log/libjade/libjade_daemon
T=/jade/pijade/tools
ERROR=0
# Public bip39 test vector; not a real wallet.
TEST_VECTOR="abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
report() { echo "ERROR: $*" >&2; ERROR=1; }

stop() {
    local i rc
    pkill -f "^$D"; rc=$?
    if [ "$rc" -gt 1 ]; then report "47: could not stop process: pkill exit $rc"; return 1; fi
    for ((i = 0; i < 40; i++)); do
        pgrep -f "^$D" >/dev/null; rc=$?
        case $rc in
            0) sleep 0.3 ;;
            1) return 0 ;;
            *) report "47: could not read process status: pgrep exit $rc"; return 1 ;;
        esac
    done
    report "47: process still alive after 40 polls"; return 1
}

start() { # start <suffix>
    stop || exit 1
    rm -f "/probe/sock47$1" "/probe/settings47$1.a" "/probe/settings47$1.b" \
        || { report "47: could not remove previous measurement files"; exit 1; }
    (nohup $D --socketfile "/probe/sock47$1" --settings "/probe/settings47$1" --log-level info \
        > "/probe/daemon47$1.log" 2>&1 &)
    local i
    for i in $(seq 1 40); do [ -S "/probe/sock47$1" ] && break; sleep 0.5; done
    [ -S "/probe/sock47$1" ] || { report "47: daemon socket did not open: /probe/sock47$1"; exit 1; }
    sleep 2
}

# Both comparison helpers FIRST verify both files exist. This matters: `cmp -s`
# returns nonzero for a missing file, so `different` could report PASS even when no
# frames were produced. Absence is not evidence of difference.
frames_exist() { # frames_exist <name> <frame-a> <frame-b>
    local d
    for d in "$2" "$3"; do
        [ -s "$d" ] && continue
        report "47: $1 (frame missing or empty: $d)"
        return 1
    done
    return 0
}
equal() { # equal <name> <frame-a> <frame-b>
    frames_exist "$@" || return
    if cmp -s "$2" "$3"; then echo "47 PASS: $1"; else report "47: $1"; fi
}
different() { # different <name> <frame-a> <frame-b>
    frames_exist "$@" || return
    if cmp -s "$2" "$3"; then report "47: $1"; else echo "47 PASS: $1"; fi
}

# ---------------------------------------------------------------- P1: camera screen
start A
rm -f /probe/k47a_*.rgb565
M="python3 $T/menu_audit.py /probe/sock47A /probe/daemon47A.log k47a"
J="python3 $T/jadectl.py /probe/sock47A"

# Anchor: dashboard with "Scan SeedQR" selected. Escape must return to exactly this frame.
$M btn:right shot:anchor || report "P1: could not capture dashboard anchor"
$M btn:click wait:1.0 shot:camera || report "P1: could not enter camera screen"

# Press while frames are STREAMING: escape must work while the panel is continuously updated.
( $J flatcam:120:60 >/dev/null 2>&1 ) & FEED=$!
sleep 1
# Positive control: did the feed actually produce frames? If flatcam silently failed
# (its output is suppressed and its exit status is not checked), the camera could
# keep returning a stale frame, KEY3 would still exit, and the comparison below would
# pass without proving escape while streaming. Compare against the unfed camera frame
# (measured 2026-09-08: the two frames differ).
$J shot:k47a_streaming >/dev/null 2>&1
different "P1 camera feed actually streamed (measurement baseline established)" \
    /probe/k47a_camera.rgb565 /probe/k47a_streaming.rgb565
$M btn:alt wait:1.5 shot:exit || report "P1: KEY3 did not change screen"
kill $FEED 2>/dev/null; wait $FEED 2>/dev/null
equal "P1 returned to dashboard from camera screen" /probe/k47a_anchor.rgb565 /probe/k47a_exit.rgb565

# N4: escape did not leave the device stuck; the same screen can be entered again.
#
# This assertion is deliberately narrow. It does NOT measure the dashboard's OWN
# gui_escape_clear() (main/process/dashboard.c:3760): the intervening btn:click already
# clears the flag (gui_front_click(), main/gui.c), so this comparison would pass even
# without dashboard cleanup. It does measure successful reentry without immediately
# bouncing back; a stuck flag would close the new screen as soon as it opened.
#
# Dashboard cleanup is visible only to screens opened WITHOUT A PRESS, since every
# physical press clears the flag (seven handlers in gui.c). Available RPC paths were
# examined (sign_message, debug_set_mnemonic, register_otp, update_pinserver): none tests
# the flag before drawing, so none can distinguish this. The only RPC path testing it
# is register_multisig -> show_multisig_activity (main/ui/multisig.c:350); constructing
# a valid multisig payload is separate work and was not measured.
$M btn:click wait:1.0 shot:camera2 || report "N4: could not enter camera a second time"
different "N4 camera reentry succeeded after escape (screen does not bounce back)" \
    /probe/k47a_anchor.rgb565 /probe/k47a_camera2.rgb565

# N3: a direction key does not trigger escape.
( $J flatcam:120:60 >/dev/null 2>&1 ) & FEED=$!
sleep 1
$J btn:right >/dev/null 2>&1; sleep 1
$J shot:k47a_direction >/dev/null 2>&1
kill $FEED 2>/dev/null; wait $FEED 2>/dev/null
different "N3 direction key stays in camera" /probe/k47a_anchor.rgb565 /probe/k47a_direction.rgb565

# ---------------------------------------------------------------- P2/P3: settings screens
start B
rm -f /probe/k47b_*.rgb565
M="python3 $T/menu_audit.py /probe/sock47B /probe/daemon47B.log k47b"

# Dashboard anchor with Options selected; Options list and Display menu (two levels).
$M btn:right btn:right shot:anchor || report "P2: could not capture dashboard anchor"
$M btn:click btn:down btn:down btn:down btn:down btn:click shot:display \
    || report "P2: could not reach Display menu"
$M btn:alt shot:p2 || report "P2: KEY3 did not change screen"
equal "P2 returned to dashboard from Display menu (two levels)" /probe/k47b_anchor.rgb565 /probe/k47b_p2.rgb565

# Display > Brightness wheel. ACTUALLY change the value before escaping.
# Escaping unchanged proves nothing; an implementation that accidentally SAVES the
# current value would pass too. The wheel opens at Max(5), so move left (right is
# inert at the ceiling, measured 2026-09-08). Three criteria: turning changes the frame,
# escape reaches the dashboard, and reopening the wheel shows the OLD value.
$M btn:click btn:down btn:down btn:down btn:down btn:click btn:down btn:click shot:setting \
    || report "P3: could not reach brightness wheel"
$M btn:left shot:setting_changed || report "P3: left press did not turn wheel"
different "P3 wheel value actually changed (measurement is meaningful)" \
    /probe/k47b_setting.rgb565 /probe/k47b_setting_changed.rgb565
$M btn:alt shot:p3 || report "P3: KEY3 did not change screen"
equal "P3 returned to dashboard from settings screen" /probe/k47b_anchor.rgb565 /probe/k47b_p3.rgb565
$M btn:click btn:down btn:down btn:down btn:down btn:click shot:display2 \
    || report "P3: could not return to Display menu"
equal "P3 escape did not change Display menu" \
    /probe/k47b_display.rgb565 /probe/k47b_display2.rgb565
$M btn:down btn:click shot:setting2 || report "P3: could not reopen brightness wheel"
equal "P3 escape DID NOT SAVE changed value (wheel opened with old value)" \
    /probe/k47b_setting.rgb565 /probe/k47b_setting2.rgb565
different "P3 saved value is NOT the changed value" \
    /probe/k47b_setting_changed.rgb565 /probe/k47b_setting2.rgb565

# ---------------------------------------------------------------- N1/N2: setup flow
start C
rm -f /probe/k47c_*.rgb565
M="python3 $T/menu_audit.py /probe/sock47C /probe/daemon47C.log k47c"

# N2: Advanced Setup question. Escape must return to the dashboard; treating it as yes would advance setup.
$M shot:anchor || report "N2: could not capture dashboard anchor"
$M btn:click btn:click btn:down btn:click shot:question || report "N2: could not reach Advanced Setup question"
$M btn:alt shot:n2 || report "N2: KEY3 did not change screen"
equal "N2 escape from yes/no screen did not advance, returned to dashboard" \
    /probe/k47c_anchor.rgb565 /probe/k47c_n2.rgb565

# N1: recovery-word entry screen (NOT the keyboard; separate screen, main/process/mnemonic.c).
# The criterion is UNCHANGED screen; still: enforces it. The real keyboard is measured in N5.
$M btn:click btn:click btn:click btn:down btn:click btn:down btn:click shot:keyboard \
    || report "N1: could not reach word-entry keyboard"
$M still:alt || report "N1: KEY3 changed keyboard screen"
$M shot:keyboard2 || report "N1: could not capture keyboard frame"
equal "N1 KEY3 does not exit word-entry screen" /probe/k47c_keyboard.rgb565 /probe/k47c_keyboard2.rgb565

# ------------------------------------------------------- N5: preserve shift on the real keyboard
# Measured path (2026-09-08): dashboard > Options > OTP > New OTP Record > Enter URI.
# A wallet is required, so load a public test vector. The keyboard has four pages
# (lowercase, uppercase, digits/symbols, remaining symbols); KEY3 is a four-step cycle, not a toggle.
start D
rm -f /probe/k47d_*.rgb565
M="python3 $T/menu_audit.py /probe/sock47D /probe/daemon47D.log k47d"

$M "seed:$TEST_VECTOR" shot:dashboard || report "N5: wallet setup failed"
$M btn:right btn:right btn:click btn:down btn:click \
   btn:down btn:down btn:down btn:click btn:down btn:down btn:click shot:kb0 \
    || report "N5: could not reach OTP name keyboard"

# First assertion: KEY3 CHANGES the screen (shift works) but does NOT reach the dashboard (no exit).
$M btn:alt shot:kb1 || report "N5: KEY3 did nothing on keyboard"
different "N5 KEY3 changed keyboard (shift)" /probe/k47d_kb0.rgb565 /probe/k47d_kb1.rgb565
different "N5 KEY3 did not exit keyboard (not dashboard)" /probe/k47d_dashboard.rgb565 /probe/k47d_kb1.rgb565

# Second assertion: four presses close the cycle AND the four intermediate pages differ.
# Returning on the fourth press alone is insufficient: a broken two-page shift would
# also return then. Compare all four frames pairwise.
$M btn:alt shot:kb2 btn:alt shot:kb3 btn:alt shot:kb4 || report "N5: could not complete keyboard cycle"
equal "N5 four presses cycled through four keyboards and returned to start" /probe/k47d_kb0.rgb565 /probe/k47d_kb4.rgb565
for a in 0 1 2; do
    for b in 1 2 3; do
        [ "$a" -lt "$b" ] || continue
        different "N5 keyboard pages $a and $b differ" \
            "/probe/k47d_kb$a.rgb565" "/probe/k47d_kb$b.rgb565"
    done
done

# --------------------------------------------------------- N6: no signature on signing confirmation
start E
python3 $T/t47_signature.py /probe/sock47E /probe/daemon47E.log k47e || report "N6: signing check"

# ------------------------- N11-N13: escape decision itself (destructive and outward-facing paths)
# Use a separate daemon: N11 starts UNINITIALIZED and N12 actually erases the wallet.
# These state transitions must not share another measurement's anchor.
start K
rm -f /probe/k47k_*.rgb565
python3 $T/t47_destructive.py /probe/sock47K /probe/daemon47K.log k47k || report "N11-N13: destructive path check"

# ------------------------------------------ N7: most destructive action, factory reset
# Factory reset has two gates: the yes/no question "Reset Jade and erase all PIN and wallet data?
# This cannot be undone!", then entry of a randomly generated confirmation code on
# DIGIT_ENTRY_PIN (main/process/dashboard.c:690-704).
# Measure with a SET-UP WALLET: a real reset would change the card file, making
# "card unchanged" meaningful. On an uninitialized device it would prove nothing.
start F
rm -f /probe/k47f_*.rgb565
M="python3 $T/menu_audit.py /probe/sock47F /probe/daemon47F.log k47f"
card_hash() { cat /probe/settings47F.a /probe/settings47F.b 2>/dev/null | md5sum | cut -d' ' -f1; }

CARD_EMPTY=$(card_hash)
$M "seed:$TEST_VECTOR" || report "N7: wallet setup failed"
CARD_SEEDED=$(card_hash)
# Positive control against a BLIND criterion: does the hash actually track card writes?
# Without it, "hash unchanged" could pass in a world where the hash never changes,
# leaving the measurement permanently green.
[ "$CARD_EMPTY" = "$CARD_SEEDED" ] && report "47: N7 card hash did not detect seeding (criterion is blind)"
{ [ -s /probe/settings47F.a ] && [ -s /probe/settings47F.b ]; } || report "47: N7 card files are empty"

# Capture the anchor in the state escape should reach: after seeding, the dashboard
# opens on Session; Options is two right presses away (measured 2026-09-08).
$M btn:right btn:right shot:anchor || report "N7: could not capture dashboard anchor"

# Factory Reset is eight down presses into Options; the uninitialized device needs
# seven, but wallet setup changes that count (measured 2026-09-08), so it is fixed here.
MENU="btn:click btn:down btn:down btn:down btn:down btn:down btn:down btn:down btn:down btn:click"

# First gate. Default is "No"; treating escape as "Yes" would open confirmation-code entry.
$M $MENU shot:question || report "N7: could not reach factory reset question"
$M btn:alt shot:n7a || report "N7: KEY3 did not change question screen"
equal "N7 escape from factory reset question did not advance, returned to dashboard" \
    /probe/k47f_anchor.rgb565 /probe/k47f_n7a.rgb565

# Second gate. The random code cannot be entered correctly here, nor does the test
# require that. ACTUALLY enter one digit (change and accept its value), because
# escaping an empty screen does not measure what happens to partial input.
LINES_BEFORE=$(wc -l < /probe/daemon47F.log)
CARD_BEFORE=$(card_hash)
$M $MENU btn:right btn:click shot:code || report "N7: could not reach confirmation-code screen"
$M btn:up btn:click shot:code2 || report "N7: could not enter digit"
$M btn:alt shot:n7b || report "N7: KEY3 did not change code screen"
equal "N7 escape from confirmation-code screen returned to dashboard" /probe/k47f_anchor.rgb565 /probe/k47f_n7b.rgb565

CARD_AFTER=$(card_hash)
if [ "$CARD_BEFORE" = "$CARD_AFTER" ]; then
    echo "47 PASS: N7 escape did not change card (reset did not run)"
else
    report "47: N7 card changed after escape"
fi
# Cancellation must be observed and logged, not silently dropped (dashboard.c:704).
if tail -n +$((LINES_BEFORE + 1)) /probe/daemon47F.log | grep -q "not wiping data"; then
    echo "47 PASS: N7 cancellation was logged (no silent drop)"
else
    report "47: N7 cancellation log line missing"
fi

# ------------------------------------------------- N8: first press on a dimmed screen
# An idle screen dims (main/idletimer.c:289-292). A press on a screen the user CANNOT SEE
# must not act; gui_alt_click() first calls idletimer_register_activity(true) and returns
# early, behaving like the other seven keys. Verify that the first press only wakes
# and the second escapes. This section sets the shortest threshold (30 seconds) and
# waits, making it the slowest part; real dimming requires a real timeout.
start G
rm -f /probe/k47g_*.rgb565
M="python3 $T/menu_audit.py /probe/sock47G /probe/daemon47G.log k47g"

$M btn:right btn:right shot:anchor || report "N8: could not capture dashboard anchor"
# Options > Preferences > Screen Timeout. Preferences is two down presses; Screen Timeout
# is the second row. The scrolling list opens with its first row selected (measured 2026-09-08).
$M btn:click btn:down btn:down btn:click btn:down btn:click shot:carousel \
    || report "N8: could not reach Screen Timeout wheel"
# Wheel opens at the default 60; one left press reaches the shortest value, 30 seconds.
$M btn:left shot:thirty || report "N8: could not turn wheel"
$M btn:click shot:screen || report "N8: could not save threshold"

LINES_BEFORE=$(wc -l < /probe/daemon47G.log)
sleep 35
if tail -n +$((LINES_BEFORE + 1)) /probe/daemon47G.log | grep -q "dimming screen"; then
    echo "47 PASS: N8 screen actually dimmed (measurement baseline established)"
else
    # Without dimming, the two assertions below measure nothing. Continue here,
    # but record a finding.
    report "47: N8 screen did not dim, dimming measurement has no baseline"
fi

# First press: screen must NOT CHANGE. still: enforces this as an inverse assertion.
$M still:alt || report "N8: first KEY3 changed dimmed screen"
if tail -n +$((LINES_BEFORE + 1)) /probe/daemon47G.log | grep -q "powering screen"; then
    echo "47 PASS: N8 first press only woke screen (did not start escape)"
else
    report "47: N8 first press did not wake screen"
fi

# Second press: the screen is now on, so escape should work normally.
$M btn:alt shot:n8 || report "N8: second KEY3 did not change screen"
equal "N8 second press returned to dashboard" /probe/k47g_anchor.rgb565 /probe/k47g_n8.rgb565

# N9 and N10: two gaps found by the comprehensive handoff (2026-09-08), missed by the
# previous 29 checks. Both use uninitialized devices but separate daemons:
# run_list_activity preserves selection across visits, so one check could shift the other's path.

# N9: KEY3 on seed pages. This screen waited on only two GUI_BUTTON_EVENTs
# (main/process/mnemonic.c await_mnemonic_pages_exit); KEY3 arrives as GUI_EVENT and
# never woke it, disabling escape precisely while recovery words were displayed.
# Two-part criterion: return to the dashboard AND leave the card unchanged, without completing setup.
start H
rm -f /probe/k47h_*.rgb565
M="python3 $T/menu_audit.py /probe/sock47H /probe/daemon47H.log k47h"

$M shot:anchor || report "N9: could not capture dashboard anchor"
# Five clicks: dashboard -> introduction -> Setup Type -> Setup Method -> warning -> word pages
# (measured 2026-09-08; basic setup has no word-count menu, main/ui/mnemonic.c:38-42).
$M btn:click btn:click btn:click btn:click btn:click shot:page || report "N9: could not reach word pages"
# Positive control: actually leave the dashboard. Without this, "frame matches anchor"
# would also pass a run that never advanced.
different "N9 word pages differ from dashboard (path actually traversed)" \
    /probe/k47h_anchor.rgb565 /probe/k47h_page.rgb565

$M btn:alt shot:n9 || report "N9: KEY3 did not change screen"

# The anchor is the UNINITIALIZED dashboard ("Set Up Jade", "Uninitialized"). Completing
# setup would show a fingerprint, so this comparison proves both dashboard return AND
# no wallet setup. No extra card hash is measured; it could be blind because neither
# case may have written a file.
equal "N9 escape from seed pages reached uninitialized dashboard (wallet not set up)" \
    /probe/k47h_anchor.rgb565 /probe/k47h_n9.rgb565
# Second positive control, the decisive one. A frame differing from the dashboard
# proves departure, not that five clicks ended on seed pages. If menu counts drift,
# the same five clicks could stop at a warning banner; KEY3 would still escape and
# match the dashboard, letting the protected bug return while the check stayed green.
# Only seed dependence distinguishes these cases: a second traversal generates a NEW
# seed, so the two frames must differ. A menu or banner would remain byte-identical
# (measured 2026-09-08).
$M btn:click btn:click btn:click btn:click btn:click shot:page2 \
    || report "N9: could not reach word pages a second time"
different "N9 second traversal produced a different seed (destination really is seed page)" \
    /probe/k47h_page.rgb565 /probe/k47h_page2.rgb565
$M btn:alt || report "N9: could not exit second traversal"

# N10: KEY3 in the I/O test camera. Escape formerly left the camera but immediately
# opened "Frames seen" (main/process/dashboard.c handle_io_test_camera), requiring
# a second press to reach the dashboard. Criterion: dashboard in ONE press.
start I
rm -f /probe/k47i_*.rgb565
M="python3 $T/menu_audit.py /probe/sock47I /probe/daemon47I.log k47i"

$M btn:right btn:right shot:anchor || report "N10: could not capture dashboard anchor"
$M btn:click btn:down btn:down btn:down btn:down btn:down btn:down btn:click shot:info \
    || report "N10: could not reach Info menu"
$M btn:down btn:down btn:down btn:click shot:iotest || report "N10: could not reach I/O Test menu"
# I/O Test rows: Screen, Buttons, Camera (main/ui/dashboard.c:568-580); three down presses reach Camera.
$M btn:down btn:down btn:down btn:click shot:camera || report "N10: could not open camera"
# Positive control: the camera actually ran. Frame comparison alone could pass
# even if the camera had never been entered.
if grep -q "Camera init done" /probe/daemon47I.log; then
    echo "47 PASS: N10 camera actually opened (measurement baseline established)"
else
    report "47: N10 camera did not open, measurement has no baseline"
fi
different "N10 camera screen differs from dashboard" /probe/k47i_anchor.rgb565 /probe/k47i_camera.rgb565

$M btn:alt shot:n10 || report "N10: KEY3 did not change screen"
equal "N10 returned from camera to dashboard in one press (no intermediate result screen)" \
    /probe/k47i_anchor.rgb565 /probe/k47i_n10.rgb565
if grep -q "Camera task complete" /probe/daemon47I.log; then
    echo "47 PASS: N10 camera task closed properly"
else
    report "47: N10 camera task did not close"
fi

echo "--- errors in logs ---"
grep -ihE "assert|abort| error" /probe/daemon47A.log /probe/daemon47B.log /probe/daemon47C.log \
    /probe/daemon47D.log /probe/daemon47E.log /probe/daemon47F.log /probe/daemon47G.log \
    /probe/daemon47H.log /probe/daemon47I.log \
    | head -5 || true

stop || exit 1
if [ $ERROR -eq 0 ]; then echo "47: measurement complete"; else echo "47: MEASUREMENT FAILED"; fi
exit $ERROR
