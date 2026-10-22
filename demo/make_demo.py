"""Builds demo/prometheus99_demo.html: one self-contained page (the screenshots are inside it) that
shows what the headless UI harness rendered, with a caption for each step. Send that one file to
teammates; it opens in any browser, offline.

    python demo/make_demo.py [shot prefix, default build/shot]

The screenshots come from `mingw32-make ui-host` (see demo/run_demo.bat). They are renderings of the
real LVGL UI on a PC, not photographs of a device.
"""
import base64
import glob
import os
import re
import struct
import sys
import zlib

PREFIX = sys.argv[1] if len(sys.argv) > 1 else "build/shot"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CAPTIONS = {
    "normal": ("Overview", "Tag table with live values and quality. The setpoint panel is at the bottom. This build is the test build, so the banner says the role is not authenticated."),
    "alarm": ("An alarm is raised", "TANK_LEVEL passes its high limit. The header shows the alarm summary and the row is coloured."),
    "acked_write": ("Acknowledge and write", "An operator acknowledges the alarm and sends a setpoint. Both are recorded in the audit file before they take effect."),
    "commloss": ("PLC link lost", "The link indicator changes, values show as unusable and no stale number is drawn as good."),
    "recovered": ("Link recovered", "The runtime reconnected by itself and the values are good again."),
    "viewer_denied": ("Viewer role", "A viewer cannot acknowledge or write. The core refuses even if a button is forced."),
    "first_boot": ("First boot", "No accounts exist yet, so the login screen offers to create the administrator."),
    "login_refused": ("Wrong password", "The wording is the same for a wrong user and a wrong password."),
    "logged_in": ("Logged in", "The header shows the role and the user name."),
    "alarm_list": ("Alarm list", "Most urgent first: priority, then unacknowledged, then newest. Each row has its own ACK."),
    "last_page": ("Overview, last page", "The tag table is paged."),
    "trend": ("Trends", "Min and max per period for one tag. A break in the lines means no good data in that period."),
    "no_trend": ("No trended tags", "A device without trended tags says so."),
    "settings_system": ("Settings: system", "Read-only facts about the device: PLC, writes, configuration serial, storage, clock."),
    "add_account": ("Settings: add an account", "An administrator adds an account on the device. Passwords are never shown."),
    "accounts": ("Settings: accounts", "Add, change password, change role, remove (two presses). Your own account and the last administrator are protected."),
    "config_viewer": ("Settings: configuration, viewer", "Only an engineer or an administrator can change settings."),
    "config_keypad": ("Settings: number pad", "An engineer edits an alarm limit with the number pad."),
    "config_apply": ("Settings: apply", "Edits are shown as 'old -> new'. APPLY needs two presses. The rules are checked first."),
    "config_result": ("Settings: result", "The request goes to a root helper that re-checks it, signs the new configuration and restarts the service. The last answer is shown here."),
}


def ppm_to_png(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    pix = parts[3]
    raw = b"".join(b"\x00" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    files = glob.glob(os.path.join(ROOT, PREFIX + "_*.ppm"))
    shots = []
    for f in files:
        m = re.search(r"_(\d+)_(.+)\.ppm$", os.path.basename(f))
        if m:
            shots.append((int(m.group(1)), m.group(2), f))
    shots.sort()
    if not shots:
        sys.exit("no screenshots found: run 'mingw32-make ui-host' first")
    slides = []
    for _, name, f in shots:
        title, text = CAPTIONS.get(name, (name.replace("_", " ").title(), ""))
        b64 = base64.b64encode(ppm_to_png(f)).decode("ascii")
        slides.append((title, text, b64))

    items = "\n".join(
        '<section class="slide"><h2>%d. %s</h2><p>%s</p><img alt="%s" src="data:image/png;base64,%s"></section>'
        % (i + 1, t.replace("<", "&lt;"), x.replace("<", "&lt;"), t.replace('"', ""), b)
        for i, (t, x, b) in enumerate(slides))
    html = PAGE.replace("@@SLIDES@@", items).replace("@@COUNT@@", str(len(slides)))
    out = os.path.join(HERE, "prometheus99_demo.html")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(html)
    print("wrote %s (%d screens, %.0f KB)" % (out, len(slides), os.path.getsize(out) / 1024))


PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Prometheus99 HMI demo</title>
<style>
:root{--bg:#14171b;--panel:#1d2227;--text:#e8e8e8;--muted:#9aa3ad;--acc:#3a8dde}
@media (prefers-color-scheme: light){:root{--bg:#f2f4f6;--panel:#fff;--text:#1b1f24;--muted:#5b6670}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.5 system-ui,Segoe UI,sans-serif}
header{padding:16px 20px 0}
h1{margin:0;font-size:20px}
.sub{color:var(--muted);font-size:14px;margin:2px 0 0}
main{max-width:900px;margin:0 auto;padding:12px 16px 80px}
.slide{display:none}.slide.on{display:block}
h2{font-size:18px;margin:14px 0 4px}
p{margin:0 0 12px;color:var(--muted);min-height:3em}
img{width:100%;height:auto;border-radius:6px;border:1px solid #0003;background:#000;image-rendering:auto}
nav{position:fixed;left:0;right:0;bottom:0;background:var(--panel);border-top:1px solid #0003;display:flex;gap:8px;align-items:center;justify-content:center;padding:10px}
button{background:var(--acc);color:#fff;border:0;border-radius:6px;padding:9px 16px;font-size:15px;cursor:pointer}
button.alt{background:transparent;color:var(--text);border:1px solid var(--muted)}
.count{min-width:64px;text-align:center;color:var(--muted)}
.note{color:var(--muted);font-size:13px;margin-top:10px}
</style></head><body>
<header><h1>Prometheus99 HMI</h1>
<div class="sub">Screens of the real LVGL UI, rendered on a PC by the headless harness. Keys: left and right arrows, space plays.</div></header>
<main>@@SLIDES@@
<div class="note">Renderings from a PC with a fake PLC. Not shown on a real display, not measured on a Raspberry Pi.</div></main>
<nav><button id="prev">Prev</button><span class="count" id="count"></span><button id="next">Next</button><button class="alt" id="play">Play</button></nav>
<script>
var s=document.querySelectorAll('.slide'),i=0,t=null;
function show(n){i=(n+s.length)%s.length;for(var k=0;k<s.length;k++)s[k].classList.toggle('on',k===i);document.getElementById('count').textContent=(i+1)+' / '+s.length;}
function stop(){if(t){clearInterval(t);t=null;document.getElementById('play').textContent='Play';}}
document.getElementById('prev').onclick=function(){stop();show(i-1)};
document.getElementById('next').onclick=function(){stop();show(i+1)};
document.getElementById('play').onclick=function(){if(t){stop();}else{t=setInterval(function(){show(i+1)},3500);this.textContent='Pause';}};
document.addEventListener('keydown',function(e){if(e.key==='ArrowRight'){stop();show(i+1)}else if(e.key==='ArrowLeft'){stop();show(i-1)}else if(e.key===' '){e.preventDefault();document.getElementById('play').click()}});
show(0);
</script></body></html>
"""

if __name__ == "__main__":
    main()
