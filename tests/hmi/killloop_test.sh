#!/bin/bash
# Kill-loop test of the storage layer (Linux). The runtime is started on a
# directory, kept busy (alarms, acknowledgements, PLC writes), killed with
# SIGKILL at a random moment, and the files it left are checked by
# killloop_verify. Then it is started again on the same files, so every run also
# proves that the previous run's files can be reloaded. Every few rounds the
# process is stopped cleanly with SIGTERM instead and must exit with status 0.
#
# This simulates the process dying, not the power failing: the operating system
# still writes what the process handed to it. A power cut can only be tested
# on the target hardware.
#
#   usage: killloop_test.sh <build dir> [rounds]
#   environment: KILL_SEED=<n> repeats a random sequence
set -u
B=${1:-build-linux}
N=${2:-30}
CHILD="$B/killloop_child"
VERIFY="$B/killloop_verify"
[ -x "$CHILD" ] && [ -x "$VERIFY" ] || { echo "build killloop_child and killloop_verify first"; exit 1; }
W=$(mktemp -d /tmp/p99_kill.XXXXXX)
[ -n "${KILL_SEED:-}" ] && RANDOM=$KILL_SEED
fail=0
kills=0
cleans=0
tot_writes=0
tot_rot=0
last=""

wait_started() {
    for _ in $(seq 1 100); do
        grep -q STARTED "$1" 2>/dev/null && return 0
        sleep 0.05
    done
    return 1
}

for i in $(seq 1 "$N"); do
    rm -f "$W/plc.log"
    "$CHILD" "$W" > "$W/child.out" 2>&1 &
    pid=$!
    if ! wait_started "$W/child.out"; then
        echo "  FAIL round $i: the process did not start"
        cat "$W/child.out"
        fail=1
        kill -9 $pid 2>/dev/null
        break
    fi
    ms=$((300 + RANDOM % 2200))
    sleep "$((ms / 1000)).$(printf '%03d' $((ms % 1000)))"
    if [ $((i % 6)) -eq 0 ]; then
        kill -TERM $pid
        wait $pid
        rc=$?
        cleans=$((cleans + 1))
        if [ $rc -ne 0 ] || ! grep -q "STOPPED clean=1" "$W/child.out"; then
            echo "  FAIL round $i: clean stop exited with $rc"
            cat "$W/child.out"
            fail=1
        fi
        how="stop"
    else
        kill -9 $pid
        wait $pid 2>/dev/null
        kills=$((kills + 1))
        how="kill"
    fi
    out=$("$VERIFY" "$W" "$i")
    rc=$?
    echo "  round $i ($how after ${ms} ms): $out"
    if [ $rc -ne 0 ]; then
        fail=1
        echo "  files kept in $W"
        break
    fi
    last=$out
done

if [ $fail -eq 0 ]; then
    echo "killloop: PASS ($N rounds: $kills kills, $cleans clean stops)"
    echo "  last state: $last"
    rm -rf "$W"
    exit 0
fi
echo "killloop: FAIL (see above; files in $W)"
exit 1
