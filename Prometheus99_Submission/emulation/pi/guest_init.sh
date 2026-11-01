#!/bin/sh
# /init of the emulated Raspberry Pi-class guest (aarch64 Linux under QEMU). It loads the display, pointer and network
# drivers, signs a configuration for the PLC on the host (reached through QEMU's user network at 10.0.2.2), starts the
# real aarch64 program on the emulated framebuffer and the emulated touch device, and prints its memory use to the console.
export PATH=/sbin:/bin:/usr/sbin:/usr/bin
/bin/busybox --install -s
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2> /dev/null
mkdir -p /tmp /run
mount -t tmpfs tmpfs /tmp
echo "p99-guest: kernel $(uname -r) $(uname -m)"
for m in virtio_input evdev virtio-gpu virtio_net; do modprobe $m 2>&1 || echo "p99-guest: modprobe $m failed"; done

HOST=10.0.2.2
PORT=502
for x in $(cat /proc/cmdline); do
    case $x in
        p99host=*) HOST=${x#p99host=} ;;
        p99port=*) PORT=${x#p99port=} ;;
        p99role=*) ROLE=${x#p99role=} ;;
    esac
done
ifconfig lo up
ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up 2> /dev/null
route add default gw 10.0.2.2 2> /dev/null

i=0
while [ $i -lt 50 ] && [ ! -e /dev/fb0 ]; do sleep 0.2; i=$((i + 1)); done
ls -l /dev/fb0 /dev/input/ 2>&1 | sed 's/^/p99-guest: /'
cat /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/virtual_size 2>&1 | sed 's/^/p99-guest: fb0 /'

cd /p99 || exit 1
sh make_demo_config.sh hmi.example.json hmi.json "$HOST" "$PORT"
./p99_sign keygen --key config.key > /dev/null
./p99_sign sign --config hmi.json --key config.key > /dev/null
ln -sf prometheus99 p99_cfg
mkdir -p /var/lib/p99

./prometheus99 --config hmi.json --audit /var/lib/p99/audit.log --alarms /var/lib/p99/alarms.bin \
    --config-key config.key --config-state /var/lib/p99/config.serial --fb /dev/fb0 --touch /dev/input/event0 \
    --role "${ROLE:-operator}" &
PID=$!
echo "p99-guest: prometheus99 pid $PID, PLC $HOST:$PORT"
while kill -0 $PID 2> /dev/null; do
    sleep 5
    echo "p99-guest: MEM pss_kb=$(awk '/^Pss:/{print $2}' /proc/$PID/smaps_rollup 2> /dev/null) rss_kb=$(awk '/^VmRSS:/{print $2}' /proc/$PID/status 2> /dev/null) fds=$(ls /proc/$PID/fd 2> /dev/null | wc -l)"
done
echo "p99-guest: prometheus99 exited"
poweroff -f
