#!/bin/bash
# Boots the emulated Raspberry Pi-class target (aarch64, Cortex-A53, virtual GPU and touch device) and runs the real
# aarch64 Prometheus99 program in it against a fake PLC on this machine (a real Modbus TCP connection through QEMU's
# user network). See docs/EMULATION.md for what this proves and what it does not.
#
#   bash emulation/pi/run_qemu.sh window        a visible emulated screen (GTK window; WSLg on Windows 11)
#   bash emulation/pi/run_qemu.sh headless      no window; screenshots through the QEMU monitor (used by drive.py)
# Environment: WORK (default /root/work/pi), REPO (default: the repo containing this file), ROLE, SECONDS_TO_RUN.
set -eu
MODE=${1:-window}
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:-/root/work/pi}
FAKE=${FAKE_PLC:-$REPO/build-sdl/fake_plc}
RUN=${RUN_DIR:-$WORK/run}
mkdir -p "$RUN"
rm -f "$RUN/qmp.sock" "$RUN/guest.log" "$RUN/plc.out"
"$FAKE" "${SECONDS_TO_RUN:-900}" 100 2 "$RUN/plc_stats.txt" > "$RUN/plc.out" &
PLC=$!
trap 'kill $PLC 2>/dev/null' EXIT
sleep 1
PORT=$(awk '/^port/{print $2}' "$RUN/plc.out")
echo "$PLC" > "$RUN/plc.pid"
echo "fake PLC pid $PLC on host port $PORT"
case "$MODE" in
    window) DISPLAY_OPT="-display gtk,zoom-to-fit=off" ;;
    *) DISPLAY_OPT="-display none" ;;
esac
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/mnt/wslg/runtime-dir}
export DISPLAY=${DISPLAY:-:0}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 2 -m 256M \
    -kernel "$WORK/Image" -initrd "$WORK/initramfs-p99.cpio.gz" \
    -append "console=ttyAMA0 rdinit=/init quiet video=Virtual-1:800x480 p99host=10.0.2.2 p99port=$PORT p99role=${ROLE:-operator} ${APPEND_EXTRA:-}" \
    -device virtio-gpu-pci,xres=800,yres=480 -device virtio-tablet-pci \
    -nic user,model=virtio-net-pci \
    -serial "file:$RUN/guest.log" -qmp "unix:$RUN/qmp.sock,server,nowait" $DISPLAY_OPT -name "Prometheus99 emulated Pi-class target"
