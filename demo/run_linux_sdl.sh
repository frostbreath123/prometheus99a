#!/bin/bash
# Builds and starts the Linux desktop-window demo: the real program in an SDL2 window, talking Modbus TCP over loopback
# to the fake PLC (tests/hmi/fake_plc_main.c). A development and demonstration run, not the device.
#
#   sudo apt install build-essential pkg-config libmodbus-dev libsdl2-dev
#   bash demo/run_linux_sdl.sh [build-dir]        ROLE=viewer|operator|engineer|admin (default operator)
#
# Needs a window system: WSLg on Windows 11 (DISPLAY and WAYLAND_DISPLAY are set automatically), or X11/Wayland on Linux.
# The window can be clicked like a touch screen. Esc or closing the window stops the demo.
# Fault injection while it runs (from another terminal): kill -USR1 <fake_plc pid> drops the PLC link,
# kill -HUP <fake_plc pid> restores it; kill -USR1 <prometheus99-sdl pid> saves a picture of the window ($P99_SHOT).
set -e
cd "$(dirname "$0")/.."
B=${1:-build-sdl}
make -f Makefile.linux BUILD="$B" -j"$(nproc)" hmi-sdl
W="$B/demo_run"
mkdir -p "$W"
"$B/fake_plc" 36000 100 1 "$W/plc_stats.txt" > "$W/plc.out" &
PLC=$!
trap 'kill $PLC 2>/dev/null' EXIT
sleep 1
PORT=$(awk '/^port/{print $2}' "$W/plc.out")
echo "fake PLC pid $PLC on 127.0.0.1:$PORT"
sh demo/make_demo_config.sh config/hmi.example.json "$W/hmi.json" 127.0.0.1 "$PORT"
rm -f "$W/config.key" "$W/hmi.json.sig" "$W/config.serial"
"$B/p99_sign" keygen --key "$W/config.key"
"$B/p99_sign" sign --config "$W/hmi.json" --key "$W/config.key"
"$B/prometheus99-sdl" --config "$W/hmi.json" --audit "$W/audit.log" --alarms "$W/alarms.bin" \
    --config-key "$W/config.key" --config-state "$W/config.serial" --role "${ROLE:-operator}"
