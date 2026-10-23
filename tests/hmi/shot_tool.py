#!/usr/bin/env python3
"""Screenshot helper for the headless UI harness (tests/hmi/ui_headless.c writes PPM files).

  shot_tool.py png  <in.ppm> <out.png>      convert (standard library only)
  shot_tool.py colors <in.ppm> [palette]    count the distinct colours; with "palette" also check them against the
                                            sixteen-colour palette of src/ui/ui_priv.h and report the pixels outside it
  shot_tool.py check <in.ppm>...            gate: palette colours only (the chart may have 1.5 % anti-aliased pixels), exact 4-bit round trip
  shot_tool.py i4 <in.ppm>                  pack the screen to 4 bits per pixel through the palette and unpack it again:
                                            reports the size and whether the round trip is exact

The palette is read from src/ui/ui_priv.h (the C_* hex values), converted to RGB565 and back, because the display
is RGB565 and the screenshots show the 565 values expanded to 8 bits.
"""
import re
import struct
import sys
import zlib
from collections import Counter
from pathlib import Path


def read_ppm(path):
    data = Path(path).read_bytes()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not m:
        raise SystemExit("not a P6 file: " + path)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, data[m.end():m.end() + w * h * 3]


def write_png(path, w, h, rgb):
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    rows = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def expand565(r, g, b):
    """RGB888 -> RGB565 -> RGB888 exactly as ui_headless.c save_ppm() does it."""
    p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return ((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31


def palette():
    here = Path(__file__).resolve().parent.parent.parent / "src" / "ui" / "ui_priv.h"
    cols = []
    for line in here.read_text().splitlines():
        m = re.match(r"#define C_[A-Z]+\s+lv_color_hex\(0x([0-9A-Fa-f]{6})\)", line)
        if m:
            v = int(m.group(1), 16)
            cols.append(expand565((v >> 16) & 255, (v >> 8) & 255, v & 255))
    return cols


def colors(path, check):
    w, h, rgb = read_ppm(path)
    cnt = Counter(zip(rgb[0::3], rgb[1::3], rgb[2::3]))
    print("%s: %dx%d, %d distinct colours" % (path, w, h, len(cnt)))
    if check:
        pal = set(palette())
        outside = sum(n for c, n in cnt.items() if c not in pal)
        print("  palette: %d colours defined; %d pixels (%.2f %%) are outside it; %d distinct outside colours"
              % (len(pal), outside, 100.0 * outside / (w * h), sum(1 for c in cnt if c not in pal)))
        for c, n in cnt.most_common(18):
            print("   #%02X%02X%02X %7d %s" % (c[0], c[1], c[2], n, "" if c in pal else "(not in the palette)"))


def i4(path):
    w, h, rgb = read_ppm(path)
    pal = palette()
    index = {c: i for i, c in enumerate(pal)}
    packed = bytearray()
    bad = 0
    idx = []
    for i in range(w * h):
        c = (rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2])
        k = index.get(c)
        if k is None:  # nearest palette entry
            bad += 1
            k = min(range(len(pal)), key=lambda j: sum((a - b) ** 2 for a, b in zip(c, pal[j])))
        idx.append(k)
    for i in range(0, len(idx), 2):
        packed.append((idx[i] << 4) | (idx[i + 1] if i + 1 < len(idx) else 0))
    back = bytearray()
    for b in packed:
        for k in (b >> 4, b & 15):
            back += bytes(pal[k])
    exact = bytes(back[:len(rgb)]) == rgb
    print("%s: 4 bits per pixel = %d bytes (RGB565 would be %d); %d pixels were not in the palette; round trip %s"
          % (path, len(packed), w * h * 2, bad, "exact" if exact else "NOT exact"))


def check(paths):
    """Gate on the screenshots: every screen except the chart uses only palette colours, a 4-bit round trip is exact,
    and the chart (anti-aliased lines at a slant) has at most 1.5 percent of its pixels outside the palette."""
    pal = set(palette())
    bad = 0
    for path in sorted(paths):
        w, h, rgb = read_ppm(path)
        cnt = Counter(zip(rgb[0::3], rgb[1::3], rgb[2::3]))
        outside = sum(n for c, n in cnt.items() if c not in pal)
        limit = 0.015 if "_trend" in path and "no_trend" not in path else 0.0
        frac = outside / float(w * h)
        verdict = "ok" if frac <= limit else "FAIL"
        if verdict == "FAIL":
            bad += 1
        print("  %-4s %s: %d colours, %.3f %% of the pixels outside the palette (limit %.1f %%)"
              % (verdict, path.split("/")[-1], len(cnt), 100.0 * frac, 100.0 * limit))
        if limit == 0.0 and outside == 0:
            idx = {c: i for i, c in enumerate(palette())}
            back = bytearray()
            packed = bytearray()
            ks = [idx[(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2])] for i in range(w * h)]
            for i in range(0, len(ks), 2):
                packed.append((ks[i] << 4) | (ks[i + 1] if i + 1 < len(ks) else 0))
            pl = palette()
            for b in packed:
                for k in (b >> 4, b & 15):
                    back += bytes(pl[k])
            if bytes(back[:len(rgb)]) != rgb:
                print("  FAIL %s: the 4-bit round trip is not exact" % path.split("/")[-1])
                bad += 1
    print("screenshots: %d files, %s" % (len(paths), "PASS" if bad == 0 and paths else "FAIL"))
    return 0 if bad == 0 and paths else 1


def main(argv):
    if len(argv) >= 4 and argv[1] == "png":
        w, h, rgb = read_ppm(argv[2])
        write_png(argv[3], w, h, rgb)
    elif len(argv) >= 3 and argv[1] == "colors":
        colors(argv[2], len(argv) > 3 and argv[3] == "palette")
    elif len(argv) >= 3 and argv[1] == "check":
        return check(argv[2:])
    elif len(argv) >= 3 and argv[1] == "i4":
        i4(argv[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
