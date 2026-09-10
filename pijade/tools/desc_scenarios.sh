#!/bin/bash
# BBB-AIRGAP: descriptor QR import and sealed-registration scenarios (S1-S13 + follow-up measurements).
#
# Run: docker exec jade-dev bash /probe/desc_scenarios.sh
# Requires: build_linux_nci_log daemon (CI disabled, real presses, LOG enabled, CAMERA enabled),
#           Task 5 frames (/probe/desc_*.gray, /probe/msfile*.gray),
#           Task 0 old-format settings file (/probe/settingsR8old.a and .b).
#
# This script ran on 2026-09-06; the four measurements below shaped its current form.
# Each corrected an assumption that silently misdirected the previous round. Comments
# preserve the reason for each correction to avoid repeating the same trap.

D=/jade/build_linux_nci_log/libjade/libjade_daemon; L=/probe/daemonR8.log
stop() { pkill -f "^$D"; for i in $(seq 1 40); do pgrep -f "^$D" >/dev/null || break; sleep 0.3; done; }
start() { stop; rm -f /probe/sockR8; (nohup $D --socketfile /probe/sockR8 --settings /probe/$1 --log-level info > $L 2>&1 &); for i in $(seq 1 40); do [ -S /probe/sockR8 ] && break; sleep 0.5; done; sleep 2; }
J="python3 /jade/pijade/tools/jadectl.py /probe/sockR8"; M="python3 /jade/pijade/tools/menu_audit.py /probe/sockR8 $L"

# load: load the wallet and refresh the home-screen signature. The signature depends
# on the seed (fingerprint in the bottom strip), so recapture it on every load.
# Session is the selected tile after loading.
load() { $J btn:right wait:0.5 btn:click wait:1.5; $J camfile:/probe/$1:12; for i in $(seq 1 60); do [ "$(grep -c "showing home screen/Active" $L)" -ge "$2" ] && break; sleep 0.5; done; $J wait:1 shot:$3; $J shot:homesig; }

# ishome: capture the current frame and determine whether it is home. Compare the TOP BAND.
# The home-screen tile strip scrolls horizontally: returning from scanning selects Scan QR
# rather than Session, so full-frame comparison misses home. Measured 2026-09-06:
# tile changes first differ at byte 43703 (row 91), while every non-home screen differs
# from byte 1; a 40000-byte prefix is therefore a reliable discriminator.
# The old criterion (dashboard.c:3666 "showing home screen/Active" counter) is INVALID:
# that line is written only on keychain changes, never on return from scanning.
ishome() { $J shot:$1 >/dev/null; cmp -s -n 40000 /probe/homesig.rgb565 /probe/$1.rgb565; }

# gosession: return to the Session tile on the home screen. btn:first (gui_select_first)
# has no effect there (measured); the tile strip is a carousel, not a menu.
# Pressing left is the only reliable route.
gosession() { for i in 1 2 3 4 5 6; do cmp -s /probe/homesig.rgb565 /probe/$1.rgb565 && return 0; $J btn:left wait:0.4 shot:$1 >/dev/null; done; echo "NO_SESSION_$1"; return 1; }

waitchange() { for i in $(seq 1 40); do $J shot:$2 >/dev/null; cmp -s /probe/$1.rgb565 /probe/$2.rgb565 || return 0; sleep 0.5; done; echo "NO_CHANGE_$2"; }

# scan: normalize home to Session, enter the camera, push the frame, catch the first
# screen change, and confirm until home returns.
# Normalization is required: the previous scan leaves Scan QR selected, and btn:right
# from there goes to Options (the cause of drift after S2).
# btn:first in the confirmation loop is also required: if back is selected, btn:right
# moves to confirmation; if confirmation is selected, it wraps to back. The former
# writes the registration; the latter cancels. btn:first is inert at home, so home
# detection remains valid.
scan() { local ok=0; if ishome ${2}_pre; then gosession ${2}_pre; else echo "PRE_NOT_HOME_$2"; fi; $J btn:right wait:0.5 btn:click wait:1.5 shot:${2}_cam; if [ -n "$3" ]; then $J camfiles:/probe/$1:$3; else $J camfile:/probe/$1:12; fi; $J wait:2; waitchange ${2}_cam ${2}_r1; for i in 1 2 3 4 5 6 7 8; do $J btn:first wait:0.4 >/dev/null; if ishome ${2}_p$i; then ok=1; break; fi; $J shot:${2}_r$i wait:0.2 btn:right wait:0.3 btn:click wait:1.5; done; [ "$ok" = 1 ] || echo "NOT_HOME_$2"; $J shot:${2}_after; }

# Registration carousel: Session > wallet menu > four down presses = Registered Wallets (measured).
carousel() { if ishome ${1}_pre; then gosession ${1}_pre; else echo "PRE_NOT_HOME_$1"; fi; $M $1 btn:click shot:${1}_session btn:click shot:${1}_wmenu btn:down btn:down btn:down btn:down shot:${1}_regrow btn:click shot:${1}_car1 btn:right shot:${1}_car2 btn:right shot:${1}_car3 btn:right shot:${1}_car4 || echo NAV_FAIL_$1; }

# --- S1-S6, S12, S13: QR import ---------------------------------------------------------------
rm -f /probe/settingsR8.a /probe/settingsR8.b
start settingsR8; load mne.gray 1 s0_home
scan desc_text.gray s1              # S1  plain-text descriptor
scan desc_specter.gray s2           # S2  Specter JSON
scan desc_ur_single.gray s3         # S3  single-part UR (same name -> identical)
scan desc_ur "s4" 3                 # S4  multipart UR
scan desc_ur_children.gray s5       # S5  UR children <0;1>/*
scan desc_ur_bad.gray s6            # S6  unsupported descriptor
scan msfile.gray s12                # S12 multisig file
scan msfile_ur "s13" 3              # S13 multisig, multipart UR:BYTES
carousel s6c

# --- S7: card privacy ---------------------------------------------------------------------
# The settings store alternates between two slots (measured 2026-09-06: .a 485 bytes / .b 878
# bytes in one round, second registration only in .b). Absence of leakage is valid only
# when BOTH slots are clean; inspecting one slot falsely suggests there is no registration.
echo "=== S7 card file (new vs old) ==="
for f in /probe/settingsR8.a /probe/settingsR8.b /probe/settingsR8old.a /probe/settingsR8old.b; do [ -f "$f" ] || continue; echo "$f: bytes $(stat -c %s $f) | xpub6 $(strings -n 6 $f | grep -c 'xpub6') | wsh $(strings -n 4 $f | grep -c 'wsh(') | fp $(strings -n 8 $f | grep -ci '73c5da0a\|3442193e') | name $(strings -n 4 $f | grep -c 'desc-\|Strongbox\|Vault')"; done
echo "=== S7 binary xpub search ==="
cd /jade/pijade/tools && python3 - <<'PYEOF'
from mk_registered import b58decode
xp = b58decode('xpub6DkFAXWQ2dHxq2vatrt9qyA3bXYU4ToWQwCHbf5XB2mSTexcHZCeKS1VZYcPoBd5X8yVcbXFHJR9R8UCVpt82VX1VhR28mCyxUFL4r6KFrf')[:-4]
def read(y):
    try:
        return open(y, 'rb').read()
    except FileNotFoundError:
        return b''
new = [read('/probe/settingsR8.a'), read('/probe/settingsR8.b')]
old = [read('/probe/settingsR8old.a'), read('/probe/settingsR8old.b')]
print('binary xpub in new slots:', any(d.find(xp) >= 0 for d in new),
      '| in old slots (positive control):', any(d.find(xp) >= 0 for d in old))
PYEOF
cd /jade
# S8 restart truncates $L; copy the S1-S13 log first
cp $L /probe/daemonR8_s1s13.log
echo "=== log S1-S13 ==="
grep -niE "identical registration|Detected|Unsupported|parsed as type" /probe/daemonR8_s1s13.log | cut -c1-150

# --- S8 persistence, S9 cross-wallet ------------------------------------------------------------
start settingsR8; load mne.gray 1 s8_home; carousel s8
start settingsR8; load mne24.gray 1 s9_home; carousel s9

# --- S10 old-format registrations ------------------------------------------------------------------
cp /probe/settingsR8old.a /probe/settingsR10.a; cp /probe/settingsR8old.b /probe/settingsR10.b
start settingsR10; load mne.gray 1 s10_home; carousel s10
echo "=== S10 log (old-format registration rejection) ==="
grep -niE "Bad version|unexpected length|not readable|HMAC" $L | tail -8 | cut -c1-150
echo "=== S10 before deletion ==="; cat /probe/settingsR10.a /probe/settingsR10.b | strings -n 4 | grep -c Vault
# The deletion sequence assumes home; restart because the carousel leaves the UI on its fourth frame.
# The registration menu opens without a highlighted row: three down presses for Details, Export, Delete.
start settingsR10; load mne.gray 1 s10d_home
$M s10d btn:click btn:click btn:down btn:down btn:down btn:down btn:click shot:s10d_car1 btn:click shot:s10d_menu btn:down btn:down btn:down shot:s10d_delrow btn:click shot:s10d_delq || echo NAV_FAIL_s10d
$J btn:right wait:0.5 shot:s10d_delyes btn:click wait:2 shot:s10d_after
echo "=== S10 after deletion ==="; cat /probe/settingsR10.a /probe/settingsR10.b | strings -n 4 | grep -c Vault

# S10e and S10f: Details and Export branches for an unreadable registration. Deletion was
# measured but these two were not; they were the last place a misleading-button fault could hide.
cp /probe/settingsR8old.a /probe/settingsR10e.a; cp /probe/settingsR8old.b /probe/settingsR10e.b
start settingsR10e; load mne.gray 1 s10e_home
$M s10e btn:click btn:click btn:down btn:down btn:down btn:down btn:click btn:click shot:s10e_menu btn:down shot:s10e_detrow btn:click shot:s10e_details || echo NAV_FAIL_s10e
$J wait:1 shot:s10e_det_after
cp /probe/settingsR8old.a /probe/settingsR10f.a; cp /probe/settingsR8old.b /probe/settingsR10f.b
start settingsR10f; load mne.gray 1 s10f_home
$M s10f btn:click btn:click btn:down btn:down btn:down btn:down btn:click btn:click shot:s10f_menu btn:down btn:down shot:s10f_exprow btn:click shot:s10f_export || echo NAV_FAIL_s10f
$J wait:1.5 shot:s10f_after
echo "=== S10e / S10f log ==="
grep -niE "unexpected length|assert|ABORT|Unable to export" $L | tail -6 | cut -c1-150
pgrep -f "^$D" >/dev/null && echo "DAEMON ALIVE (no assert)" || echo "DAEMON DEAD"

# --- S11 Address Explorer ----------------------------------------------------------------------
# One down press in the wallet menu reaches Address Explorer (measured). The question
# DEFAULTS to No; clicking blindly would never measure the registered-wallet branch.
start settingsR8; load mne.gray 1 s11_home
$M s11 btn:click btn:click btn:down shot:s11_explorer_row btn:click shot:s11_q || echo NAV_FAIL_s11
# S11b: Yes branch. After wallet selection, Receive is selected by default; click directly.
# btn:first would select the back arrow and leave the screen.
start settingsR8; load mne.gray 1 s11b_home
$M s11b btn:click btn:click btn:down btn:click shot:s11b_q btn:right shot:s11b_yes btn:click shot:s11b_pick1 btn:right shot:s11b_pick2 || echo NAV_FAIL_s11b
$J btn:click wait:1.5 shot:s11b_rcv btn:click wait:2.5 shot:s11b_addr1 btn:right wait:1.5 shot:s11b_addr2 btn:right wait:1.5 shot:s11b_addr3
echo "=== log S11 ==="; grep -niE "Descriptor .* depth|identical registration" $L | tail -12 | cut -c1-150

echo "=== final card state ==="
for f in /probe/settingsR8.a /probe/settingsR8.b; do [ -f "$f" ] && echo "$(basename $f) $(stat -c %s $f) bytes"; done
cat /probe/settingsR8.a /probe/settingsR8.b 2>/dev/null | strings -n 4 | grep -E 'desc-|Strongbox|Vault' | sort -u
echo DESC_SCENARIOS_DONE
