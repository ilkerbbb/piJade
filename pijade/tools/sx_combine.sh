#!/bin/bash
# BBB-AIRGAP: runner for the Seed XOR combine measurement (pijade/tools/sx_combine.py).
# Runs in the container: /jade = repository, /probe = scratch.
# Usage: bash /jade/pijade/tools/sx_combine.sh [cancel|identity]...
#
# Two daemons per case and both fresh: the one being driven must start uninitialised, because
# the path measured here is the wallet-less Options > Temporary Signer row, and the reference
# one must hold only the phrase it is asked about.
set -u
D=/jade/build_linux_nci_log/libjade/libjade_daemon
RC=0

start() { # start <suffix>
    rm -f "/probe/sockSX$1" "/probe/settingsSX$1.a" "/probe/settingsSX$1.b"
    (nohup $D --socketfile "/probe/sockSX$1" --settings "/probe/settingsSX$1" --log-level info \
        > "/probe/daemonSX$1.log" 2>&1 &)
    local i
    for i in $(seq 1 60); do [ -S "/probe/sockSX$1" ] && break; sleep 0.5; done
    [ -S "/probe/sockSX$1" ] || { echo "ERROR: daemon socket did not open: /probe/sockSX$1"; exit 1; }
}

for CASE in "$@"; do
    pkill -f "^$D"; sleep 1
    start "${CASE}U"
    start "${CASE}R"
    sleep 1
    python3 -u /jade/pijade/tools/sx_combine.py \
        "/probe/sockSX${CASE}U" "/probe/sockSX${CASE}R" "$CASE" "sx_$CASE" || RC=1
    grep -iE "error|assert|abort" "/probe/daemonSX${CASE}U.log" | head -5
done
pkill -f "^$D"
exit $RC
