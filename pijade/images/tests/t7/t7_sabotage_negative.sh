set +e
apk add --no-cache util-linux >/dev/null 2>&1
mkdir -p /mnt/root /mnt/boot
D=/mnt/root/etc/systemd/system
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

s3() { mkdir -p $D/sysinit.target.wants; ln -sf /lib/systemd/system/systemd-timesyncd.service $D/sysinit.target.wants/systemd-timesyncd.service; }
r3() { rm -f $D/sysinit.target.wants/systemd-timesyncd.service; }
s5() { ln -sf /lib/systemd/system/bluetooth.service $D/dbus-org.bluez.service; }
r5() { ln -sf /dev/null $D/dbus-org.bluez.service; }
s6() { ln -sf ../init.d/resize2fs_once /mnt/root/etc/rc3.d/S01resize2fs_once; }
r6() { rm -f /mnt/root/etc/rc3.d/S01resize2fs_once; }
s7() { echo "PARTUUID=deadbeef-03 /mnt ext4 defaults 0 0" >> /mnt/root/etc/fstab; }
r7() { sed -i "/deadbeef-03/d" /mnt/root/etc/fstab; }
s8() { sed -i "s|rootwait|root=PARTUUID=deadbeef-99 rootwait|" /mnt/boot/cmdline.txt; }
r8() { sed -i "s|root=PARTUUID=deadbeef-99 ||" /mnt/boot/cmdline.txt; }
s9() { ln -sf /lib/systemd/system/missing.target $D/default.target; }
r9() { ln -sf /lib/systemd/system/multi-user.target $D/default.target; }
s10() { sed -i "/^After=pijade.service/d" $D/pijade-boot-verify.service; }
r10() { sed -i "/^RequiresMountsFor/a After=pijade.service" $D/pijade-boot-verify.service; }
s11() { cp /mnt/root/usr/local/sbin/pijade-boot-verify /mnt/root/tmp/v.bak
        printf '#!/bin/sh\necho OK\n' > /mnt/root/usr/local/sbin/pijade-boot-verify
        chmod 755 /mnt/root/usr/local/sbin/pijade-boot-verify; }
r11() { cp /mnt/root/tmp/v.bak /mnt/root/usr/local/sbin/pijade-boot-verify
        chmod 755 /mnt/root/usr/local/sbin/pijade-boot-verify; rm -f /mnt/root/tmp/v.bak; }

test_one() {
  name="$1"; expected="$2"; sab="$3"; rev="$4"
  mount_rw; "$sab"; umount_all
  # Inspect ALL output, not just the final line: a failing gate may dump a diagnostic list
  # whose last line is not an error message (exactly what happened in s30). Also verify that
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

echo "=== GATE NEGATIVE TESTS ==="
test_one "PROOF: no sabotage applied" "text-that-will-never-match" true true
s12() { sed -i "/^After=pijade.service/i After=multi-user.target" $D/pijade-boot-verify.service; }
r12() { sed -i "/^After=multi-user.target/d" $D/pijade-boot-verify.service; }
s13() { cp /mnt/root/usr/local/sbin/pijade-boot-verify /mnt/root/tmp/v2.bak
        echo 'exit 0' >> /mnt/root/usr/local/sbin/pijade-boot-verify; }
r13() { cp /mnt/root/tmp/v2.bak /mnt/root/usr/local/sbin/pijade-boot-verify
        chmod 755 /mnt/root/usr/local/sbin/pijade-boot-verify; rm -f /mnt/root/tmp/v2.bak; }
s14() { cp /mnt/root/usr/local/sbin/pijade-boot-verify /mnt/root/tmp/v3.bak
        sed -i 's|PIJADE_STATUS=$(systemctl is-active pijade.service 2>&1); PIJADE_RC=$?|PIJADE_STATUS=$(systemctl is-active pijade.service 2>\&1)|' /mnt/root/usr/local/sbin/pijade-boot-verify; }
r14() { cp /mnt/root/tmp/v3.bak /mnt/root/usr/local/sbin/pijade-boot-verify
        chmod 755 /mnt/root/usr/local/sbin/pijade-boot-verify; rm -f /mnt/root/tmp/v3.bak; }


# --- T7 persistent identity scheme (r3 fix) ---
MID=$D/pijade-machine-id.service
MSC=/mnt/root/usr/local/sbin/pijade-machine-id
VS=/mnt/root/usr/local/sbin/pijade-boot-verify

s15() { mv $MID /mnt/root/tmp/mid.bak; }
r15() { mv /mnt/root/tmp/mid.bak $MID; }
s16() { ln -sf /etc/systemd/system/pijade.service $D/multi-user.target.wants/pijade-machine-id.service; }
r16() { ln -sf /etc/systemd/system/pijade-machine-id.service $D/multi-user.target.wants/pijade-machine-id.service; }
s17() { ln -sf /lib/systemd/system/systemd-machine-id-commit.service $D/systemd-machine-id-commit.service; }
r17() { ln -sf /dev/null $D/systemd-machine-id-commit.service; }
s18() { cp $MSC /mnt/root/tmp/msc.bak; sed -i '/^mount --bind/d' $MSC; }
r18() { cp /mnt/root/tmp/msc.bak $MSC; chmod 755 $MSC; rm -f /mnt/root/tmp/msc.bak; }
s19() { cp $MSC /mnt/root/tmp/msc2.bak; sed -i 's|/dev/random|/dev/urandom|' $MSC; }
r19() { cp /mnt/root/tmp/msc2.bak $MSC; chmod 755 $MSC; rm -f /mnt/root/tmp/msc2.bak; }
s20() { sed -i '/^Before=pijade.service$/d' $MID; }
r20() { sed -i '/^RequiresMountsFor=/a Before=pijade.service' $MID; }
s21() { sed -i '/^Before=pijade.service$/a Requires=network.target' $MID; }
r21() { sed -i '/^Requires=network.target$/d' $MID; }
s22() { cp $VS /mnt/root/tmp/v4.bak
        sed -i 's|\[ "\$MACHINE_ID" != "\$STORED_ID" \]|[ -z "$MACHINE_ID" ]|' $VS; }
r22() { cp /mnt/root/tmp/v4.bak $VS; chmod 755 $VS; rm -f /mnt/root/tmp/v4.bak; }
s23() { sed -i '1s| ro$||' /mnt/boot/cmdline.txt; }
r23() { sed -i '1s|$| ro|' /mnt/boot/cmdline.txt; }



# --- T7 clock (r4 fix) ---
HWD=$D/fake-hwclock.service.d/pijade.conf
s24() { mv $HWD /mnt/root/tmp/hw.bak; }
r24() { mv /mnt/root/tmp/hw.bak $HWD; }
s25() { printf '[Service]\nExecStop=/sbin/fake-hwclock save\n' > $HWD; }
r25() { printf '[Service]\nExecStop=\n' > $HWD; }
s26() { ln -sf /dev/null $D/fake-hwclock.service; }
r26() { rm -f $D/fake-hwclock.service; }



# --- r5 fixes: image identity, full validation, D-Bus class, vendor wants ---
MSC2=/mnt/root/usr/local/sbin/pijade-machine-id
DBS=/mnt/root/usr/share/dbus-1/system-services
VW=/mnt/root/usr/lib/systemd/system/multi-user.target.wants

s27() { printf '0123456789abcdef0123456789abcdef\n' > /mnt/boot/pijade-machine-id; }
r27() { rm -f /mnt/boot/pijade-machine-id; }
s28() { cp $MSC2 /mnt/root/tmp/m5.bak; sed -i 's|wc -c < "$ID_FILE"|echo 33|' $MSC2; }
r28() { cp /mnt/root/tmp/m5.bak $MSC2; chmod 755 $MSC2; rm -f /mnt/root/tmp/m5.bak; }
s29() { printf '[D-BUS Service]\nName=org.freedesktop.fake\nSystemdService=dbus.service\n' \
          > $DBS/org.freedesktop.fake.service; }
r29() { rm -f $DBS/org.freedesktop.fake.service; }
s30() { ln -sf /lib/systemd/system/rsync.service $VW/rsync.service; }
r30() { rm -f $VW/rsync.service; }



# --- r6 fixes: direct Exec= activation and other activation directories ---
DBS2=/mnt/root/usr/share/dbus-1/system-services
DBS_ALT=/mnt/root/usr/local/share/dbus-1/system-services

s31() { printf '[D-BUS Service]\nName=org.freedesktop.fake2\nExec=/usr/sbin/sshd\nUser=root\n' \
          > $DBS2/org.freedesktop.fake2.service; }
r31() { rm -f $DBS2/org.freedesktop.fake2.service; }
s32() { mkdir -p $DBS_ALT
        printf '[D-BUS Service]\nName=org.freedesktop.fake3\nSystemdService=dbus.service\n' \
          > $DBS_ALT/org.freedesktop.fake3.service; }
r32() { rm -f $DBS_ALT/org.freedesktop.fake3.service
        rmdir $DBS_ALT /mnt/root/usr/local/share/dbus-1 2>/dev/null; }
s33() { printf '[D-BUS Service]\nName=org.freedesktop.fake4\n' > $DBS2/org.freedesktop.fake4.service; }
r33() { rm -f $DBS2/org.freedesktop.fake4.service; }


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
