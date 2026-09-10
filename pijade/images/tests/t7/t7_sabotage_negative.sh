set +e
apk add --no-cache util-linux >/dev/null 2>&1
mkdir -p /mnt/root /mnt/boot
# Fail-open closed: report the result through the EXIT CODE, without requiring output inspection.
# A cleanup hook also prevents leftover mounts/loops after interruptions or intermediate errors.
MISSED=0
CAUGHT=0
cleanup() { umount /mnt/boot 2>/dev/null; umount /mnt/root 2>/dev/null
            losetup -d "$L2" 2>/dev/null; losetup -d "$L1" 2>/dev/null; }
trap cleanup EXIT

mount_rw() {
  L1=$(losetup -o 545259520 --sizelimit 1954545664 -f --show /img); mount "$L1" /mnt/root
  L2=$(losetup -o 8388608 --sizelimit 536870912 -f --show /img); mount "$L2" /mnt/boot
}
umount_all() { sync; umount /mnt/boot; umount /mnt/root; losetup -d "$L2"; losetup -d "$L1"; }

test_one() {
  name="$1"; expected="$2"; sab="$3"; rev="$4"
  mount_rw; "$sab"; umount_all
  # Inspect ALL output, not just the final line: a failing gate may dump a diagnostic list
  # whose last line is not an error message (exactly what happened in t7_sabotage.sh's s30). Also
  # the chain ACTUALLY failed; a text match alone is insufficient.
  out=$(sh /s/t7_gates_only.sh 2>&1)
  if printf '%s' "$out" | grep -q "ALL GATES PASSED"; then
    echo "  MISSED   $name  -> chain PASSED, sabotage undetected"; MISSED=$((MISSED + 1))
  elif printf '%s' "$out" | grep -q "$expected"; then
    echo "  CAUGHT  $name"; CAUGHT=$((CAUGHT + 1))
  else
    echo "  MISSED   $name  -> chain failed but expected text missing: $(printf '%s' "$out" | grep ERROR | head -1)"
    MISSED=$((MISSED + 1))
  fi
  mount_rw; "$rev"; umount_all
}

# This file deliberately runs ONE case: the framework itself, with no sabotage applied.
# The correct outcome is "0 caught, 1 missed" and exit 1 -- counting a catch where nothing
# was sabotaged would mean the harness is fail-open. The 44 real sabotage scenarios live in
# t7_sabotage.sh; this file is NOT part of t7_chain.sh because its expected exit is nonzero.
echo "=== GATE NEGATIVE TESTS ==="
test_one "PROOF: no sabotage applied" "text-that-will-never-match" true true

echo "=== sabotage reverted; final state ==="
sh /s/t7_gates_only.sh 2>&1 | tail -1

echo
echo "SABOTAGE RESULT: $CAUGHT caught, $MISSED missed"
if [ "$MISSED" -ne 0 ]; then echo "SABOTAGE: ERROR"; exit 1; fi
if [ "$CAUGHT" -eq 0 ]; then echo "SABOTAGE: ERROR (no scenario executed)"; exit 1; fi
# The exit code also requires the chain to return to a clean state.
sh /s/t7_gates_only.sh > /tmp/final.log 2>&1
if ! grep -q "ALL GATES PASSED" /tmp/final.log; then
  echo "SABOTAGE: ERROR (image did not return to clean state)"; tail -3 /tmp/final.log; exit 1
fi
echo "SABOTAGE: OK ($CAUGHT/$CAUGHT)"
