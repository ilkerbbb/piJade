#!/bin/bash
# Embeds the pijade binaries INSIDE a Raspberry Pi OS image.
# Goal: write card -> insert -> run. NO keyboard/HDMI setup step on the device.
#
# Since T3.11 the binaries live on FAT (/boot/firmware/pijade/), outside the root filesystem.
# Reason: macOS can read and write FAT but cannot read ext4. To try a new version, insert the
# card into the Mac and copy two files, without rebuilding the image and writing 2.5 GB again.
#
# Runs in a container; /img = image to prepare, /pkg = binary package. Invocation
# (from images/, produced this output on 2026-08-25):
#   docker run --rm -i --privileged \
#       -e PKG_SHA=<package hash from the build log> \
#       -v "$PWD/pijade-t33a.img:/img" -v "$PWD/armv6-out:/pkg" \
#       alpine:3.20@sha256:d9e853e87e55526f6b2917df91a2115c36dd7c696a35be12163d44e6e2a4b6bc \
#           sh -s < ../Jade/pijade/images/prepare-image.sh
# Without PKG_SHA, the adjacent .sha256 file is used (GATE 0); it detects staleness
# but provides no authenticity. For authenticity, pass the hash from the build log MANUALLY.
# --privileged is required: losetup and mount need kernel privileges. The image is modified IN PLACE,
# without a copy; the script is repeatable and safe to rerun on the same image.
# This repeatability prevents pinning the INPUT IMAGE SHA-256 here: the second run consumes
# the first run's output. The acceptance runner (tests/t7/t7_chain.sh) refreshes the test copy
# from the immutable master image and records two starting hashes. The PACKAGE is different:
# it comes from outside this script and stays unchanged during the run, so its hash is verified HERE
# (step 3, GATE 0). PREPARATION_OK certifies only the output this script measures; it does not
# certify that the test copy came from a known initial state.
set -e

# The supported base contract is MBR partition 1 FAT, partition 2 ext4. Pinning offsets and sizes
# to one image would mount a differently sized copy of the same release at the wrong location;
# all four values come directly from the MBR partition entries' 512-byte sector counts.
mbr_le32() {
    bytes=$(od -An -v -tx1 -j "$1" -N 4 /img)
    set -- $bytes
    [ "$#" -eq 4 ] || return 1
    printf '%u\n' "$((0x$4$3$2$1))"
}
if [ "$(od -An -v -tx1 -j 510 -N 2 /img | tr -d ' \n')" != "55aa" ]; then
    echo "ERROR: supported MBR signature missing"; exit 1
fi
BOOT_START=$(mbr_le32 454) || { echo "ERROR: could not read MBR boot start"; exit 1; }
BOOT_SECTORS=$(mbr_le32 458) || { echo "ERROR: could not read MBR boot size"; exit 1; }
ROOT_START=$(mbr_le32 470) || { echo "ERROR: could not read MBR root start"; exit 1; }
ROOT_SECTORS=$(mbr_le32 474) || { echo "ERROR: could not read MBR root size"; exit 1; }
IMG_SECTORS=$(($(stat -c %s /img) / 512))
if [ "$BOOT_START" -le 0 ] || [ "$BOOT_SECTORS" -le 0 ] || \
   [ "$ROOT_START" -le 0 ] || [ "$ROOT_SECTORS" -le 0 ] || \
   [ $((BOOT_START + BOOT_SECTORS)) -gt "$ROOT_START" ] || \
   [ $((ROOT_START + ROOT_SECTORS)) -gt "$IMG_SECTORS" ]; then
    echo "ERROR: MBR partition geometry invalid or unsupported"; exit 1
fi
BOOT_OFFSET=$((BOOT_START * 512))
BOOT_SIZE=$((BOOT_SECTORS * 512))
ROOT_OFFSET=$((ROOT_START * 512))
ROOT_SIZE=$((ROOT_SECTORS * 512))
# Each loop gets its own bounds so both partitions in the same file can be mounted at once;
# an unbounded second loop covers the whole file and util-linux rejects the overlap.

# Package VERSIONS are deliberately unpinned; the image digest is pinned (invocation above).
# Reason: a pin such as "util-linux=2.40.1-r1" goes stale with each point release of the 3.20 branch,
# causing apk "not found" and breaking the chain. Its benefit affects only the INTERMEDIATE IMAGE,
# since these four packages prepare the image and are not written to the card. The pinned image
# digest selects the same apk repository; that repository's updates remain variable.
apk add --no-cache util-linux e2fsprogs coreutils binutils >/dev/null 2>&1
mkdir -p /mnt/root /mnt/boot

# The container shares the kernel: `losetup -D` also detaches loops opened by other jobs on this
# machine. Track our own loops and close only those. The exit trap ensures no mounted partitions
# or loops remain when a gate stops the script; even when container termination removes the
# mounts, it does not remove the loop's association with its backing file.
LOOPS=""
cleanup() {
    set +e
    umount /mnt/boot 2>/dev/null
    umount /mnt/root 2>/dev/null
    for l in $LOOPS; do losetup -d "$l" 2>/dev/null; done
}
trap cleanup EXIT

echo "=== 1) ROOT FILESYSTEM ==="
# Start read-only. If a package gate fails, the image stays bit-for-bit unchanged; even a writable
# mount alone changes metadata (ext4 mount counter, FAT dirty bit), invalidating the stored
# SHA-256. Write access is granted in step 4, after all package gates pass.
ROOT_LOOP=$(losetup -o $ROOT_OFFSET --sizelimit $ROOT_SIZE -f --show /img); LOOPS="$LOOPS $ROOT_LOOP"
mount -o ro "$ROOT_LOOP" /mnt/root
echo "root loop: $ROOT_LOOP"
echo "root: $(head -1 /mnt/root/etc/os-release)"

echo "=== 2) BOOT PARTITION ==="
# GATE: running a binary from FAT requires both conditions from the fstab options field:
# no noexec mount option, and a mask that preserves the x bit. If either condition fails,
# the device fails at boot with 203/EXEC and a blank screen, so both are measured here.
FSTAB_LINE=$(grep -vE "^[[:space:]]*#" /mnt/root/etc/fstab | grep -E "[[:space:]]/boot/firmware[[:space:]]" || true)
echo "fstab: ${FSTAB_LINE:-(no line)}"
if [ -z "$FSTAB_LINE" ]; then
    echo "ERROR: fstab /boot/firmware line missing"; exit 1
fi
if echo "$FSTAB_LINE" | grep -q noexec; then
    echo "ERROR: /boot/firmware mounts with noexec; cannot execute binary there"; exit 1
fi

# Mount with the card's own fstab options, not the container defaults. Otherwise the x-bit gate
# below measures the wrong mount: even if fstab umask/fmask disables execution, the container's
# default fmask=0022 exposes the x bit and the gate would pass.
BOOT_OPTS=$(echo "$FSTAB_LINE" | awk '{print $4}')
BOOT_LOOP=$(losetup -o $BOOT_OFFSET --sizelimit $BOOT_SIZE -f --show /img); LOOPS="$LOOPS $BOOT_LOOP"
echo "boot loop: $BOOT_LOOP, fstab options: $BOOT_OPTS"
if ! mount -o "ro,$BOOT_OPTS" "$BOOT_LOOP" /mnt/boot; then
    # If the card's options prevent mounting, measurement is impossible. Falling back to defaults
    # would certify the wrong mount; stopping is the correct response.
    echo "ERROR: could not mount /boot/firmware with fstab options ($BOOT_OPTS)"; exit 1
fi
echo "actual mount: $(grep ' /mnt/boot ' /proc/mounts)"
echo "boot: $(ls /mnt/boot/config.txt)"


# The unit's ExecStart line and the argument gate below share one source; duplicating the list
# would eventually cause drift, repeating this script's second bug of this round.
EXEC_ARGS="--panel /dev/spidev0.0 --gpio /dev/gpiochip0 --camera /dev/video0 --settings /boot/firmware/pijade-settings.bin"

echo "=== 3) PACKAGE VERIFICATION (image NOT YET modified) ==="
# All package gates run before the image is touched. Otherwise a failed gate leaves a partially
# written image: the old binary removed, the new one absent, and the service pointing at
# a card where neither binary can be found.
# GATE 0: the package fingerprint. The following gates measure CONTENT (arguments,
# symbols, size); this gate checks that it is the EXPECTED package and runs before them all.
# Two sources, in priority order: if PKG_SHA is supplied in the environment (manually copied
# from the build log), THAT value is mandatory; otherwise use the adjacent .sha256 file.
# The sidecar provides no AUTHENTICITY; anyone changing the tar can change its .sha256 too. It
# detects staleness and corruption (2026-08-25: the unit passed a new argument to an old package).
# For authenticity the caller must supply PKG_SHA. Without either source the gate CLOSES;
# "no hash source" cannot be silently skipped.
PKG_TGZ=/pkg/pijade-armv6.tar.gz
[ -f "$PKG_TGZ" ] || { echo "PACKAGE MISSING: $PKG_TGZ"; exit 1; }
PKG_SHA_MEASURED=$(sha256sum "$PKG_TGZ" | awk '{print $1}')
if [ -n "${PKG_SHA:-}" ]; then
    PKG_SHA_EXPECTED=$PKG_SHA
    PKG_SHA_SOURCE="PKG_SHA (supplied manually)"
elif [ -f "$PKG_TGZ.sha256" ]; then
    PKG_SHA_EXPECTED=$(awk '{print $1}' "$PKG_TGZ.sha256")
    PKG_SHA_SOURCE="sidecar"
else
    echo "PACKAGE HASH SOURCE MISSING: neither PKG_SHA nor $PKG_TGZ.sha256 supplied"; exit 1
fi
if [ "$PKG_SHA_MEASURED" != "$PKG_SHA_EXPECTED" ]; then
    echo "PACKAGE HASH MISMATCH ($PKG_SHA_SOURCE)"
    echo "  expected: $PKG_SHA_EXPECTED"
    echo "  measured : $PKG_SHA_MEASURED"
    exit 1
fi
echo "package hash PASSED ($PKG_SHA_SOURCE): $PKG_SHA_MEASURED"

mkdir -p /tmp/pkg
tar -xzf "$PKG_TGZ" -C /tmp/pkg
PKG_HOST=/tmp/pkg/opt/pijade/bin/pijade-host
PKG_LIB=/tmp/pkg/opt/pijade/lib/libjade.so
# T44 measurement tool. Without the marker file it does nothing at boot; it runs through
# the service's ExecStartPre, not a separate unit. If missing, preparation stops here: silently
# skipping it would hide a missed device measurement until the empty log was inspected.
PKG_T44=/tmp/pkg/opt/pijade/bin/pijade-t44-bench
if [ ! -f "$PKG_T44" ]; then
    echo "ERROR: package lacks pijade-t44-bench; rebuild binaries (build-armv6.sh step 3b)"; exit 1
fi
ls -la "$PKG_HOST" "$PKG_LIB" "$PKG_T44"

# GATE 1: does the binary recognize every argument the service will pass?
# Package and unit come from separate sources: the build output directory and this script.
# A stale package starts with a newly added unit argument, reports "unknown option",
# and calls exit(1). This is a normal error exit, so Restart=on-abort DOES NOT RESTART it and
# the device never starts; the hash gate cannot detect this, since it only measures copy integrity.
# (Exactly this happened on 2026-08-25: the packaged host did not recognize the unit's --settings.)
# The ARM binary cannot run here, so argument strings are searched inside the binary.
ARG_FAIL=0
for a in $EXEC_ARGS; do
    case "$a" in --*) ;; *) continue ;; esac
    if grep -aq -- "$a" "$PKG_HOST"; then
        echo "  $a: present in binary"
    else
        echo "  $a: MISSING FROM BINARY"; ARG_FAIL=1
    fi
done
if [ "$ARG_FAIL" -ne 0 ]; then
    echo "ERROR: package predates unit file; rebuild binaries first"; exit 1
fi
echo "ARGUMENT GATE: PASSED"

# GATE 2: does the library define everything the binary needs?
# The argument gate measures only the host's age. With a current host and stale libjade.so,
# that gate passes but the dynamic linker cannot find a symbol at device startup. Check this
# in advance: every unresolved libjade_ symbol in the host must be defined in the library. New
# features need no manual list update; their symbols are covered automatically.
readelf -W --dyn-syms "$PKG_HOST" | awk '$7=="UND" && $8 ~ /^libjade_/ {print $8}' | sort -u > /tmp/und
readelf -W --dyn-syms "$PKG_LIB"  | awk '$7!="UND" && $8 != "" {print $8}' | sort -u > /tmp/def
MISSING=$(comm -23 /tmp/und /tmp/def)
echo "libjade symbols required by host: $(wc -l < /tmp/und)"
if [ -n "$MISSING" ]; then
    echo "ERROR: symbols missing from library:"; echo "$MISSING" | sed 's/^/  /'
    echo "packaged libjade.so predates host; rebuild both together"; exit 1
fi
echo "SYMBOL GATE: PASSED"

echo "=== 4) BINARIES (FAT PARTITION) ==="
# Image modification starts here; write access is granted only after all package gates
# have passed.
mount -o remount,rw /mnt/root
mount -o remount,rw /mnt/boot
echo "write access granted:"
grep -E ' /mnt/(root|boot) ' /proc/mounts | awk '{print "  " $2 " " $4}' | cut -d, -f1
# The package has an /opt/pijade/... hierarchy; that depth has no counterpart on FAT, and ownership
# and permissions are not preserved anyway (vfat derives permissions from fmask, not file metadata).
# Place the two files in a flat directory: when the card is inserted into a Mac, pijade/
# shows exactly the two files to replace.
# Rerunning on an image prepared before T3.11 leaves the old layout in the root filesystem,
# with two copies and uncertainty about which runs. Remove the old layout here to keep
# the script repeatable. Delete only the two files this script installed, and remove directories
# only when empty: anything else under /opt stays untouched.
OLD_DIR=/mnt/root/opt/pijade
if [ -d "$OLD_DIR" ]; then
    rm -f "$OLD_DIR/bin/pijade-host" "$OLD_DIR/lib/libjade.so"
    rmdir "$OLD_DIR/bin" "$OLD_DIR/lib" "$OLD_DIR" /mnt/root/opt 2>/dev/null || true
    [ -d "$OLD_DIR" ] && echo "WARNING: $OLD_DIR not empty, inspect manually" || echo "old /opt/pijade layout removed"
fi
mkdir -p /mnt/boot/pijade
cp "$PKG_HOST" /mnt/boot/pijade/pijade-host
cp "$PKG_LIB"  /mnt/boot/pijade/libjade.so
cp "$PKG_T44"  /mnt/boot/pijade/pijade-t44-bench
ls -la /mnt/boot/pijade/
# vfat derives permissions from fmask at mount time instead of storing them in files; if the
# copied file lacks the x bit, fmask has disabled execution.
if [ ! -x /mnt/boot/pijade/pijade-host ] || [ ! -x /mnt/boot/pijade/libjade.so ] \
   || [ ! -x /mnt/boot/pijade/pijade-t44-bench ]; then
    echo "ERROR: FAT partition lacks x bit; fmask prevents execution"; exit 1
fi
echo "FAT execution gate: PASSED"

echo "=== 5) SERVICE ==="
cat > /mnt/root/etc/systemd/system/pijade.service <<UNIT
[Unit]
Description=piJade host
After=local-fs.target
Wants=local-fs.target
# The binary, library, settings file and log all live on FAT. Without this line the service
# starts before that partition is mounted and cannot find ExecStart.
RequiresMountsFor=/boot/firmware

[Service]
Type=simple
Environment=LD_LIBRARY_PATH=/boot/firmware/pijade
# Without --camera the host never starts the camera thread (pijade_host.c, "if (camera_path)").
# The device can sign without a camera; opening is retried if absent, without blocking startup.
# Without --settings no setting persists (pijade_host.c, "if (settings_path)"): theme, screen
# orientation, camera angle, brightness, screen timeout, QR preferences AND the PIN wallet reset
# to defaults at every boot, leaving the device unconfigured each time.
# CAUTION (T5): this file now HOLDS KEY MATERIAL. It contains the PIN client's private key, the
# AES-encrypted wallet and plaintext duress PIN. The encrypted wallet needs the pinserver key half,
# but this is NOT a preferences file: treat it as a private key and remember that FAT leaves
# old copies behind. The persistent fields are listed in libjade/pijade_settings.c;
# details are in pijade/UPSTREAM.md item 22.
# A crash may leave a keychain in memory (a bad QR can throw in C++; see the header in
# libjade/cxx_terminate.cpp). A core dump would write that memory to disk.
# There currently seems to be nowhere to write a dump: root is read-only, swap is off, and
# the image has no coredump package. All three depend on base-image defaults,
# however (core_pattern, DefaultLimitCORE, sysctl.d), and those defaults were NOT MEASURED.
# Close the gate here instead of relying on three unknowns.
LimitCORE=0
# T44 measurement tool. The gate is inside the binary: without /boot/firmware/pijade-t44.enable
# it exits without printing a line, so boot time is unchanged unless the marker is present. The "-"
# prefix prevents a measurement-tool error from blocking device startup. Placing it here:
# inherits this block's stdout redirection, sends numbers directly to pijade.log, and runs the
# measurement while the machine is idle, before the host opens the camera.
ExecStartPre=-/boot/firmware/pijade/pijade-t44-bench
ExecStart=/boot/firmware/pijade/pijade-host $EXEC_ARGS
# If the screen stays blank, this file is the only diagnostic channel; read the card on a Mac.
StandardOutput=append:/boot/firmware/pijade.log
StandardError=append:/boot/firmware/pijade.log
ExecStopPost=/bin/sync
# Jade's error screen says "Internal error ... Restarting", but in libjade that path ends in
# abort(), never reaching the ESP32 esp_restart(). With Restart=no the message was false:
# the device stayed frozen. With on-abort the message finally describes the behavior.
# Keep this narrow: only an unclean signal (including SIGABRT) restarts it. A normal error exit
# (exit != 0), such as failure to open the panel, indicates missing hardware; retrying in a loop
# fixes nothing on the card and only fills the log.
# T3.9's successful shutdown path is unaffected: that process calls _exit(EXIT_SUCCESS).
Restart=on-abort

[Install]
WantedBy=multi-user.target
UNIT
chmod 0644 /mnt/root/etc/systemd/system/pijade.service
# Equivalent to systemctl enable: a symlink in the target's wants directory.
ln -sf /etc/systemd/system/pijade.service \
       /mnt/root/etc/systemd/system/multi-user.target.wants/pijade.service
ls -la /mnt/root/etc/systemd/system/multi-user.target.wants/pijade.service

echo "=== 6) SWAP OFF (BBB constraint) ==="
SWAP_LINK=/mnt/root/etc/systemd/system/multi-user.target.wants/dphys-swapfile.service
# Use -L INSTEAD OF -e: an absolute symlink target (/lib/systemd/...) is looked up in the
# container's own root; -e reports an existing symlink as missing and silently skips this step.
if [ -L "$SWAP_LINK" ] || [ -e "$SWAP_LINK" ]; then
    rm -f "$SWAP_LINK"; echo "dphys-swapfile disabled"
else
    echo "dphys-swapfile already disabled"
fi
echo "--- remaining multi-user.target.wants ---"
ls /mnt/root/etc/systemd/system/multi-user.target.wants/ | tr '\n' ' '; echo

echo "=== 7) SPI ENABLED (config.txt) ==="
if grep -q "^dtparam=spi=on" /mnt/boot/config.txt; then
    echo "spi already enabled"
else
    printf '\n# piJade T3.3a: ST7789 panel driven over SPI\ndtparam=spi=on\n' >> /mnt/boot/config.txt
    echo "dtparam=spi=on added"
fi
tail -4 /mnt/boot/config.txt

echo "=== 7a) LOAD CAMERA DRIVER AT BOOT ==="
# start_x.elf enables the camera in firmware, but this kernel module creates the V4L2 device.
# Without it /dev/video0 never appears; the probe round loaded it manually with modprobe.
cat > /mnt/root/etc/modules-load.d/pijade-camera.conf <<'MODCONF'
# piJade: legacy camera driver that creates /dev/video0
bcm2835-v4l2
MODCONF
chmod 0644 /mnt/root/etc/modules-load.d/pijade-camera.conf
cat /mnt/root/etc/modules-load.d/pijade-camera.conf

echo "=== 7b) CAMERA (legacy stack; measured in T3.5a) ==="
# The only working path on Pi Zero W: camera-enabled firmware (start_x.elf) plus 64 MB GPU
# memory. SeedSigner uses the same four lines on the same hardware. With libcamera auto-detection
# and the KMS driver both enabled, the legacy stack never creates /dev/video0.
sed -i 's/^camera_auto_detect=1/camera_auto_detect=0/' /mnt/boot/config.txt
sed -i 's|^dtoverlay=vc4-kms-v3d|#dtoverlay=vc4-kms-v3d  # piJade: disabled for legacy camera path|' \
    /mnt/boot/config.txt
if grep -q "^start_x=1" /mnt/boot/config.txt; then
    echo "legacy camera already enabled"
else
    printf '\n# piJade T3.5: camera (legacy stack)\nstart_file=start_x.elf\nfixup_file=fixup_x.dat\nstart_x=1\ngpu_mem=64\n' \
        >> /mnt/boot/config.txt
    echo "legacy camera lines added"
fi
echo "--- camera-related lines ---"
grep -nE "^camera_auto_detect|^#?dtoverlay=vc4-kms|^start_file|^fixup_file|^start_x|^gpu_mem" /mnt/boot/config.txt

echo "=== 8) T7: HARDENING ==="
SYSTEMD_DIR=/mnt/root/etc/systemd/system
# One list feeds masking and the mask and residual-link gates from the same source.
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
# systemd-random-seed is listed: on read-only root, 'load' tries to refresh
# /var/lib/systemd/random-seed and fails with EROFS, leaving a failed service at every boot.
# Measured: (a) the image ALREADY LACKS the file, so no carried seed is lost; (b) bcm2835-rng
# is in modules.builtin for the v6 kernel, so hardware RNG feeds the pool without loading a module;
# (c) systemd assigns no entropy CREDIT to this seed by default (SYSTEMD_RANDOM_SEED_CREDIT
# defaults to false), so it is no barrier for getrandom(2). Masking the unit makes this
# an explicit decision instead of an accident. The entropy pool itself belongs to T4.
for unit in $MASKED_UNITS; do
    ln -sf /dev/null "$SYSTEMD_DIR/$unit"
    echo "$unit masked"
done

# Masking closes other service launch paths; removing wants links prevents attempts at boot.
WANT_LINKS="
multi-user.target.wants/NetworkManager.service
multi-user.target.wants/wpa_supplicant.service
multi-user.target.wants/ModemManager.service
multi-user.target.wants/avahi-daemon.service
multi-user.target.wants/sshswitch.service
multi-user.target.wants/userconfig.service
multi-user.target.wants/triggerhappy.service
multi-user.target.wants/rpi-eeprom-update.service
multi-user.target.wants/regenerate_ssh_host_keys.service
sockets.target.wants/avahi-daemon.socket
sockets.target.wants/triggerhappy.socket
bluetooth.target.wants/bluetooth.service
dev-serial1.device.wants/hciuart.service
graphical.target.wants/udisks2.service
network-online.target.wants/NetworkManager-wait-online.service
sysinit.target.wants/systemd-timesyncd.service
timers.target.wants/apt-daily.timer
timers.target.wants/apt-daily-upgrade.timer
timers.target.wants/man-db.timer
timers.target.wants/dpkg-db-backup.timer
timers.target.wants/e2scrub_all.timer
timers.target.wants/logrotate.timer
timers.target.wants/fstrim.timer
multi-user.target.wants/e2scrub_reap.service
multi-user.target.wants/cron.service
multi-user.target.wants/nfs-client.target
multi-user.target.wants/remote-fs.target
multi-user.target.wants/console-setup.service
"
for link in $WANT_LINKS; do
    WANT_LINK="$SYSTEMD_DIR/$link"
    # Use -L: -e searches absolute symlink targets in the container root and gives the wrong answer.
    if [ -L "$WANT_LINK" ]; then
        rm -f "$WANT_LINK"; echo "$link disabled"
    else
        echo "$link already disabled"
    fi
done

RESIZE_LINK=/mnt/root/etc/rc3.d/S01resize2fs_once
# This sysv job has no native unit and attempts resize2fs and rm writes on read-only root.
if [ -L "$RESIZE_LINK" ]; then
    rm -f "$RESIZE_LINK"; echo "resize2fs_once disabled"
else
    echo "resize2fs_once already disabled"
fi

# Start only the multi-user target required by the product, instead of the graphical target.
ln -sf /lib/systemd/system/multi-user.target "$SYSTEMD_DIR/default.target"

# Root becomes read-only; FAT stays writable for settings and the application log.
sed -i -E 's|^([^#[:space:]][^[:space:]]*[[:space:]]+/[[:space:]]+ext4[[:space:]]+)[^[:space:]]+|\1ro,noatime|' \
    /mnt/root/etc/fstab
if ! awk '$1 !~ /^#/ && $2 == "/tmp" && $3 == "tmpfs" { found=1 } END { exit !found }' \
    /mnt/root/etc/fstab; then
    echo 'tmpfs           /tmp            tmpfs   defaults,nosuid,nodev,mode=1777   0  0' >> /mnt/root/etc/fstab
fi
if ! awk '$1 !~ /^#/ && $2 == "/var/tmp" && $3 == "tmpfs" { found=1 } END { exit !found }' \
    /mnt/root/etc/fstab; then
    echo 'tmpfs           /var/tmp        tmpfs   defaults,nosuid,nodev,mode=1777   0  0' >> /mnt/root/etc/fstab
fi
if ! awk '$1 !~ /^#/ && $2 == "/var/log" && $3 == "tmpfs" { found=1 } END { exit !found }' \
    /mnt/root/etc/fstab; then
    echo 'tmpfs           /var/log        tmpfs   defaults,nosuid,nodev,mode=0755   0  0' >> /mnt/root/etc/fstab
fi

# Disable the first-boot path that applies FAT custom.toml with root privileges.
sed -i 's| init=/usr/lib/raspberrypi-sys-mods/firstboot||g' /mnt/boot/cmdline.txt

# Root must mount read-only. With root= and without rw the kernel already defaults to
# MS_RDONLY, but T7 identity resolution relies entirely on read-only root at PID1 startup:
# if root were writable then, systemd would write machine-id directly to disk and the persistent
# identity scheme below would behave differently and be unnecessary. Pin this explicitly.
if ! tr ' ' '\n' < /mnt/boot/cmdline.txt | grep -qx 'ro'; then
    sed -i '1s|[[:space:]]*$| ro|' /mnt/boot/cmdline.txt
fi

# --- Persistent device identity ----------------------------------------------------
# On read-only root /etc/machine-id stays empty (0 bytes, 0444 in this image). systemd 252 then
# generates a temporary ID in /run/machine-id and mounts it over the empty file; root is unwritable,
# so systemd-machine-id-commit CANNOT persist it. Result: the ID changes at every boot.
# Why it matters: esp_efuse_mac_get_default() in libjade/libjade.c hashes this file with SHA256
# to derive Jade's macid[6]; macid is DISPLAYED on the home screen (main/process/dashboard.c)
# and reported in version information (main/versioninfo.c). T7 read-only root therefore makes
# an otherwise persistent device ID change on every boot. For Jade Plus parity,
# macid comes from the ESP32 eFuse MAC and stays fixed for the device's lifetime.
#
# The fix preserves the identity CONTRACT: libjade still reads /etc/machine-id, now mounted
# from a persistent file unique to the card on FAT. Rejected alternatives:
#   - writing a fixed ID during image preparation: EVERY card written from it would share an ID,
#     conflicting with the distributable .img goal.
#   - switching the source to the /proc/cpuinfo serial number: contradicts pijade/UPSTREAM.md
#     item 3.2, and that line's presence on Pi Zero W was NOT VERIFIED (even SeedSigner wraps it
#     in try/except).
#   - adding --device-id to the host: five files and a new CLI surface for the same behavior.
#
# Files live on FAT; the most sensitive data is PIN wallet key material in pijade-settings.bin.a
# and .b (host/settings_store.h). Identity is less sensitive, and libjade already hashes it.
# macid is NOT secret: main/random.c:248 mixes it with chip_info and the firmware version
# as DOMAIN SEPARATION input; actual entropy comes from get_random().
# The required properties are uniqueness and persistence, not unpredictability.
# --- Clock: fake-hwclock shutdown cannot save ------------------------------
# fake-hwclock.service does two jobs: READ /etc/fake-hwclock.data at boot to set the clock
# (load), and WRITE it at shutdown (save). On read-only root the write fails with EROFS,
# so T7 left a failed unit at every shutdown. Loading works and is valuable:
# the device's only diagnostic channel is /boot/firmware/pijade.log; resetting to 1970 makes
# the log unreadable. A second concrete reason was MEASURED: FAT dates start at 1980 and the
# driver CLAMPS earlier dates to 1980-01-01 (tested on this image's FAT partition). At 1970,
# every file written to FAT, settings, log and identity, would carry the same clamped date,
# making newer copies indistinguishable. This matters because settings hold a PIN wallet and
# FAT leaves old copies behind (host/settings_store.h). Thus the unit stays unmasked;
# only the save step that can never work is cleared.
# Empty ExecStop= resets the list (systemd.service(5): ExecStop follows the ExecStart scheme;
# the ExecStart entry documents empty assignments resetting the list).
#
# CAUTION, the boundary must be explicit: this DOES NOT provide a persistently correct clock.
# Each boot resets to the image build time. Jade's own set_epoch QR flow adjusts device time
# (main/qrmode.c:1298 handle_epoch_qr), and it WORKS in this port: libjade/libjade.c:179
# settimeofday_host forwards to the host handler registered through libjade_set_clock_handler;
# pijade-host registers it in pijade_host.c:412. Measured on-device on 2026-08-28: the supplied epoch
# appeared on screen, and time-based TOTP (main/otpauth.c:597 time(NULL)) matched an independent
# generator. Adjusted time survives only the current session; there is NO persistence,
# nor should there be. This only disables the failing shutdown step introduced by T7 itself.
# The distribution image MUST NOT carry a card ID. This script processes a .img for distribution;
# if the input is a copy of a card already booted, a valid pijade-machine-id remains on FAT and
# EVERY card written from it gets the same ID, breaking the scheme's sole requirement
# (uniqueness per card). Deletion happens only during preparation; the device unit preserves its ID.
rm -f /mnt/boot/pijade-machine-id /mnt/boot/pijade-machine-id.tmp

mkdir -p "$SYSTEMD_DIR/fake-hwclock.service.d"
cat > "$SYSTEMD_DIR/fake-hwclock.service.d/pijade.conf" <<'HWCLOCK'
[Service]
ExecStop=
HWCLOCK
chmod 0644 "$SYSTEMD_DIR/fake-hwclock.service.d/pijade.conf"

# --- Time zone: the distribution image must display UTC -------------------------------
# Base raspios ships with Europe/London (measured: /etc/timezone = Europe/London and
# /etc/localtime -> /usr/share/zoneinfo/Europe/London; this script never set the time zone).
# Why it matters: both time display paths apply the local time zone (main/qrmode.c:1325 and
# main/process/dashboard.c:1508,1548 ctime_r). The OTP screen prints a FIXED "UTC" label
# beside it (main/ui/otpauth.c:318). This MISREPORTS an hour during daylight saving: on 2026-08-28
# the device displayed UTC 18:59:40 as "19:59:40" (BST, +1). Upstream Jade carries no tzdata
# on ESP32, so ctime_r returns UTC and the label is correct; parity requires the same here.
# tzdata uses two files; write both (as dpkg-reconfigure tzdata does). Choose Etc/UTC as the
# target: it is the actual zoneinfo file, while /usr/share/zoneinfo/UTC is merely
# a symlink pointing to it.
ln -sfn /usr/share/zoneinfo/Etc/UTC /mnt/root/etc/localtime
printf 'Etc/UTC\n' > /mnt/root/etc/timezone

cat > /mnt/root/usr/local/sbin/pijade-machine-id <<'MACHINEID'
#!/bin/sh
# T7: generate/load the card's persistent device ID and mount it over /etc/machine-id.
set -u
ID_FILE=/boot/firmware/pijade-machine-id

# machine-id(5) format: 32 lowercase hex digits. Regenerate malformed files; this is safe
# because a malformed ID has never been displayed. (Unlike settings, where an unreadable file
# may be the only copy of the PIN wallet and MUST NOT BE OVERWRITTEN.)
# Validate the WHOLE file, not just its first line. Extra bytes after a valid first line would
# leak into /etc/machine-id through the bind mount: machine-id consumers reject it, libjade
# hashes an unintended value, and boot verification sees identical malformed content in both
# places and prints OK. The only accepted format is "<32 lowercase hex>\n", exactly 33 bytes, one line.
# Equally sized malformed inputs such as 31 hex + newline + 1 hex fail the length check.
STORED=""
if [ -f "$ID_FILE" ] \
        && [ "$(wc -c < "$ID_FILE")" -eq 33 ] \
        && [ "$(wc -l < "$ID_FILE")" -eq 1 ]; then
    CANDIDATE=$(head -n 1 "$ID_FILE")
    case "$CANDIDATE" in
        *[!0-9a-f]*) ;;
        *) [ "${#CANDIDATE}" -eq 32 ] && STORED="$CANDIDATE" ;;
    esac
fi

if [ -z "$STORED" ]; then
    # /dev/random blocks until the pool is seeded, then stops blocking. If it is never seeded,
    # the unit FAILS with TimeoutStartSec; boot continues and boot verification reports
    # the missing identity as an ERROR. Prefer this to silently generating a weak identity.
    STORED=$(head -c 16 /dev/random | od -An -v -tx1 | tr -d ' \n')
    if [ "${#STORED}" -ne 32 ]; then
        echo "pijade-machine-id: could not generate identity" >&2
        exit 1
    fi
    # Temporary file + rename: FAT has no journal; a power cut leaves either the old or new file,
    # never a partial file. host/settings_store.c uses the same method.
    printf '%s\n' "$STORED" > "$ID_FILE.tmp" || exit 1
    sync
    mv "$ID_FILE.tmp" "$ID_FILE" || exit 1
    sync
fi

# PID1 may already have mounted its temporary ID over the empty file; stacking this mount on
# top is correct, since readers see the topmost mount.
mount --bind "$ID_FILE" /etc/machine-id
MACHINEID
chmod 0755 /mnt/root/usr/local/sbin/pijade-machine-id

# Before=pijade.service: host reads macid once in main.c:119; the mount must exist before then.
# No Requires: if identity generation fails the device still boots and verification logs the failure.
cat > "$SYSTEMD_DIR/pijade-machine-id.service" <<'MACHINEIDUNIT'
[Unit]
Description=piJade persistent device identity
After=local-fs.target
Wants=local-fs.target
RequiresMountsFor=/boot/firmware
Before=pijade.service

[Service]
Type=oneshot
RemainAfterExit=yes
TimeoutStartSec=30
ExecStart=/usr/local/sbin/pijade-machine-id

[Install]
WantedBy=multi-user.target
MACHINEIDUNIT
chmod 0644 "$SYSTEMD_DIR/pijade-machine-id.service"
ln -sf /etc/systemd/system/pijade-machine-id.service \
       "$SYSTEMD_DIR/multi-user.target.wants/pijade-machine-id.service"

cat > /mnt/root/usr/local/sbin/pijade-boot-verify <<'VERIFY'
#!/bin/sh
LOG=/boot/firmware/pijade.log
FAIL=0
{
    echo "=== T7 BOOT VERIFICATION ==="

    # Check each measurement's exit code SEPARATELY from output. Empty output alone is insufficient:
    # a command that never runs also leaves empty output, and would silently be reported as OK.
    SWAP_OUTPUT=$(swapon --show); SWAP_RC=$?
    if [ "$SWAP_RC" -ne 0 ]; then
        echo "  swap: ERROR (swapon exit code $SWAP_RC)"
        FAIL=$((FAIL + 1))
    elif [ -z "$SWAP_OUTPUT" ]; then
        echo "  swap: OK (empty output)"
    else
        echo "  swap: ERROR (output not empty)"
        FAIL=$((FAIL + 1))
    fi

    ROOT_OPTIONS=$(findmnt -no OPTIONS /); ROOT_RC=$?
    if [ "$ROOT_RC" -ne 0 ]; then
        echo "  root: ERROR (findmnt exit code $ROOT_RC)"
        FAIL=$((FAIL + 1))
        ROOT_OPTIONS=""
    fi
    case ",$ROOT_OPTIONS," in
        *,ro,*) echo "  root: OK ($ROOT_OPTIONS)" ;;
        *) echo "  root: ERROR ($ROOT_OPTIONS)"; FAIL=$((FAIL + 1)) ;;
    esac

    LISTEN_OUTPUT=$(ss -H -tlnu); LISTEN_RC=$?
    if [ "$LISTEN_RC" -ne 0 ]; then
        echo "  listeners: ERROR (ss exit code $LISTEN_RC)"
        FAIL=$((FAIL + 1))
    elif [ -z "$LISTEN_OUTPUT" ]; then
        echo "  listeners: OK (empty output)"
    else
        echo "  listeners: ERROR (output not empty)"
        FAIL=$((FAIL + 1))
    fi

    PIJADE_STATUS=$(systemctl is-active pijade.service 2>&1); PIJADE_RC=$?
    if [ "$PIJADE_RC" -eq 0 ] && [ "$PIJADE_STATUS" = "active" ]; then
        echo "  pijade-started: OK ($PIJADE_STATUS)"
    else
        echo "  pijade-started: ERROR ($PIJADE_STATUS, exit code $PIJADE_RC)"
        FAIL=$((FAIL + 1))
    fi
    echo "  pijade-started limit: this measures service startup, not continued operation."
    echo "  Continued operation is shown by normal flow lines (panel size, shutdown request,"
    echo "  SIGTERM exit); these three lines appear ONLY when /boot/firmware/pijade-t40.enable"
    echo "  is present on the card. Without the marker, the log contains only fault lines."

    # Identity measurement COMPARES both files; checking for content is insufficient. On read-only
    # root, systemd PID1 itself mounts /run/machine-id over empty /etc/machine-id; that temporary
    # ID is also NONEMPTY. Checking only "is it empty" would report OK even if the persistent ID
    # was never mounted, marking the very fault being fixed as a success. Matching the topmost file
    # to the source file proves that the persistent identity is actually mounted.
    ID_FILE=/boot/firmware/pijade-machine-id
    MACHINE_ID=$(cat /etc/machine-id); MACHINE_RC=$?
    STORED_ID=$(cat "$ID_FILE"); STORED_RC=$?
    if [ "$MACHINE_RC" -ne 0 ]; then
        echo "  machine-id: ERROR (cat exit code $MACHINE_RC)"
        FAIL=$((FAIL + 1))
    elif [ "$STORED_RC" -ne 0 ]; then
        echo "  machine-id: ERROR (could not read persistent identity file, exit code $STORED_RC)"
        FAIL=$((FAIL + 1))
    elif [ -z "$STORED_ID" ]; then
        echo "  machine-id: ERROR (persistent identity empty)"
        FAIL=$((FAIL + 1))
    elif [ "$MACHINE_ID" != "$STORED_ID" ]; then
        echo "  machine-id: ERROR (temporary identity in use, persistent file not mounted)"
        FAIL=$((FAIL + 1))
    else
        echo "  machine-id: OK ($MACHINE_ID)"
    fi

    # Report only, NOT a gate. The device has no RTC and this port cannot set the clock, so
    # an incorrect clock is a known condition; counting it as an ERROR would break every boot.
    # Still log it: this is the comparison value when the TOTP screen date looks wrong.
    echo "  clock (info): $(date -u '+%Y-%m-%d %H:%M:%S UTC') - no RTC, cannot be set"

    # T4 (info, NOT a gate): when the kernel CRNG became ready at boot and whether hardware RNG
    # returns 16 bytes. The CRNG line does not prove hwrng contribution; the separate read only
    # measures whether the driver returns data. Device entropy uses getrandom(2) to block until
    # the pool is ready (libjade/libjade.c get_random); no weak-byte path exists. Measure the wait.
    # Compare the "crng init done" timestamp with pijade.service startup (both are monotonic;
    # dmesg uses seconds, systemd microseconds).
    # If the line is still absent, current uptime bounds the CRNG wait after pijade startup from
    # below. Do not wait/poll here, so this boot-critical oneshot does not delay the target.
    # At first boot, pijade-machine-id reads 16 bytes from /dev/random before pijade.service.
    # On Linux >= 5.6 this blocks until CRNG is ready; the "machine-id: OK" line above separately
    # proves that the pool was ready before pijade during this boot.
    # Quality: v6.12 hw_random/core.c exposes the `rng_quality` sysfs attribute; older kernels
    # only expose the now "obsolete" `rng_core.current_quality` parameter. Try both.
    RNG_QUALITY=$(cat /sys/class/misc/hw_random/rng_quality 2>/dev/null \
        || cat /sys/module/rng_core/parameters/current_quality 2>/dev/null || echo none)
    echo "  rng (info): hwrng=$(cat /sys/class/misc/hw_random/rng_current 2>/dev/null || echo none)" \
        "quality=$RNG_QUALITY" \
        "entropy_avail=$(cat /proc/sys/kernel/random/entropy_avail 2>/dev/null || echo none)"
    HWRNG_HEX=$(timeout 5 head -c 16 /dev/hwrng 2>/dev/null \
        | od -An -tx1 | tr -d ' \n' || echo none)
    HWRNG_HEX_CHARS=$(printf '%s' "$HWRNG_HEX" | wc -c | tr -d ' ' || echo none)
    if [ "$HWRNG_HEX_CHARS" = 32 ]; then
        HWRNG_READ_STATUS="16 bytes"
    else
        HWRNG_READ_STATUS="ERROR/timeout"
    fi
    echo "  rng (info): hwrng read: $HWRNG_READ_STATUS"
    RNG_UPTIME=$(cut -d' ' -f1 /proc/uptime 2>/dev/null || echo none)
    CRNG_LINE=$(dmesg 2>/dev/null | grep -m1 'crng init done' || echo none)
    if [ "$CRNG_LINE" = none ]; then
        CRNG_STATUS="crng init done still absent (uptime=${RNG_UPTIME} s)"
    else
        CRNG_STATUS=$CRNG_LINE
    fi
    echo "  rng (info): $CRNG_STATUS"
    echo "  rng (info): pijade.service start us=$(systemctl show -p ActiveEnterTimestampMonotonic --value pijade.service 2>/dev/null || echo none)" \
        "uptime=${RNG_UPTIME} s"

    # T4 (info, NOT a gate): boot identity derived from the pool. This is the cheapest way to
    # measure whether two boot pools differ; each boot derives the value from its pool. Identical
    # values across boots mean the same seed, a critical finding; different values close T4's
    # remaining gap. Compare MANUALLY, outside the script: root is read-only, there is no writable
    # location for the previous boot's value, and the FAT log already carries this line itself.
    # This is not a gate because a single boot has no comparison reference.
    BOOT_ID=$(cat /proc/sys/kernel/random/boot_id); BOOT_ID_RC=$?
    if [ "$BOOT_ID_RC" -ne 0 ] || [ -z "$BOOT_ID" ]; then
        echo "  boot_id (info): unreadable (exit code $BOOT_ID_RC)"
    else
        echo "  boot_id (info): $BOOT_ID"
    fi

    # Power and thermal health (info, NOT a gate). get_throttled carries STICKY state: bits 0-2
    # show current conditions, bits 16-18 show events SEEN since boot. This is not a gate:
    # a device once connected to a weak supply would show failure at each subsequent boot and
    # drown out real gates (swap, read-only root, listening sockets, identity); there is also
    # no action available here other than the user replacing the power supply.
    # Record the limit: a clean result does NOT rule out local panel 3V3 fluctuations; the Pi's
    # undervoltage reporting mainly checks the upstream supply.
    THROTTLED=$(vcgencmd get_throttled 2>&1); THROTTLED_RC=$?
    if [ "$THROTTLED_RC" -ne 0 ]; then
        echo "  power (info): vcgencmd ERROR (exit code $THROTTLED_RC: $THROTTLED)"
    else
        echo "  power (info): $THROTTLED"
    fi
    # Print the match count and last matching line, not the ENTIRE kernel log: the FAT log grows
    # at every boot and full dumps would soon make it unreadable. Measure dmesg's exit code SEPARATELY:
    # a pipeline would return grep's code, making an UNREADABLE kernel log (restricted permissions
    # or execution error) appear as "matches=0 last=none", indistinguishable from CLEAN power diagnostics.
    DMESG_OUT=$(dmesg 2>&1); DMESG_RC=$?
    if [ "$DMESG_RC" -ne 0 ]; then
        echo "  power (info): kernel log UNREADABLE (exit code $DMESG_RC)"
    else
        UV_LINES=$(printf '%s\n' "$DMESG_OUT" | grep -Ei 'under-voltage|voltage|throttl|regulator')
        UV_COUNT=$(printf '%s' "$UV_LINES" | grep -c .)
        UV_LAST=$(printf '%s' "$UV_LINES" | tail -1)
        echo "  power (info): kernel log matches=$UV_COUNT last=${UV_LAST:-none}"
    fi
    TEMP_ALL=$(for zone in /sys/class/thermal/thermal_zone*/temp; do
        [ -e "$zone" ] && printf '%s=%s ' "${zone%/temp}" "$(cat "$zone" 2>/dev/null || echo none)"
    done)
    echo "  temperature (info): ${TEMP_ALL:-no thermal_zone} (mC)"

    for device in /dev/spidev0.0 /dev/gpiochip0 /dev/video0; do
        if [ -e "$device" ]; then
            echo "  device ${device}: present"
        else
            echo "  device ${device}: absent"
        fi
    done

    if [ "$FAIL" -eq 0 ]; then
        echo "T7 VERIFICATION: OK"
        RESULT=0
    else
        echo "T7 VERIFICATION: ERROR"
        RESULT=1
    fi
} >> "$LOG" 2>&1
sync
exit "$RESULT"
VERIFY
chmod 0755 /mnt/root/usr/local/sbin/pijade-boot-verify

cat > "$SYSTEMD_DIR/pijade-boot-verify.service" <<'VERIFYUNIT'
[Unit]
Description=piJade T7 boot verification
RequiresMountsFor=/boot/firmware
# After=multi-user.target is DELIBERATELY ABSENT. Tried and reverted: this unit is enabled
# through that target's multi-user.target.wants, so waiting for it may force systemd to break
# an ordering cycle and drop this optional job; then no verification block appears in the
# FAT log. The distribution's 200+ units contain no instance of
# After=<target> + WantedBy=<same target>.
# The alternative was measured and is safe: after T7 the multi-user dependencies are pijade.service,
# this unit and six distribution units (dbus, getty.target, systemd-ask-password-wall.path,
# systemd-logind, systemd-update-utmp-runlevel, systemd-user-sessions). None opens TCP/UDP
# sockets: dbus only listens on /run/dbus/system_bus_socket, a Unix socket; getty instances are
# masked; the remaining three are oneshot or have no listeners. 'ss -H -tlnu' already excludes Unix
# sockets (no -x flag), so there is no source of late-starting TCP/UDP listeners.
After=pijade.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/pijade-boot-verify

[Install]
WantedBy=multi-user.target
VERIFYUNIT
chmod 0644 "$SYSTEMD_DIR/pijade-boot-verify.service"
ln -sf /etc/systemd/system/pijade-boot-verify.service \
       "$SYSTEMD_DIR/multi-user.target.wants/pijade-boot-verify.service"

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

echo "=== 9) UNMOUNT ==="
sync
umount /mnt/boot
umount /mnt/root
losetup -d "$BOOT_LOOP"; losetup -d "$ROOT_LOOP"
LOOPS=""

echo "=== 10) GATE: FILESYSTEM INTEGRITY ==="
LOOP=$(losetup -o $ROOT_OFFSET --sizelimit $ROOT_SIZE -f --show /img); LOOPS="$LOOPS $LOOP"
e2fsck -fn "$LOOP"; FSCK=$?
losetup -d "$LOOP"; LOOPS=""
echo "FSCK_EXIT=$FSCK"

echo "=== 11) GATE: HASHES OF BINARIES INSIDE IMAGE ==="
LOOP2=$(losetup -o $BOOT_OFFSET --sizelimit $BOOT_SIZE -f --show /img); LOOPS="$LOOPS $LOOP2"
mount -o "$BOOT_OPTS" "$LOOP2" /mnt/boot
sha256sum /mnt/boot/pijade/pijade-host /mnt/boot/pijade/libjade.so /mnt/boot/pijade/pijade-t44-bench
echo "--- source ---"
sha256sum /tmp/pkg/opt/pijade/bin/pijade-host /tmp/pkg/opt/pijade/lib/libjade.so \
          /tmp/pkg/opt/pijade/bin/pijade-t44-bench
# The same three files, extracted from the package and placed in the image. A mismatch means copy corruption.
if ! cmp -s /tmp/pkg/opt/pijade/bin/pijade-host /mnt/boot/pijade/pijade-host \
   || ! cmp -s /tmp/pkg/opt/pijade/lib/libjade.so /mnt/boot/pijade/libjade.so \
   || ! cmp -s /tmp/pkg/opt/pijade/bin/pijade-t44-bench /mnt/boot/pijade/pijade-t44-bench; then
    echo "ERROR: image binary differs from extracted package binary"; exit 1
fi
echo "HASH GATE: PASSED"
umount /mnt/boot
losetup -d "$LOOP2"; LOOPS=""
echo "PREPARATION_OK"
