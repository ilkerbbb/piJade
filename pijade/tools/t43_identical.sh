# BBB-AIRGAP item 43 measurement: what happens when an identical registration is scanned again.
# Uses the measured helpers from desc_scenarios.sh (its comments describe the pitfalls).
# Run: docker exec jade-dev bash /jade/pijade/tools/t43_identical.sh
#
# The exit code is the measurement: 0 only if every step ran. Diagnostics go to stderr because
# the scan calls below redirect stdout to /dev/null; a failure printed to stdout would disappear,
# silently making the run appear to pass.
ERROR=0
report() { echo "ERROR: $*" >&2; ERROR=1; }

D=/jade/build_linux_nci_log/libjade/libjade_daemon; L=/probe/t43.log
stop() {
    local i rc
    pkill -f "^$D"
    for i in $(seq 1 40); do
        sleep 0.3
        pgrep -f "^$D" >/dev/null; rc=$?
        case $rc in
            0) ;;
            1) return 0 ;;
            *) report "could not read process status: pgrep exit $rc"; return 1 ;;
        esac
    done
    report "process still running after 40 polls: $D"
    return 1
}
stop; rm -f /probe/t43set.a /probe/t43set.b /probe/sockT43 || report "could not clear settings and socket"
(nohup $D --socketfile /probe/sockT43 --settings /probe/t43set --log-level info > $L 2>&1 &)
for i in $(seq 1 40); do [ -S /probe/sockT43 ] && break; sleep 0.5; done
[ -S /probe/sockT43 ] || report "control socket did not open: /probe/sockT43"
sleep 2
J="python3 /jade/pijade/tools/jadectl.py /probe/sockT43"
M="python3 /jade/pijade/tools/menu_audit.py /probe/sockT43 $L"
SEED="seed:abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
$M t43 "$SEED" shot:t43_home > /dev/null 2>&1 || report "menu_audit failed; could not load seed"

# The jadectl exit code is also part of the measurement: if a button or frame cannot be sent,
# the next comparison measures the old screen and reaches the wrong conclusion.
j() { $J "$@" || report "jadectl failed: $*"; }
# cmp has THREE outcomes: 0 same, 1 different, >=2 could not compare (file missing or unreadable).
# Treating the last as "different" would turn a fault into a measurement; handle it separately.
equal() { cmp -s "$@"; local rc=$?; [ $rc -ge 2 ] && report "could not compare: $*"; return $rc; }

j shot:homesig > /dev/null

ishome() { j shot:$1 >/dev/null; equal -n 40000 /probe/homesig.rgb565 /probe/$1.rgb565; }
gosession() { for i in 1 2 3 4 5 6; do equal /probe/homesig.rgb565 /probe/$1.rgb565 && return 0; j btn:left wait:0.4 shot:$1 >/dev/null; done; report "NO_SESSION_$1"; return 1; }
waitchange() { local rc; for i in $(seq 1 40); do j shot:$2 >/dev/null; equal /probe/$1.rgb565 /probe/$2.rgb565; rc=$?; [ $rc -eq 1 ] && return 0; [ $rc -ge 2 ] && return 1; sleep 0.5; done; report "NO_CHANGE_$2"; return 1; }
scan() { local ok=0; if ishome ${2}_pre; then gosession ${2}_pre; else report "PRE_NOT_HOME_$2"; fi; j btn:right wait:0.5 btn:click wait:1.5 shot:${2}_cam; if [ -n "$3" ]; then j camfiles:/probe/$1:$3; else j camfile:/probe/$1:12; fi; j wait:2; waitchange ${2}_cam ${2}_r1; for i in 1 2 3 4 5 6 7 8; do j btn:first wait:0.4 >/dev/null; if ishome ${2}_p$i; then ok=1; break; fi; j shot:${2}_r$i wait:0.2 btn:right wait:0.3 btn:click wait:1.5; done; [ "$ok" = 1 ] || report "NOT_HOME_$2"; j shot:${2}_after; }

echo "=== 1st scan: multisig file ==="
scan msfile_long.gray m1 > /dev/null
lines=$(grep -c 'Processing multisig file line' $L)
[ $? -lt 2 ] || report "could not read log: first scan registration lines"
identical=$(grep -ci 'identical registration exists' $L)
[ $? -lt 2 ] || report "could not read log: first scan identical count"
echo "registration log: $lines lines; identical: $identical"
echo "=== 2nd scan: SAME file ==="
scan msfile_long.gray m2 > /dev/null
identical=$(grep -ci 'identical registration exists' $L)
[ $? -lt 2 ] || report "could not read log: second scan identical count"
echo "identical lines (after two rounds): $identical"
grep -iE "identical|declined" $L | tail -3
statuses=("${PIPESTATUS[@]}")
[ "${statuses[0]}" -lt 2 ] || report "could not read log: identical and declined lines"
[ "${statuses[1]}" -eq 0 ] || report "could not read last log lines: tail exit ${statuses[1]}"
stop
exit $ERROR
