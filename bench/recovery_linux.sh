#!/bin/bash
# Recovery of the Linux executable after the PLC hangs and after it drops the connection (one run, about 3 minutes).
#
#   bench/recovery_linux.sh <build dir> <label>
#
# The fake PLC (build/fake_plc) is made to misbehave by signal while the executable runs: it hangs (SIGUSR2: reads
# requests, never answers), it drops every connection (SIGUSR1), it serves again (SIGHUP). The two faults are separated
# by a recovery, so each one is a new loss of the link. What is read afterwards is what the program itself wrote:
# the alarm journal (a PLC_LINK event for the loss of the link and one for its return, with the time since start), and
# the request counter of the fake PLC (requests received in each phase: no uncontrolled retries).
# "Recovered" means the journal shows the link event back to normal after a successful poll: the program decides that
# from a valid Modbus reply, not from a reconnected socket (tests/hmi/test_link_health.c, tests/hmi/test_modbus_e2e.c).
# A write that was sent and whose reply was lost is covered by the end-to-end test (UNKNOWN, exactly one transmission),
# not here.
#
# Output: bench/results/linux/<label>/recovery.txt and recovery_journal.txt. Host measurement on loopback.
set -u
B=${1:?build dir}
LABEL=${2:?label}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/bench/results/linux/$LABEL
mkdir -p "$OUT"
W=$(mktemp -d /tmp/p99_rec.XXXXXX)
ms() { echo $(( $(date +%s%N) / 1000000 )); }

"$B/fake_plc" 400 100 0.2 "$W/plc.stats" > "$W/plc.out" 2> "$W/plc.err" &
PLC=$!
for i in $(seq 1 50); do grep -q '^port ' "$W/plc.out" && break; sleep 0.1; done
PORT=$(sed -n 's/^port //p' "$W/plc.out")
sed -e 's/192\.168\.0\.10/127.0.0.1/' -e "s/\"port\": 502,/\"port\": $PORT,/" "$ROOT/config/hmi.example.json" > "$W/cfg.json"
. "$ROOT/tests/hmi/sign_fixture.sh"
sign_config "$B" "$W" || exit 1
"$B/prometheus99-bench" --config "$W/cfg.json" --fb "$W/fb.raw" --touch /nonexistent --role operator \
  --audit "$W/audit.log" --alarms "$W/alarms.bin" ${P99_EXTRA:-} \
  --config-key "$W/cfg.key" --config-state "$W/cfg.serial" > "$W/app.out" 2>&1 &
APP=$!
reqs() { sed -n 's/^requests=\([0-9]*\).*/\1/p' "$W/plc.stats" 2>/dev/null; }
link_events() { cat "$W"/alarms.bin.journal* 2>/dev/null | grep "point=PLC_LINK" | wc -l; }
OUTF=$OUT/recovery.txt
: > "$OUTF"
say() { echo "$@" | tee -a "$OUTF"; }

sleep 12                                    # the link is healthy and polling
R0=$(reqs); sleep 10; R1=$(reqs)
say "label: $LABEL   date: $(date -u +%FT%TZ)   host: $(uname -r)"
say "normal polling: about $(( (R1 - R0) / 10 )) requests per second (4 tags, poll_ms 200 to 1000); link events so far: $(link_events)"

fault() { # description signal seconds
    local e0 r0 ts seen=""
    e0=$(link_events); r0=$(reqs); ts=$(ms)
    kill -$2 $PLC
    for i in $(seq 1 $(( $3 * 10 ))); do
        sleep 0.1
        if [ "$(link_events)" -gt "$e0" ]; then seen=$(( $(ms) - ts )); break; fi
    done
    [ -z "$seen" ] || sleep $(( $3 - seen / 1000 ))
    say "$1: the link event (loss) appeared after ${seen:-NEVER} ms; requests received during the $3 s: $(( $(reqs) - r0 ))"
}
restore() { # description
    local e0 ts seen=""
    e0=$(link_events); ts=$(ms)
    kill -HUP $PLC
    for i in $(seq 1 300); do
        sleep 0.1
        if [ "$(link_events)" -gt "$e0" ]; then seen=$(( $(ms) - ts )); break; fi
    done
    say "$1: the link event (return) appeared after ${seen:-NEVER} ms"
    sleep 6
}

fault "PLC hangs (reads requests, never answers)" USR2 25
restore "PLC answers again"
fault "PLC drops every connection" USR1 25
restore "PLC answers again"
R_B0=$(reqs); sleep 10; R_B1=$(reqs)
say "normal polling again: about $(( (R_B1 - R_B0) / 10 )) requests per second"
kill -0 $APP 2>/dev/null && say "the executable is still running (no crash, no hang)" || say "THE EXECUTABLE DIED"
kill -TERM $APP 2>/dev/null; wait $APP 2>/dev/null; say "exit status on SIGTERM: $?"
kill $PLC 2>/dev/null; wait $PLC 2>/dev/null
cat "$W"/alarms.bin.journal* 2>/dev/null | grep "PLC_LINK" > "$OUT/recovery_journal.txt"
say "journal lines about the link (kept in recovery_journal.txt): $(wc -l < "$OUT/recovery_journal.txt")"
rm -rf "$W"
