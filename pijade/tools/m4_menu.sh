# BBB-AIRGAP M4 emulator scenario driver: mining menu (Options > Mining).
# Runs in the container: /jade = repository, /probe = Mac scratch mount (frames from mine_qr.py).
# Usage: sh m4_menu.sh <tag> <frame.gray> <mode>; nci daemon (waits for real presses).
# Modes: start | reward | locked
#   start  : Mining > Start to scan a template, confirm, open the mining screen, Stop
#   reward : change Reward from Template to This Wallet, then Start to select an address
#   cancel : set Reward to This Wallet, back out of address selection; mining must not start
#   locked : record Options > Mining and the mining menu without loading a wallet
set -u
TAG=$1; FRAME=$2; MODE=$3
D=/jade/build_linux_nci_log/libjade/libjade_daemon
SOCK=/tmp/m4_$TAG.sock; LOG=/tmp/m4_$TAG.log
J="python3 /jade/pijade/tools/jadectl.py $SOCK"
SEED="seed:abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
[ -S $SOCK ] && unlink $SOCK
$D --socketfile $SOCK --log-level info > $LOG 2>&1 &
DP=$!
for i in $(seq 1 200); do [ -S $SOCK ] && break; sleep 0.1; done
[ -S $SOCK ] || { echo "DAEMON DID NOT START"; exit 1; }
cd /probe

# Home to Options: the main menu starts on the header back button; the first press moves
# past it (menu_audit.py documentation). Options is a list and starts on the first row.
TO_OPTIONS="btn:right wait:0.4 btn:right wait:0.4 btn:click wait:0.8"
TO_MINING_LOADED="btn:down wait:0.4 btn:down wait:0.4 btn:down wait:0.4 btn:down wait:0.4 btn:down wait:0.4 btn:click wait:0.8"
TO_MINING_LOCKED="btn:down wait:0.4 btn:down wait:0.4 btn:down wait:0.4 btn:down wait:0.4 btn:click wait:0.8"

if [ "$MODE" = "locked" ]; then
  $J wait:1 $TO_OPTIONS shot:${TAG}_locked_options $TO_MINING_LOCKED shot:${TAG}_locked_menu > /dev/null
else
  python3 /jade/pijade/tools/menu_audit.py $SOCK $LOG $TAG "$SEED" shot:home | tail -1
  $J $TO_OPTIONS $TO_MINING_LOADED shot:${TAG}_menu > /dev/null
fi

case $MODE in
  start)
    # Mining uses the Jade menu (make_menu_activity, 2026-09-06): focus starts on the header
    # back arrow; the first down press selects Start.
    $J btn:down wait:0.4 btn:click wait:1 > /dev/null
    $J camfile:$FRAME:12 wait:2 shot:${TAG}_confirm > /dev/null
    $J btn:right wait:0.4 btn:click wait:3 shot:${TAG}_mine > /dev/null
    $J btn:click wait:1 shot:${TAG}_after > /dev/null ;;
  reward)
    # Reward Address is the second row: two down presses, click opens the carousel; right selects This Wallet, click accepts.
    $J btn:down wait:0.4 btn:down wait:0.4 btn:click wait:0.8 shot:${TAG}_carousel btn:right wait:0.4 shot:${TAG}_toggled btn:click wait:0.8 > /dev/null
    # Rebuilt menu starts on the back arrow: down selects Start, click scans; the address explorer should open.
    $J btn:down wait:0.4 btn:click wait:1 > /dev/null
    $J camfile:$FRAME:12 wait:2 shot:${TAG}_picker > /dev/null
    # The explorer (2026-09-02 layout) lists Receive/Change/Options and starts on the first row:
    # Receive, then the first address; confirmation starts on the back arrow, right selects the check mark.
    $J btn:click wait:0.8 shot:${TAG}_list btn:click wait:1.5 shot:${TAG}_confirm > /dev/null
    $J btn:right wait:0.4 btn:click wait:3 shot:${TAG}_mine > /dev/null
    $J btn:click wait:1 shot:${TAG}_after > /dev/null ;;
  cancel)
    $J btn:down wait:0.4 btn:down wait:0.4 btn:click wait:0.8 btn:right wait:0.4 shot:${TAG}_toggled btn:click wait:0.8 > /dev/null
    $J btn:down wait:0.4 btn:click wait:1 > /dev/null
    $J camfile:$FRAME:12 wait:2 shot:${TAG}_picker > /dev/null
    # Move to the header back button: focus is on the first row, one up press selects it.
    $J btn:up wait:0.4 btn:click wait:1.5 shot:${TAG}_after > /dev/null ;;
  locked) ;;
esac

echo "--- log ---"
grep -n "Mining\|mining\|Failed\|ERROR\|reward" $LOG | grep -v "wire.c" | tail -12
kill $DP 2>/dev/null; wait $DP 2>/dev/null
echo "HARNESS_DONE"
