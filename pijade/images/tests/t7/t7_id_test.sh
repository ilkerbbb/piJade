set -e
apk add --no-cache util-linux >/dev/null 2>&1
RL=$(losetup -f --show -o 545259520 --sizelimit 1954545664 /img)
BL=$(losetup -f --show -o 8388608 --sizelimit 536870912 /img)
mkdir -p /mnt/root /boot/firmware
mount -t ext4 -o ro "$RL" /mnt/root
mount -t vfat "$BL" /boot/firmware        # product's ACTUAL FAT partition
SCRIPT=/usr/local/sbin/pijade-machine-id
mkdir -p /usr/local/sbin
cp /mnt/root/usr/local/sbin/pijade-machine-id "$SCRIPT"
VERIFY=/mnt/root/usr/local/sbin/pijade-boot-verify

: > /etc/machine-id                        # initial image state: 0-byte regular file
rm -f /boot/firmware/pijade-machine-id /boot/firmware/pijade-machine-id.tmp

fail=0
check() { if [ "$2" = "$3" ]; then echo "  OK  $1"; else echo "  ERROR   $1: expected [$2], got [$3]"; fail=$((fail+1)); fi; }
hexcount() { printf '%s' "$1" | grep -cE '^[0-9a-f]{32}$' || true; }

echo "=== 1) fresh card: generation + mounting ==="
"$SCRIPT"
ID1=$(cat /etc/machine-id); FILE1=$(cat /boot/firmware/pijade-machine-id)
check "identity has 32 lowercase hex digits" "1" "$(hexcount "$ID1")"
check "mounted file = source file" "$FILE1" "$ID1"
check "/etc/machine-id mount point" "1" "$(mountpoint -q /etc/machine-id && echo 1 || echo 0)"
check "no temporary file left" "0" "$(ls /boot/firmware/pijade-machine-id.tmp 2>/dev/null | wc -l | tr -d ' ')"

echo "=== 2) second boot: identity must stay the SAME (core requirement) ==="
umount /etc/machine-id; "$SCRIPT"
check "identity unchanged" "$ID1" "$(cat /etc/machine-id)"

echo "=== 3) malformed file regenerated ==="
umount /etc/machine-id; echo "this is not an identity" > /boot/firmware/pijade-machine-id; "$SCRIPT"
ID3=$(cat /etc/machine-id)
check "new identity has 32 hex digits" "1" "$(hexcount "$ID3")"
check "differs from old identity" "0" "$(test "$ID3" = "$ID1" && echo 1 || echo 0)"

echo "=== 4) truncated (16 digits) rejected ==="
umount /etc/machine-id; printf '%s\n' "0123456789abcdef" > /boot/firmware/pijade-machine-id; "$SCRIPT"
check "truncated value not used" "0" "$(test "$(cat /etc/machine-id)" = "0123456789abcdef" && echo 1 || echo 0)"

echo "=== 5) uppercase hex rejected (machine-id(5) requires lowercase) ==="
umount /etc/machine-id; printf '%s\n' "0123456789ABCDEF0123456789ABCDEF" > /boot/firmware/pijade-machine-id; "$SCRIPT"
check "uppercase not used" "0" "$(test "$(cat /etc/machine-id)" = "0123456789ABCDEF0123456789ABCDEF" && echo 1 || echo 0)"

echo "=== 5b) valid first line + extra bytes: rejected (r5 finding 4) ==="
umount /etc/machine-id
GOOD="0123456789abcdef0123456789abcdef"
printf '%s\nextra line\n' "$GOOD" > /boot/firmware/pijade-machine-id
"$SCRIPT"
check "file with extra bytes not used" "0" "$(test "$(cat /etc/machine-id)" = "$GOOD" && echo 1 || echo 0)"
check "new identity has 32 hex digits" "1" "$(hexcount "$(cat /etc/machine-id)")"
check "mounted file has one line, 33 bytes" "33" "$(wc -c < /boot/firmware/pijade-machine-id | tr -d ' ')"

echo "=== 5c) same-size malformed input (31hex + newline + 1hex) rejected ==="
umount /etc/machine-id
printf '0123456789abcdef0123456789abcde\nf' > /boot/firmware/pijade-machine-id
check "fixture has 33 bytes" "33" "$(wc -c < /boot/firmware/pijade-machine-id | tr -d ' ')"
"$SCRIPT"
check "malformed format rejected" "1" "$(hexcount "$(cat /etc/machine-id)")"
check "corrected to one line" "1" "$(wc -l < /boot/firmware/pijade-machine-id | tr -d ' ')"

echo "=== 6) simulate PID1: stack over an already mounted temporary identity ==="
umount /etc/machine-id
mkdir -p /run/mid; echo "ffffffffffffffffffffffffffffffff" > /run/mid/machine-id
mount --bind /run/mid/machine-id /etc/machine-id
BEFORE=$(cat /etc/machine-id); "$SCRIPT"; AFTER=$(cat /etc/machine-id)
check "temporary identity visible before" "ffffffffffffffffffffffffffffffff" "$BEFORE"
check "persistent identity visible after" "$(cat /boot/firmware/pijade-machine-id)" "$AFTER"

echo "=== 7) verification measurement: MOUNTED state must say OK ==="
OUT=$(sh -c "$(sed -n '/ID_FILE=\/boot\/firmware\/pijade-machine-id/,/^    fi$/p' "$VERIFY" | sed 's/FAIL=\$((FAIL + 1))/FAIL=1/')" 2>&1 || true)
check "OK when mounted" "1" "$(printf '%s' "$OUT" | grep -c 'machine-id: OK' || true)"

echo "=== 8) verification measurement: TEMPORARY identity must say ERROR (the exact r3 fault) ==="
umount /etc/machine-id
umount /run/mid/machine-id 2>/dev/null || true
mount --bind /run/mid/machine-id /etc/machine-id      # persistent file exists but is NOT mounted
OUT2=$(sh -c "$(sed -n '/ID_FILE=\/boot\/firmware\/pijade-machine-id/,/^    fi$/p' "$VERIFY" | sed 's/FAIL=\$((FAIL + 1))/FAIL=1/')" 2>&1 || true)
check "ERROR for temporary identity" "1" "$(printf '%s' "$OUT2" | grep -c 'temporary identity in use' || true)"
check "DOES NOT report OK for temporary identity" "0" "$(printf '%s' "$OUT2" | grep -c 'machine-id: OK' || true)"

umount /etc/machine-id 2>/dev/null || true
rm -f /boot/firmware/pijade-machine-id /boot/firmware/pijade-machine-id.tmp
umount /boot/firmware; umount /mnt/root; losetup -d "$BL" "$RL"
echo; echo "TOTAL ERRORS: $fail"
[ "$fail" -eq 0 ] || exit 1
