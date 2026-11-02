#!/bin/bash
# Idle work profile of the headless executable (docs/MASTER_PROMPT_FINAL.md, phase 0 item 3).
#
#   bench/idle_profile.sh <build dir> <label> [seconds]
#
# Needs the targets of `make -f Makefile.linux hmi-bench` (prometheus99-bench, fake_plc, p99_sign) in <build dir>.
# The executable runs against the fake PLC on loopback with a regular file as its framebuffer and the overview screen
# shown, untouched. After a 3 s settling time it measures, over <seconds> (default 30):
#   - memory: Rss, Pss, Private (smaps_rollup), VmHWM, threads;
#   - CPU: user+system ticks of the whole process and of each thread;
#   - wake-ups: voluntary and involuntary context switches of each thread (/proc/<pid>/task/<tid>/status).
# Threads are named by creation order, which is fixed by the code: main (UI loop), store (storage worker,
# hmi_store_start), comms (hmi_runtime_start), super (supervisor). The framebuffer file mapping is reported on its own.
# P99_PLC_ARGS: extra arguments for the fake PLC ("10 40" is the heavy-update load); P99_EXTRA: extra options for the
# executable (for example "--report FILE").
# Output: bench/results/linux/<label>/idle_profile.txt and app.log.
set -u
B=${1:?build dir}
LABEL=${2:?label}
SECS=${3:-30}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/bench/results/linux/$LABEL
mkdir -p "$OUT"
W=$(mktemp -d /tmp/p99_idle.XXXXXX)

"$B/fake_plc" $((SECS + 60)) ${P99_PLC_ARGS:-} > "$W/plc.out" 2> "$W/plc.err" &
PLC=$!
for i in $(seq 1 50); do grep -q '^port ' "$W/plc.out" && break; sleep 0.1; done
PORT=$(sed -n 's/^port //p' "$W/plc.out")
sed -e 's/192.168.0.10/127.0.0.1/' -e "s/\"port\": 502,/\"port\": $PORT,/" -e "${P99_CFG_SED:-s/^$//}" "$ROOT/config/hmi.example.json" > "$W/cfg.json"
. "$ROOT/tests/hmi/sign_fixture.sh"
sign_config "$B" "$W" || { echo "cannot sign"; kill $PLC; exit 1; }

FB=$W/fb.raw
"$B/prometheus99-bench" --config "$W/cfg.json" --fb "$FB" --touch /nonexistent --role operator \
  --audit "$W/audit.log" --alarms "$W/alarms.bin" \
  --config-key "$W/cfg.key" --config-state "$W/cfg.serial" ${P99_EXTRA:-} > "$OUT/app.log" 2>&1 &
APP=$!
sleep 3
kill -0 $APP 2>/dev/null || { echo "executable died"; cat "$OUT/app.log"; kill $PLC; exit 1; }

NAMES=(main store comms super)
snap() { # prefix: per-thread ticks and switches into arrays
    local i=0
    for t in $(ls /proc/$APP/task | sort -n); do
        eval "$1_tid[$i]=$t"
        eval "$1_cpu[$i]=$(awk '{print $14+$15}' /proc/$APP/task/$t/stat)"
        eval "$1_vol[$i]=$(awk '/^voluntary_ctxt_switches:/ {print $2}' /proc/$APP/task/$t/status)"
        eval "$1_inv[$i]=$(awk '/^nonvoluntary_ctxt_switches:/ {print $2}' /proc/$APP/task/$t/status)"
        i=$((i + 1))
    done
    eval "$1_n=$i"
    eval "$1_proc=$(awk '{print $14+$15}' /proc/$APP/stat)"
    eval "$1_ms=$(( $(date +%s%N) / 1000000 ))"
}
snap a
sleep "$SECS"
kill -0 $APP 2>/dev/null || { echo "executable died during the run"; kill $PLC; exit 1; }
snap b
roll() { awk -v k="$1:" '$1==k {print $2}' /proc/$APP/smaps_rollup; }
RSS=$(roll Rss); PSS=$(roll Pss); PRIV=$(( $(roll Private_Dirty) + $(roll Private_Clean) ))
FBPSS=$(awk -v f="$FB" '/^[0-9a-f]+-[0-9a-f]+ / {on=($6==f)} on && /^Pss:/ {s+=$2} END {print s+0}' /proc/$APP/smaps)
HWM=$(awk '/^VmHWM:/ {print $2}' /proc/$APP/status)
THR=$(awk '/^Threads:/ {print $2}' /proc/$APP/status)
CLK=$(getconf CLK_TCK)
DT=$((b_ms - a_ms))
kill -TERM $APP; wait $APP 2>/dev/null; RC=$?
kill $PLC 2>/dev/null; wait $PLC 2>/dev/null

{
    echo "label: $LABEL"
    echo "binary: $B/prometheus99-bench ($(stat -c %s "$B/prometheus99-bench") bytes unstripped)"
    echo "date: $(date -u +%FT%TZ)   kernel: $(uname -r)   cpus: $(nproc)   clk_tck: $CLK"
    echo "workload: fake PLC on loopback, example configuration, overview screen, no input, ${SECS} s after 3 s settling"
    echo "memory: rss_kb=$RSS pss_kb=$PSS private_kb=$PRIV vmhwm_kb=$HWM pss_fb_mapping_kb=$FBPSS pss_excl_fb_kb=$((PSS - FBPSS)) threads=$THR"
    awk -v dc=$((b_proc - a_proc)) -v dt=$DT -v clk=$CLK 'BEGIN {printf "cpu_process_pct_of_one_core: %.2f\n", 100.0 * (dc / clk) / (dt / 1000.0)}'
    echo "per thread over $((DT / 1000)) s (ticks of 1/$CLK s; switches per second):"
    total=0
    for i in $(seq 0 $((b_n - 1))); do
        v=$(( b_vol[i] - a_vol[i] )); n=$(( b_inv[i] - a_inv[i] )); c=$(( b_cpu[i] - a_cpu[i] ))
        total=$((total + v + n))
        awk -v name="${NAMES[$i]:-thread$i}" -v v=$v -v n=$n -v c=$c -v dt=$DT \
            'BEGIN {printf "  %-6s cpu_ticks=%-4d voluntary=%-6d (%.1f/s) involuntary=%-4d (%.1f/s)\n", name, c, v, v * 1000.0 / dt, n, n * 1000.0 / dt}'
    done
    awk -v t=$total -v dt=$DT 'BEGIN {printf "  all    switches=%d (%.1f/s)\n", t, t * 1000.0 / dt}'
    echo "exit_status_on_SIGTERM: $RC"
    echo "fake_plc: $(cat "$W/plc.err")"
} | tee "$OUT/idle_profile.txt"
rm -rf "$W"
