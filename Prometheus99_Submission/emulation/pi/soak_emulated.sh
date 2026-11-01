#!/bin/bash
# Continuous-operation run of the emulated Pi-class target: boots the guest headless, injects a PLC fault every 5
# minutes (60 s of dropped connections, then recovery), and records the program's memory every 5 s (guest console).
#   bash emulation/pi/soak_emulated.sh [seconds]    default 2400
# The emulation runs the aarch64 program on a slow emulated CPU; the figures that matter here are stability and memory,
# not CPU time.
set -eu
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:-/root/work/pi}
DUR=${1:-2400}
export REPO SECONDS_TO_RUN=$((DUR + 300)) RUN_DIR=${RUN_DIR:-$WORK/soak}
mkdir -p "$RUN_DIR"
bash "$REPO/emulation/pi/run_qemu.sh" headless > "$RUN_DIR/qemu.out" 2>&1 &
sleep 4
PLC=$(cat "$RUN_DIR/plc.pid")
T0=$(date +%s)
until grep -q "prometheus99 pid" "$RUN_DIR/guest.log" 2> /dev/null; do sleep 2; done
echo "program started $(( $(date +%s) - T0 )) s after boot start" > "$RUN_DIR/soak_events.txt"
faults=0
while [ $(( $(date +%s) - T0 )) -lt "$DUR" ]; do
    sleep 300
    kill -USR1 "$PLC"; faults=$((faults + 1))
    echo "$(( $(date +%s) - T0 )) s: fault $faults (connections dropped)" >> "$RUN_DIR/soak_events.txt"
    sleep 60
    kill -HUP "$PLC"
    echo "$(( $(date +%s) - T0 )) s: PLC restored" >> "$RUN_DIR/soak_events.txt"
done
echo "ended at $(( $(date +%s) - T0 )) s; faults injected: $faults" >> "$RUN_DIR/soak_events.txt"
alive=no; grep -q "prometheus99 exited" "$RUN_DIR/guest.log" || alive=yes
echo "program still running at the end: $alive" >> "$RUN_DIR/soak_events.txt"
cp "$RUN_DIR/guest.log" "$RUN_DIR/guest_console.log"
pkill -f qemu-system-aarch64 || true
pkill fake_plc || true
cat "$RUN_DIR/soak_events.txt"
grep -a "MEM" "$RUN_DIR/guest.log" | awk '{print $3}' | sed 's/pss_kb=//' | sort -n | awk 'NR==1{min=$1} {a[NR]=$1} END{print "pss_kb samples:", NR, "min", min, "max", a[NR]}'
