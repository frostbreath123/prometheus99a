#!/bin/bash
# Phase 0 measurement of the Linux executable (docs/OPTIMIZATION_ROADMAP.md, metrics A and B).
#
#   bench/measure_linux.sh <build dir> <label> [seconds]
#
# Needs the targets of `make -f Makefile.linux hmi-bench` (prometheus99-bench, fake_plc, p99_sign).
# The executable runs against a fake PLC on loopback with a regular file as its framebuffer, so the
# numbers are for the process only. The framebuffer mapping is reported on its own line and is NOT
# part of the process figure ("pss_excl_fb_kb"). Host measurement: x86-64 under WSL2, not a Pi.
#
# Output in bench/results/linux/<label>/:
#   summary.txt            one block of results (start-up time, PSS after the first frame and at the end, ...)
#   samples.csv            one row every 5 s: pss, rss, private, threads, cpu percent of one core
#   smaps_first_frame.txt  every mapping, size and PSS, right after the first frame
#   smaps_end.txt          the same at the end
#   app.log                what the executable printed
set -u
# P99_EXTRA: extra options for the executable (the baseline run of the old tree needed "--users FILE").
# P99_PLC_ARGS: arguments for the fake PLC (for example "10 40" for heavy updates); P99_CFG_SED: a sed expression applied
# to the configuration (for example s/"poll_ms": 200/"poll_ms": 20/); P99_FAKE_PLC: another fake PLC binary.
B=${1:?build dir}
LABEL=${2:?label}
SECS=${3:-600}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/bench/results/linux/$LABEL
mkdir -p "$OUT"
W=$(mktemp -d /tmp/p99_bench.XXXXXX)
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

"${P99_FAKE_PLC:-$B/fake_plc}" $((SECS + 60)) ${P99_PLC_ARGS:-} > "$W/plc.out" 2> "$W/plc.err" &
PLC=$!
for i in $(seq 1 50); do grep -q '^port ' "$W/plc.out" && break; sleep 0.1; done
PORT=$(sed -n 's/^port //p' "$W/plc.out")
sed -e 's/192.168.0.10/127.0.0.1/' -e "s/\"port\": 502,/\"port\": $PORT,/" -e "${P99_CFG_SED:-s/^$//}" "$ROOT/config/hmi.example.json" > "$W/cfg.json"
. "$ROOT/tests/hmi/sign_fixture.sh"
sign_config "$B" "$W" || { echo "cannot sign"; kill $PLC; exit 1; }

FB=$W/fb.raw
T0=$(now_ms)
"$B/prometheus99-bench" --config "$W/cfg.json" --fb "$FB" --touch /nonexistent --role operator \
  --audit "$W/audit.log" --alarms "$W/alarms.bin" ${P99_EXTRA:-} \
  --config-key "$W/cfg.key" --config-state "$W/cfg.serial" > "$OUT/app.log" 2>&1 &
APP=$!

# First complete frame (not yet PLC data: the values appear when the first poll answers).
TFIRST=0
for i in $(seq 1 2000); do
    # The last pixel row (1600 bytes) is drawn last, so non-zero content there means the whole screen has been flushed.
    if [ -s "$FB" ] && [ -n "$(tail -c 1600 "$FB" 2>/dev/null | tr -d '\0' | head -c 1)" ]; then TFIRST=$(now_ms); break; fi
    kill -0 $APP 2>/dev/null || break
    sleep 0.005
done
if [ "$TFIRST" -eq 0 ]; then echo "no frame"; kill $APP $PLC 2>/dev/null; exit 1; fi
START_MS=$((TFIRST - T0))

smaps_total() { # pid file: kb values from smaps_rollup
    awk -v k="$2:" '$1==k {print $2}' /proc/$1/smaps_rollup
}
dump_maps() { # pid out: one line per mapping with Size and Pss
    awk '/^[0-9a-f]+-[0-9a-f]+ / {name=$6; if (name=="") name="[anon]"; range=$1; perms=$2}
         /^Size:/ {sz=$2} /^Pss:/ {pss=$2; printf "%-8s %-5s size_kb=%-7s pss_kb=%-7s %s\n", range, perms, sz, pss, name}' /proc/$1/smaps > "$2"
}
fb_pss() { # kb of the mapping of the framebuffer file
    awk -v f="$FB" '/^[0-9a-f]+-[0-9a-f]+ / {on=($6==f)} on && /^Pss:/ {s+=$2} END {print s+0}' /proc/$1/smaps
}

sleep 1   # let the first poll cycle finish
dump_maps $APP "$OUT/smaps_first_frame.txt"
PSS1=$(smaps_total $APP Pss); RSS1=$(smaps_total $APP Rss); FB1=$(fb_pss $APP)
THR1=$(awk '/^Threads:/ {print $2}' /proc/$APP/status)
VMDATA=$(awk '/^VmData:/ {print $2}' /proc/$APP/status); VMSTK=$(awk '/^VmStk:/ {print $2}' /proc/$APP/status)
NMAP=$(wc -l < /proc/$APP/maps)

CLK=$(getconf CLK_TCK)
cpu_ticks() { awk '{print $14+$15}' /proc/$APP/stat; }
echo "t_s,pss_kb,rss_kb,private_kb,threads,cpu_pct_one_core" > "$OUT/samples.csv"
C0=$(cpu_ticks); TS0=$(now_ms); CSTART=$C0; TSTART=$TS0
PSSMAX=$PSS1
while true; do
    sleep 5
    kill -0 $APP 2>/dev/null || { echo "executable died" >> "$OUT/app.log"; break; }
    NOW=$(now_ms); C=$(cpu_ticks)
    PCT=$(awk -v dc=$((C - C0)) -v dt=$((NOW - TS0)) -v clk=$CLK 'BEGIN {printf "%.2f", 100.0 * (dc / clk) / (dt / 1000.0)}')
    PSS=$(smaps_total $APP Pss); RSS=$(smaps_total $APP Rss)
    PRIV=$(( $(smaps_total $APP Private_Dirty) + $(smaps_total $APP Private_Clean) ))
    THR=$(awk '/^Threads:/ {print $2}' /proc/$APP/status)
    echo "$(( (NOW - TSTART) / 1000 )),$PSS,$RSS,$PRIV,$THR,$PCT" >> "$OUT/samples.csv"
    [ "$PSS" -gt "$PSSMAX" ] && PSSMAX=$PSS
    C0=$C; TS0=$NOW
    [ $(( (NOW - TSTART) / 1000 )) -ge "$SECS" ] && break
done
kill -0 $APP 2>/dev/null && {
    dump_maps $APP "$OUT/smaps_end.txt"
    PSS2=$(smaps_total $APP Pss); RSS2=$(smaps_total $APP Rss); FB2=$(fb_pss $APP)
    THR2=$(awk '/^Threads:/ {print $2}' /proc/$APP/status)
    CEND=$(cpu_ticks); TEND=$(now_ms)
    CPUAVG=$(awk -v dc=$((CEND - CSTART)) -v dt=$((TEND - TSTART)) -v clk=$CLK 'BEGIN {printf "%.2f", 100.0 * (dc / clk) / (dt / 1000.0)}')
    IO=$(awk '/^(wchar|write_bytes):/ {printf "%s=%s ", $1, $2}' /proc/$APP/io 2>/dev/null)
    FDS=$(ls /proc/$APP/fd 2>/dev/null | wc -l)
    AUDIT_LINES=$(cat "$W"/audit.log* 2>/dev/null | wc -l); AUDIT_BYTES=$(cat "$W"/audit.log* 2>/dev/null | wc -c)
    JOURNAL_LINES=$(cat "$W"/alarms.bin.journal* 2>/dev/null | wc -l)
    kill -TERM $APP; wait $APP 2>/dev/null; RC=$?
}
kill $PLC 2>/dev/null; wait $PLC 2>/dev/null

{
    echo "label: $LABEL"
    echo "binary: $B/prometheus99-bench ($(stat -c %s "$B/prometheus99-bench") bytes unstripped)"
    echo "date: $(date -u +%FT%TZ)   kernel: $(uname -r)   cpus: $(nproc)"
    echo "framebuffer: file-backed, 800x480 RGB565 = 768000 bytes (its pages are shared file pages)"
    echo "start_to_first_frame_ms: $START_MS   (process launch to non-zero content in the last pixel row of the framebuffer, polled every 5 ms)"
    echo "after_first_frame:  pss_kb=$PSS1 rss_kb=$RSS1 pss_fb_mapping_kb=$FB1 pss_excl_fb_kb=$((PSS1 - FB1)) threads=$THR1 vmdata_kb=$VMDATA vmstk_kb=$VMSTK mappings=$NMAP"
    echo "after_${SECS}s:       pss_kb=${PSS2:-na} rss_kb=${RSS2:-na} pss_fb_mapping_kb=${FB2:-na} pss_excl_fb_kb=$(( ${PSS2:-0} - ${FB2:-0} )) threads=${THR2:-na}"
    echo "pss_peak_kb_sampled: $PSSMAX   (5 s sampling; a shorter peak can be missed)"
    echo "cpu_avg_pct_of_one_core_over_run: ${CPUAVG:-na}"
    echo "io_at_end: ${IO:-na}  open_fds=${FDS:-na}  audit_lines=${AUDIT_LINES:-na} audit_bytes=${AUDIT_BYTES:-na} journal_lines=${JOURNAL_LINES:-na}"
    echo "exit_status_on_SIGTERM: ${RC:-na}"
    echo "fake_plc: $(cat "$W/plc.err")"
} | tee "$OUT/summary.txt"
cp "$W/audit.log" "$OUT/audit_tail.log" 2>/dev/null
rm -rf "$W"
