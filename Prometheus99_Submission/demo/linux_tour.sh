#!/bin/bash
# Scripted tour of the Linux desktop-window build (hmi-sdl): the real program in an SDL2 window, Modbus TCP over
# loopback to the fake PLC. It clicks the window with xdotool (like a touch), changes the PLC through signals, and saves
# pictures of what the window shows (SIGUSR1 makes the program save its frame). Evidence and test tool.
#
#   sudo apt install xdotool libsdl2-dev ...      (needs a window system: WSLg or X11)
#   bash demo/linux_tour.sh [build-dir] [out-dir]
set -u
cd "$(dirname "$0")/.."
B=${1:-build-sdl}
OUT=${2:-evidence/linux}
W="$B/tour_run"
export DISPLAY=${DISPLAY:-:0}
mkdir -p "$OUT" "$W"
rm -f "$OUT"/*.ppm "$OUT"/*.png "$OUT"/tour.log "$OUT"/measure.txt
LOG="$OUT/tour.log"
: > "$LOG"
say() { printf '%6d ms  %s\n' $(( ($(date +%s%N) - T0) / 1000000 )) "$1" >> "$LOG"; }

# the fake PLC: one TANK_LEVEL cycle every 30 s, so the high limit (80) is crossed a few seconds after the start
"$B/fake_plc" 600 100 2 "$W/plc_stats.txt" > "$W/plc.out" &
PLC=$!
sleep 1
PORT=$(awk '/^port/{print $2}' "$W/plc.out")
T0=$(date +%s%N)
sh demo/make_demo_config.sh config/hmi.example.json "$W/hmi.json" 127.0.0.1 "$PORT"
rm -f "$W/config.key" "$W/hmi.json.sig" "$W/config.serial" "$W/audit.log" "$W/alarms.bin"
"$B/p99_sign" keygen --key "$W/config.key" > /dev/null
"$B/p99_sign" sign --config "$W/hmi.json" --key "$W/config.key" > /dev/null

start_hmi() { # role
    P99_SHOT="$W/shot.ppm" "$B/prometheus99-sdl" --config "$W/hmi.json" --audit "$W/audit.log" --alarms "$W/alarms.bin" \
        --config-key "$W/config.key" --config-state "$W/config.serial" --role "$1" > "$W/hmi_$1.log" 2>&1 &
    HMI=$!
    WID=$(xdotool search --sync --onlyvisible --pid "$HMI" 2>/dev/null | tail -1)
}
shot() { # name
    rm -f "$W/shot.ppm"
    kill -USR1 "$HMI"
    for _ in $(seq 1 20); do [ -s "$W/shot.ppm" ] && break; sleep 0.1; done
    sleep 0.2
    mv "$W/shot.ppm" "$OUT/$1.ppm" 2>/dev/null && say "picture $1"
}
TARGETS=$(dirname "$0")/ui_targets.txt
declare -A T   # touch targets by name: "x y" (demo/ui_targets.txt, shared with the other tour drivers)
while read -r name x y; do
    case $name in "" | \#*) ;; *) T[$name]="$x $y" ;; esac
done < "$TARGETS"
click() { # target-name text
    local x y
    read -r x y <<< "${T[$1]}"
    say "$2"
    xdotool mousemove --window "$WID" "$x" "$y" mousedown 1
    sleep 0.15
    xdotool mouseup 1
}
rss() { awk '/^Pss:/{print $2}' "/proc/$HMI/smaps_rollup"; }

: > "$W/mem.csv"
start_hmi operator
say "program started (pid $HMI, window $WID)"
[ -n "$WID" ] && xdotool windowfocus "$WID" 2>/dev/null
sleep 0.8
echo "$(rss)" >> "$W/mem.csv"
shot 01_overview_normal            # about 2 s of the PLC clock: the tank is still below 80 %
sleep 3.2
echo "$(rss)" >> "$W/mem.csv"
# the tank passes 80 % between about 3.6 and 11.4 s of the PLC clock
shot 02_alarm_high_unacknowledged
click tab_alarms "tap ALARMS"; sleep 1.2
shot 03_alarm_list
click ack_all "tap ACK ALL"; sleep 1.2
shot 04_alarm_acknowledged
click tab_overview "tap OVERVIEW"; sleep 0.8
click setpoint_plus "tap setpoint +"; sleep 0.5
click setpoint_plus "tap setpoint +"; sleep 0.5
click setpoint_send "tap SEND (authorised write by the operator)"; sleep 2
shot 05_setpoint_written
click tab_trends "tap TRENDS"; sleep 1.5
shot 06_trends
click tab_settings "tap SETTINGS"; sleep 1.2
shot 07_settings
click tab_overview "tap OVERVIEW"; sleep 0.8
echo "$(rss)" >> "$W/mem.csv"
CPU0=$(awk '{print $14+$15}' "/proc/$HMI/stat"); WALL0=$(date +%s%N)
say "PLC link cut (fake PLC drops the connection)"
kill -USR1 "$PLC"
sleep 5
shot 08_plc_disconnected
say "PLC link restored"
RESTORE=$(date +%s%N)
kill -HUP "$PLC"
sleep 8
echo "$(rss)" >> "$W/mem.csv"
shot 09_recovered
CPU1=$(awk '{print $14+$15}' "/proc/$HMI/stat"); WALL1=$(date +%s%N)
kill "$HMI" 2>/dev/null; wait "$HMI" 2>/dev/null
say "program stopped"

start_hmi viewer
sleep 3
shot 10_viewer_no_write
kill "$HMI" 2>/dev/null; wait "$HMI" 2>/dev/null
kill "$PLC" 2>/dev/null

CLK=$(getconf CLK_TCK)
{
    echo "pss_kb samples (every few seconds, operator run): $(tr '\n' ' ' < "$W/mem.csv")"
    echo "pss_kb max: $(sort -n "$W/mem.csv" | tail -1)"
    echo "cpu_percent_of_one_core during the fault and recovery part: $(awk -v a="$CPU0" -v b="$CPU1" -v c="$CLK" -v w0="$WALL0" -v w1="$WALL1" 'BEGIN{printf "%.2f", 100*((b-a)/c)/((w1-w0)/1e9)}')"
    echo "plc requests/writes at the end: $(cat "$W/plc_stats.txt" 2>/dev/null)"
    echo "audit lines: $(wc -l < "$W/audit.log" 2>/dev/null)"
} > "$OUT/measure.txt"
for f in "$OUT"/*.ppm; do python3 tests/hmi/shot_tool.py png "$f" "${f%.ppm}.png" && rm -f "$f"; done
cat "$OUT/measure.txt"
echo "--- tour log"; cat "$LOG"
echo "--- audit"; cat "$W/audit.log" 2>/dev/null | cut -c1-160 | head -20
