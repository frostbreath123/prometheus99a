#!/usr/bin/env python3
"""Which static objects of a running executable are resident, from /proc/<pid>/pagemap and the symbol table.

  touched_symbols.py <pid> <unstripped executable> [top N]

For every data/bss object larger than 256 bytes it counts the 4 KiB pages that are present in RAM (the
bit 63 of the pagemap entry) and prints the pages shared with its neighbours as "shared". Needs root.
Linux only, and the executable must be the unstripped file the process was started from (PIE or not).
This is a bench tool: it shows where the anonymous PSS of the process comes from, so that RAM reductions are made
where pages are really touched and not where a symbol merely looks large.
"""
import re
import subprocess
import sys

PAGE = 4096


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    pid, exe = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
    maps = open("/proc/%s/maps" % pid).read().splitlines()
    base = None
    for line in maps:
        if exe.split("/")[-1] in line:
            base = int(line.split("-")[0], 16)
            break
    if base is None:
        print("executable not found in the maps of the process")
        return 1
    syms = []
    out = subprocess.run(["nm", "-S", "-n", "--defined-only", exe], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in "bBdD":
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    pm = open("/proc/%s/pagemap" % pid, "rb")

    def present(addr):
        pm.seek((addr // PAGE) * 8)
        e = int.from_bytes(pm.read(8), "little")
        return bool(e >> 63 & 1)

    rows = []
    for off, size, name in syms:
        if size < 256:
            continue
        a, b = base + off, base + off + size
        pages = range(a // PAGE, (b - 1) // PAGE + 1)
        n = sum(1 for pg in pages if present(pg * PAGE))
        rows.append((n * PAGE, size, name))
    rows.sort(reverse=True)
    tot = 0
    print("%10s %10s  %s" % ("resident", "size", "object (pages present x 4 KiB, objects over 255 bytes)"))
    for res, size, name in rows[:top]:
        print("%10d %10d  %s" % (res, size, name))
    for res, size, name in rows:
        tot += res
    print("sum of resident pages over listed objects (pages shared by two objects count twice): %d bytes" % tot)
    return 0


if __name__ == "__main__":
    sys.exit(main())
