set -e
apk add --no-cache util-linux >/dev/null 2>&1
L=$(losetup -f --show -o 545259520 --sizelimit 1954545664 /img)
mkdir -p /mnt/root; mount -t ext4 -o ro "$L" /mnt/root
mkdir -p /out/usr/local/sbin /out/bin /out/boot/firmware/pijade
cp -a /mnt/root/usr/local/sbin/pijade-machine-id /mnt/root/usr/local/sbin/pijade-boot-verify /out/usr/local/sbin/
printf '#!/bin/sh\n' > /out/bin/sync; chmod 755 /out/bin/sync
printf '#!/bin/sh\n' > /out/boot/firmware/pijade/pijade-host; chmod 755 /out/boot/firmware/pijade/pijade-host
umount /mnt/root; losetup -d "$L"
