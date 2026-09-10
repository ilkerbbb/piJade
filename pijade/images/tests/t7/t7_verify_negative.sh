# Targeted negative scenarios for t7_verify.sh: does the independent checker also reject
# gate 14's rejection cases? Runs on a hardened image, reverting after every scenario.
# Container: docker run --rm -i --privileged -v <img>:/img -v <this directory>:/s alpine:3.20 sh /s/t7_verify_negative.sh
set +e
apk add --no-cache util-linux >/dev/null 2>&1
mkdir -p /mnt/root /mnt/boot
L1=$(losetup -o 545259520 --sizelimit 1954545664 -f --show /img); mount "$L1" /mnt/root || exit 1
L2=$(losetup -o 8388608 --sizelimit 536870912 -f --show /img); mount "$L2" /mnt/boot || exit 1
A=/mnt/root/usr/share/dbus-1/system-services/org.freedesktop.hostname1.service
U=/mnt/root/etc/systemd/system/pijade-boot-verify.service
W=/mnt/root/etc/systemd/system/multi-user.target.wants/pijade-boot-verify.service
P=/mnt/root/etc/systemd/system/multi-user.target.wants/pijade.service
D=/mnt/root/etc/systemd/system/default.target
F=/mnt/root/etc/fstab
C=/mnt/boot/cmdline.txt
V=/mnt/root/usr/local/sbin/pijade-boot-verify
M=/mnt/root/usr/local/sbin/pijade-machine-id
MID=/mnt/root/etc/systemd/system/pijade-machine-id.service
PS=/mnt/root/etc/systemd/system/pijade.service
T=/mnt/root/sbin/swapon
[ -f "$A" ] && [ -f "$U" ] && [ -L "$W" ] && [ -L "$P" ] && [ -L "$D" ] \
  && [ -f "$F" ] && [ -f "$C" ] && [ -f "$V" ] && [ -f "$M" ] && [ -f "$MID" ] \
  && [ -f "$PS" ] && [ -x "$T" ] \
  || { echo "ERROR: negative scenario input missing"; umount /mnt/boot /mnt/root; losetup -d "$L2"; losetup -d "$L1"; exit 1; }
cp "$A" /tmp/a.bak
cp "$U" /tmp/u.bak
cp -P "$W" /tmp/w.bak
cp -P "$P" /tmp/p.bak
cp -P "$D" /tmp/d.bak
cp "$F" /tmp/f.bak
cp "$C" /tmp/c.bak
cp -p "$V" /tmp/v.bak
cp -p "$M" /tmp/m.bak
cp "$MID" /tmp/mid.bak
cp "$PS" /tmp/ps.bak
fail=0
run_verify() { umount /mnt/boot /mnt/root; losetup -d "$L2"; losetup -d "$L1"; sh /s/t7_verify.sh > /tmp/v.log 2>&1; rc=$?; L1=$(losetup -o 545259520 --sizelimit 1954545664 -f --show /img); mount "$L1" /mnt/root; L2=$(losetup -o 8388608 --sizelimit 536870912 -f --show /img); mount "$L2" /mnt/boot; return $rc; }
# S1: add a second SystemdService= line AFTER the allowed SystemdService= line.
printf 'SystemdService=evil.service\n' >> "$A"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "multiple action fields: org.freedesktop.hostname1 (SystemdService=2" /tmp/v.log; then echo "S1 duplicate SystemdService: CAUGHT"; else echo "S1 duplicate SystemdService: MISSED (rc=$rc)"; grep -i "hostname1" /tmp/v.log; fail=1; fi
cp /tmp/a.bak "$A"
# S2 (control): adding a NON-ACTION line must not be rejected; the file already has one Exec=
# and one SystemdService= (normal D-Bus activation format), leaving counts at 1/1.
printf '# comment line\n' >> "$A"
run_verify; rc=$?
if [ $rc -eq 0 ] && grep -q "TOTAL ERRORS: 0" /tmp/v.log; then echo "S2 non-action line: no rejection, check clean (counts 1/1)"; else echo "S2 non-action line: REJECTED or other error (rc=$rc)"; grep -i "error\|multiple" /tmp/v.log | head -3; fail=1; fi
cp /tmp/a.bak "$A"
# S3: add a second Exec= after the existing Exec= line (count becomes 2).
printf 'Exec=/bin/true\n' >> "$A"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "multiple action fields: org.freedesktop.hostname1 (SystemdService=1 Exec=2)" /tmp/v.log; then echo "S3 duplicate Exec: CAUGHT"; else echo "S3 duplicate Exec: MISSED (rc=$rc)"; grep "hostname1" /tmp/v.log; fail=1; fi
cp /tmp/a.bak "$A"
# S4: the restored image is clean.
run_verify; rc=$?
if [ $rc -eq 0 ] && grep -q "TOTAL ERRORS: 0" /tmp/v.log; then echo "S4 restored image: CLEAN"; else echo "S4 restored image: DIRTY (rc=$rc)"; tail -3 /tmp/v.log; fail=1; fi
cmp /tmp/a.bak "$A" && echo "activation file unchanged"
# S5: add a second ExecStart= line to the unit.
printf 'ExecStart=/bin/true\n' >> "$U"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "ExecStart count=2" /tmp/v.log; then echo "S5 duplicate ExecStart: CAUGHT"; else echo "S5 duplicate ExecStart: MISSED (rc=$rc)"; grep "ExecStart" /tmp/v.log; fail=1; fi
cp /tmp/u.bak "$U"
# S6: point the wants symlink at a wrong full target with the same unit basename.
ln -sfn /tmp/pijade-boot-verify.service "$W"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "wants symlink: /tmp/pijade-boot-verify.service" /tmp/v.log; then echo "S6 incorrect wants target: CAUGHT"; else echo "S6 incorrect wants target: MISSED (rc=$rc)"; grep "wants symlink" /tmp/v.log; fail=1; fi
mv "$W" /tmp/w.bad
cp -P /tmp/w.bak "$W"
# S7: append an extra 0 to the /boot/firmware PARTUUID field.
awk '$2=="/boot/firmware"{$1=$1 "0"} {print}' OFS='\t' "$F" > /tmp/f.bad
mv /tmp/f.bad "$F"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "boot PARTUUID incorrect" /tmp/v.log; then s7_caught=1; else echo "S7 extended boot PARTUUID: MISSED (rc=$rc)"; grep "boot PARTUUID" /tmp/v.log; s7_caught=0; fail=1; fi
cp /tmp/f.bak "$F"
run_verify; rc=$?
if [ "$s7_caught" -eq 1 ] && [ $rc -eq 0 ] && grep -q "TOTAL ERRORS: 0" /tmp/v.log; then echo "S7 extended boot PARTUUID: CAUGHT; restored image CLEAN"; else echo "S7 restored image: DIRTY (rc=$rc)"; tail -3 /tmp/v.log; fail=1; fi

# S8: append 0 to the cmdline root token; prefix matching must not let it through.
awk '{for (i=1; i<=NF; i++) if ($i ~ /^root=PARTUUID=/) {$i=$i "0"; break} print}' OFS=' ' "$C" > /tmp/c.bad
mv /tmp/c.bad "$C"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "root=PARTUUID count 0" /tmp/v.log; then echo "S8 extended root PARTUUID: CAUGHT"; else echo "S8 extended root PARTUUID: MISSED (rc=$rc)"; grep "root=PARTUUID" /tmp/v.log; fail=1; fi
cp /tmp/c.bak "$C"

# S9: replace the sole cmdline ro token with rw.
awk '{for (i=1; i<=NF; i++) if ($i=="ro") $i="rw"; print}' OFS=' ' "$C" > /tmp/c.bad
mv /tmp/c.bad "$C"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "cmdline ro count 0" /tmp/v.log && grep -q "cmdline rw count 1" /tmp/v.log; then echo "S9 cmdline rw replaces ro: CAUGHT"; else echo "S9 cmdline rw replaces ro: MISSED (rc=$rc)"; grep "cmdline r" /tmp/v.log; fail=1; fi
cp /tmp/c.bak "$C"

# S10: set root options to rw,errors=remount-ro; a ro substring is insufficient.
awk '$1 !~ /^#/ && $2=="/" {$4="rw,errors=remount-ro"} {print}' OFS='\t' "$F" > /tmp/f.bad
mv /tmp/f.bad "$F"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "root PARTUUID incorrect or root NOT ro" /tmp/v.log; then echo "S10 root ro substring: CAUGHT"; else echo "S10 root ro substring: MISSED (rc=$rc)"; grep "root PARTUUID" /tmp/v.log; fail=1; fi
cp /tmp/f.bak "$F"

# S11: remove nosuid from the exact option set of the /tmp line.
awk '$1 !~ /^#/ && $2=="/tmp" {sub(/nosuid,/, "", $4)} {print}' OFS='\t' "$F" > /tmp/f.bad
mv /tmp/f.bad "$F"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "tmpfs /tmp 0 times" /tmp/v.log; then echo "S11 tmpfs nosuid missing: CAUGHT"; else echo "S11 tmpfs nosuid missing: MISSED (rc=$rc)"; grep "tmpfs /tmp" /tmp/v.log; fail=1; fi
cp /tmp/f.bak "$F"

# S12: extend the unit field with a prefix and add the forbidden target to After.
sed -e 's/^Type=oneshot$/Type=oneshot-extra/' \
    -e 's/^After=pijade.service$/After=pijade.service multi-user.target/' "$U" > /tmp/u.bad
mv /tmp/u.bad "$U"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "unit: Type=oneshot MISSING" /tmp/v.log \
  && grep -q "unit: After=pijade.service MISSING" /tmp/v.log \
  && grep -q "After= contains multi-user.target" /tmp/v.log; then echo "S12 unit fields not exact: CAUGHT"; else echo "S12 unit fields not exact: MISSED (rc=$rc)"; grep "unit:" /tmp/v.log; fail=1; fi
cp /tmp/u.bak "$U"

# S13: remove the executable bit from a tool production checks at its exact path.
chmod -x "$T"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "swapon MISSING or not executable: /sbin/swapon" /tmp/v.log; then echo "S13 swapon x bit missing: CAUGHT"; else echo "S13 swapon x bit missing: MISSED (rc=$rc)"; grep "swapon" /tmp/v.log; fail=1; fi
chmod +x "$T"

# S14: production accepts field names in comments and the ro substring in a boot option.
printf '# PARTUUID=comment none swap defaults 0 0\n' >> "$F"
awk '$1 !~ /^#/ && $2=="/boot/firmware" {$4=$4 ",errors=remount-ro"} {print}' OFS='\t' "$F" > /tmp/f.control
mv /tmp/f.control "$F"
sed '1s/$/ foo=init=bar/' "$C" > /tmp/c.control; mv /tmp/c.control "$C"
run_verify; rc=$?
if [ $rc -eq 0 ] && grep -q "TOTAL ERRORS: 0" /tmp/v.log; then echo "S14 comment and boot ro substring: NO REJECTION"; else echo "S14 comment and boot ro substring: UNEXPECTED REJECTION (rc=$rc)"; grep -i "error" /tmp/v.log | head -5; fail=1; fi
cp /tmp/f.bak "$F"
cp /tmp/c.bak "$C"

# S15: a plain file in an emptied wants directory is not a symlink measured by the production gate.
Q=/mnt/root/etc/systemd/system/sockets.target.wants/ssh.service
printf 'control\n' > "$Q"
run_verify; rc=$?
if [ $rc -eq 0 ] && grep -q "TOTAL ERRORS: 0" /tmp/v.log; then echo "S15 plain file in wants directory: NO REJECTION"; else echo "S15 plain file in wants directory: UNEXPECTED REJECTION (rc=$rc)"; grep "sockets.target.wants" /tmp/v.log; fail=1; fi
rm -f "$Q"

# S16: default.target resolves to a real file outside the image root.
printf 'control\n' > /tmp/multi-user.target
ln -sfn ../../../../../tmp/multi-user.target "$D"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "default.target resolves outside image root" /tmp/v.log; then echo "S16 default.target outside root: CAUGHT"; else echo "S16 default.target outside root: MISSED (rc=$rc)"; grep "default.target" /tmp/v.log; fail=1; fi
mv "$D" /tmp/d.bad
cp -P /tmp/d.bak "$D"

# S17: redirect the product service's activation link to a wrong full target.
ln -sfn /tmp/pijade.service "$P"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "pijade.service wants symlink: /tmp/pijade.service" /tmp/v.log; then echo "S17 pijade wants target: CAUGHT"; else echo "S17 pijade wants target: MISSED (rc=$rc)"; grep "pijade.service wants" /tmp/v.log; fail=1; fi
mv "$P" /tmp/p.bad
cp -P /tmp/p.bak "$P"

# S18: break measurement, return-code, result and exit contracts together.
sed -e 's/swapon --show/swapon --list/' -e 's/RESULT=0/RESULT=2/' \
    -e 's/MACHINE_ID" != "\$STORED_ID/MACHINE_ID" != "\$OTHER_ID/' \
    -e 's/exit "\$RESULT"/exit 0/' "$V" > /tmp/v.bad
mv /tmp/v.bad "$V"; chmod +x "$V"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "measurement MISSING: swapon --show" /tmp/v.log \
  && grep -q "exit-code contract incomplete" /tmp/v.log \
  && grep -q "verdict or exit not derived from error counter" /tmp/v.log \
  && grep -q "verification only checks for nonempty content" /tmp/v.log; then echo "S18 verification script contract: CAUGHT"; else echo "S18 verification script contract: MISSED (rc=$rc)"; grep "measurement MISSING\|contract\|verdict or\|only checks for nonempty content" /tmp/v.log; fail=1; fi
cp -p /tmp/v.bak "$V"

# S19: break the identity script, unit, product dependency and temporary image ID together.
sed -e 's|^mount --bind "$ID_FILE" /etc/machine-id$|mount --bind "$ID_FILE" /etc/machine-id extra|' \
    -e 's|wc -c < "$ID_FILE"|wc -c  < "$ID_FILE"|' \
    -e 's|wc -l < "$ID_FILE"|wc -l  < "$ID_FILE"|' "$M" > /tmp/m.bad
mv /tmp/m.bad "$M"; chmod +x "$M"
sed 's/^Type=oneshot$/Type=oneshot-extra/' "$MID" > /tmp/mid.bad; mv /tmp/mid.bad "$MID"
printf '\nAfter=pijade-machine-id.service\n' >> "$PS"
printf 'control\n' > /mnt/boot/pijade-machine-id.tmp
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "script does not establish bind mount" /tmp/v.log \
  && grep -q "full file size not validated" /tmp/v.log \
  && grep -q "full line count not validated" /tmp/v.log \
  && grep -Fq 'identity unit missing directive: ^Type=oneshot$' /tmp/v.log \
  && grep -q "product service depends on identity unit" /tmp/v.log \
  && grep -q "image contains preset identity" /tmp/v.log; then echo "S19 identity scheme exact contract: CAUGHT"; else echo "S19 identity scheme exact contract: MISSED (rc=$rc)"; grep "does not establish bind mount\|full file size\|missing directive\|depends on\|preset identity" /tmp/v.log; fail=1; fi
cp -p /tmp/m.bak "$M"
cp /tmp/mid.bak "$MID"
cp /tmp/ps.bak "$PS"
rm -f /mnt/boot/pijade-machine-id.tmp

# S20: see hidden entries in the multi-user allowlist directory, as production does.
H=/mnt/root/etc/systemd/system/multi-user.target.wants/.parity-check
printf 'control\n' > "$H"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "multi-user.target.wants" /tmp/v.log; then echo "S20 hidden wants entry: CAUGHT"; else echo "S20 hidden wants entry: MISSED (rc=$rc)"; grep "multi-user.target.wants" /tmp/v.log; fail=1; fi
rm -f "$H"

# S21: production strips the same two characters after S even when they are not digits.
X=/mnt/root/etc/rc3.d/Sxxparity
Y=/mnt/root/usr/lib/systemd/system/xxparity.service
ln -s ../init.d/parity "$X"
printf '[Service]\nType=oneshot\n' > "$Y"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "sysv Sxxparity -> parity.service native unit MISSING" /tmp/v.log; then echo "S21 sysv two-character stripping: CAUGHT"; else echo "S21 sysv two-character stripping: MISSED (rc=$rc)"; grep "Sxxparity" /tmp/v.log; fail=1; fi
rm -f "$X" "$Y"

# S22: silently drop an info line. These lines do not touch FAIL, so their disappearance
# shows no device failure; only the checker's label count holds them in place.
sed -i '/temperature (info)/d' "$V"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "info line: temperature (info) x0" /tmp/v.log; then echo "S22 info line removed: CAUGHT"; else echo "S22 info line removed: MISSED (rc=$rc)"; grep "info line" /tmp/v.log; fail=1; fi
cp -p /tmp/v.bak "$V"

# S23: turn the info block into a gate. Opposite regression: if boot_id starts incrementing
# the error counter, every boot fails; pin the decision here (info, NOT a gate).
sed -i 's|        echo "  boot_id (info): $BOOT_ID"|        echo "  boot_id (info): $BOOT_ID"; FAIL=$((FAIL + 1))|' "$V"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -q "info lines increment error counter" /tmp/v.log; then echo "S23 info line converted to gate: CAUGHT"; else echo "S23 info line converted to gate: MISSED (rc=$rc)"; grep "error counter\|error counter" /tmp/v.log; fail=1; fi
cp -p /tmp/v.bak "$V"

# S24-S26: COMMAND and EXIT-CODE contracts of info measurements. These three mutations leave echo
# lines intact, so label counts and the "does not touch error counter" check CANNOT detect them;
# only the checker's measurement/contract list catches them. Production gate 11 rejects all three
# (t7_sabotage.sh s42/s43/s45); these scenarios pin both sides to the same measured set.
# S24: replace the boot_id measurement with an empty assignment.
sed -i 's|BOOT_ID=$(cat /proc/sys/kernel/random/boot_id); BOOT_ID_RC=$?|BOOT_ID=""; BOOT_ID_RC=0|' "$V"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -Fq "measurement MISSING: cat /proc/sys/kernel/random/boot_id" /tmp/v.log \
  && grep -Fq '[BOOT_ID=$(cat /proc/sys/kernel/random/boot_id); BOOT_ID_RC=$?]' /tmp/v.log; then echo "S24 boot_id measurement emptied: CAUGHT"; else echo "S24 boot_id measurement emptied: MISSED (rc=$rc)"; grep "measurement MISSING\|contract incomplete" /tmp/v.log; fail=1; fi
cp -p /tmp/v.bak "$V"

# S25: remove get_throttled exit-code assignment; keep the measurement command.
sed -i 's|; THROTTLED_RC=$?||' "$V"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -Fq '[THROTTLED=$(vcgencmd get_throttled 2>&1); THROTTLED_RC=$?]' /tmp/v.log; then echo "S25 get_throttled exit code removed: CAUGHT"; else echo "S25 get_throttled exit code removed: MISSED (rc=$rc)"; grep "contract incomplete" /tmp/v.log; fail=1; fi
cp -p /tmp/v.bak "$V"

# S26: put dmesg back into a pipeline. Full regression: its exit code becomes unreadable and an
# UNREADABLE kernel log again looks like clean power with "matches=0" (Codex finding, 2026-08-29).
sed -i 's|DMESG_OUT=$(dmesg 2>&1); DMESG_RC=$?|DMESG_OUT=$(dmesg 2>/dev/null)|' "$V"
run_verify; rc=$?
if [ $rc -ne 0 ] && grep -Fq '[DMESG_OUT=$(dmesg 2>&1); DMESG_RC=$?]' /tmp/v.log; then echo "S26 dmesg exit code removed: CAUGHT"; else echo "S26 dmesg exit code removed: MISSED (rc=$rc)"; grep "contract incomplete" /tmp/v.log; fail=1; fi
cp -p /tmp/v.bak "$V"

# Final check: the image must remain clean after all mutations are reverted.
run_verify; rc=$?
if [ $rc -eq 0 ] && grep -q "TOTAL ERRORS: 0" /tmp/v.log; then echo "S26 restored image: CLEAN"; else echo "S26 restored image: DIRTY (rc=$rc)"; tail -3 /tmp/v.log; fail=1; fi
umount /mnt/boot /mnt/root; losetup -d "$L2"; losetup -d "$L1"
[ $fail -eq 0 ] && echo "VERIFY NEGATIVE RESULT: 26/26" || echo "VERIFY NEGATIVE RESULT: ERROR"
exit $fail
