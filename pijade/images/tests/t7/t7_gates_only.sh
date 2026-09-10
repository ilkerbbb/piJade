set -e
# Test copy of the main script T7 gates. The refresh boundary is not an ambiguous "first GATE 1":
# in prepare-image.sh it starts at `echo "--- T7 offline gates ---"`, followed by
# `# GATE 1: every mask`, and ends before `echo "=== 9) UNMOUNT ==="`. MASKED_UNITS comes from
# the same script's `echo "=== 8) T7: HARDENING ==="` block; never include the package GATE 1.
apk add --no-cache util-linux >/dev/null 2>&1
mkdir -p /mnt/root /mnt/boot
cleanup() { umount /mnt/boot 2>/dev/null; umount /mnt/root 2>/dev/null; losetup -d "$L2" 2>/dev/null; losetup -d "$L1" 2>/dev/null; }
trap cleanup EXIT
L1=$(losetup -o 545259520 --sizelimit 1954545664 -f --show /img); mount -o ro "$L1" /mnt/root
L2=$(losetup -o 8388608 --sizelimit 536870912 -f --show /img); mount -o ro "$L2" /mnt/boot
SYSTEMD_DIR=/mnt/root/etc/systemd/system
MASKED_UNITS="
NetworkManager.service
wpa_supplicant.service
ModemManager.service
avahi-daemon.service
avahi-daemon.socket
bluetooth.service
hciuart.service
ssh.service
ssh.socket
sshswitch.service
regenerate_ssh_host_keys.service
triggerhappy.service
triggerhappy.socket
userconfig.service
rpi-eeprom-update.service
udisks2.service
systemd-timesyncd.service
NetworkManager-wait-online.service
dphys-swapfile.service
rsync.service
NetworkManager-dispatcher.service
nm-priv-helper.service
systemd-networkd.service
systemd-networkd.socket
dbus-org.freedesktop.Avahi.service
dbus-org.freedesktop.ModemManager1.service
dbus-org.freedesktop.timesync1.service
dbus-org.freedesktop.nm-dispatcher.service
dbus-org.bluez.service
dbus-fi.w1.wpa_supplicant1.service
getty@.service
serial-getty@.service
autovt@.service
systemd-random-seed.service
systemd-machine-id-commit.service
"
echo "--- T7 offline gates ---"
# Do not hardcode PARTUUID; read the MBR disk ID's little-endian bytes from the image.
DISK_ID=$(od -An -tx1 -j 440 -N 4 /img | awk '{ print $4 $3 $2 $1 }')
case "$DISK_ID" in
    ????????) ;;
    *) echo "ERROR: could not read MBR disk ID"; exit 1 ;;
esac
BOOT_PARTUUID="$DISK_ID-01"
ROOT_PARTUUID="$DISK_ID-02"

# GATE 1: every mask must point exactly to /dev/null.
for unit in $MASKED_UNITS; do
    if [ "$(readlink "$SYSTEMD_DIR/$unit" 2>/dev/null || true)" != "/dev/null" ]; then
        echo "ERROR: $unit not masked"; exit 1
    fi
done
echo "T7 GATE 1 MASK: PASSED"

# GATE 2: ALL persistent wants directories feeding multi-user must stay within the allowlist.
# Previously only /etc was checked, contradicting the claim "only the product service remains":
# the vendor directory usr/lib/systemd/system/multi-user.target.wants contributes six more entries
# (dbus.service, getty.target, systemd-logind.service, systemd-user-sessions.service,
# systemd-update-utmp-runlevel.service, systemd-ask-password-wall.path). These are systemd's own
# local units; none listens on TCP/UDP (also measured with "ss" at boot), and dbus.service must
# remain because gate 5 already checks the alias surface of masked units.
# The list is NOW VISIBLE. A new unit in the vendor preset makes the gate fail.
# Count all three persistent unit search paths: /etc, /usr/local/lib and /usr/lib. /run is
# tmpfs at boot, hiding image contents; /lib is a usr/lib symlink, so do not count it twice.
# Each find needs "|| true": this script uses set -e and this image LACKS /usr/local/lib.
# find returns 1 for an absent directory; under set -e the group stops there and never counts
# later sources. This happened on 2026-08-27 in the first dynamic run after handoff: gate 2 saw
# only /etc, assumed the vendor directory empty, and failed. A minimal shell example confirmed it;
# shellcheck misses this class. An empty source contributes zero to the allowlist; it is no error.
{
    find "$SYSTEMD_DIR/multi-user.target.wants" -mindepth 1 -maxdepth 1 -print 2>/dev/null || true
    find /mnt/root/usr/local/lib/systemd/system/multi-user.target.wants \
        -mindepth 1 -maxdepth 1 -print 2>/dev/null || true
    find /mnt/root/usr/lib/systemd/system/multi-user.target.wants \
        -mindepth 1 -maxdepth 1 -print 2>/dev/null || true
} | sed 's|.*/||' | sort > /tmp/t7-wants-actual
printf '%s\n' pijade-boot-verify.service pijade-machine-id.service pijade.service \
    dbus.service getty.target systemd-logind.service systemd-user-sessions.service \
    systemd-update-utmp-runlevel.service systemd-ask-password-wall.path \
    | sort > /tmp/t7-wants-expected
if ! cmp -s /tmp/t7-wants-actual /tmp/t7-wants-expected; then
    echo "ERROR: multi-user.target.wants outside allowlist"
    cat /tmp/t7-wants-actual
    exit 1
fi
echo "T7 GATE 2 ALLOWLIST: PASSED"

# GATE 3: no masked unit name may remain as a symlink in any wants directory.
# systemd unit and wants directory names contain no raw spaces; unit names escape spaces.
# shellcheck disable=SC2044
for wants_dir in $(find "$SYSTEMD_DIR" -type d -name '*.wants'); do
    for unit in $MASKED_UNITS; do
        if [ -L "$wants_dir/$unit" ]; then
            echo "ERROR: residual wants link: $wants_dir/$unit"; exit 1
        fi
    done
done
echo "T7 GATE 3 RESIDUAL LINK: PASSED"

# GATE 4: no symlink may remain in the measured wants directories that must be empty.
for wants_name in sockets.target.wants bluetooth.target.wants graphical.target.wants \
                  network-online.target.wants timers.target.wants dev-serial1.device.wants; do
    if find "$SYSTEMD_DIR/$wants_name" -mindepth 1 -maxdepth 1 -type l | grep -q .; then
        echo "ERROR: $wants_name still contains symlink"; exit 1
    fi
done
echo "T7 GATE 4 EMPTY DIRECTORY: PASSED"

# GATE 5: no root alias may point directly to a masked canonical unit.
# Do not decide by target spelling: /lib, /usr/lib and relative paths can name the same unit.
# Measure the unit basename at the end of the symlink target.
for alias_path in "$SYSTEMD_DIR"/*; do
    [ -L "$alias_path" ] || continue
    alias_target=$(readlink "$alias_path")
    alias_unit=${alias_target##*/}
    for unit in $MASKED_UNITS; do
        if [ "$alias_unit" = "$unit" ]; then
            echo "ERROR: alias bypasses mask: $alias_path"; exit 1
        fi
    done
done
echo "T7 GATE 5 ALIAS: PASSED"

# GATE 6: no enabled sysv job without a native unit may remain.
for sysv_link in /mnt/root/etc/rc[2-5].d/S*; do
    [ -L "$sysv_link" ] || continue
    sysv_name=${sysv_link##*/}
    sysv_name=${sysv_name#S??}
    sysv_name=${sysv_name%.sh}
    # Do NOT use -f: sudo.service is the distribution's own mask, a /dev/null symlink; -f does not
    # treat it as a regular file and wrongly fails the gate. Nor is -e alone enough: a symlink to
    # /lib/systemd/... cannot resolve in the container root (the same trap is noted in step 6).
    # What matters is the path's PRESENCE in the unit directory; a masked unit also stops
    # the generator.
    sysv_unit="/mnt/root/usr/lib/systemd/system/$sysv_name.service"
    if [ ! -e "$sysv_unit" ] && [ ! -L "$sysv_unit" ]; then
        echo "ERROR: sysv job without native unit: $sysv_link"; exit 1
    fi
done
echo "T7 GATE 6 SYSV: PASSED"

# GATE 7: fstab must contain PARTUUID, read-only root, writable FAT and three tmpfs lines.
ROOT_ROWS=$(awk '$1 !~ /^#/ && $2 == "/" { n++ } END { print n+0 }' /mnt/root/etc/fstab)
ROOT_OK=$(awk -v id="PARTUUID=$ROOT_PARTUUID" '
    $1 !~ /^#/ && $1 == id && $2 == "/" {
        n++; split($4, opts, ","); for (i in opts) if (opts[i] == "ro") ro=1
    } END { print (n == 1 && ro) ? 1 : 0 }' /mnt/root/etc/fstab)
BOOT_ROWS=$(awk '$1 !~ /^#/ && $2 == "/boot/firmware" { n++ } END { print n+0 }' /mnt/root/etc/fstab)
BOOT_OK=$(awk -v id="PARTUUID=$BOOT_PARTUUID" '
    $1 !~ /^#/ && $1 == id && $2 == "/boot/firmware" {
        n++; has_ro=0; split($4, opts, ","); for (i in opts) if (opts[i] == "ro") has_ro=1
        if (!has_ro) writable=1
    } END { print (n == 1 && writable) ? 1 : 0 }' /mnt/root/etc/fstab)
TMP_ROWS=$(awk '
    $1 !~ /^#/ && ($2 == "/tmp" || $2 == "/var/tmp" || $2 == "/var/log") {
        count[$2]++
        if ($1 == "tmpfs" && $3 == "tmpfs" &&
            (($2 == "/tmp" && $4 == "defaults,nosuid,nodev,mode=1777") ||
             ($2 == "/var/tmp" && $4 == "defaults,nosuid,nodev,mode=1777") ||
             ($2 == "/var/log" && $4 == "defaults,nosuid,nodev,mode=0755"))) exact[$2]++
    }
    END { print (count["/tmp"] == 1 && count["/var/tmp"] == 1 && count["/var/log"] == 1 &&
                  exact["/tmp"] == 1 && exact["/var/tmp"] == 1 && exact["/var/log"] == 1) ? 1 : 0 }
' /mnt/root/etc/fstab)
SWAP_ROWS=$(awk '$1 !~ /^#/ && $3 == "swap" { n++ } END { print n+0 }' /mnt/root/etc/fstab)
PARTUUID_ROWS=$(awk '$1 !~ /^#/ && $1 ~ /^PARTUUID=/ { n++ } END { print n+0 }' /mnt/root/etc/fstab)
if [ "$ROOT_ROWS" -ne 1 ] || [ "$ROOT_OK" -ne 1 ] || \
   [ "$BOOT_ROWS" -ne 1 ] || [ "$BOOT_OK" -ne 1 ] || \
   [ "$TMP_ROWS" -ne 1 ] || [ "$SWAP_ROWS" -ne 0 ] || [ "$PARTUUID_ROWS" -ne 2 ]; then
    echo "ERROR: fstab does not meet T7 requirements"; exit 1
fi
echo "T7 GATE 7 FSTAB: PASSED"

# GATE 8: no firstboot init path; the computed root PARTUUID must be the sole root parameter.
INIT_ARGS=$(awk '{ for (i=1; i<=NF; i++) if ($i ~ /^init=/) n++ } END { print n+0 }' /mnt/boot/cmdline.txt)
ROOT_ARGS=$(awk '{ for (i=1; i<=NF; i++) if ($i ~ /^root=/) n++ } END { print n+0 }' /mnt/boot/cmdline.txt)
ROOT_ARG_OK=$(awk -v root="root=PARTUUID=$ROOT_PARTUUID" '
    { for (i=1; i<=NF; i++) if ($i == root) n++ } END { print n+0 }' /mnt/boot/cmdline.txt)
# ro/rw: the entire identity scheme relies on root being read-only when PID1 starts.
RO_ARGS=$(tr ' ' '\n' < /mnt/boot/cmdline.txt | grep -cx 'ro' || true)
RW_ARGS=$(tr ' ' '\n' < /mnt/boot/cmdline.txt | grep -cx 'rw' || true)
if [ "$INIT_ARGS" -ne 0 ] || [ "$ROOT_ARGS" -ne 1 ] || [ "$ROOT_ARG_OK" -ne 1 ] \
        || [ "$RO_ARGS" -ne 1 ] || [ "$RW_ARGS" -ne 0 ]; then
    echo "ERROR: cmdline.txt does not meet T7 requirements"; exit 1
fi
echo "T7 GATE 8 CMDLINE: PASSED"

# GATE 9: default.target must resolve to the real multi-user.target file inside the image root.
DEFAULT_LINK="$SYSTEMD_DIR/default.target"
DEFAULT_TARGET=$(readlink "$DEFAULT_LINK" 2>/dev/null || true)
case "$DEFAULT_TARGET" in
    /*) DEFAULT_RESOLVED=$(readlink -f "/mnt/root$DEFAULT_TARGET" 2>/dev/null || true) ;;
    *) DEFAULT_RESOLVED=$(readlink -f "$SYSTEMD_DIR/$DEFAULT_TARGET" 2>/dev/null || true) ;;
esac
case "$DEFAULT_RESOLVED" in
    /mnt/root/*) ;;
    *) echo "ERROR: default.target resolves outside image root"; exit 1 ;;
esac
if [ ! -f "$DEFAULT_RESOLVED" ] || [ "${DEFAULT_RESOLVED##*/}" != "multi-user.target" ]; then
    echo "ERROR: default.target does not point to real multi-user.target file"; exit 1
fi
echo "T7 GATE 9 DEFAULT TARGET: PASSED"

# GATE 10: measure the verification unit's fields, binary and both activation links.
VERIFY_SERVICE="$SYSTEMD_DIR/pijade-boot-verify.service"
for field in Type=oneshot RequiresMountsFor=/boot/firmware After=pijade.service \
             WantedBy=multi-user.target; do
    if ! grep -qx "$field" "$VERIFY_SERVICE"; then
        echo "ERROR: verification unit field missing: $field"; exit 1
    fi
done
# Known r2 regression: a wants link and After=multi-user.target on the same target create a job cycle.
if grep -Eq '^After=([^[:space:]]+[[:space:]]+)*multi-user\.target([[:space:]]|$)' \
        "$VERIFY_SERVICE"; then
    echo "ERROR: verification unit ordered after multi-user.target"; exit 1
fi
VERIFY_EXEC=$(sed -n 's/^ExecStart=//p' "$VERIFY_SERVICE")
case "$VERIFY_EXEC" in
    /*) ;;
    *) echo "ERROR: verification ExecStart is not an absolute path"; exit 1 ;;
esac
if [ ! -x "/mnt/root$VERIFY_EXEC" ]; then
    echo "ERROR: verification ExecStart missing or not executable"; exit 1
fi
if [ "$(readlink "$SYSTEMD_DIR/multi-user.target.wants/pijade-boot-verify.service" 2>/dev/null || true)" != "/etc/systemd/system/pijade-boot-verify.service" ] || \
   [ "$(readlink "$SYSTEMD_DIR/multi-user.target.wants/pijade.service" 2>/dev/null || true)" != "/etc/systemd/system/pijade.service" ]; then
    echo "ERROR: service activation link incorrect"; exit 1
fi
echo "T7 GATE 10 UNIT: PASSED"

# GATE 11: check nine measurements, the counter-based verdict, exit code and invoked binaries.
# The last four measurements (boot_id, power, kernel log, temperature) are INFO lines and do not
# touch FAIL. The gate still pins their presence: a silently dropped info line leaves no trace,
# making "not measured" indistinguishable from "clean result" during device testing.
VERIFY_SCRIPT=/mnt/root/usr/local/sbin/pijade-boot-verify
for measurement in 'swapon --show' 'findmnt -no OPTIONS /' 'ss -H -tlnu' \
                   'systemctl is-active pijade.service' 'cat /etc/machine-id' \
                   'cat /proc/sys/kernel/random/boot_id' 'vcgencmd get_throttled' \
                   'under-voltage|voltage|throttl|regulator' \
                   '/sys/class/thermal/thermal_zone*/temp'; do
    if ! grep -Fq "$measurement" "$VERIFY_SCRIPT"; then
        echo "ERROR: verification script measurement missing: $measurement"; exit 1
    fi
done
# Finding the command string alone is insufficient: a failed command may produce empty output.
# Pin each measurement's exit-code assignment to a separate variable and its use in the verdict.
for rc_contract in \
        'SWAP_OUTPUT=$(swapon --show); SWAP_RC=$?' 'if [ "$SWAP_RC" -ne 0 ]; then' \
        'ROOT_OPTIONS=$(findmnt -no OPTIONS /); ROOT_RC=$?' 'if [ "$ROOT_RC" -ne 0 ]; then' \
        'LISTEN_OUTPUT=$(ss -H -tlnu); LISTEN_RC=$?' 'if [ "$LISTEN_RC" -ne 0 ]; then' \
        'PIJADE_STATUS=$(systemctl is-active pijade.service 2>&1); PIJADE_RC=$?' \
        'if [ "$PIJADE_RC" -eq 0 ] && [ "$PIJADE_STATUS" = "active" ]; then' \
        'MACHINE_ID=$(cat /etc/machine-id); MACHINE_RC=$?' \
        'if [ "$MACHINE_RC" -ne 0 ]; then' \
        'STORED_ID=$(cat "$ID_FILE"); STORED_RC=$?' \
        'elif [ "$STORED_RC" -ne 0 ]; then' \
        'BOOT_ID=$(cat /proc/sys/kernel/random/boot_id); BOOT_ID_RC=$?' \
        'if [ "$BOOT_ID_RC" -ne 0 ] || [ -z "$BOOT_ID" ]; then' \
        'THROTTLED=$(vcgencmd get_throttled 2>&1); THROTTLED_RC=$?' \
        'if [ "$THROTTLED_RC" -ne 0 ]; then' \
        'DMESG_OUT=$(dmesg 2>&1); DMESG_RC=$?' \
        'if [ "$DMESG_RC" -ne 0 ]; then'; do
    if ! sed 's/^[[:space:]]*//' "$VERIFY_SCRIPT" | grep -Fxq "$rc_contract"; then
        echo "ERROR: verification measurement exit-code contract missing: $rc_contract"; exit 1
    fi
done
# Do NOT use getline: it consumes the next line, hiding RESULT= from the counter and making
# the gate reject its own generated script (measured). Track the previous line instead.
RESULT_OK=$(awk '
    /^[[:space:]]*RESULT=/ { count++ }
    prev ~ /echo "T7 VERIFICATION: OK"/ && $0 ~ /^[[:space:]]*RESULT=0[[:space:]]*$/ { ok=1 }
    prev ~ /echo "T7 VERIFICATION: ERROR"/  && $0 ~ /^[[:space:]]*RESULT=1[[:space:]]*$/ { fail=1 }
    { prev = $0 }
    END { print (count == 2 && ok && fail) ? 1 : 0 }
' "$VERIFY_SCRIPT")
EXIT_OK=$(awk '
    /^[[:space:]]*exit([[:space:]]|$)/ {
        count++; if ($0 ~ /^[[:space:]]*exit "\$RESULT"[[:space:]]*$/) exact++
    }
    END { print (count == 1 && exact == 1) ? 1 : 0 }
' "$VERIFY_SCRIPT")
if ! grep -Fq 'FAIL=$((FAIL + 1))' "$VERIFY_SCRIPT" || \
   ! grep -Fq 'if [ "$FAIL" -eq 0 ]' "$VERIFY_SCRIPT" || \
   [ "$RESULT_OK" -ne 1 ] || [ "$EXIT_OK" -ne 1 ]; then
    echo "ERROR: verification verdict or exit code not derived from error counter"; exit 1
fi
for binary_path in /sbin/swapon /bin/findmnt /bin/ss /bin/systemctl /bin/cat /bin/sync /bin/date \
                   /usr/bin/tail /usr/bin/head /usr/bin/od /usr/bin/tr /usr/bin/wc \
                   /usr/bin/cut /bin/grep /usr/bin/timeout \
                   /usr/bin/vcgencmd /bin/dmesg; do
    if [ ! -x "/mnt/root$binary_path" ]; then
        echo "ERROR: verification binary missing from image: $binary_path"; exit 1
    fi
done
echo "T7 GATE 11 VERIFICATION SCRIPT: PASSED"

# GATE 12: the persistent device identity scheme must be complete.
# This gate exists because losing ANY of its three components causes a SILENT failure:
# the device still boots and displays an ID, but that ID changes with every boot.
MACHINEID_SCRIPT=/mnt/root/usr/local/sbin/pijade-machine-id
MACHINEID_UNIT="$SYSTEMD_DIR/pijade-machine-id.service"
if [ ! -x "$MACHINEID_SCRIPT" ]; then
    echo "ERROR: identity script missing or not executable"; exit 1
fi
# Both generation and mounting are required. A script that only generates writes the file
# but leaves /etc/machine-id temporary, fixing nothing.
if ! grep -q '^mount --bind "\$ID_FILE" /etc/machine-id$' "$MACHINEID_SCRIPT"; then
    echo "ERROR: identity script does not bind mount over /etc/machine-id"; exit 1
fi
if ! grep -q '/dev/random' "$MACHINEID_SCRIPT"; then
    echo "ERROR: identity script does not use blocking entropy source"; exit 1
fi
# urandom returns bytes even before seeding; two cards could receive the same identity.
# Uniqueness is this scheme's sole real requirement, so explicitly forbid that source.
if grep -q '/dev/urandom' "$MACHINEID_SCRIPT"; then
    echo "ERROR: identity script uses /dev/urandom"; exit 1
fi
if [ ! -f "$MACHINEID_UNIT" ]; then
    echo "ERROR: identity unit missing"; exit 1
fi
for directive in '^Before=pijade\.service$' '^Type=oneshot$' '^RequiresMountsFor=/boot/firmware$' \
        '^TimeoutStartSec=' '^ExecStart=/usr/local/sbin/pijade-machine-id$'; do
    if ! grep -Eq "$directive" "$MACHINEID_UNIT"; then
        echo "ERROR: identity unit missing directive: $directive"; exit 1
    fi
done
# No Requires/BindsTo: boot must continue if ID generation fails, with the failure in verification.
if grep -Eq '^(Requires|BindsTo|Requisite)=' "$MACHINEID_UNIT"; then
    echo "ERROR: identity unit dependencies would block boot"; exit 1
fi
if grep -Eq '^(Requires|Wants|After)=.*pijade-machine-id' "$SYSTEMD_DIR/pijade.service"; then
    echo "ERROR: product service depends on identity unit"; exit 1
fi
if [ "$(readlink "$SYSTEMD_DIR/multi-user.target.wants/pijade-machine-id.service" 2>/dev/null || true)" \
        != "/etc/systemd/system/pijade-machine-id.service" ]; then
    echo "ERROR: identity unit not enabled"; exit 1
fi
# Verification must compare CONTENT; a nonempty check cannot detect the temporary ID.
if ! grep -q 'MACHINE_ID" != "\$STORED_ID' "$VERIFY_SCRIPT"; then
    echo "ERROR: boot verification does not compare identity with source file"; exit 1
fi
# The list comes from the identity script's OWN calls (body scan excluding comments): mount, mv,
# head, od, tr, wc, sync. Adding boot-verify tools here (grep, timeout, cut, dmesg,
# systemctl) obscures what the gate protects: those tools belong to GATE 11 and are enforced
# there. The distinction matters: two gates hold the same binary for different reasons; if
# boot-verify stops calling sync, GATE 11 drops it, but the identity script still needs it.
for binary_path in /bin/mount /bin/mv /usr/bin/head /usr/bin/od /usr/bin/tr /usr/bin/wc \
        /bin/sync; do
    if [ ! -x "/mnt/root$binary_path" ]; then
        echo "ERROR: identity script binary missing from image: $binary_path"; exit 1
    fi
done
# The distributed image must carry no card ID, or all cards written from it would share one.
if [ -e /mnt/boot/pijade-machine-id ] || [ -e /mnt/boot/pijade-machine-id.tmp ]; then
    echo "ERROR: image contains card identity; all cards would share it"; exit 1
fi
# The script must validate the WHOLE file; checking only its first line would let extra bytes leak.
if ! grep -q 'wc -c < "\$ID_FILE"' "$MACHINEID_SCRIPT"; then
    echo "ERROR: identity script does not validate file size"; exit 1
fi
if ! grep -q 'wc -l < "\$ID_FILE"' "$MACHINEID_SCRIPT"; then
    echo "ERROR: identity script does not validate line count"; exit 1
fi
echo "T7 GATE 12 IDENTITY: PASSED"

# GATE 13: clear the shutdown step that cannot run on read-only root.
# PRESERVE loading: a reset to 1970 makes the log, the device's only diagnostic channel, unreadable.
HWCLOCK_DROPIN="$SYSTEMD_DIR/fake-hwclock.service.d/pijade.conf"
if [ ! -f "$HWCLOCK_DROPIN" ]; then
    echo "ERROR: fake-hwclock shutdown step not cleared"; exit 1
fi
if [ "$(sed -n '/^ExecStop=/p' "$HWCLOCK_DROPIN")" != "ExecStop=" ]; then
    echo "ERROR: fake-hwclock drop-in does not reset list with a single empty ExecStop"; exit 1
fi
if [ -L "$SYSTEMD_DIR/fake-hwclock.service" ]; then
    echo "ERROR: fake-hwclock masked, load step would also be lost"; exit 1
fi
if [ ! -r /mnt/root/etc/fake-hwclock.data ]; then
    echo "ERROR: fake-hwclock data file unreadable, load step has no data"; exit 1
fi
# The time zone must be UTC: ctime_r applies the local zone while the OTP screen prints fixed "UTC";
# leaving London would misreport an hour during daylight saving.
if [ "$(cat /mnt/root/etc/timezone 2>/dev/null)" != "Etc/UTC" ]; then
    echo "ERROR: image time zone is not Etc/UTC"; exit 1
fi
if [ "$(readlink /mnt/root/etc/localtime)" != "/usr/share/zoneinfo/Etc/UTC" ]; then
    echo "ERROR: /etc/localtime does not point to Etc/UTC"; exit 1
fi
echo "T7 GATE 13 CLOCK: PASSED"

# GATE 14: the service surface launchable through D-Bus must remain within the allowlist.
# Why a class gate instead of individual masks: dbus.socket is enabled from the vendor directory
# (usr/lib/systemd/system/sockets.target.wants/dbus.socket), so the bus is active and every
# activation file is a launch path. Gate 5 checks aliases by target; this gate checks
# the ACTIVATION side.
#
# Scan directories, do NOT GUESS: dbus system.conf says <standard_system_servicedirs/> and the
# actual list is compiled into the binary. Instead of checking which known directories exist,
# find ALL ".../dbus-1/system-services" directories under root. This is broader than enumerating
# a known list: files in nonstandard locations are also caught (erring toward false positives
# is appropriate for a hardening gate).
# Measured on 2026-08-27: this image has only /usr/share/dbus-1/system-services, with 15 files;
# counterparts under /usr/local/share, /lib, /usr/lib and /etc are absent.
DBUS_DIRS=$(find /mnt/root -type d -path '*/dbus-1/system-services' 2>/dev/null || true)
if [ -z "$DBUS_DIRS" ]; then
    echo "ERROR: no D-Bus activation directory found; gate 14 cannot measure anything"; exit 1
fi
# Allow more than just the NAME: pin each allowed name's effective action to its measured target;
# otherwise changing SystemdService= or Exec= in an allowed file would bypass the gate.
for dbus_dir in $DBUS_DIRS; do
    for activation in "$dbus_dir"/*.service; do
        [ -e "$activation" ] || continue
        act_name=$(basename "$activation" .service)
        declared_name=$(sed -n 's/^Name=[[:space:]]*//p' "$activation")
        if [ "$(grep -c '^Name=' "$activation" || true)" -ne 1 ] || \
           [ "$declared_name" != "$act_name" ]; then
            echo "ERROR: D-Bus filename and single Name= field differ: $activation"; exit 1
        fi
        systemd_rows=$(grep -c '^SystemdService=' "$activation" || true)
        exec_rows=$(grep -c '^Exec=' "$activation" || true)
        if [ "$systemd_rows" -gt 1 ] || [ "$exec_rows" -gt 1 ]; then
            echo "ERROR: D-Bus file has multiple action fields: $activation"; exit 1
        fi
        act_unit=$(sed -n 's/^SystemdService=[[:space:]]*//p' "$activation" | head -1)
        if [ -z "$act_unit" ]; then
            # Traditional activation: without SystemdService=, dbus-daemon runs Exec= DIRECTLY,
            # bypassing systemd. Skipping such a file as "unmeasurable" would bypass the gate;
            # it counts as launchable and is subject to the same allowlist.
            # The sole example in this image is org.freedesktop.systemd1 (Exec=/bin/false), on the allowlist.
            act_exec=$(sed -n 's/^Exec=[[:space:]]*//p' "$activation" | head -1)
            if [ -z "$act_exec" ]; then
                echo "ERROR: D-Bus file has neither SystemdService= nor Exec=: $activation"; exit 1
            fi
            act_state="launchable"
            act_action="Exec=$act_exec"
        else
            act_action="SystemdService=$act_unit"
            act_state="unit missing"
            for unit_dir in /mnt/root/etc/systemd/system /mnt/root/run/systemd/system \
                            /mnt/root/usr/local/lib/systemd/system \
                            /mnt/root/usr/lib/systemd/system; do
                if [ -L "$unit_dir/$act_unit" ] \
                        && [ "$(readlink "$unit_dir/$act_unit")" = "/dev/null" ]; then
                    act_state="masked"; break
                fi
                if [ -e "$unit_dir/$act_unit" ] || [ -L "$unit_dir/$act_unit" ]; then
                    act_state="launchable"; break
                fi
            done
        fi
        expected_action=""
        case "$act_name" in
            org.freedesktop.PolicyKit1)
                expected_action="SystemdService=polkit.service" ;;
            org.freedesktop.hostname1)
                expected_action="SystemdService=dbus-org.freedesktop.hostname1.service" ;;
            org.freedesktop.locale1)
                expected_action="SystemdService=dbus-org.freedesktop.locale1.service" ;;
            org.freedesktop.login1)
                expected_action="SystemdService=dbus-org.freedesktop.login1.service" ;;
            org.freedesktop.timedate1)
                expected_action="SystemdService=dbus-org.freedesktop.timedate1.service" ;;
            org.freedesktop.systemd1)
                expected_action="Exec=/bin/false" ;;
        esac
        if [ -n "$expected_action" ] && [ "$act_action" != "$expected_action" ]; then
            echo "ERROR: allowed D-Bus name has changed action: $act_name -> $act_action"
            exit 1
        fi
        [ "$act_state" = "launchable" ] || continue
        if [ -z "$expected_action" ]; then
            echo "ERROR: D-Bus launchable service outside allowlist: $act_name -> $act_action"
            exit 1
        fi
    done
done
# Masking an absent unit is unnecessary, but its appearance must be reported: networkd's alias
# unit is created by "systemctl enable" and has never existed in this image (r5 finding 3,
# rejected by measurement). The gate pins that fact.
echo "T7 GATE 14 DBUS: PASSED"

echo "ALL GATES PASSED"
