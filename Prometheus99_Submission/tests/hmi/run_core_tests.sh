#!/bin/sh
# Runs the per-module HMI core test programs and prints one line per module
# plus the total. Exits non-zero if any module fails or prints no summary.
#   usage: run_core_tests.sh <dir with test_<module>[.exe]> <module>...
dir=$1
shift
fail=0
total=0
for t in "$@"; do
    exe="$dir/test_$t"
    [ -x "$exe" ] || exe="$dir/test_$t.exe"
    out=$("$exe" 2>&1)
    rc=$?
    line=$(printf '%s\n' "$out" | grep ' checks,' | tail -1)
    echo "$t: $line"
    if [ $rc -ne 0 ] || [ -z "$line" ]; then
        printf '%s\n' "$out"
        fail=1
    fi
    n=$(printf '%s\n' "$line" | sed -E 's/^ *([0-9]+) checks.*/\1/')
    total=$((total + ${n:-0}))
done
echo "hmi core total: $total checks"
exit $fail
