#!/bin/bash
# End-to-end liveness test of the real prometheus99 executable (Linux).
# A fake systemd notify socket receives READY=1 and WATCHDOG=1. The process is
# then frozen with SIGSTOP (pings must stop), resumed with SIGCONT (pings must
# resume) and finally killed with SIGKILL (pings must stop for good).
# Uses a loopback config and /dev/null as the framebuffer: no display, no PLC.
#   usage: liveness_e2e.sh <build dir> <config template>
set -u
B=${1:-build-linux}
TPL=${2:-config/hmi.example.json}
W=$(mktemp -d /tmp/p99_live.XXXXXX)
sed -e 's/192\.168\.0\.10/127.0.0.1/' -e 's/"port": 502,/"port": 5502,/' "$TPL" > "$W/cfg.json"
fail=0
check() { if [ "$2" = ok ]; then echo "  ok   $1"; else echo "  FAIL $1"; fail=1; fi; }

"$B/notify_listener" "$W/notify.sock" 16 > "$W/notify.log" 2>&1 &
LP=$!
for i in $(seq 1 50); do grep -q listening "$W/notify.log" 2>/dev/null && break; sleep 0.1; done

NOTIFY_SOCKET="$W/notify.sock" WATCHDOG_USEC=1000000 \
  "$B/prometheus99" --config "$W/cfg.json" --fb /dev/null --touch /nonexistent \
  --audit "$W/audit.log" --alarms "$W/alarms.bin" --users "$W/users.bin" > "$W/app.out" 2>&1 &
APP=$!

sleep 3
count() { grep -c "WATCHDOG=1" "$W/notify.log"; }
ready=$(grep -c "READY=1" "$W/notify.log")
c1=$(count)
[ "$ready" -eq 1 ] && check "READY=1 sent exactly once" ok || check "READY=1 sent exactly once (got $ready)" no
[ "$c1" -ge 4 ] && check "WATCHDOG=1 cadence while running (>=4 in 3 s, got $c1)" ok || check "WATCHDOG=1 cadence (got $c1)" no

kill -STOP $APP
sleep 0.3; s0=$(count); sleep 2.5; s1=$(count)
[ "$s1" -eq "$s0" ] && check "no pings while SIGSTOPped (2.5 s)" ok || check "pings continued while stopped ($s0 -> $s1)" no

kill -CONT $APP
sleep 2.5; r1=$(count)
[ "$r1" -gt "$s1" ] && check "pings resume after SIGCONT ($s1 -> $r1)" ok || check "pings did not resume" no

kill -KILL $APP 2>/dev/null
wait $APP 2>/dev/null
sleep 0.3; k0=$(count); sleep 2; k1=$(count)
[ "$k1" -eq "$k0" ] && check "no pings after SIGKILL" ok || check "pings after SIGKILL" no

wait $LP 2>/dev/null

# ---- scenario 2: PLC that accepts and never answers must NOT stop the pings ----
"$B/hang_server" 5502 12 > "$W/hang.log" 2>&1 &
HP=$!
sleep 0.5
"$B/notify_listener" "$W/notify2.sock" 9 > "$W/notify2.log" 2>&1 &
LP2=$!
for i in $(seq 1 50); do grep -q listening "$W/notify2.log" 2>/dev/null && break; sleep 0.1; done
NOTIFY_SOCKET="$W/notify2.sock" WATCHDOG_USEC=1000000 \
  "$B/prometheus99" --config "$W/cfg.json" --fb /dev/null --touch /nonexistent \
  --audit "$W/audit2.log" --alarms "$W/alarms2.bin" --users "$W/users2.bin" > "$W/app2.out" 2>&1 &
APP2=$!
sleep 8
hp=$(grep -c "WATCHDOG=1" "$W/notify2.log")
[ "$hp" -ge 10 ] && check "pings continue against a hung PLC (>=10 in 8 s, got $hp)" ok || check "pings against hung PLC (got $hp)" no
kill -TERM $APP2 2>/dev/null
wait $APP2 2>/dev/null; rc=$?
[ "$rc" -eq 0 ] && check "clean exit on SIGTERM against a hung PLC" ok || check "SIGTERM exit status $rc" no
grep -q "STOPPING=1" "$W/notify2.log" && check "STOPPING=1 sent on shutdown" ok || check "STOPPING=1 sent on shutdown" no
wait $LP2 2>/dev/null
kill $HP 2>/dev/null; wait $HP 2>/dev/null
echo "--- notify log"; cat "$W/notify.log"
rm -rf "$W"
[ $fail -eq 0 ] && echo "liveness e2e: PASS" || echo "liveness e2e: FAIL"
exit $fail
