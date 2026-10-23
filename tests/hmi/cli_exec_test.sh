#!/bin/bash
# Real-executable check of command-line strictness and of the claimed role (Linux).
#   - Invalid options exit with status 2 before any hardware is touched, in both builds.
#   - There is no login and no user store: --users is an unknown option, and no file for accounts is made.
#   - --role is honoured in both builds, is announced as "claimed ... not authenticated", and a role above
#     viewer also produces a warning. The default role is viewer.
#   - --unsigned-config exists only in the test build (HMI_TEST_ROLE) and is refused by the other.
# Uses /dev/null as the framebuffer and a loopback config.
#   usage: cli_exec_test.sh <build dir> <config template>
B=${1:-build-linux}
TPL=${2:-config/hmi.example.json}
W=$(mktemp -d /tmp/p99_cli.XXXXXX)
sed -e 's/192\.168\.0\.10/127.0.0.1/' -e 's/"port": 502,/"port": 5502,/' "$TPL" > "$W/cfg.json"
. "$(dirname "$0")/sign_fixture.sh"
sign_config "$B" "$W" || { echo "cannot sign the test configuration"; exit 1; }
fail=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
PROD="$B/prometheus99"
SIM="$B/prometheus99-sim"
COMMON=(--config "$W/cfg.json" --config-key "$W/cfg.key" --config-state "$W/cfg.serial" --fb /dev/null --touch /nonexistent)

expect_exit2() { # binary, description, args...
  local bin=$1 d=$2; shift 2
  "$bin" "$@" > "$W/out" 2>&1; local rc=$?
  if [ $rc -eq 2 ] && grep -q "prometheus99:" "$W/out"; then ok "$d (exit 2)"; else bad "$d (exit $rc)"; cat "$W/out"; fi
}
expect_exit2_msg() { # binary, description, expected text, args...
  local bin=$1 d=$2 txt=$3; shift 3
  "$bin" "$@" > "$W/out" 2>&1; local rc=$?
  if [ $rc -eq 2 ] && grep -q -e "$txt" "$W/out"; then ok "$d (exit 2, says: $txt)"; else bad "$d (exit $rc)"; cat "$W/out"; fi
}
expect_runs() { # binary, description, stderr pattern, args...
  local bin=$1 d=$2 pat=$3; shift 3
  "$bin" "$@" > "$W/out" 2>&1 &
  local p=$!
  sleep 1
  if kill -0 $p 2>/dev/null; then
    if grep -q "$pat" "$W/out"; then ok "$d (starts, says: $pat)"; else bad "$d (expected text missing: $pat)"; cat "$W/out"; fi
  else
    bad "$d (did not start)"; cat "$W/out"
  fi
  kill -TERM $p 2>/dev/null; wait $p 2>/dev/null
}
expect_silent() { # binary, description, forbidden pattern, args...
  local bin=$1 d=$2 pat=$3; shift 3
  "$bin" "$@" > "$W/out" 2>&1 &
  local p=$!
  sleep 1
  kill -TERM $p 2>/dev/null; wait $p 2>/dev/null
  if grep -q -e "$pat" "$W/out"; then bad "$d (printed: $pat)"; cat "$W/out"; else ok "$d (does not print: $pat)"; fi
}

# Strictness applies to both builds.
for BIN in "$PROD" "$SIM"; do
  n=$(basename "$BIN")
  expect_exit2 "$BIN" "$n: unknown role typo"            "${COMMON[@]}" --role operatr
  expect_exit2 "$BIN" "$n: wrong-case role"              "${COMMON[@]}" --role Operator
  expect_exit2 "$BIN" "$n: empty role value"             "${COMMON[@]}" --role ""
  expect_exit2 "$BIN" "$n: role flag without value"      "${COMMON[@]}" --role
  expect_exit2 "$BIN" "$n: unknown flag"                 "${COMMON[@]}" --bogus
  expect_exit2 "$BIN" "$n: misspelled --audit"           "${COMMON[@]}" --audt "$W/a.log"
  expect_exit2_msg "$BIN" "$n: --users (accounts were removed)" "unknown option: --users" "${COMMON[@]}" --users "$W/u.bin"
  expect_exit2 "$BIN" "$n: stray argument"               "${COMMON[@]}" stray
  expect_exit2 "$BIN" "$n: repeated --role"              "${COMMON[@]}" --role viewer --role admin
  expect_exit2 "$BIN" "$n: --role=value form"            "${COMMON[@]}" --role=admin
done

# Both builds: the claimed role is announced; above viewer there is also a warning.
for BIN in "$PROD" "$SIM"; do
  n=$(basename "$BIN")
  expect_runs "$BIN" "$n: no --role is viewer" "role=viewer (claimed on the command line, not authenticated)" "${COMMON[@]}" --audit "$W/a.log"
  expect_silent "$BIN" "$n: viewer gives no role warning" "nothing authenticates this role" "${COMMON[@]}" --audit "$W/a.log"
  for r in operator engineer admin; do
    expect_runs "$BIN" "$n: --role $r" "role=$r (claimed on the command line, not authenticated)" "${COMMON[@]}" --audit "$W/a.log" --role $r
    expect_runs "$BIN" "$n: --role $r warns" "nothing authenticates this role" "${COMMON[@]}" --audit "$W/a.log" --role $r
  done
done
# No file for accounts is ever made, and no login text exists.
if ls "$W" | grep -qi "users\|lock"; then bad "a start created a file for accounts"; else ok "no accounts or lockout file is created"; fi
expect_silent "$PROD" "production: no user-store text" "user store\|login" "${COMMON[@]}" --audit "$W/a.log"

# Production build: an unsigned configuration cannot be allowed by a flag.
expect_exit2_msg "$PROD" "production: --unsigned-config" "--unsigned-config is not available" "${COMMON[@]}" --unsigned-config
expect_exit2 "$PROD" "production: --unsigned-config with a value" "${COMMON[@]}" --unsigned-config yes
expect_exit2 "$SIM" "simulation: repeated --unsigned-config" "${COMMON[@]}" --unsigned-config --unsigned-config
expect_exit2 "$SIM" "simulation: repeated --config-key" "${COMMON[@]}" --config-key "$W/other.key"

# Test build: banner and unsigned configuration.
expect_runs "$SIM" "simulation: test banner" "TEST BUILD" "${COMMON[@]}" --audit "$W/a.log" --role operator
cp "$W/cfg.json" "$W/unsigned.json"
expect_runs "$SIM" "simulation: --unsigned-config says it is unverified" "configuration NOT verified" --config "$W/unsigned.json" --fb /dev/null --touch /nonexistent --unsigned-config

rm -rf "$W"
[ $fail -eq 0 ] && echo "cli exec test: PASS" || echo "cli exec test: FAIL"
exit $fail
