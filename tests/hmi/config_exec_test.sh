#!/bin/bash
# Real-executable checks for configuration integrity (Linux): the signing tool
# and what the production executable does with every kind of bad configuration.
# No display and no real PLC: /dev/null as framebuffer, a loopback listener that
# only counts connections.
#   usage: config_exec_test.sh <build dir> <config template>
B=${1:-build-linux}
TPL=${2:-config/hmi.example.json}
W=$(mktemp -d /tmp/p99_cfg.XXXXXX)
sed -e 's/192\.168\.0\.10/127.0.0.1/' -e 's/"port": 502,/"port": 5502,/' "$TPL" > "$W/cfg.json"
fail=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
SIGN="$B/p99_sign"
APP="$B/prometheus99"
C="$W/cfg.json"; K="$W/cfg.key"; S="$W/cfg.serial"; A="$W/audit.log"
mode_of() { stat -c %a "$1" 2>/dev/null; }

# Counts TCP connections made to 127.0.0.1:5502 while it runs (python3 if present).
HAVE_PY=0; command -v python3 > /dev/null 2>&1 && HAVE_PY=1
start_listener() {
  [ $HAVE_PY -eq 1 ] || return 0
  rm -f "$W/conns"
  python3 - "$W/conns" <<'PYEOF' &
import socket, sys, time
out = sys.argv[1]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 5502)); s.listen(8); s.settimeout(0.2)
n = 0; end = time.time() + 2.2
while time.time() < end:
    try:
        c, _ = s.accept(); n += 1; c.close()
    except socket.timeout:
        pass
open(out, "w").write(str(n))
PYEOF
  LP=$!
}
stop_listener() { [ $HAVE_PY -eq 1 ] && wait $LP 2>/dev/null; }

run_app() { # extra args after the fixed ones; stderr goes to $W/app.out, listener counts connections
  start_listener
  "$APP" --config "$C" --config-key "$K" --config-state "$S" --audit "$A" \
         --fb /dev/null --touch /nonexistent "$@" > "$W/app.out" 2>&1 &
  local p=$!
  sleep 1.5
  kill -TERM $p 2>/dev/null; wait $p 2>/dev/null
  stop_listener
}
conns() { [ $HAVE_PY -eq 1 ] && cat "$W/conns" 2>/dev/null || echo "n/a"; }
expect_rejected() { # description, reason code
  : > "$A"      # only this run's audit lines may satisfy the check
  run_app
  if grep -q "CONFIGURATION REJECTED ($2)" "$W/app.out"; then ok "$1: rejected ($2)"; else bad "$1: expected $2"; cat "$W/app.out"; fi
  if grep -q "reason=$2" "$A"; then ok "$1: audit has CONFIG_REJECTED reason=$2"; else bad "$1: audit line missing"; fi
  c=$(conns); if [ "$c" = "n/a" ] || [ "$c" = 0 ]; then ok "$1: no PLC connection was made (safe mode)"; else bad "$1: $c connection(s) made"; fi
}

echo "== signing tool usage"
"$SIGN" > "$W/o" 2>&1; [ $? -eq 2 ] && ok "no arguments: exit 2" || bad "no arguments"
"$SIGN" frobnicate > "$W/o" 2>&1; [ $? -eq 2 ] && ok "unknown command: exit 2" || bad "unknown command"
"$SIGN" sign --config > "$W/o" 2>&1; [ $? -eq 2 ] && ok "missing value: exit 2" || bad "missing value"
"$SIGN" sign --config "$C" --key "$K" --serial 12x > "$W/o" 2>&1; [ $? -eq 2 ] && ok "bad serial: exit 2" || bad "bad serial"
"$SIGN" sign --config "$C" --config "$C" --key "$K" > "$W/o" 2>&1; [ $? -eq 2 ] && ok "repeated option: exit 2" || bad "repeated option"
"$SIGN" keygen > "$W/o" 2>&1; [ $? -eq 2 ] && ok "keygen without --key: exit 2" || bad "keygen without --key"

echo "== keygen and sign"
"$SIGN" sign --config "$C" --key "$K" > "$W/o" 2>&1; [ $? -eq 1 ] && ok "sign without a key refused" || bad "sign without key"
"$SIGN" keygen --key "$K" > "$W/o" 2>&1; [ $? -eq 0 ] && [ "$(stat -c %s "$K")" = 32 ] && ok "keygen writes 32 bytes" || bad "keygen"
[ "$(mode_of "$K")" = 600 ] && ok "key is created with mode 600" || bad "key mode $(mode_of "$K")"
cp "$K" "$W/key.copy"
"$SIGN" keygen --key "$K" > "$W/o" 2>&1; [ $? -eq 1 ] && cmp -s "$K" "$W/key.copy" && ok "keygen never overwrites a key" || bad "keygen overwrote"
"$SIGN" sign --config "$C" --key "$K" > "$W/o" 2>&1; [ $? -eq 0 ] && grep -q "serial 1" "$W/o" && ok "first signature has serial 1" || { bad "sign"; cat "$W/o"; }
[ "$(stat -c %s "$C.sig")" = 88 ] && ok "signature file is 88 bytes" || bad "signature size"
grep -q "writes disabled" "$W/o" && ok "sign reports the write master switch" || bad "sign does not report writes"
"$SIGN" sign --config "$C" --key "$K" > "$W/o" 2>&1; grep -q "serial 2" "$W/o" && ok "signing again increases the serial" || bad "serial increase"
"$SIGN" sign --config "$C" --key "$K" --serial 10 > "$W/o" 2>&1; "$SIGN" show --config "$C" > "$W/o" 2>&1
grep -q "serial 10," "$W/o" && ok "--serial is honoured" || { bad "--serial"; cat "$W/o"; }

echo "== the tool refuses to sign a bad configuration"
cp "$C" "$W/good.json"
echo '{ not json' > "$W/bad.json"
"$SIGN" sign --config "$W/bad.json" --key "$K" > "$W/o" 2>&1; rc=$?
[ $rc -eq 1 ] && [ ! -e "$W/bad.json.sig" ] && grep -q "invalid" "$W/o" && ok "invalid configuration is not signed" || bad "signed invalid config (rc $rc)"
grep -v '"allowed_hosts"' "$W/good.json" > "$W/noallow.json"
"$SIGN" sign --config "$W/noallow.json" --key "$K" > "$W/o" 2>&1; rc=$?
[ $rc -eq 1 ] && [ ! -e "$W/noallow.json.sig" ] && grep -q "allowed_hosts" "$W/o" && ok "configuration without allowed_hosts is not signed" || bad "signed without allow-list (rc $rc)"
sed 's/"allowed_hosts": \["127.0.0.1"\]/"allowed_hosts": ["10.9.9.9"]/' "$W/good.json" > "$W/other.json"
"$SIGN" sign --config "$W/other.json" --key "$K" > "$W/o" 2>&1; rc=$?
[ $rc -eq 1 ] && [ ! -e "$W/other.json.sig" ] && ok "host outside the allow-list is not signed" || bad "signed host outside the list (rc $rc)"
head -c 20 "$K" > "$W/short.key"
"$SIGN" sign --config "$C" --key "$W/short.key" > "$W/o" 2>&1; [ $? -eq 1 ] && ok "a short key is refused" || bad "short key"

echo "== verify"
"$SIGN" sign --config "$C" --key "$K" --serial 5 > /dev/null 2>&1
"$SIGN" verify --config "$C" --key "$K" > "$W/o" 2>&1; rc=$?
[ $rc -eq 0 ] && grep -q "^OK: serial 5" "$W/o" && ok "verify accepts the signed configuration" || { bad "verify (rc $rc)"; cat "$W/o"; }
[ ! -e "$S" ] && ok "verify does not touch the device's serial state" || bad "verify created state"
cp "$C" "$W/c.keep"
printf ' ' >> "$C"
"$SIGN" verify --config "$C" --key "$K" > "$W/o" 2>&1; rc=$?
[ $rc -eq 1 ] && grep -q "config_corrupt" "$W/o" && ok "verify rejects an edited file" || { bad "verify edited (rc $rc)"; cat "$W/o"; }
cp "$W/c.keep" "$C"

echo "== production executable: accepted"
run_app
grep -q "configuration verified (signature serial 5)" "$W/app.out" && ok "signed configuration accepted" || { bad "accepted"; cat "$W/app.out"; }
grep -q "CONFIG_ACCEPTED serial=5" "$A" && ok "audit has CONFIG_ACCEPTED serial=5" || bad "audit missing CONFIG_ACCEPTED"
c=$(conns); if [ "$c" = "n/a" ]; then ok "(python3 missing: connection count skipped)"; elif [ "$c" -ge 1 ]; then ok "the runtime connected to the loopback listener ($c)"; else bad "no connection made although the configuration is valid"; fi
[ -f "$S" ] && [ "$(stat -c %s "$S")" = 16 ] && ok "serial state file written" || bad "serial state"
grep -q "the service user can modify the configuration" "$W/app.out" && ok "warns when the service user owns the configuration" || bad "no ownership warning"

echo "== production executable: every kind of tampering is safe mode"
# missing signature
mv "$C.sig" "$W/sig.keep"
expect_rejected "missing signature" signature_missing
mv "$W/sig.keep" "$C.sig"
# one flipped bit
cp "$C" "$W/c.keep"
python3 -c "
import sys
b = bytearray(open('$C','rb').read()); b[60] ^= 1; open('$C','wb').write(b)" 2>/dev/null || { printf 'X' | dd of="$C" bs=1 seek=60 conv=notrunc 2>/dev/null; }
expect_rejected "one flipped bit" config_corrupt
cp "$W/c.keep" "$C"
# switch the write master switch on, same length, still valid JSON
sed 's/"writes_enabled": false/"writes_enabled": true /' "$W/c.keep" > "$C"
if cmp -s "$C" "$W/c.keep"; then bad "test setup: the switch text was not found"; fi
expect_rejected "writes_enabled turned on by hand" config_corrupt
grep -q "writes ENABLED" "$W/app.out" && bad "writes were enabled" || ok "writes stayed off"
cp "$W/c.keep" "$C"
# a different key
cp "$K" "$W/k.keep"
head -c 32 /dev/urandom > "$K"
expect_rejected "wrong key" signature_mismatch
cp "$W/k.keep" "$K"
# rollback
"$SIGN" sign --config "$C" --key "$K" --serial 3 > /dev/null 2>&1
expect_rejected "older signed configuration put back" rollback
"$SIGN" sign --config "$C" --key "$K" --serial 6 > /dev/null 2>&1
run_app
grep -q "configuration verified (signature serial 6)" "$W/app.out" && ok "a higher serial is accepted again" || { bad "serial 6"; cat "$W/app.out"; }
# file modes
chmod 644 "$K"; expect_rejected "world-readable key" key_mode; chmod 600 "$K"
chmod 666 "$C"; expect_rejected "world-writable configuration" config_mode; chmod 644 "$C"
chmod 666 "$C.sig"; expect_rejected "world-writable signature" config_mode; chmod 600 "$C.sig"
# missing key and missing configuration
mv "$K" "$W/k.keep"; expect_rejected "missing key" key_missing; mv "$W/k.keep" "$K"
mv "$C" "$W/c.keep"; expect_rejected "missing configuration" config_unreadable; mv "$W/c.keep" "$C"
# state file damage is reported, not fatal
printf 'garbage-garbage!' > "$S"
run_app
grep -q "serial file .* was damaged" "$W/app.out" && grep -q "configuration verified" "$W/app.out" && ok "a damaged serial file is reported and treated as absent" || { bad "damaged state"; cat "$W/app.out"; }
# back to good
run_app
grep -q "configuration verified" "$W/app.out" && ok "restored configuration is accepted again" || bad "restore"

echo "== the signed text is parsed only after the signature verifies"
echo '{ this is not json' > "$C"
expect_rejected "unsigned garbage" config_corrupt
cp "$W/c.keep" "$C" 2>/dev/null || cp "$W/good.json" "$C"

rm -rf "$W"
[ $fail -eq 0 ] && echo "config exec test: PASS" || echo "config exec test: FAIL"
exit $fail
