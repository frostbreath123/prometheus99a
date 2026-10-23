#!/bin/bash
# Size and memory gates (docs/OPTIMIZATION_ROADMAP.md, phase 6).
#
#   bench/gate.sh <build dir> [profile]       profile: full (default) or lite
#
# Builds nothing. It reads the static musl PRODUCT executable <build dir>/prometheus99 (the one that has the framebuffer
# and touch drivers; `make ... STATIC=1 CC=musl-gcc BUILD=<dir> hmi`) and fails (exit 1) when
#   - the stripped size is over the budget of the profile,
#   - the static data (.bss + .data) is over the budget,
#   - Argon2id code is still linked in (the password hash was removed),
#   - the executable needs a shared library (it must stay one static file).
# The budgets are the measured results of 2026-10-23 plus about 8 percent, so a normal change passes and a feature
# that adds tens of kilobytes does not. Change a budget only together with a note in docs/OPTIMIZATION_ROADMAP.md.
set -u
B=${1:?build dir}
P=${2:-full}
EXE=$B/prometheus99
[ -f "$EXE" ] || { echo "gate: $EXE not found (build the target hmi with STATIC=1)"; exit 2; }
case "$P" in
    full) MAX_STRIPPED=322000; MAX_STATIC=440000 ;;   # measured: 297088 and 405640
    lite) MAX_STRIPPED=290000; MAX_STATIC=350000 ;;   # measured: 268416 and 321760
    *) echo "gate: unknown profile $P"; exit 2 ;;
esac
T=$(mktemp)
cp "$EXE" "$T"; strip -s "$T"
stripped=$(stat -c %s "$T")
read -r text data bss rest < <(size "$T" | tail -1)
static=$((data + bss))
fail=0
check() { # description value max
    if [ "$2" -le "$3" ]; then echo "  ok   $1: $2 <= $3"; else echo "  FAIL $1: $2 > $3"; fail=1; fi
}
echo "gate ($P) on $EXE"
check "stripped size (bytes)" "$stripped" "$MAX_STRIPPED"
check "static data .data+.bss (bytes)" "$static" "$MAX_STATIC"
if nm "$EXE" 2>/dev/null | grep -qiE "argon2|blake2b"; then echo "  FAIL Argon2id or BLAKE2b code is linked in"; fail=1; else echo "  ok   no Argon2id code in the executable"; fi
if readelf -d "$EXE" 2>/dev/null | grep -q NEEDED; then echo "  FAIL the executable needs a shared library"; fail=1; else echo "  ok   no shared library needed (one static file)"; fi
rm -f "$T"
[ $fail -eq 0 ] && echo "gate ($P): PASS" || echo "gate ($P): FAIL"
exit $fail
