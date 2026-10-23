#!/bin/bash
# Start-up time of the Linux executable: process launch to a completely drawn first frame.
#
#   bench/startup_linux.sh <build dir> <label> [runs]
#
# Run 1 is a cold start (the page cache is dropped first; needs root), the others are warm starts.
# Same set-up as measure_linux.sh (fake PLC on loopback, file-backed framebuffer). The time stops when
# the last pixel row of the framebuffer file has content; it does not include a real display driver, and
# the PLC values appear a little later (first poll). Host measurement, not a Raspberry Pi.
# Output: bench/results/linux/<label>/startup.txt
set -u
# P99_EXTRA: extra options for the executable (the baseline run of the old tree needed "--users FILE").
B=${1:?build dir}
LABEL=${2:?label}
RUNS=${3:-15}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/bench/results/linux/$LABEL
mkdir -p "$OUT"
W=$(mktemp -d /tmp/p99_start.XXXXXX)
now_us() { echo $(( $(date +%s%N) / 1000 )); }

"$B/fake_plc" 600 > "$W/plc.out" 2>/dev/null &
PLC=$!
for i in $(seq 1 50); do grep -q '^port ' "$W/plc.out" && break; sleep 0.1; done
PORT=$(sed -n 's/^port //p' "$W/plc.out")
sed -e 's/192\.168\.0\.10/127.0.0.1/' -e "s/\"port\": 502,/\"port\": $PORT,/" "$ROOT/config/hmi.example.json" > "$W/cfg.json"
. "$ROOT/tests/hmi/sign_fixture.sh"
sign_config "$B" "$W" || exit 1

TIMES=""
for r in $(seq 1 "$RUNS"); do
    [ "$r" -eq 1 ] && { sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; }
    rm -f "$W/fb.raw" "$W/audit.log" "$W/alarms.bin"
    T0=$(now_us)
    "$B/prometheus99-bench" --config "$W/cfg.json" --fb "$W/fb.raw" --touch /nonexistent --role operator \
      --audit "$W/audit.log" --alarms "$W/alarms.bin" ${P99_EXTRA:-} \
      --config-key "$W/cfg.key" --config-state "$W/cfg.serial" > /dev/null 2>&1 &
    APP=$!
    T1=0
    for i in $(seq 1 4000); do
        if [ -s "$W/fb.raw" ] && [ -n "$(tail -c 1600 "$W/fb.raw" 2>/dev/null | tr -d '\0' | head -c 1)" ]; then T1=$(now_us); break; fi
        kill -0 $APP 2>/dev/null || break
        sleep 0.002
    done
    kill -TERM $APP 2>/dev/null; wait $APP 2>/dev/null
    if [ "$T1" -eq 0 ]; then echo "run $r: no frame" >&2; continue; fi
    TIMES="$TIMES $(( (T1 - T0) / 1000 ))"
done
kill $PLC 2>/dev/null; wait $PLC 2>/dev/null
rm -rf "$W"
{
    echo "label: $LABEL   date: $(date -u +%FT%TZ)   runs: $RUNS"
    echo "cold_start_ms: $(echo $TIMES | awk '{print $1}')"
    echo "warm_start_ms (runs 2..): $(echo $TIMES | cut -d' ' -f2-)"
    echo $TIMES | tr ' ' '\n' | tail -n +2 | sort -n | awk '{a[NR]=$1} END {printf "warm min=%d median=%d max=%d ms\n", a[1], a[int((NR+1)/2)], a[NR]}'
    echo "(poll granularity about 2 ms plus process start; includes configuration signature check, runtime threads and the LVGL screen build)"
} | tee "$OUT/startup.txt"
