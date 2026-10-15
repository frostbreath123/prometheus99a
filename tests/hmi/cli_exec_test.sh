#!/bin/bash
# Real-executable check of command-line strictness (Linux). Invalid options
# must exit with status 2 before any hardware is touched; valid roles must
# start. Uses /dev/null as the framebuffer and a loopback config.
#   usage: cli_exec_test.sh <build dir> <config template>
B=${1:-build-linux}
TPL=${2:-config/hmi.example.json}
W=$(mktemp -d /tmp/p99_cli.XXXXXX)
sed -e 's/192\.168\.0\.10/127.0.0.1/' -e 's/"port": 502,/"port": 5502,/' "$TPL" > "$W/cfg.json"
fail=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
BIN="$B/prometheus99"
COMMON=(--config "$W/cfg.json" --fb /dev/null --touch /nonexistent)

expect_exit2() { # description, args...
  local d=$1; shift
  "$BIN" "$@" > "$W/out" 2>&1; local rc=$?
  if [ $rc -eq 2 ] && grep -q "prometheus99:" "$W/out"; then ok "$d (exit 2)"; else bad "$d (exit $rc)"; cat "$W/out"; fi
}
expect_runs() { # description, expected role, args...
  local d=$1 role=$2; shift 2
  "$BIN" "$@" > "$W/out" 2>&1 &
  local p=$!
  sleep 1
  if kill -0 $p 2>/dev/null; then
    if grep -q "role=$role (claimed on the command line, not authenticated)" "$W/out"; then ok "$d (starts as $role)"; else bad "$d (role banner missing)"; cat "$W/out"; fi
  else
    bad "$d (did not start)"; cat "$W/out"
  fi
  kill -TERM $p 2>/dev/null; wait $p 2>/dev/null
}

expect_exit2 "unknown role typo"            "${COMMON[@]}" --role operatr
expect_exit2 "wrong-case role"              "${COMMON[@]}" --role Operator
expect_exit2 "empty role value"             "${COMMON[@]}" --role ""
expect_exit2 "role flag without value"      "${COMMON[@]}" --role
expect_exit2 "role value is another flag"   "${COMMON[@]}" --role --audit
expect_exit2 "unknown flag"                 "${COMMON[@]}" --bogus
expect_exit2 "misspelled --audit"           "${COMMON[@]}" --audt "$W/a.log"
expect_exit2 "stray argument"               "${COMMON[@]}" stray
expect_exit2 "repeated --role"              "${COMMON[@]}" --role viewer --role admin
expect_exit2 "--role=value form"            "${COMMON[@]}" --role=admin

expect_runs "no --role given"  viewer   "${COMMON[@]}" --audit "$W/a.log"
for r in viewer operator engineer admin; do
  expect_runs "--role $r" $r "${COMMON[@]}" --audit "$W/a.log" --role $r
done

rm -rf "$W"
[ $fail -eq 0 ] && echo "cli exec test: PASS" || echo "cli exec test: FAIL"
exit $fail
