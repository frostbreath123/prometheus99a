#!/bin/sh
# Makes the configuration of a demonstration from config/hmi.example.json: the PLC address and port you give, writes
# enabled, one-second trend buckets, and trends on TANK_LEVEL, TEMP and FLOW. Only sed is used, so it also runs in the
# busybox guest of emulation/pi. The result still has to be signed (p99_sign sign) before the program accepts it.
#
#   sh demo/make_demo_config.sh config/hmi.example.json out.json 127.0.0.1 40123
set -e
IN=$1; OUT=$2; HOST=$3; PORT=$4
[ -n "$PORT" ] || { echo "usage: make_demo_config.sh IN OUT HOST PORT" >&2; exit 2; }
sed -e "s/\"host\": \"192.168.0.10\"/\"host\": \"$HOST\"/" \
    -e "s/\"allowed_hosts\": \[\"192.168.0.10\"\]/\"allowed_hosts\": [\"$HOST\"]/" \
    -e "s/\"port\": 502/\"port\": $PORT/" \
    -e 's/"writes_enabled": false,/"writes_enabled": true, "trend_bucket_ms": 1000,/' \
    -e 's/{ "name": "TANK_LEVEL",/{ "name": "TANK_LEVEL", "trend": true,/' \
    -e 's/{ "name": "TEMP",/{ "name": "TEMP", "trend": true,/' \
    -e 's/{ "name": "FLOW",/{ "name": "FLOW", "trend": true,/' \
    "$IN" > "$OUT"
