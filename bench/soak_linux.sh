#!/bin/bash
# Continuous-operation test of the Linux executable against the fake PLC, with faults injected on a schedule.
#
#   bench/soak_linux.sh <build dir> <label> <seconds> [fault_period_s]
#
# The executable (prometheus99-bench) runs with an audit file, an alarm state file and the example configuration.
# The fake PLC (build/fake_plc) moves its data so that the alarm limits are crossed all the time, and every
# fault_period_s (default 300) it is made to misbehave in turn: it hangs (reads requests, never answers), it drops
# the connection, it serves normally again. Every 30 s the script samples the process: PSS, RSS, threads, open
# files, bytes written, CPU, and the size of the audit and journal files. At the end the process is stopped with
# SIGTERM (the exit status must be 0) and the files are checked with build/killloop_verify (every audit line is a complete
# record, every write id follows its rules, the journal lines parse).
#
# Host measurement (a loopback PLC, a file as the framebuffer): it says nothing about a real display, a real PLC, a
# real network or an SD card. The duration is what was asked for; the summary prints what was actually run.
# Output in bench/results/linux/<label>/: soak_samples.csv, soak_summary.txt, soak_events.txt
set -u
B=${1:?build dir}
LABEL=${2:?label}
SECS=${3:?seconds}
FAULT=${4:-300}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/bench/results/linux/$LABEL
mkdir -p "$OUT"
W=$(mktemp -d /tmp/p99_soak.XXXXXX)
now_s() { date +%s; }

"$B/fake_plc" $((SECS + 120)) 100 1 "$W/plc.stats" > "$W/plc.out" 2> "$W/plc.err" &
PLC=$!
for i in $(seq 1 50); do grep -q '^port ' "$W/plc.out" && break; sleep 0.1; done
PORT=$(sed -n 's/^port //p' "$W/plc.out")
sed -e 's/192\.168\.0\.10/127.0.0.1/' -e "s/\"port\": 502,/\"port\": $PORT,/" "$ROOT/config/hmi.example.json" > "$W/cfg.json"
. "$ROOT/tests/hmi/sign_fixture.sh"
sign_config "$B" "$W" || { echo "cannot sign"; kill $PLC; exit 1; }

START=$(now_s)
"$B/prometheus99-bench" --config "$W/cfg.json" --fb "$W/fb.raw" --touch /nonexistent --role operator \
  --audit "$W/audit.log" --alarms "$W/alarms.bin" ${P99_EXTRA:-} \
  --config-key "$W/cfg.key" --config-state "$W/cfg.serial" > "$OUT/soak_app.log" 2>&1 &
APP=$!
sleep 2
CLK=$(getconf CLK_TCK)
echo "t_s,pss_kb,rss_kb,threads,fds,write_bytes,cpu_pct,audit_bytes,journal_lines,plc_requests,state" > "$OUT/soak_samples.csv"
: > "$OUT/soak_events.txt"
state=normal
next_fault=$((START + FAULT))
fault_i=0
prev_ticks=$(awk '{print $14+$15}' /proc/$APP/stat); prev_t=$(date +%s%N)
while true; do
    sleep 30
    kill -0 $APP 2>/dev/null || { echo "$(( $(now_s) - START )) s: EXECUTABLE DIED" >> "$OUT/soak_events.txt"; break; }
    t=$(( $(now_s) - START ))
    ticks=$(awk '{print $14+$15}' /proc/$APP/stat); tn=$(date +%s%N)
    cpu=$(awk -v dc=$((ticks - prev_ticks)) -v dt=$(( (tn - prev_t) / 1000 )) -v clk=$CLK 'BEGIN {printf "%.2f", 100.0 * (dc / clk) / (dt / 1e6)}')
    prev_ticks=$ticks; prev_t=$tn
    pss=$(awk '$1=="Pss:" {print $2}' /proc/$APP/smaps_rollup); rss=$(awk '$1=="Rss:" {print $2}' /proc/$APP/smaps_rollup)
    thr=$(awk '/^Threads:/ {print $2}' /proc/$APP/status)
    fds=$(ls /proc/$APP/fd | wc -l)
    wb=$(awk '/^write_bytes:/ {print $2}' /proc/$APP/io)
    ab=$(cat "$W"/audit.log* 2>/dev/null | wc -c)
    jl=$(cat "$W"/alarms.bin.journal* 2>/dev/null | wc -l)
    rq=$(sed -n 's/^requests=\([0-9]*\).*/\1/p' "$W/plc.stats" 2>/dev/null)
    echo "$t,$pss,$rss,$thr,$fds,$wb,$cpu,$ab,$jl,${rq:-0},$state" >> "$OUT/soak_samples.csv"
    now=$(now_s)
    if [ "$now" -ge $((START + SECS)) ]; then break; fi
    if [ "$now" -ge "$next_fault" ]; then
        case $((fault_i % 2)) in
            0) kill -USR2 $PLC; state=plc_hung;    echo "$t s: PLC hangs (reads, never answers)" >> "$OUT/soak_events.txt" ;;
            1) kill -USR1 $PLC; state=plc_dropped; echo "$t s: PLC drops the connection" >> "$OUT/soak_events.txt" ;;
        esac
        fault_i=$((fault_i + 1))
        next_fault=$((now + FAULT))
        fault_until=$((now + 60))
    fi
    if [ "$state" != normal ] && [ "$now" -ge "${fault_until:-0}" ]; then
        kill -HUP $PLC; state=normal; echo "$t s: PLC serves normally again" >> "$OUT/soak_events.txt"
    fi
done
END=$(now_s)
kill -HUP $PLC 2>/dev/null
kill -TERM $APP 2>/dev/null; wait $APP 2>/dev/null; RC=$?
kill $PLC 2>/dev/null; wait $PLC 2>/dev/null
cp "$W/audit.log"* "$W/alarms.bin"* "$OUT/" 2>/dev/null || true
VERIFY="not run"
if [ -x "$B/killloop_verify" ]; then
    # the third argument makes the verifier read the journal with this run's tag names (it counts CONFIG_ACCEPTED, a form it
    # does not know, as one damaged audit line; the allowance of 5 covers it)
    VERIFY=$("$B/killloop_verify" "$W" 5 "$ROOT/config/hmi.example.json" 2>&1 | tail -3 | tr '\n' ' ')
fi
{
    echo "label: $LABEL   date: $(date -u +%FT%TZ)   host: $(uname -r)"
    echo "requested_seconds: $SECS   actual_seconds: $((END - START))   fault_period_s: $FAULT   faults_injected: $fault_i"
    echo "exit_status_on_SIGTERM: $RC"
    echo "verify: $VERIFY"
    awk -F, 'NR>1 && NR==2 {p0=$2; t0=$1} NR>1 {p1=$2; t1=$1; if ($2>pmax) pmax=$2; if (pmin==""||$2<pmin) pmin=$2; if ($4>tmax) tmax=$4; if ($5>fmax) fmax=$5; if ($7>cmax) cmax=$7; cs+=$7; n++}
        END {printf "samples: %d   pss_kb first=%d last=%d min=%d max=%d   threads_max=%d   fds_max=%d   cpu_pct mean=%.2f max=%.2f\n", n, p0, p1, pmin, pmax, tmax, fmax, cs/n, cmax}' "$OUT/soak_samples.csv"
    awk -F, 'NR>1 {a[NR]=$2; t[NR]=$1; n=NR} END { if (n > 4) {
        # least-squares slope of PSS over time, KB per hour
        for (i=2;i<=n;i++){sx+=t[i];sy+=a[i];sxx+=t[i]*t[i];sxy+=t[i]*a[i];m++}
        d=m*sxx-sx*sx; if (d>0) printf "pss_slope_kb_per_hour: %.2f\n", (m*sxy-sx*sy)/d*3600 } }' "$OUT/soak_samples.csv"
    tail -1 "$OUT/soak_samples.csv" | awk -F, '{printf "last_sample: audit_bytes=%s journal_lines=%s plc_requests=%s write_bytes=%s\n", $8, $9, $10, $6}'
    echo "plc: $(cat "$W/plc.err")"
} | tee "$OUT/soak_summary.txt"
rm -rf "$W"
