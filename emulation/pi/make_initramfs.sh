#!/bin/bash
# Builds the initramfs of the emulated Pi-class guest: Alpine's busybox userspace, the kernel modules chosen by
# prepare.sh, and the stripped aarch64 Prometheus99 files (build-aarch64 from "make -f Makefile.linux STATIC=1
# CC=aarch64-linux-musl-gcc BUILD=build-aarch64 hmi").
#   bash emulation/pi/make_initramfs.sh [repo-dir]
set -eu
REPO=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:-/root/work/pi}
STRIP=${STRIP:-/opt/toolchains/aarch64-linux-musl-cross/bin/aarch64-linux-musl-strip}
B="$REPO/build-aarch64"
rm -rf "$WORK/rootfs" && cp -a "$WORK/base" "$WORK/rootfs"
cp -a "$WORK/modset/lib/modules/." "$WORK/rootfs/lib/modules/"
mkdir -p "$WORK/rootfs/p99"
"$STRIP" -s -o "$WORK/rootfs/p99/prometheus99" "$B/prometheus99"
"$STRIP" -s -o "$WORK/rootfs/p99/p99_sign" "$B/p99_sign"
cp "$REPO/config/hmi.example.json" "$REPO/demo/make_demo_config.sh" "$WORK/rootfs/p99/"
cp "$REPO/emulation/pi/guest_init.sh" "$WORK/rootfs/init"
chmod 755 "$WORK/rootfs/init" "$WORK/rootfs/p99/prometheus99" "$WORK/rootfs/p99/p99_sign"
(cd "$WORK/rootfs" && find . | cpio -o -H newc 2> /dev/null | gzip -9 > "$WORK/initramfs-p99.cpio.gz")
ls -l "$WORK/initramfs-p99.cpio.gz" "$WORK/rootfs/p99"
