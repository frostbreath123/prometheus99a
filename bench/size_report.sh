#!/bin/bash
# Phase 0 size report (metric C).
#
#   bench/size_report.sh <executable> <label> [linker map]
#
# Writes bench/results/linux/<label>/size_report.txt: stripped size, sections, the largest symbols,
# the largest static objects (.bss/.data), the shared libraries needed, and (with a linker map) the
# bytes of code and data per source directory. Host build only.
set -u
EXE=${1:?executable}
LABEL=${2:?label}
MAP=${3:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/bench/results/linux/$LABEL
mkdir -p "$OUT"
R=$OUT/size_report.txt
T=$(mktemp)
cp "$EXE" "$T"; strip -s "$T"
{
    echo "executable: $EXE"
    echo "unstripped_bytes: $(stat -c %s "$EXE")"
    echo "stripped_bytes:   $(stat -c %s "$T")"
    echo
    echo "== size -A (stripped)"
    size -A "$T" | awk 'NF==3 && $2>0'
    echo
    echo "== size (Berkeley, stripped)"
    size "$T"
    echo
    echo "== shared libraries needed"
    readelf -d "$EXE" | awk '/NEEDED/'
    file "$EXE" | sed 's/^/file: /'
    echo
    echo "== 40 largest text symbols (bytes)"
    nm -S --size-sort -t d "$EXE" | awk '$3 ~ /^[tTwW]$/' | tail -40 | tac
    echo
    echo "== 25 largest static data objects (.data/.bss, bytes)"
    nm -S --size-sort -t d "$EXE" | awk '$3 ~ /^[bBdD]$/' | tail -25 | tac
    if [ -n "$MAP" ] && [ -f "$MAP" ]; then
        echo
        echo "== bytes per source group from the linker map (input sections that were kept)"
        gawk '
            /^Linker script and memory map/ {on=1; next}
            !on {next}
            /^ \.[A-Za-z_.0-9-]+$/ {sec=$1; next}                 # name on its own line, size on the next
            /^ \.[A-Za-z_.0-9-]+ +0x/ {sec=$1; addr=$2; sz=$3; file=$4}
            /^ +0x[0-9a-f]+ +0x[0-9a-f]+ / && sec != "" {sz=$2; file=$3}
            {
                if (sz == "" || file == "") next
                n = strtonum(sz)
                if (n == 0) {sec=""; sz=""; file=""; next}
                g = file
                if (g ~ /\/tmp\/cc/) {              # the application sources are compiled in one command: group by symbol
                    s = sec; sub(/^\.(text|rodata|data|bss|data\.rel\.ro|rodata\.str[0-9.]*|rodata\.cst[0-9]*)\./, "", s)
                    if (s ~ /^(crypto_|blake2b|argon|sha512|ed25519|fe_|ge_|x25519|poly1305|chacha|sign_|wipe)/) g = "monocypher"
                    else if (s ~ /^(hmi_|g_rt|g_cfg|s_chk|cfg\.|tail\.|buf\.|toks\.|req\.|s_pend)/) g = "src/hmi (and file statics)"
                    else if (s ~ /^(ui_|s_|t_)/) g = "src/ui"
                    else if (s ~ /^(plc_|lm_|mb_)/) g = "src/hmi/plc_libmodbus"
                    else g = "app sources, other"
                }
                else if (g ~ /third_party\/lvgl\/src\/[a-z_0-9]+/) { match(g, /lvgl\/src\/[a-z_0-9]+/); g = "lvgl/" substr(g, RSTART+10, RLENGTH-10) }
                else if (g ~ /lvgl-linux\/src\/[a-z_0-9]+/) { match(g, /src\/[a-z_0-9]+/); g = "lvgl/" substr(g, RSTART+4, RLENGTH-4) }
                else if (g ~ /libmodbus|\.so|\.a\(/) { g = "libs: " g }
                else if (g ~ /monocypher/) g = "monocypher"
                else if (g ~ /src\/hmi/) g = "src/hmi"
                else if (g ~ /src\/ui/) g = "src/ui"
                else if (g ~ /src\/app/) g = "src/app"
                else g = "other: " g
                kind = (sec ~ /^\.(text|rodata|eh_frame)/) ? "code_rodata" : "data_bss"
                tot[g " " kind] += n
                sec=""; sz=""; file=""
            }
            END { for (k in tot) printf "%8d  %s\n", tot[k], k }
        ' "$MAP" | sort -rn | head -45
    fi
} > "$R"
rm -f "$T"
cat "$R"
