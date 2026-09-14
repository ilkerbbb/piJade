#!/bin/bash
# BBB-AIRGAP: runner for the SLIP-0039 recovery measurement (pijade/tools/s39_recover.py).
# Runs in the container: /jade = repository, /probe = scratch.
# Usage: bash /jade/pijade/tools/s39_recover.sh <case>...
#
# One fresh daemon per case: the path measured starts at the wallet-less Options > Temporary
# Signer row, and a share collection left behind by one case must not reach the next.
# The 'persist' case (run 5, pijade/tools/s39_persist.py) is the exception and runs its own
# daemons: two against one settings file, for the restart, and a third with a settings file of
# its own, for the wallet the 'Backup' row is measured against.  It is reachable from here so
# that every SLIP-0039 measurement has one entry point.
set -u
D=/jade/build_linux_nci_log/libjade/libjade_daemon
FRAMES=/probe/slip39
RC=0

# The typed case needs no frames; every other case reads them.
if printf "%s\n" "$@" | grep -qv "^words"; then
    [ -d "$FRAMES" ] || { echo "ERROR: no frames in $FRAMES - run slip39_qr.py first"; exit 1; }
fi

start() { # start <socket suffix> <log suffix>
    rm -f "/probe/sockS39$1" "/probe/settingsS39$1".*
    (nohup $D --socketfile "/probe/sockS39$1" --settings "/probe/settingsS39$1" --log-level info \
        > "/probe/daemonS39_$2.log" 2>&1 &)
    local i
    for i in $(seq 1 60); do [ -S "/probe/sockS39$1" ] && break; sleep 0.5; done
    [ -S "/probe/sockS39$1" ] || { echo "ERROR: daemon socket did not open: /probe/sockS39$1"; exit 1; }
}

for CASE in "$@"; do
    pkill -f "^$D"; sleep 1
    if [ "$CASE" = "persist" ]; then
        # Run 5 starts and restarts its own daemon, because the second half has to read the
        # settings file the first half wrote; a runner that owned the daemon would have to
        # keep those files alive across a case, which is exactly what the loop above erases.
        python3 -u /jade/pijade/tools/s39_persist.py "$D" /probe/sockS39P /probe/settingsS39P \
            "$FRAMES" s39_persist || RC=1
        continue
    fi
    start "" "$CASE"
    if [ "$CASE" = "pass" ]; then
        # The reference daemon is given the vector's master secret and nothing else, so it starts
        # fresh for the same reason the driven one does.
        start "R" "${CASE}R"
        sleep 1
        python3 -u /jade/pijade/tools/s39_recover.py /probe/sockS39 "$CASE" "s39_$CASE" "$FRAMES" \
            /probe/sockS39R || RC=1
    else
        sleep 1
        python3 -u /jade/pijade/tools/s39_recover.py /probe/sockS39 "$CASE" "s39_$CASE" "$FRAMES" || RC=1
    fi
    grep -iE "error|assert|abort" "/probe/daemonS39_$CASE.log" | head -5
done
pkill -f "^$D"
exit $RC
