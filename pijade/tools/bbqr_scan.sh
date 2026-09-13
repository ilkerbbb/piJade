# BBB-AIRGAP BBQr emulator scan driver.  Runs in the container: /jade = repository,
# /probe/bbqr = frames written by pijade/tools/bbqr_frames.py.
# Usage: sh bbqr_scan.sh <tag> <jadectl step> [<jadectl step> ...]
# Fresh daemon and fresh wallet per run, so one scenario cannot leave collector state behind for
# the next.  The steps run from /probe/bbqr, which is where camfiles: prefixes resolve.
set -u
TAG=$1; shift
D=/jade/build_linux_nci_log/libjade/libjade_daemon
SOCK=/tmp/bbqr_$TAG.sock; LOG=/tmp/bbqr_$TAG.log

# The mnemonic the psbt_ss_* fixtures were generated from, read out of the test suite rather than
# copied here: the fixture and the wallet that signs it have to stay the same one, and a copy
# would go stale silently.
MNEMONIC=$(python3 - <<'PY'
import io
import re
text = io.open('/jade/test_jade.py', encoding='utf-8').read()
match = re.search(r"^TEST_MNEMONIC_SINGLE_SIG = \\\n +'([^']+)'", text, re.M)
if not match:
    raise SystemExit('TEST_MNEMONIC_SINGLE_SIG not found in test_jade.py')
print(match.group(1))
PY
)
[ -n "$MNEMONIC" ] || { echo "NO MNEMONIC"; exit 1; }

[ -S $SOCK ] && unlink $SOCK
$D --socketfile $SOCK --log-level info > $LOG 2>&1 &
DP=$!
for i in $(seq 1 200); do [ -S $SOCK ] && break; sleep 0.1; done
[ -S $SOCK ] || { echo "DAEMON DID NOT START"; exit 1; }

# Frames live here, so camfiles: prefixes are given relative to this directory.  Screenshots do
# not follow: jadectl writes them to its own OUT (pijade/tools/jadectl.py:33, /probe), so a
# shot:<name> lands in /probe/<name>.rgb565 whatever the working directory is.
cd /probe/bbqr
python3 /jade/pijade/tools/menu_audit.py $SOCK $LOG $TAG "seed:$MNEMONIC" shot:${TAG}_home | tail -2
python3 /jade/pijade/tools/jadectl.py $SOCK "$@"
RC=$?

echo "--- log ---"
grep -n "BBQr\|Unhandled\|Unsupported\|bcur\|psbt\|PSBT\|Failed\|E (\|W (" $LOG | tail -20
# A daemon that died takes its socket with it; the scenarios that feed rejected frames are
# measured on it still being alive, so report that separately from the jadectl exit code.
kill -0 $DP 2>/dev/null && echo "DAEMON ALIVE" || echo "DAEMON GONE"
kill $DP 2>/dev/null; wait $DP 2>/dev/null
echo "HARNESS_DONE rc=$RC"
