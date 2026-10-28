#!/usr/bin/env python3
"""Drives the emulated Pi-class guest through QEMU's QMP socket: waits for the program to start, taps the emulated
touch screen, cuts and restores the fake PLC on the host, and saves what the emulated display shows (QEMU screendump).

  drive.py <run-dir> <out-dir>      run-dir holds qmp.sock, guest.log, plc.pid (written by run_qemu.sh)

Test and evidence tool. The pictures are the contents of the emulated screen (the virtual GPU's scanout), not a mockup.
"""
import json
import os
import signal
import socket
import sys
import time

run, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
log_path = os.path.join(out, "tour.log")
t0 = time.time()
log = open(log_path, "w")


def say(msg):
    line = "%6d ms  %s" % (int((time.time() - t0) * 1000), msg)
    log.write(line + "\n")
    log.flush()
    print(line, flush=True)


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.2)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                return r

    def tap(self, x, y, hold=0.25):
        ax, ay = int(x * 32767 / 799), int(y * 32767 / 479)
        move = [{"type": "abs", "data": {"axis": "x", "value": ax}}, {"type": "abs", "data": {"axis": "y", "value": ay}}]
        self.cmd("input-send-event", events=move + [{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(hold)
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])

    def shot(self, name):
        path = os.path.abspath(os.path.join(out, name + ".ppm"))
        r = self.cmd("screendump", filename=path)
        time.sleep(0.5)
        say("picture %s %s" % (name, "ok" if "return" in r else r))


def wait_for(text, timeout):
    end = time.time() + timeout
    p = os.path.join(run, "guest.log")
    while time.time() < end:
        try:
            if text in open(p, errors="replace").read():
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


plc = int(open(os.path.join(run, "plc.pid")).read())

# touch targets by name, from the one file all tour drivers share (demo/ui_targets.txt)
T = {}
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "demo", "ui_targets.txt")):
    f = line.split()
    if len(f) == 3 and not f[0].startswith("#"):
        T[f[0]] = (int(f[1]), int(f[2]))

q = Qmp(os.path.join(run, "qmp.sock"))
say("connected to QEMU")
if not wait_for("prometheus99 pid", 240):
    say("the program did not start in 240 s")
    sys.exit(1)
say("the program started in the guest")
time.sleep(4)
q.shot("01_overview_normal")
# the fake PLC runs one TANK_LEVEL cycle every 30 s; the high limit (80) is crossed for about 8 s of every cycle
time.sleep(1)
q.shot("02_state_a")
q.tap(*T["tab_alarms"]); say("tap ALARMS"); time.sleep(2.5)
q.shot("03_alarm_list")
q.tap(*T["ack_all"]); say("tap ACK ALL"); time.sleep(2.5)
q.shot("04_after_ack")
q.tap(*T["tab_overview"]); say("tap OVERVIEW"); time.sleep(2)
q.tap(*T["setpoint_plus"]); say("tap setpoint +"); time.sleep(1.2)
q.tap(*T["setpoint_plus"]); say("tap setpoint +"); time.sleep(1.2)
q.tap(*T["setpoint_send"]); say("tap SEND (authorised write by the operator)"); time.sleep(4)
q.shot("05_setpoint_written")
q.tap(*T["tab_trends"]); say("tap TRENDS"); time.sleep(3)
q.shot("06_trends")
q.tap(*T["tab_settings"]); say("tap SETTINGS"); time.sleep(2.5)
q.shot("07_settings")
q.tap(*T["tab_overview"]); say("tap OVERVIEW"); time.sleep(2)
os.kill(plc, signal.SIGUSR1); say("PLC link cut (fake PLC on the host drops the connection)")
time.sleep(8)
q.shot("08_plc_disconnected")
os.kill(plc, signal.SIGHUP); say("PLC link restored")
t_restore = time.time()
time.sleep(14)
q.shot("09_recovered")
say("done")
q.cmd("quit")
