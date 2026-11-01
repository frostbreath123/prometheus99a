#!/bin/bash
# Fetches the pieces of the emulated Raspberry Pi-class target and prepares them in $WORK (default /root/work/pi):
#   - an arm64 Linux kernel and its modules (Alpine Linux "virt" flavour, 6.18.x, aarch64): a generic arm64 kernel,
#     NOT a Raspberry Pi kernel, running on QEMU's "virt" machine with a Cortex-A53 CPU (the core of a Pi 3 / Zero 2 W)
#   - a small userspace (Alpine's initramfs with busybox) that the next script extends with the Prometheus99 files
# Needs: qemu-system-arm, curl, squashfs-tools, cpio, python3, kmod. Network access for the download (about 90 MB).
set -eu
WORK=${WORK:-/root/work/pi}
U=https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/aarch64/netboot
KVER=6.18.58-0-virt
mkdir -p "$WORK/dl" && cd "$WORK/dl"
for f in vmlinuz-virt initramfs-virt modloop-virt "config-$KVER"; do
    [ -s "$f" ] || curl -sSfO "$U/$f"
done
# vmlinuz-virt is an EFI "zboot" image: the arm64 Image is the gzip stream inside it, which QEMU can load directly
python3 - <<'PY'
import zlib
d = open("vmlinuz-virt", "rb").read()
i = d.find(b"\x1f\x8b\x08")
img = zlib.decompressobj(31).decompress(d[i:])
open("../Image", "wb").write(img)
print("kernel Image bytes", len(img), "magic", img[56:60])
PY
[ -d ml ] || { mkdir ml && unsquashfs -f -d ml modloop-virt > /dev/null; }
# the modules the display, the pointer and the network need, with their dependencies
mkdir -p "$WORK/root/lib" && ln -sfn "$WORK/dl/ml/modules" "$WORK/root/lib/modules"
rm -rf "$WORK/modset" && mkdir -p "$WORK/modset/lib/modules/$KVER"
for m in virtio-gpu virtio_input evdev virtio_net; do
    modprobe -d "$WORK/root" -S "$KVER" --show-depends "$m" | awk '{print $2}'
done | sort -u > "$WORK/modset/list.txt"
while read -r f; do
    rel=${f#"$WORK/root/lib/modules/$KVER/"}
    mkdir -p "$WORK/modset/lib/modules/$KVER/$(dirname "$rel")"
    cp "$f" "$WORK/modset/lib/modules/$KVER/$rel"
done < "$WORK/modset/list.txt"
cp "$WORK/dl/ml/modules/$KVER"/modules.* "$WORK/modset/lib/modules/$KVER/"
rm -rf "$WORK/base" && mkdir "$WORK/base" && cd "$WORK/base"
gzip -dc "$WORK/dl/initramfs-virt" | cpio -idm 2> /dev/null
echo "modules to load: $(wc -l < "$WORK/modset/list.txt")"; cat "$WORK/modset/list.txt" | sed "s|.*/||"
echo "base initramfs files: $(find . | wc -l)"
ls -l "$WORK/Image"
