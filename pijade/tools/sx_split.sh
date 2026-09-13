#!/bin/bash
# BBB-AIRGAP: emulator measurement of the Seed XOR split path (2026-09-13).
# Run: docker exec jade-dev bash /jade/pijade/tools/sx_split.sh
#
# What this can measure and what it cannot, said up front.  A split shows freshly generated
# words and then quizzes them, and the quiz can only be answered by reading the words off the
# screen.  No measurement here reads them: the split is therefore driven as far as the screen
# BEFORE the words and no further, and the arithmetic - that the parts xor back to the seed -
# is measured in libjade/selfcheck/seedxor.c instead, where the vectors are public.
#
# The screens measured here carry no seed words at all:
#   S1 - the Backup list offers 'Split (SeedXOR)' as its fourth row.
#   S2 - the intro screen opens on Continue, as every other user-opened screen in this
#        firmware does; the three that open on 'back' are all host-initiated key exports
#        (main/process/get_bip85_entropy.c).
#   S3 - the part-count list turns each row into a different split: 'Part A of 2', 'of 3', 'of 4'.
#        This is the mapping the static assert in split_wallet_seedxor() is about.
#   S4 - leaving at the 'Part A of N' heading, BEFORE any words are drawn, is an ordinary cancel:
#        it returns to the Backup list and says nothing about destroying anything.
#   S5 - leaving one screen later, once the split has been made and the words are about to be
#        shown, DOES say it: 'Split abandoned / Destroy what you wrote'.  S4 and S5 are the two
#        sides of the words_reached_screen flag, and they are what this run exists for.
set -u
D=/jade/build_linux_nci_log/libjade/libjade_daemon
T=/jade/pijade/tools
ERROR=0
# Public bip39 test vector; not a real wallet.  Its words are the only ones this file names.
TEST_VECTOR="abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"

report() { echo "ERROR: $*" >&2; ERROR=1; }

frames_exist() { # frames_exist <name> <frame>...
    local name=$1 d
    shift
    for d in "$@"; do
        [ -s "$d" ] && continue
        report "$name (frame missing or empty: $d)"
        return 1
    done
    return 0
}
equal() { # equal <name> <frame-a> <frame-b>
    frames_exist "$@" || return
    if cmp -s "$2" "$3"; then echo "SPLIT PASS: $1"; else report "$1"; fi
}
different() { # different <name> <frame-a> <frame-b>
    frames_exist "$@" || return
    if cmp -s "$2" "$3"; then report "$1"; else echo "SPLIT PASS: $1"; fi
}

pkill -f "^$D"; sleep 1
rm -f /probe/sockSPL /probe/settingsSPL.a /probe/settingsSPL.b /probe/spl_*.rgb565
(nohup $D --socketfile /probe/sockSPL --settings /probe/settingsSPL --log-level info \
    > /probe/daemonSPL.log 2>&1 &)
for i in $(seq 1 60); do [ -S /probe/sockSPL ] && break; sleep 0.5; done
[ -S /probe/sockSPL ] || { report "daemon socket did not open"; exit 1; }
sleep 2

M="python3 $T/menu_audit.py /probe/sockSPL /probe/daemonSPL.log spl"

# Session > the wallet > Backup.  'Backup' is the seventh row of the wallet menu and appears
# only for a wallet that still holds its entropy (main/process/dashboard.c), which a wallet put
# in with debug_set_mnemonic does.
$M "seed:$TEST_VECTOR" || { report "S1: wallet setup failed"; exit 1; }
$M btn:click btn:click btn:down btn:down btn:down btn:down btn:down btn:down btn:click \
    || { report "S1: could not reach the Backup list"; exit 1; }
# S1: the row itself.  This frame is also the anchor the two cancel arms have to return to,
# so it is taken with 'Split (SeedXOR)' selected - which is where the list leaves the selection
# after the split screens close.
$M btn:down btn:down btn:down shot:anchor || { report "S1: could not select the split row"; exit 1; }

# S2 and S3: the intro, then each part count in turn.  Re-entering is three presses because the
# list keeps its selection, so 'Split (SeedXOR)' is still the row under the cursor.
$M btn:click shot:intro || report "S2: could not open the intro screen"
$M btn:click shot:parts || report "S3: could not reach the part-count list"
$M btn:click shot:head2 || report "S3: could not reach the 2-part heading"
$M btn:down btn:click shot:back2 || report "S4: could not leave at the heading"
# S4: leaving before any words were drawn returns to the list and shows nothing else.
equal "S4 leaving at the heading is an ordinary cancel (no 'destroy' screen)" \
    /probe/spl_anchor.rgb565 /probe/spl_back2.rgb565

$M btn:click btn:click btn:down btn:click shot:head3 || report "S3: could not reach the 3-part heading"
$M btn:down btn:click || report "S3: could not leave the 3-part heading"
$M btn:click btn:click btn:down btn:down btn:click shot:head4 || report "S3: could not reach the 4-part heading"
$M btn:down btn:click || report "S3: could not leave the 4-part heading"
different "S3 two parts and three parts are different splits" /probe/spl_head2.rgb565 /probe/spl_head3.rgb565
different "S3 three parts and four parts are different splits" /probe/spl_head3.rgb565 /probe/spl_head4.rgb565
different "S3 two parts and four parts are different splits" /probe/spl_head2.rgb565 /probe/spl_head4.rgb565

# S5: one screen further on.  Continue at the heading makes the split and opens the warning
# banner that precedes the words (display_confirm_mnemonic, main/process/mnemonic.c); the words
# themselves are never drawn here, and the banner carries none.
$M btn:click btn:click btn:click shot:head2b || report "S5: could not reach the heading again"
equal "S5 the heading reached again is the same screen" /probe/spl_head2.rgb565 /probe/spl_head2b.rgb565
$M btn:click shot:banner || report "S5: Continue did not open the warning banner"
$M btn:down btn:click shot:abandoned || report "S5: could not leave at the banner"
different "S5 leaving once the split is made does NOT return quietly" \
    /probe/spl_anchor.rgb565 /probe/spl_abandoned.rgb565
$M btn:click shot:dismissed || report "S5: could not dismiss the abandoned screen"
equal "S5 dismissing the abandoned screen returns to the Backup list" \
    /probe/spl_anchor.rgb565 /probe/spl_dismissed.rgb565

grep -iE "error|assert|abort" /probe/daemonSPL.log | head -5
pkill -f "^$D"
[ "$ERROR" -eq 0 ] && echo "SPLIT: all measurements passed"
exit $ERROR
