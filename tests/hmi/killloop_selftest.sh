#!/bin/bash
# Checks the verifier of the kill-loop test: it must pass on the files of a
# healthy run and must refuse each of the faults below. A verifier that cannot
# fail proves nothing.
#   usage: killloop_selftest.sh <build dir>
set -u
B=${1:-build-linux}
CHILD="$B/killloop_child"
VERIFY="$B/killloop_verify"
W=$(mktemp -d /tmp/p99_killself.XXXXXX)
fail=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

# a healthy run, stopped cleanly after a few seconds so that all three files exist
"$CHILD" "$W" > "$W/child.out" 2>&1 &
pid=$!
for _ in $(seq 1 100); do grep -q STARTED "$W/child.out" 2>/dev/null && break; sleep 0.05; done
sleep 3
kill -TERM $pid
wait $pid
[ $? -eq 0 ] || bad "the healthy run did not stop cleanly"
cp -r "$W" "$W.good"
"$VERIFY" "$W" 0 > "$W/v.out" 2>&1 && ok "healthy run passes" || { bad "healthy run is refused"; cat "$W/v.out"; }
[ -s "$W/audit.log" ] && [ -s "$W/alarms.bin" ] && [ -s "$W/alarms.bin.journal" ] && [ -s "$W/plc.log" ] || bad "the healthy run left an empty file"

expect_refused() { # name, expected text, allowed damaged lines
    if "$VERIFY" "$W" "${3:-0}" > "$W/v.out" 2>&1; then
        bad "$1: accepted"
    elif grep -q "$2" "$W/v.out"; then
        ok "$1: refused ($2)"
    else
        bad "$1: refused for another reason"; cat "$W/v.out"
    fi
    rm -rf "$W"; cp -r "$W.good" "$W"
}

# a write reached the PLC without a SENT record
echo "W addr=0 n=1 v0=777 v1=0" >> "$W/plc.log"
expect_refused "a write the audit file does not know" "audit before send"

# the SENT record of a write is missing (the line is cut out)
first_sent=$(grep -n " SENT id=" "$W/audit.log" | head -1 | cut -d: -f1)
sed -i "${first_sent}d" "$W/audit.log"
# refused either because its final record has no SENT before it, or because the PLC log has a write nobody recorded
expect_refused "a SENT record removed" "audit before send\|without SENT"

# a request recorded as sent twice
line=$(grep " SENT id=" "$W/audit.log" | tail -1)
echo "$line" >> "$W/audit.log"
expect_refused "SENT twice" "SENT twice"

# SENT without RESERVED
rid=$(grep " SENT id=" "$W/audit.log" | tail -1 | sed -e 's/.* id=\([0-9]*\) .*/\1/')
sed -i "/ RESERVED id=${rid} /d" "$W/audit.log"
expect_refused "SENT without RESERVED" "SENT without RESERVED"

# a final record before the intent was recorded
echo "1 CONFIRMED id=99999 tag=0 role=1 eng=1 raw=0x00000001 rb=0x00000001 exc=0 user=killer old=1 oldq=GOOD wall=- sync=no" >> "$W/audit.log"
expect_refused "a final record without a request" "without"

# damaged lines beyond the allowance, and one in the middle
echo "this is not an audit record" >> "$W/audit.log"
echo "1 BOOT wall=- sync=no" >> "$W/audit.log"
expect_refused "a damaged audit line over the allowance" "damaged"
echo "garbage in the middle" >> "$W/audit.log"
echo "7 ALARM_ACK role=operator action=ACK_ALL result=GRANTED n=0 points= user=killer wall=- sync=no" >> "$W/audit.log"
expect_refused "a damaged line followed by an ordinary record" "followed by an ordinary record" 1

# a record without the wall-clock stamp
echo "5 BOOT" >> "$W/audit.log"
expect_refused "a record without a stamp" "damaged"

# the alarm state file damaged
printf 'X' | dd of="$W/alarms.bin" bs=1 seek=20 conv=notrunc 2>/dev/null
expect_refused "alarm state file damaged" "alarm state file"

# the journal: a flipped byte, and sequence numbers going backwards
sed -i '1s/ALARM_EVENT/ALARM_EVEN7/' "$W/alarms.bin.journal"
expect_refused "journal line damaged" "journal"
tail -n 1 "$W/alarms.bin.journal" > "$W/last.tmp"
head -n 1 "$W/alarms.bin.journal" >> "$W/last.tmp"
cat "$W/last.tmp" > "$W/alarms.bin.journal.1"
expect_refused "journal sequence goes backwards" "sequence"

# a torn last line is allowed once per kill, and the next record may follow it
printf '123 RESERVED id=5 tag=0 role=1 eng=1 ra' >> "$W/audit.log"
"$VERIFY" "$W" 1 > "$W/v.out" 2>&1 && ok "one torn line at the end is allowed" || { bad "one torn line at the end is refused"; cat "$W/v.out"; }
rm -rf "$W" "$W.good"
[ $fail -eq 0 ] && echo "killloop self test: PASS" || echo "killloop self test: FAIL"
exit $fail
