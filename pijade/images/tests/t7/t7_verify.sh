# Contract: this independent checker remeasures the 14 offline gates in prepare-image.sh
# using its own expressions. Each check's rejection set covers the corresponding production
# gate's rejection set and is at least as strict.
# Do not add a rejection that cannot be traced to the production script's fstab/cmdline output
# or a gate's reject condition. Record such findings as outside the contract.
set +e
apk add --no-cache util-linux e2fsprogs >/dev/null 2>&1
mkdir -p /mnt/root /mnt/boot
R=/mnt/root; B=/mnt/boot
L1=$(losetup -o 545259520 --sizelimit 1954545664 -f --show /img); mount -o ro "$L1" $R
L2=$(losetup -o 8388608 --sizelimit 536870912 -f --show /img); mount -o ro "$L2" $B

FAIL=0
ok()   { echo "  OK  $1"; }
bad()  { echo "  ERROR   $1"; FAIL=$((FAIL+1)); }

MASKED="NetworkManager.service wpa_supplicant.service ModemManager.service
avahi-daemon.service avahi-daemon.socket bluetooth.service hciuart.service
ssh.service ssh.socket sshswitch.service regenerate_ssh_host_keys.service
triggerhappy.service triggerhappy.socket userconfig.service rpi-eeprom-update.service
udisks2.service systemd-timesyncd.service NetworkManager-wait-online.service
dphys-swapfile.service rsync.service NetworkManager-dispatcher.service
nm-priv-helper.service systemd-networkd.service systemd-networkd.socket
dbus-org.freedesktop.Avahi.service dbus-org.freedesktop.ModemManager1.service
dbus-org.freedesktop.timesync1.service dbus-org.freedesktop.nm-dispatcher.service
dbus-org.bluez.service dbus-fi.w1.wpa_supplicant1.service
getty@.service serial-getty@.service autovt@.service systemd-random-seed.service systemd-machine-id-commit.service"

# Do not hardcode PARTUUID: the main script computes it from the MBR disk ID (bytes 440-443,
# little-endian). Fixed text would prevent checking whether both sides measure the same fact.
DISK_ID=$(od -An -tx1 -j 440 -N 4 /img | awk '{ print $4 $3 $2 $1 }')
case "$DISK_ID" in
  ????????) ;;
  *) bad "could not read MBR disk ID"; DISK_ID="00000000" ;;
esac
ROOT_PU="PARTUUID=$DISK_ID-02"
BOOT_PU="PARTUUID=$DISK_ID-01"

echo "=== 1) MASK ==="
for u in $MASKED; do
  t=$(readlink "$R/etc/systemd/system/$u" 2>/dev/null)
  [ "$t" = "/dev/null" ] && ok "$u" || bad "$u (target: ${t:-MISSING})"
done

echo "=== 2) ALLOWLIST ==="
got=$( { find "$R/etc/systemd/system/multi-user.target.wants" -mindepth 1 -maxdepth 1 -print 2>/dev/null
         find "$R/usr/local/lib/systemd/system/multi-user.target.wants" -mindepth 1 -maxdepth 1 -print 2>/dev/null
         find "$R/usr/lib/systemd/system/multi-user.target.wants" -mindepth 1 -maxdepth 1 -print 2>/dev/null; } \
       | sed 's|.*/||' | sort | tr '\n' ' ')
exp="dbus.service getty.target pijade-boot-verify.service pijade-machine-id.service pijade.service systemd-ask-password-wall.path systemd-logind.service systemd-update-utmp-runlevel.service systemd-user-sessions.service "
[ "$got" = "$exp" ] && ok "multi-user.target.wants = $got" || bad "multi-user.target.wants = [$got], expected [$exp]"

echo "=== 3) RESIDUAL LINK (class) ==="
resid=0
for u in $MASKED; do
  hits=$(find "$R/etc/systemd/system" -type l -path "*.wants/$u" 2>/dev/null)
  [ -n "$hits" ] && { bad "residual link: $(echo $hits | sed "s|$R||g")"; resid=1; }
done
[ "$resid" = 0 ] && ok "no residual wants links to masked units"

echo "=== 4) EMPTY DIRECTORIES ==="
for d in sockets.target.wants bluetooth.target.wants graphical.target.wants \
         network-online.target.wants timers.target.wants dev-serial1.device.wants; do
  n=$(find "$R/etc/systemd/system/$d" -mindepth 1 -maxdepth 1 -type l 2>/dev/null | wc -l)
  [ "$n" -eq 0 ] && ok "$d empty or missing" || bad "$d contains $n symlinks"
done

echo "=== 5) ALIAS (by target) ==="
find "$R/etc/systemd/system" -maxdepth 1 -type l | while read f; do
  tgt=$(readlink "$f"); base=$(basename "$tgt")
  case " $MASKED " in *" $base "*) echo "  ERROR   $(basename $f) -> $tgt (points to masked unit)";; esac
done | tee /tmp/alias.out
[ -s /tmp/alias.out ] && FAIL=$((FAIL+1)) || ok "no root directory symlink points to a masked unit"

echo "=== 6) SYSV (class) ==="
sysv_bad=0
for f in "$R"/etc/rc[2-5].d/S*; do
  [ -L "$f" ] || continue
  n=$(basename "$f"); n=${n#S??}; n=${n%.sh}
  # Do NOT use -f: sudo.service is the distribution's /dev/null mask; -e also fails for /lib
  # symlinks unresolved in the container. What matters is the path's presence in the unit directory.
  P="$R/usr/lib/systemd/system/$n.service"
  if [ -e "$P" ] || [ -L "$P" ]; then :; else
    bad "sysv $(basename $f) -> $n.service native unit MISSING"; sysv_bad=1
  fi
done
[ "$sysv_bad" = 0 ] && ok "native unit exists for every rc[2-5].d/S*"

echo "=== 7) FSTAB ==="
F="$R/etc/fstab"
c_root=$(awk '$1 !~ /^#/ && $2=="/"{n++} END{print n+0}' "$F")
c_boot=$(awk '$1 !~ /^#/ && $2=="/boot/firmware"{n++} END{print n+0}' "$F")
c_puid=$(awk '$1 !~ /^#/ && $1 ~ /^PARTUUID=/{n++} END{print n+0}' "$F")
[ "$c_root" = 1 ] && ok "root line exactly once" || bad "root line $c_root times"
[ "$c_boot" = 1 ] && ok "/boot/firmware exactly once" || bad "/boot/firmware $c_boot times"
[ "$c_puid" = 2 ] && ok "exactly two PARTUUID lines" || bad "PARTUUID lines $c_puid"
root_ok=$(awk -v id="$ROOT_PU" '
  $1 !~ /^#/ && $1==id && $2=="/" {
    n++; split($4, o, ","); for (i in o) if (o[i]=="ro") ro=1
  } END {print (n==1 && ro) ? 1 : 0}' "$F")
[ "$root_ok" = 1 ] && ok "root PARTUUID correct and ro" || bad "root PARTUUID incorrect or root NOT ro"
boot_ok=$(awk -v id="$BOOT_PU" '
  $1 !~ /^#/ && $1==id && $2=="/boot/firmware" {
    n++; has_ro=0; split($4, o, ","); for (i in o) if (o[i]=="ro") has_ro=1
    if (!has_ro) writable=1
  } END {print (n==1 && writable) ? 1 : 0}' "$F")
[ "$boot_ok" = 1 ] && ok "boot PARTUUID correct and writable" || bad "boot PARTUUID incorrect or ro"
for m in /tmp /var/tmp /var/log; do
  case "$m" in
    /tmp|/var/tmp) opts=defaults,nosuid,nodev,mode=1777 ;;
    /var/log) opts=defaults,nosuid,nodev,mode=0755 ;;
  esac
  n=$(awk -v m="$m" -v opts="$opts" '
    $1 !~ /^#/ && $2==m {rows++; if ($1=="tmpfs" && $3=="tmpfs" && $4==opts) exact++}
    END{print (rows==1 && exact==1) ? 1 : 0}' "$F")
  [ "$n" = 1 ] && ok "tmpfs $m exactly once" || bad "tmpfs $m $n times"
done
swap_rows=$(awk '$1 !~ /^#/ && $3=="swap"{n++} END{print n+0}' "$F")
[ "$swap_rows" = 0 ] && ok "no swap line" || bad "fstab contains swap line"

echo "=== 8) CMDLINE ==="
C="$B/cmdline.txt"
init_n=$(awk '{for (i=1; i<=NF; i++) if ($i ~ /^init=/) n++} END{print n+0}' "$C")
[ "$init_n" = 0 ] && ok "no init=" || bad "cmdline contains init="
root_n=$(awk '{for (i=1; i<=NF; i++) if ($i ~ /^root=/) n++} END{print n+0}' "$C")
root_ok=$(awk -v root="root=$ROOT_PU" '{for (i=1; i<=NF; i++) if ($i==root) n++} END{print n+0}' "$C")
[ "$root_ok" = 1 ] && ok "root=PARTUUID correct and unique" || bad "root=PARTUUID count $root_ok"
[ "$root_n" = 1 ] && ok "single root= parameter" || bad "$root_n root= parameters"
ro_n=$(awk '{for (i=1; i<=NF; i++) if ($i=="ro") n++} END{print n+0}' "$C")
rw_n=$(awk '{for (i=1; i<=NF; i++) if ($i=="rw") n++} END{print n+0}' "$C")
[ "$ro_n" = 1 ] && ok "cmdline ro exactly once" || bad "cmdline ro count $ro_n"
[ "$rw_n" = 0 ] && ok "no cmdline rw" || bad "cmdline rw count $rw_n"

echo "=== 9) DEFAULT.TARGET ==="
dt=$(readlink "$R/etc/systemd/system/default.target" 2>/dev/null)
if [ -z "$dt" ]; then bad "default.target symlink missing"
else
  case "$dt" in
    /*) real=$(readlink -f "$R$dt" 2>/dev/null) ;;
    *) real=$(readlink -f "$R/etc/systemd/system/$dt" 2>/dev/null) ;;
  esac
  case "$real" in
    "$R"/*) [ -f "$real" ] && [ "${real##*/}" = "multi-user.target" ] \
      && ok "default.target -> $dt (file exists)" \
      || bad "default.target -> $dt (unresolved or incorrect)" ;;
    *) bad "default.target resolves outside image root" ;;
  esac
fi

echo "=== 10) VERIFICATION UNIT ==="
U="$R/etc/systemd/system/pijade-boot-verify.service"
if [ -f "$U" ]; then
  for k in "Type=oneshot" "RequiresMountsFor=/boot/firmware" "After=pijade.service" "WantedBy=multi-user.target"; do
    grep -qx "$k" "$U" && ok "unit: $k" || bad "unit: $k MISSING"
  done
  grep -Eq '^After=([^[:space:]]+[[:space:]]+)*multi-user\.target([[:space:]]|$)' "$U" \
    && bad "unit: After= contains multi-user.target" || ok "unit not ordered after multi-user.target"
  ex_count=$(awk '/^ExecStart=/{n++} END{print n+0}' "$U")
  ex=$(awk '/^ExecStart=/{sub(/^ExecStart=/, ""); print}' "$U")
  [ "$ex_count" = 1 ] && [ "$ex" = "/usr/local/sbin/pijade-boot-verify" ] && [ -x "$R$ex" ] \
    && ok "ExecStart $ex present and executable" \
    || bad "ExecStart count=$ex_count value=[$ex] incorrect or not executable"
  lt=$(readlink "$R/etc/systemd/system/multi-user.target.wants/pijade-boot-verify.service" 2>/dev/null)
  [ "$lt" = "/etc/systemd/system/pijade-boot-verify.service" ] \
    && ok "wants symlink correct" || bad "wants symlink: $lt"
  pt=$(readlink "$R/etc/systemd/system/multi-user.target.wants/pijade.service" 2>/dev/null)
  [ "$pt" = "/etc/systemd/system/pijade.service" ] \
    && ok "pijade.service wants symlink correct" || bad "pijade.service wants symlink: $pt"
else
  bad "pijade-boot-verify.service MISSING"
fi

echo "=== 11) VERIFICATION SCRIPT CONTENTS ==="
V="$R/usr/local/sbin/pijade-boot-verify"
if [ -f "$V" ]; then
  for c in "swapon --show" "findmnt -no OPTIONS /" "ss -H -tlnu" \
           "systemctl is-active pijade.service" "cat /etc/machine-id" \
           "cat /proc/sys/kernel/random/boot_id" "vcgencmd get_throttled" \
           "under-voltage|voltage|throttl|regulator" \
           "/sys/class/thermal/thermal_zone*/temp"; do
    grep -Fq "$c" "$V" && ok "measurement present: $c" || bad "measurement MISSING: $c"
  done
  rc_ok=1; rc_missing=""
  for c in \
    'SWAP_OUTPUT=$(swapon --show); SWAP_RC=$?' 'if [ "$SWAP_RC" -ne 0 ]; then' \
    'ROOT_OPTIONS=$(findmnt -no OPTIONS /); ROOT_RC=$?' 'if [ "$ROOT_RC" -ne 0 ]; then' \
    'LISTEN_OUTPUT=$(ss -H -tlnu); LISTEN_RC=$?' 'if [ "$LISTEN_RC" -ne 0 ]; then' \
    'PIJADE_STATUS=$(systemctl is-active pijade.service 2>&1); PIJADE_RC=$?' \
    'if [ "$PIJADE_RC" -eq 0 ] && [ "$PIJADE_STATUS" = "active" ]; then' \
    'MACHINE_ID=$(cat /etc/machine-id); MACHINE_RC=$?' 'if [ "$MACHINE_RC" -ne 0 ]; then' \
    'STORED_ID=$(cat "$ID_FILE"); STORED_RC=$?' 'elif [ "$STORED_RC" -ne 0 ]; then' \
    'BOOT_ID=$(cat /proc/sys/kernel/random/boot_id); BOOT_ID_RC=$?' \
    'if [ "$BOOT_ID_RC" -ne 0 ] || [ -z "$BOOT_ID" ]; then' \
    'THROTTLED=$(vcgencmd get_throttled 2>&1); THROTTLED_RC=$?' \
    'if [ "$THROTTLED_RC" -ne 0 ]; then' \
    'DMESG_OUT=$(dmesg 2>&1); DMESG_RC=$?' \
    'if [ "$DMESG_RC" -ne 0 ]; then'; do
    sed 's/^[[:space:]]*//' "$V" | grep -Fxq "$c" || { rc_ok=0; rc_missing="$rc_missing [$c]"; }
  done
  [ "$rc_ok" -eq 1 ] && ok "measurement exit-code contracts complete" \
    || bad "exit-code contract incomplete:$rc_missing"
  result_ok=$(awk '
    /^[[:space:]]*RESULT=/ {count++}
    previous ~ /echo "T7 VERIFICATION: OK"/ && $0 ~ /^[[:space:]]*RESULT=0[[:space:]]*$/ {success=1}
    previous ~ /echo "T7 VERIFICATION: ERROR"/ && $0 ~ /^[[:space:]]*RESULT=1[[:space:]]*$/ {failure=1}
    {previous=$0}
    END{print (count==2 && success && failure) ? 1 : 0}' "$V")
  exit_ok=$(awk '
    /^[[:space:]]*exit([[:space:]]|$)/ {count++; if ($0 ~ /^[[:space:]]*exit "\$RESULT"[[:space:]]*$/) exact++}
    END{print (count==1 && exact==1) ? 1 : 0}' "$V")
  grep -Fq 'FAIL=$((FAIL + 1))' "$V" && grep -Fq 'if [ "$FAIL" -eq 0 ]' "$V" \
    && [ "$result_ok" -eq 1 ] && [ "$exit_ok" -eq 1 ] \
    && ok "verdict and exit derived from error counter" \
    || bad "verdict or exit not derived from error counter"

  # Independent view: preparation searches for measurement COMMAND strings; here we count the
  # script's OUTPUT labels. Two different views of the same fact force a regression to
  # satisfy both sides at once.
  for pair in "rng (info)|4" "boot_id (info)|2" "power (info)|4" "temperature (info)|1"; do
    label=${pair%|*}; expected=${pair#*|}
    found=$(grep -c "$label" "$V")
    [ "$found" = "$expected" ] && ok "info line: $label x$found" \
      || bad "info line: $label x$found (expected $expected)"
  done

  # Info lines MUST NOT touch FAIL. Design decision: boot_id has no comparison reference
  # within a single boot, and get_throttled carries sticky bits; making these gates would
  # show a failure at each boot after using a weak supply once, drowning out the real gates.
  # There is no production gate counterpart because it checks measurement PRESENCE;
  # this check pins the decision itself.
  info_fail=$(awk '
    index($0,"BOOT_ID=$(cat /proc/sys/kernel/random/boot_id)"){f=1}
    index($0,"for device in"){f=0}
    f && index($0,"FAIL=$((FAIL + 1))"){n++}
    END{print n+0}' "$V")
  [ "$info_fail" = 0 ] && ok "info lines do not touch error counter" \
    || bad "info lines increment error counter ($info_fail locations)"
else
  bad "pijade-boot-verify script MISSING"
fi

echo "=== 12) TOOLS ==="
# The list is the UNION of two gates: GATE 11 holds boot-verify's 17 tools, GATE 12 the identity
# script's seven tools (mount, mv, head, od, tr, wc, sync). Only GATE 12 holds mount and mv;
# the others appear in both. The checker must hold the union because the parity contract
# requires its rejection set to COVER the gates' rejection set.
for p in /sbin/swapon /bin/findmnt /bin/ss /bin/systemctl /bin/cat /bin/sync /bin/date \
         /bin/mount /bin/mv /usr/bin/head /usr/bin/od /usr/bin/tr /bin/grep /usr/bin/wc \
         /usr/bin/timeout /usr/bin/cut /usr/bin/dmesg /usr/bin/vcgencmd /usr/bin/tail; do
  b=${p##*/}
  [ -x "$R$p" ] && ok "$b" || bad "$b MISSING or not executable: $p"
done

echo "=== 13) PERSISTENT DEVICE IDENTITY ==="
# Independent measurement: checks the same fact with DIFFERENT expressions from preparation gates.
MSC="$R/usr/local/sbin/pijade-machine-id"
MID="$R/etc/systemd/system/pijade-machine-id.service"
[ -x "$MSC" ] && ok "identity script executable" || bad "identity script missing/not executable"
grep -q '^mount --bind "\$ID_FILE" /etc/machine-id$' "$MSC" 2>/dev/null && ok "script establishes bind mount" || bad "script does not establish bind mount"
grep -q "/dev/random" "$MSC" 2>/dev/null && ok "blocking entropy source" || bad "no blocking source"
grep -q "/dev/urandom" "$MSC" 2>/dev/null && bad "uses urandom" || ok "does not use urandom"
grep -q "ID_FILE.tmp" "$MSC" 2>/dev/null && ok "temporary file + rename" || bad "no atomic write"
grep -q 'wc -c' "$MSC" 2>/dev/null && ok "file size validated" || bad "size not validated"
grep -q 'wc -l' "$MSC" 2>/dev/null && ok "line count validated" || bad "line count not validated"
[ -f "$MID" ] && ok "identity unit present" || bad "identity unit missing"
for directive in '^Before=pijade\.service$' '^Type=oneshot$' '^RequiresMountsFor=/boot/firmware$' \
                 '^TimeoutStartSec=' '^ExecStart=/usr/local/sbin/pijade-machine-id$'; do
  grep -Eq "$directive" "$MID" 2>/dev/null && ok "identity unit directive: $directive" \
    || bad "identity unit missing directive: $directive"
done
grep -Eq "^(Requires|BindsTo|Requisite)=" "$MID" 2>/dev/null && bad "blocks boot" || ok "does not block boot"
grep -Eq '^(Requires|Wants|After)=.*pijade-machine-id' "$R/etc/systemd/system/pijade.service" 2>/dev/null \
  && bad "product service depends on identity unit" || ok "product service does not depend on identity unit"
[ "$(readlink "$R/etc/systemd/system/multi-user.target.wants/pijade-machine-id.service" 2>/dev/null)" \
  = "/etc/systemd/system/pijade-machine-id.service" ] && ok "enabled" || bad "not enabled"
[ -L "$R/etc/systemd/system/systemd-machine-id-commit.service" ] && \
  [ "$(readlink "$R/etc/systemd/system/systemd-machine-id-commit.service")" = "/dev/null" ] \
  && ok "commit unit masked" || bad "commit unit not masked"
# Verification must distinguish a temporary ID: demonstrate that it compares both files.
grep -q 'MACHINE_ID" != "\$STORED_ID' "$R/usr/local/sbin/pijade-boot-verify" 2>/dev/null \
  && ok "verification compares with source file" || bad "verification only checks for nonempty content"
# The image MUST NOT carry a persistent ID, or every card would receive the same one.
[ -e "$B/pijade-machine-id" ] || [ -e "$B/pijade-machine-id.tmp" ] \
  && bad "image contains preset identity (all cards would share it)" \
  || ok "image contains no preset identity"
grep -q 'wc -c < "\$ID_FILE"' "$MSC" 2>/dev/null && ok "full file size validated" \
  || bad "full file size not validated"
grep -q 'wc -l < "\$ID_FILE"' "$MSC" 2>/dev/null && ok "full line count validated" \
  || bad "full line count not validated"

echo "=== 14) CLOCK ==="
HWD="$R/etc/systemd/system/fake-hwclock.service.d/pijade.conf"
[ -f "$HWD" ] && ok "shutdown step drop-in present" || bad "drop-in missing"
[ "$(sed -n '/^ExecStop=/p' "$HWD" 2>/dev/null)" = "ExecStop=" ] \
  && ok "list reset with single empty ExecStop" || bad "ExecStop list not fully reset"
[ -L "$R/etc/systemd/system/fake-hwclock.service" ] && bad "unit masked (load lost)" \
  || ok "unit not masked"
[ -r "$R/etc/fake-hwclock.data" ] && ok "data file readable" || bad "data file missing"
# Time zone: ctime_r applies the local zone, but the OTP screen prints fixed "UTC" beside it
# (main/ui/otpauth.c:318); leaving London makes the label wrong during daylight saving.
[ "$(cat "$R/etc/timezone" 2>/dev/null)" = "Etc/UTC" ] \
  && ok "time zone Etc/UTC" || bad "time zone is not Etc/UTC"
[ "$(readlink "$R/etc/localtime")" = "/usr/share/zoneinfo/Etc/UTC" ] \
  && ok "localtime points to Etc/UTC" || bad "localtime does not point to Etc/UTC"
grep -q "clock (info)" "$R/usr/local/sbin/pijade-boot-verify" 2>/dev/null \
  && ok "clock reported in log" || bad "clock not reported"
# The clock must NOT be a GATE: requiring correct time without an RTC would break every boot.
# Exclude comments: exactly one CODE line may contain "clock", and it must be an echo.
# (The previous pattern matched my own comment about counting the clock as an ERROR.)
CLOCK_CODE=$(grep -n "clock" "$R/usr/local/sbin/pijade-boot-verify" 2>/dev/null | grep -v ":[[:space:]]*#" || true)
[ "$(printf '%s\n' "$CLOCK_CODE" | grep -c .)" = "1" ] && \
  printf '%s' "$CLOCK_CODE" | grep -q 'echo' && ok "clock only reported, not a gate" \
  || bad "clock appears multiple times in code path: $CLOCK_CODE"

echo "=== 15) D-BUS ACTIVATION SURFACE ==="
# Scope must MATCH gate 14 or the "independent check" would not verify the fix:
# (a) ALL */dbus-1/system-services directories under root, not just one;
# (b) files without SystemdService= also count as launchable (dbus-daemon runs Exec=
#     directly); do not skip them.
DB_DIRS=$(find "$R" -type d -path '*/dbus-1/system-services' 2>/dev/null)
[ -n "$DB_DIRS" ] && ok "activation directories found: $(echo "$DB_DIRS" | wc -l | tr -d ' ')" \
  || bad "no activation directory"
db_bad=0
for dir in $DB_DIRS; do
  for a in "$dir"/*.service; do
    [ -e "$a" ] || continue
    n=$(basename "$a" .service)
    declared=$(sed -n 's/^Name=[[:space:]]*//p' "$a")
    [ "$(grep -c '^Name=' "$a")" -eq 1 ] && [ "$declared" = "$n" ] \
      || { bad "filename and Name= mismatch: $n -> $declared"; db_bad=1; continue; }
    # Each action field must be UNIQUE: if an allowed line is followed by a different target,
    # "head -1" misses the second action in the apparently allowed file. Same boundary as gate 14.
    sr=$(grep -c '^SystemdService=' "$a" || true); er=$(grep -c '^Exec=' "$a" || true)
    [ "$sr" -le 1 ] && [ "$er" -le 1 ] \
      || { bad "multiple action fields: $n (SystemdService=$sr Exec=$er)"; db_bad=1; continue; }
    u=$(sed -n 's/^SystemdService=[[:space:]]*//p' "$a" | head -1)
    if [ -z "$u" ]; then
      e=$(sed -n 's/^Exec=[[:space:]]*//p' "$a" | head -1)
      if [ -z "$e" ]; then bad "neither SystemdService nor Exec: $n"; db_bad=1; continue; fi
      st="active"; action="Exec=$e"
    else
      action="SystemdService=$u"
      st="none"
      for d in "$R/etc/systemd/system" "$R/run/systemd/system" \
               "$R/usr/local/lib/systemd/system" "$R/usr/lib/systemd/system"; do
        if [ -L "$d/$u" ] && [ "$(readlink "$d/$u")" = "/dev/null" ]; then st="masked"; break; fi
        if [ -e "$d/$u" ] || [ -L "$d/$u" ]; then st="active"; break; fi
      done
    fi
    case "$n" in
      org.freedesktop.PolicyKit1) expected="SystemdService=polkit.service" ;;
      org.freedesktop.hostname1) expected="SystemdService=dbus-org.freedesktop.hostname1.service" ;;
      org.freedesktop.locale1) expected="SystemdService=dbus-org.freedesktop.locale1.service" ;;
      org.freedesktop.login1) expected="SystemdService=dbus-org.freedesktop.login1.service" ;;
      org.freedesktop.timedate1) expected="SystemdService=dbus-org.freedesktop.timedate1.service" ;;
      org.freedesktop.systemd1) expected="Exec=/bin/false" ;;
      *) expected="" ;;
    esac
    [ -z "$expected" ] || [ "$action" = "$expected" ] \
      || { bad "allowed name has changed action: $n -> $action"; db_bad=1; continue; }
    [ "$st" = "active" ] || continue
    [ -n "$expected" ] || { bad "active activation outside allowlist: $n -> $action"; db_bad=1; }
  done
done
[ "$db_bad" -eq 0 ] && ok "all active activations within allowlist" || true
NW=$(sed -n 's/^SystemdService=[[:space:]]*//p' \
     "$R/usr/share/dbus-1/system-services/org.freedesktop.network1.service" 2>/dev/null | head -1)
if [ -n "$NW" ]; then
  found=0
  for d in "$R/etc/systemd/system" "$R/run/systemd/system" \
           "$R/usr/local/lib/systemd/system" "$R/usr/lib/systemd/system"; do
    { [ -e "$d/$NW" ] || [ -L "$d/$NW" ]; } && found=1
  done
  [ "$found" -eq 0 ] && ok "networkd alias unit missing, cannot launch" \
    || bad "networkd alias unit appeared: $NW"
fi

umount $B; umount $R; losetup -d "$L2"; losetup -d "$L1"
echo
echo "TOTAL ERRORS: $FAIL"
if [ "$FAIL" -eq 0 ]; then
  echo "INDEPENDENT VERIFICATION: OK"; exit 0
else
  echo "INDEPENDENT VERIFICATION: ERROR"; exit 1
fi
