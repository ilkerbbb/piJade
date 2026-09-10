set -e
apk add --no-cache util-linux >/dev/null 2>&1
mkdir -p /mnt/root /mnt/boot
ROOT_OFF=$((1064960*512)); BOOT_OFF=$((16384*512))
mount -o ro,offset=$ROOT_OFF,sizelimit=$((3817472*512)) /img /mnt/root
mount -o ro,offset=$BOOT_OFF,sizelimit=$((1048576*512)) /img /mnt/boot
trap 'umount /mnt/root /mnt/boot 2>/dev/null' EXIT
KV=$(ls /mnt/root/lib/modules | grep v6 | head -1); echo "kernel: $KV"
echo "--- modules.builtin: rng ---"; grep -i "rng\|random" /mnt/root/lib/modules/$KV/modules.builtin || echo "(none)"
echo "--- kernel config ---"; CFG=$(ls /mnt/root/boot/config-$KV 2>/dev/null || true); echo "config file: ${CFG:-none}"
[ -n "$CFG" ] && { grep -E "CONFIG_HW_RANDOM(_BCM2835)?=|CONFIG_RANDOM_TRUST|CONFIG_HW_RANDOM_TPM" "$CFG" || echo "(none)"; }
echo "--- userspace rng tools ---"; for f in usr/sbin/rngd usr/sbin/haveged usr/bin/rngd; do [ -e /mnt/root/$f ] && echo "PRESENT: $f" || echo "none: $f"; done
echo "--- rng-related units ---"; find /mnt/root/usr/lib/systemd/system /mnt/root/etc/systemd/system -iname "*rng*" -o -iname "*haveged*" -o -iname "*random*" 2>/dev/null | sed 's|/mnt/root||'
echo "--- cmdline ---"; cat /mnt/boot/cmdline.txt
echo "--- config.txt rng/dtparam ---"; grep -i "rng\|random\|dtoverlay\|dtparam" /mnt/boot/config.txt | head -12
echo "--- device tree rng node ---"; DTB=$(ls /mnt/boot/bcm2708-rpi-zero-w.dtb 2>/dev/null); [ -n "$DTB" ] && strings "$DTB" | grep -i "rng" | head -5
