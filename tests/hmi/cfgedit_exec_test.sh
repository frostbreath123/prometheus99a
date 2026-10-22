#!/bin/bash
# Real-executable checks for the root helper p99_cfg (Linux, run as root): a request
# written by the service is applied, or refused, against real files. No display, no
# PLC and no systemd. The "service user" is uid 65534 (nobody).
#   usage: cfgedit_exec_test.sh <build dir> <config template>
B=${1:-build-linux}
TPL=${2:-config/hmi.example.json}
if [ "$(id -u)" != 0 ]; then echo "cfgedit-exec needs root (it uses chown); skipped"; exit 0; fi
W=$(mktemp -d /tmp/p99_cfgedit.XXXXXX)
fail=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
SIGN="$B/p99_sign"
CFG="$B/p99_cfg"
SVC=65534
C="$W/hmi.json"; K="$W/config.key"; A="$W/audit.log"; SP="$W/spool"
sed -e 's/"allowed_hosts": \["192.168.0.10"\]/"allowed_hosts": ["192.168.0.10", "192.168.0.11"]/' "$TPL" > "$C"
grep -q '192.168.0.11' "$C" || { echo "template has no allowed_hosts line in the expected form"; exit 1; }
"$SIGN" keygen --key "$K" > /dev/null && "$SIGN" sign --config "$C" --key "$K" > /dev/null || exit 1
chmod 644 "$C" "$C.sig"; chmod 440 "$K"
mkdir "$SP"; chown $SVC:$SVC "$SP"; chmod 750 "$SP"
: > "$A"; chown $SVC:$SVC "$A"; chmod 600 "$A"

apply() { "$CFG" apply --spool "$SP" --config "$C" --key "$K" --audit "$A" --owner $SVC "$@" > "$W/out" 2>&1; }
req() { # <base serial> <user> <lines...>; writes a good request owned by the service user
  local s=$1 u=$2; shift 2
  { echo "P99REQ 1"; echo "base_serial=$s"; echo "user=$u"; for l in "$@"; do echo "$l"; done; echo "END"; } > "$SP/request"
  chown $SVC:$SVC "$SP/request"; chmod 600 "$SP/request"
}
serial_now() { "$SIGN" show --config "$C" | sed -E 's/^serial ([0-9]+),.*/\1/'; }
sum() { cat "$C" "$C.sig" | cksum; }
result() { head -1 "$SP/result" 2>/dev/null; }
expect_refused() { # description, text the result must contain; config must be unchanged and the request gone
  local before; before=$(sum)
  apply; local rc=$?
  if [ $rc -eq 1 ] && result | grep -q "^rejected: .*$2"; then ok "$1: refused: $(result)"; else bad "$1: rc $rc, result '$(result)'"; cat "$W/out"; fi
  [ ! -e "$SP/request" ] && [ ! -L "$SP/request" ] && ok "$1: the request is removed" || bad "$1: the request is still there"
  [ "$(sum)" = "$before" ] && ok "$1: configuration and signature unchanged" || bad "$1: configuration changed"
}

echo "== usage"
"$CFG" > "$W/out" 2>&1; [ $? -eq 2 ] && ok "no arguments: exit 2" || bad "no arguments"
"$CFG" apply --spool "$SP" > "$W/out" 2>&1; [ $? -eq 2 ] && ok "missing options: exit 2" || bad "missing options"
apply --restart 'x;rm -rf /'; [ $? -eq 2 ] && ok "a unit name with shell characters: exit 2" || bad "bad unit name"
apply; [ $? -eq 0 ] && [ ! -e "$SP/result" ] && ok "no request: nothing happens, exit 0" || bad "no request"

echo "== a good request"
S0=$(serial_now)
req "$S0" eng1 "tag.TANK_LEVEL.alarm.high=82" "tag.TEMP.alarm.priority=high" "plc.host=192.168.0.11"
apply; rc=$?
[ $rc -eq 0 ] && ok "applied: exit 0" || { bad "applied: rc $rc"; cat "$W/out"; }
[ "$(result)" = "applied serial=$((S0 + 1)) changes=3 by=eng1" ] && ok "result: $(result)" || bad "result '$(result)'"
[ ! -e "$SP/request" ] && ok "the request is removed" || bad "request left behind"
[ "$(serial_now)" = "$((S0 + 1))" ] && ok "serial plus one" || bad "serial $(serial_now)"
"$SIGN" verify --config "$C" --key "$K" > "$W/out" 2>&1 && grep -q "plc 192.168.0.11" "$W/out" && ok "the device's own check accepts it, with the new host" || { bad "verify"; cat "$W/out"; }
grep -q '"high":82' "$C" && ok "the new limit is in the file" || bad "limit not in file"
[ "$(stat -c %a "$C")" = 644 ] && [ "$(stat -c %a "$C.sig")" = 644 ] && ok "mode 0644 for both files" || bad "modes $(stat -c %a "$C") $(stat -c %a "$C.sig")"
[ -s "$C.prev" ] && [ -s "$C.sig.prev" ] && ok "the previous pair is kept" || bad "no .prev files"
cp "$C.prev" "$W/old.json"; cp "$C.sig.prev" "$W/old.json.sig"
"$SIGN" verify --config "$W/old.json" --key "$K" > "$W/out" 2>&1 && grep -q "^OK: serial $S0" "$W/out" && ok "the kept pair still verifies, with serial $S0" || { bad "kept pair"; cat "$W/out"; }
grep -q " CONFIG_CHANGED serial=$((S0 + 1)) from_serial=$S0 changes=3 by=eng1 wall=" "$A" && ok "audit has CONFIG_CHANGED with the user and a wall stamp" || { bad "audit line"; cat "$A"; }
[ "$(stat -c %U "$C" 2>/dev/null)" = root ] && ok "the configuration is still owned by root" || bad "owner changed"

echo "== refused requests leave everything unchanged"
S=$(serial_now)
req $((S - 1)) eng1 "tag.TANK_LEVEL.alarm.high=83"; expect_refused "stale serial" "stale request"
req $((S + 1)) eng1 "tag.TANK_LEVEL.alarm.high=83"; expect_refused "serial from the future" "stale request"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83" "tag.TANK_LEVEL.alarm.high=84"; expect_refused "the same setting twice" "twice"
req "$S" eng1 "writes_enabled=true"; expect_refused "unknown key (write switch)" "bad request"
req "$S" eng1 "tag.TANK_LEVEL.write.max=500"; expect_refused "forbidden key (write limit)" "bad request"
req "$S" eng1 "plc.allowed_hosts=10.9.9.9"; expect_refused "forbidden key (allow-list)" "bad request"
req "$S" eng1 "tag.TANK_LEVEL.address=5"; expect_refused "forbidden key (address)" "bad request"
req "$S" eng1 "plc.host=10.9.9.9"; expect_refused "host outside the allow-list" "allowed_hosts"
req "$S" eng1 "tag.NOSUCH.alarm.high=5"; expect_refused "no such tag" "no such tag"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=500"; expect_refused "limit outside the tag range" "inside"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=96"; expect_refused "limit above the high-high limit" "high_high must exceed high"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=82"; expect_refused "a change that changes nothing" "nothing would change"
{ echo "P99REQ 1"; echo "base_serial=$S"; echo "user=eng1"; echo "tag.TANK_LEVEL.alarm.high=83"; } > "$SP/request"; chown $SVC "$SP/request"; chmod 600 "$SP/request"
expect_refused "missing END" "END"
{ echo "P99REQ 1"; echo "base_serial=$S"; echo "user=eng1"; echo "tag.TANK_LEVEL.alarm.high=83"; echo "END"; echo "tag.TEMP.alarm.high=85"; } > "$SP/request"; chown $SVC "$SP/request"; chmod 600 "$SP/request"
expect_refused "bytes after END" "after END"
printf 'P99REQ 1\nbase_serial=%s\nuser=eng1\ntag.TANK_LEVEL.alarm.high=83\nEND\n\001' "$S" > "$SP/request"; chown $SVC "$SP/request"; chmod 600 "$SP/request"
expect_refused "a control character" "illegal character"

echo "== a request that is not what it should be as a file"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83"
ln -s /etc/hostname "$W/link-target-check" 2>/dev/null; rm -f "$SP/request"; ln -s /etc/hostname "$SP/request"
before_h=$(cksum < /etc/hostname)
expect_refused "symbolic link" "symbolic link"
[ "$(cksum < /etc/hostname)" = "$before_h" ] && ok "the link target is untouched" || bad "link target changed"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83"; chown 1:1 "$SP/request"
expect_refused "wrong owner" "wrong owner"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83"; chmod 660 "$SP/request"
expect_refused "group-writable request" "writable"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83"; ln "$SP/request" "$W/hardlink"
expect_refused "second hard link" "more than one link"; rm -f "$W/hardlink"
head -c 9000 /dev/zero | tr '\0' 'a' > "$SP/request"; chown $SVC "$SP/request"; chmod 600 "$SP/request"
expect_refused "oversized request" "larger than 8 KiB"
rm -f "$SP/request"; mkfifo "$SP/request"; chown $SVC "$SP/request"
expect_refused "named pipe" "not a regular file"
rm -f "$SP/request"; mkdir "$SP/request"
apply; rc=$?
[ $rc -eq 3 ] && ok "a directory in place of the request: exit 3, nothing changed" || bad "directory: rc $rc"
rmdir "$SP/request" 2>/dev/null

echo "== the current configuration must verify first"
cp "$C" "$W/c.keep"; cp "$C.sig" "$W/s.keep"
S=$(serial_now)
printf ' ' >> "$C"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83"
expect_refused "edited configuration" "does not verify"
cp "$W/c.keep" "$C"
cp "$K" "$W/k.keep"; head -c 32 /dev/urandom > "$K"; chmod 440 "$K"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83"
expect_refused "wrong key" "signature_mismatch"
cp "$W/k.keep" "$K"; chmod 440 "$K"
mv "$C.sig" "$W/sig.moved"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83"
apply; rc=$?
[ $rc -eq 1 ] && result | grep -q "signature_missing" && ok "missing signature: refused" || bad "missing signature: rc $rc '$(result)'"
mv "$W/sig.moved" "$C.sig"
chmod 666 "$C"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83"
apply; rc=$?
[ $rc -eq 1 ] && result | grep -q "config_mode" && ok "group-writable configuration: refused" || bad "writable config: rc $rc '$(result)'"
chmod 644 "$C"

echo "== the audit record comes first"
S=$(serial_now); before=$(sum)
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83"
mv "$A" "$W/audit.moved"
expect_refused "no audit file" "audit record"
ln -s /etc/hostname "$A"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83"
expect_refused "audit file is a symbolic link" "audit record"
[ "$(cksum < /etc/hostname)" = "$before_h" ] && ok "the audit link target is untouched" || bad "audit link target changed"
rm -f "$A"; mv "$W/audit.moved" "$A"
chown 1:1 "$A"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83"
expect_refused "audit file with the wrong owner" "audit record"
chown $SVC:$SVC "$A"

echo "== the spool directory"
chmod 757 "$SP"
req "$S" eng1 "tag.TANK_LEVEL.alarm.high=83"
apply; rc=$?
[ $rc -eq 3 ] && [ "$(sum)" = "$before" ] && ok "a world-writable spool directory: exit 3, nothing changed" || bad "spool mode: rc $rc"
chmod 750 "$SP"
rm -f "$SP/request"
mv "$SP" "$W/spool.real"; ln -s "$W/spool.real" "$SP"
apply; rc=$?
[ $rc -eq 3 ] && ok "a symbolic link as the spool directory: exit 3" || bad "spool link: rc $rc"
rm -f "$SP"; mv "$W/spool.real" "$SP"

echo "== a second good request after all the refusals"
S=$(serial_now)
req "$S" "-" "tag.TEMP.alarm.high=off" "tag.TANK_LEVEL.alarm.deadband=3"
apply; rc=$?
[ $rc -eq 0 ] && [ "$(result)" = "applied serial=$((S + 1)) changes=2 by=-" ] && ok "applied again: $(result)" || { bad "second apply rc $rc '$(result)'"; cat "$W/out"; }
"$SIGN" verify --config "$C" --key "$K" > /dev/null 2>&1 && ok "still verifies" || bad "verify after second apply"

echo "== the largest serial cannot be increased"
"$SIGN" sign --config "$C" --key "$K" --serial 4294967295 > /dev/null 2>&1; chmod 644 "$C.sig"
req 4294967295 eng1 "tag.TANK_LEVEL.alarm.high=84"; expect_refused "serial 4294967295" "cannot be increased"

rm -rf "$W"
[ $fail -eq 0 ] && echo "cfgedit-exec: all checks passed" || echo "cfgedit-exec: FAILURES"
exit $fail
