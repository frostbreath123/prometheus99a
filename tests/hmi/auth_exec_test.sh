#!/bin/bash
# Real-executable checks for authentication (Linux): the offline user tool and
# what the production executable reports about the user store at start-up.
# No display and no PLC: /dev/null as framebuffer, loopback config.
#   usage: auth_exec_test.sh <build dir> <config template>
B=${1:-build-linux}
TPL=${2:-config/hmi.example.json}
W=$(mktemp -d /tmp/p99_auth.XXXXXX)
sed -e 's/192\.168\.0\.10/127.0.0.1/' -e 's/"port": 502,/"port": 5502,/' "$TPL" > "$W/cfg.json"
. "$(dirname "$0")/sign_fixture.sh"
sign_config "$B" "$W" || { echo "cannot sign the test configuration"; exit 1; }
fail=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
check() { if [ "$2" = ok ]; then ok "$1"; else bad "$1"; fi; }
TOOL="$B/p99_user"
APP="$B/prometheus99"
U="$W/users.bin"
A="$W/audit.log"
t() { "$TOOL" --users "$U" --audit "$A" --password-stdin "$@"; }   # password comes on stdin
tn() { "$TOOL" --users "$U" --audit "$A" "$@" < /dev/null; }        # no password needed

mode_of() { stat -c %a "$1" 2>/dev/null; }

echo "== usage"
"$TOOL" > "$W/o" 2>&1 < /dev/null; [ $? -eq 2 ] && ok "no arguments: exit 2" || bad "no arguments"
"$TOOL" --bogus list > "$W/o" 2>&1 < /dev/null; [ $? -eq 2 ] && ok "unknown option: exit 2" || bad "unknown option"
"$TOOL" --users "$U" frobnicate > "$W/o" 2>&1 < /dev/null; [ $? -eq 2 ] && ok "unknown command: exit 2" || bad "unknown command"
"$TOOL" --users "$U" add onlyname > "$W/o" 2>&1 < /dev/null; [ $? -eq 2 ] && ok "add without role: exit 2" || bad "add without role"
"$TOOL" --users "$U" > "$W/o" 2>&1 < /dev/null; [ $? -eq 2 ] && ok "missing command: exit 2" || bad "missing command"

echo "== no store yet"
tn list > "$W/o" 2>&1; rc=$?
[ $rc -eq 1 ] && grep -q "no user store" "$W/o" && ok "list: refused, says there is no store" || bad "list on absent store (rc $rc)"
printf 'operator-pass-1\n' | t add op1 operator > "$W/o" 2>&1; rc=$?
[ $rc -eq 1 ] && [ ! -e "$U" ] && ok "add: refused without a store, nothing created" || bad "add on absent store (rc $rc)"

echo "== init (first administrator)"
printf 'short\n' | t init admin > "$W/o" 2>&1; rc=$?
[ $rc -eq 1 ] && grep -q "too short" "$W/o" && [ ! -e "$U" ] && ok "init: weak password refused, no file" || bad "init weak password (rc $rc)"
printf 'admin-pass-1\n' | t init Admin > "$W/o" 2>&1; rc=$?
[ $rc -eq 1 ] && [ ! -e "$U" ] && ok "init: bad user name refused" || bad "init bad name (rc $rc)"
printf 'admin-pass-1\n' | t init admin > "$W/o" 2>&1; rc=$?
[ $rc -eq 0 ] && [ -f "$U" ] && ok "init: administrator created" || { bad "init (rc $rc)"; cat "$W/o"; }
[ "$(mode_of "$U")" = 600 ] && ok "store mode is 600" || bad "store mode $(mode_of "$U")"
printf 'admin-pass-1\n' | t init admin2 > "$W/o" 2>&1; rc=$?
[ $rc -eq 1 ] && ok "init: refuses to replace an existing store" || bad "second init (rc $rc)"
grep -q "admin-pass-1" "$W/o" && bad "password echoed in output" || ok "password not echoed"

echo "== add, passwd, role, remove"
printf 'operator-pass-1\n' | t add op1 operator > "$W/o" 2>&1; [ $? -eq 0 ] && ok "add operator" || { bad "add operator"; cat "$W/o"; }
printf 'operator-pass-1\n' | t add op1 operator > "$W/o" 2>&1; [ $? -eq 1 ] && ok "add duplicate refused" || bad "add duplicate"
printf 'viewer-pass-12\n' | t add vv viewer > "$W/o" 2>&1; [ $? -eq 1 ] && ok "add viewer refused (no account for viewer)" || bad "add viewer"
printf 'abcdefgh1\n' | t add "Bad Name" operator > "$W/o" 2>&1; [ $? -eq 1 ] && ok "add bad name refused" || bad "add bad name"
printf 'short\n' | t add eng1 engineer > "$W/o" 2>&1; [ $? -eq 1 ] && ok "add weak password refused" || bad "add weak password"
printf 'engineer-pass-1\n' | t add eng1 engineer > "$W/o" 2>&1; [ $? -eq 0 ] && ok "add engineer" || bad "add engineer"
tn list > "$W/o" 2>&1
n=$(grep -c "argon2id" "$W/o"); [ "$n" = 3 ] && grep -q "^admin .*admin" "$W/o" && grep -q "^op1 .*operator" "$W/o" && ok "list shows 3 accounts" || { bad "list ($n lines)"; cat "$W/o"; }
grep -Eqi "pass|hash|salt" "$W/o" && bad "list leaks secrets" || ok "list shows no secrets"

cp "$U" "$W/before.bin"
printf 'operator-pass-2\n' | t passwd op1 > "$W/o" 2>&1; [ $? -eq 0 ] && ok "passwd" || bad "passwd"
cmp -s "$U" "$W/before.bin" && bad "passwd changed nothing" || ok "passwd changed the store"
printf 'operator-pass-2\n' | t passwd nobody > "$W/o" 2>&1; [ $? -eq 1 ] && ok "passwd unknown user refused" || bad "passwd unknown"
tn role op1 engineer > "$W/o" 2>&1; [ $? -eq 0 ] && tn list | grep -q "^op1 .*engineer" && ok "role changed" || bad "role"
tn role op1 viewer > "$W/o" 2>&1; [ $? -eq 1 ] && ok "role viewer refused" || bad "role viewer"
tn remove nobody > "$W/o" 2>&1; [ $? -eq 1 ] && ok "remove unknown refused" || bad "remove unknown"
tn remove admin > "$W/o" 2>&1; rc=$?
[ $rc -eq 1 ] && grep -q "no administrator" "$W/o" && ok "the last administrator cannot be removed" || bad "remove last admin (rc $rc)"
tn role admin operator > "$W/o" 2>&1; [ $? -eq 1 ] && ok "the last administrator cannot be demoted" || bad "demote last admin"
printf 'second-admin-1\n' | t add admin2 admin > "$W/o" 2>&1
tn remove admin > "$W/o" 2>&1; [ $? -eq 0 ] && ok "an administrator can be removed when another remains" || bad "remove admin with a spare"
[ "$(mode_of "$U")" = 600 ] && ok "store mode still 600 after edits" || bad "mode $(mode_of "$U")"

echo "== audit"
for ev in "USER_CREATE user=admin role=admin" "USER_ADD user=op1 role=operator" "USER_ADD user=eng1 role=engineer" \
          "USER_PASSWORD user=op1 role=operator" "USER_PASSWORD user=op1 role=engineer" "USER_ADD user=admin2 role=admin" \
          "USER_REMOVE user=admin"; do
  grep -q "$ev" "$A" && ok "audit has: $ev" || bad "audit missing: $ev"
done
grep -Eq "pass|Bad Name|vv" "$A" && bad "audit contains a password or a refused name" || ok "audit holds no credentials and no refused input"
cp "$U" "$W/keep.bin"
printf 'x-password-123\n' | "$TOOL" --users "$U" --audit "$W/no_such_dir/audit.log" --password-stdin add ghost1 operator > "$W/o" 2>&1; rc=$?
[ $rc -eq 1 ] && cmp -s "$U" "$W/keep.bin" && ok "no audit record, no change (fail closed)" || bad "unwritable audit changed the store (rc $rc)"

echo "== production executable and the store"
run_app() { # extra args; prints stderr after 1 s
  "$APP" --config "$W/cfg.json" --config-key "$W/cfg.key" --config-state "$W/cfg.serial" --users "$U" --fb /dev/null --touch /nonexistent --audit "$W/app_audit.log" "$@" > "$W/app.out" 2>&1 &
  local p=$!
  sleep 1
  kill -TERM $p 2>/dev/null; wait $p 2>/dev/null
}
run_app
grep -q "user store .*: ready" "$W/app.out" && ok "valid store: ready" || { bad "valid store"; cat "$W/app.out"; }

chmod 644 "$U"; run_app
grep -q "can be read by group or others" "$W/app.out" && grep -q ": ready" "$W/app.out" && ok "mode 644: starts, warns" || { bad "mode 644"; cat "$W/app.out"; }
chmod 666 "$U"; run_app
grep -q "INVALID, login disabled (user store is writable by group or others)" "$W/app.out" && ok "mode 666: store refused" || { bad "mode 666"; cat "$W/app.out"; }
tn list > "$W/o" 2>&1; grep -q "warning" "$W/o" && ok "tool warns about the mode too" || bad "tool mode warning"
chmod 600 "$U"

cp "$U" "$W/good.bin"
printf '' | dd of="$U" bs=1 seek=40 conv=notrunc 2>/dev/null   # byte 40 is the first record's role byte
cp "$U" "$W/tampered.bin"
run_app
grep -q "INVALID, login disabled" "$W/app.out" && ok "tampered store: INVALID" || { bad "tampered store"; cat "$W/app.out"; }
cmp -s "$U" "$W/tampered.bin" && ok "an invalid store is left untouched (no first-boot overwrite)" || bad "invalid store was modified"
tn list > "$W/o" 2>&1; [ $? -eq 1 ] && grep -q "not usable" "$W/o" && ok "tool refuses a tampered store" || bad "tool on tampered store"
printf 'abcdefgh-12\n' | t init again > "$W/o" 2>&1; [ $? -eq 1 ] && ok "init refuses over a tampered store" || bad "init over tampered store"

head -c 100 "$W/good.bin" > "$U"; run_app
grep -q "INVALID, login disabled" "$W/app.out" && ok "truncated store: INVALID" || bad "truncated store"
: > "$U"; run_app
grep -q "INVALID, login disabled" "$W/app.out" && ok "empty store file: INVALID, not first boot" || bad "empty store file"

rm -f "$U"; run_app
grep -q "absent (first boot" "$W/app.out" && ok "no store: first boot" || { bad "no store"; cat "$W/app.out"; }
[ ! -e "$U" ] && ok "start-up does not create the store" || bad "start-up created a store"
grep -q "no --audit" "$W/app.out" && bad "warned about audit although one was given" || ok "no audit warning when --audit is given"

echo "== bench"
tn bench > "$W/o" 2>&1; rc=$?
[ $rc -eq 0 ] && [ "$(grep -c ' ms$' "$W/o")" = 3 ] && ok "bench runs three hashes" || bad "bench (rc $rc)"
grep "run" "$W/o" | sed 's/^/       /'

rm -rf "$W"
[ $fail -eq 0 ] && echo "auth exec test: PASS" || echo "auth exec test: FAIL"
exit $fail
