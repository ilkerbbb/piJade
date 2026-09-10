set -e
apk add --no-cache util-linux >/dev/null 2>&1
L=$(losetup -f --show -o 545259520 --sizelimit 1954545664 /img)
mkdir -p /mnt/root; mount -t ext4 -o ro "$L" /mnt/root
mkdir -p /out/etc/systemd/system /out/lib/systemd/system /out/usr/local/sbin
cp -a /mnt/root/etc/systemd/system/pijade*.service /out/etc/systemd/system/ 2>/dev/null || true
cp -a /mnt/root/lib/systemd/system/*.target /out/lib/systemd/system/ 2>/dev/null || true
cp -a /mnt/root/lib/systemd/system/*.service /out/lib/systemd/system/ 2>/dev/null || true
umount /mnt/root; losetup -d "$L"
