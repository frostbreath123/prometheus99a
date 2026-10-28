# Linux validation record
> **Note, 2026-10-23:** the login, the user store and the password hash described in the parts of this file that mention logins or accounts were removed from the product (`docs/AUTHENTICATION.md`). Those passages are kept as the record of M1 to M4 and no longer describe the program: there are no login or account records in the audit file and no user file, and the audit line shows `user=-`.

**Status: executed on WSL2 (host PC), 2026-10-15. Not executed on a
Raspberry Pi, not executed on a real framebuffer or touch device, not run
against a real PLC.** Every number below is a "WSL2 host, not Raspberry Pi"
number and must not be quoted as target-hardware evidence.

## Environment

| Item | Value |
|---|---|
| Host | Windows 11 Home 10.0.26200, WSL2 distro `Ubuntu-24.04` (VERSION 2) |
| Kernel | Linux 6.18.54.1-microsoft-standard-WSL2 x86_64 |
| Compiler | gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) |
| libmodbus | system 3.1.10 (dynamic, LGPL) |
| LVGL | v9.2.2, built from `third_party/lvgl` |
| Source tree | Windows tree synced to `/root/p99` with rsync (excluding `build*`, `*.exe`, `.git`) |
| Display | none: no `/dev/fb0` and no evdev device exist in this WSL2 |

## Results

| Item | Environment | Command | Result |
|---|---|---|---|
| core tests | WSL2 host | `make -f Makefile.linux test` | 535 checks, 0 failed (521 before the config and tag-accessor tests) |
| config golden corpus | WSL2 host | `make -f Makefile.linux test-config-golden` | 184 cases byte-identical to the golden file; also identical under ASan+UBSan with 0 reports |
| view-model tests | WSL2 host | `make -f Makefile.linux test-view` | 79 checks, 0 failed (added with the view-model split) |
| e2e (libmodbus + POSIX fake PLC) | WSL2 host | `make -f Makefile.linux e2e` | 75 checks, 0 failed; link declared lost after 601 ms for 3 x 200 ms |
| ui-host (headless LVGL) | WSL2 host | `make -f Makefile.linux ui-host` | 22 scenario assertions, 0 failed |
| hmi link | WSL2 host | `make -f Makefile.linux hmi` | linked, `build-linux/prometheus99`, 606,448 B unstripped, needs `libmodbus.so.5` |
| hmi launched, no framebuffer | WSL2 host | see "Launch test" below | after the fix: prints an error and exits with status 2 before starting the runtime |
| hmi launched on framebuffer | none available | not run | **NOT RUN** |
| register change -> tag -> UI text | WSL2 host | ui-host scenario | passed (UI label text, in memory) |
| disconnect / stale / reconnect / alarm recovery | WSL2 host | ui-host scenario | passed |
| AddressSanitizer + UBSan, core | WSL2 host | `-fsanitize=address,undefined -O1` | 521 checks, 0 failed, 0 sanitizer reports |
| AddressSanitizer + UBSan, e2e | WSL2 host | same flags | 75 checks, 0 failed, 0 sanitizer reports |
| ThreadSanitizer, e2e | WSL2 host | `-fsanitize=thread -O1`, `setarch -R` | 75 checks, 0 failed, 0 reports |
| ThreadSanitizer, core | WSL2 host | same | 521 checks, 0 failed; first run had 3 reports, all in test code; after the test fix 0 reports in 3 runs (see below) |

ui-host measurements from the run above (one run, WSL2 host, not Raspberry Pi):
steady state 0 flushes and 0 heap allocation calls in 2.0 s; handler
`lv_timer_handler` over 421 samples mean 16 us, p50 10 us, p95 22 us, p99 152 us,
max 924 us; LVGL heap 20 KB used, 21 KB peak. Raw output is in
`bench/results/linux/linux_ui_host.txt`.

## Screenshots

`bench/results/linux/shot_1..5_*.png` (and the original `.ppm`) were produced
by the headless Linux harness. They are LVGL rendering into a memory buffer.
**No LVGL screen was displayed on any physical or virtual display on Linux.**

## Launch test (no framebuffer)

The executable was started with a loopback copy of `config/hmi.example.json`
(host `127.0.0.1`, port `5502`, nothing listening) so that it could never
reach a real PLC:

```sh
./build-linux/prometheus99 --config /root/p99_loopback.json --audit /tmp/a.log \
   --alarms /tmp/al.bin --fb /nonexistent_fb --touch /nonexistent_ev
```

Observed before the fix: stderr `Error: cannot open framebuffer device: No
such file or directory`; the process was still alive after 3 s with 3 threads;
SIGTERM produced exit status 0.

Finding: `src/app/main_linux.c` did not check whether the framebuffer device
opened (`lv_linux_fbdev_set_file` returns void), so the process kept polling
without a display. Fix: `main_linux.c` now `open()`s the framebuffer path
first and exits with status 2 and a message if that fails, before any PLC
connection or runtime thread starts.

Observed after the fix: `--fb /nonexistent_fb` printed
`cannot open framebuffer '/nonexistent_fb': No such file or directory`,
exit status 2, no alarm file created. With `--fb /dev/null` (openable but not
a framebuffer) the process still starts and runs, LVGL prints
`ioctl(FBIOBLANK)` errors, and SIGTERM gives exit 0: a device that opens but
is not a framebuffer is not detected. That remains open.

## ThreadSanitizer notes

- On this kernel TSan aborts at start with "unexpected memory mapping" unless
  ASLR is disabled for the process; `setarch x86_64 -R` was used. No system
  setting was changed.
- The three first-run reports were all in `tests/hmi/test_hmi_core.c`: the
  test's main thread wrote the mock PLC register array while the comms thread
  read it, and two test reader threads incremented plain `g_reads`. No report
  pointed into `src/hmi/`. Fix (test code only): relaxed `__atomic` load/store
  on the mock register, `__atomic_fetch_add` on the counters. Result: TSan on
  core, 3 runs, 0 reports, 521 checks, 0 failed. The Windows suites still pass.
- Raw output: `bench/results/linux/core_thread.out`, `e2e_thread.out`,
  `*_address*.out`.

## Not validated

Raspberry Pi or any ARM device, real `/dev/fb0` flushing, 16 bpp vs 32 bpp
framebuffer conversion, evdev touch, touch latency, real PLC interoperability,
soak, systemd integration.

## Re-run after the tag-table accessor refactor (2026-10-15)

Same host (WSL2, not Raspberry Pi). `make -f Makefile.linux check-tagdb-access
test-config-golden test-view test e2e ui-host hmi`: all exit 0, 0 warnings;
view 79, core 535, e2e 75, ui-host 22 assertions, golden identical.
Sanitizers re-run on the changed scheduler code, `-O1`, TSan under
`setarch -R`: ASan+UBSan core 535 and e2e 75, TSan core 535 and e2e 75, all
with 0 reports.

## Re-run after splitting the core test file (2026-10-15)

`tests/hmi/test_hmi_core.c` no longer exists; the earlier sections above refer
to it by its old name. It is now eight per-module programs
(`make -f Makefile.linux test`, one line per module, total 535 checks). WSL2
host, not Raspberry Pi: guard ok, golden identical, view 79, core modules 535,
e2e 75, ui-host 22 assertions, `hmi` links with 0 warnings. Every module under
ASan+UBSan and under TSan (`setarch -R`): 535 checks, 0 reports; e2e 75 checks,
0 reports under both. Raw output: `bench/results/linux/sanitizers_split/`.

## Liveness supervision (2026-10-15)

WSL2 host (Ubuntu 24.04, glibc 2.39, systemd as PID 1), not Raspberry Pi.

| Item | Command | Result |
|---|---|---|
| Watchdog decision logic | `make -f Makefile.linux test` (module `watchdog`) | 29 checks, 0 failed |
| Runtime heartbeat counters | same (module `runtime`) | 44 checks, 0 failed |
| sd_notify sender | `make -f Makefile.linux test-notify` | 13 checks, 0 failed (path, abstract `@` form, unset, too long) |
| Real executable under a fake systemd | `make -f Makefile.linux liveness` | 8 of 8: READY once, pings at cadence, none while SIGSTOPped, resume after SIGCONT, none after SIGKILL, pings continue against a PLC that never answers, clean SIGTERM exit, STOPPING sent |
| Heartbeat mark flush cost | `make -f Makefile.linux ui-host` | off: 0 flushes in 2 s; on: 3 to 4 flushes and about 4,000 to 5,000 pixels in 3 s |
| Real systemd | transient unit, `Type=notify`, `WatchdogSec=4`, SIGSTOP of the main process | systemd logged "Watchdog timeout", killed it with SIGABRT and restarted it with a new PID, `NRestarts=1`; repeated twice; unit removed afterwards |
| Unit file | `systemd-analyze verify deploy/prometheus99.service` | only "binary not installed" (expected on a build host) |
| Sanitizers | ASan+UBSan and TSan, every core module, notify test, e2e | 570 + 13 + 75 checks, 0 reports |

Note on ThreadSanitizer: replacing `pthread_timedjoin_np` with
`pthread_clockjoin_np` made this libtsan lose the join edge and report 29 false
races; the monotonic join is therefore implemented as a 1 ms
`pthread_tryjoin_np` poll.

Not validated: Raspberry Pi, the hardware watchdog, installing the unit as the
`prometheus` user under `ProtectSystem=strict`, a real display.

## Authorization and strict command line (2026-10-15)

WSL2 host (Ubuntu 24.04, gcc 13.3), not Raspberry Pi, no real PLC.

| Item | Command | Result |
|---|---|---|
| Authorization decision point | `make -f Makefile.linux test` (module `authz`) | 41 checks, 0 failed |
| Viewer denied, Operator allowed, ACK ALL denied leaves every alarm unchanged, audit hook order and fail-closed policy | module `alarm` | 81 checks, 0 failed (was 36) |
| Real runtime: denial and grant lines in the audit file, unwritable audit path refuses grants, no audit path allows | module `runtime` | 64 checks, 0 failed (was 44) |
| Strict command line parsing | module `cli` | 66 checks, 0 failed |
| Real executable | `make -f Makefile.linux cli-exec` | 15 of 15: ten invalid invocations exit 2, default role viewer, each valid role starts |
| UI path (viewer then operator) and audit lines | `make -f Makefile.linux ui-host` | 36 assertions, 0 failed; `bench/results/linux/shot_6_viewer_denied.png` |
| Write safety unchanged | modules `writes`, e2e | 252 and 75 checks, 0 failed |
| Sanitizers | ASan+UBSan and TSan, every module, notify, e2e | 742 + 13 + 75 checks, 0 reports |

Limits: the claimed role is not authentication; "durable" means `fsync`
returned success on this filesystem; no power-cut test was run.


## Authentication and sessions (2026-10-22)

WSL2 host (Ubuntu 24.04, gcc 13.3), x86-64, not Raspberry Pi, no real PLC, no
display. Design record: [AUTHENTICATION.md](AUTHENTICATION.md). The source tree
was synced to `/root/p99` as before.

| Item | Command | Result |
|---|---|---|
| Core tests, all modules | `make -f Makefile.linux test` | 1168 checks, 0 failed (kdf 32, platform 26, users 114, auth 237, cli 83) |
| View-model | `make -f Makefile.linux test-view` | 163 checks, 0 failed |
| End to end | `make -f Makefile.linux e2e` | 75 checks, 0 failed |
| Config corpus | `make -f Makefile.linux test-config-golden` | identical |
| Headless UI with first boot, wrong password, lock wait, login, idle logout, logout and audit lines | `make -f Makefile.linux ui-host` | 77 assertions, 0 failed; 0 flushes in 1.5 s idle with the login layer present |
| Liveness under a fake systemd | `make -f Makefile.linux liveness` | pass |
| Command line and `--role` gate (production build refuses it, simulation build accepts it) | `make -f Makefile.linux cli-exec` | pass |
| User tool, store states, file modes, audit lines | `make -f Makefile.linux auth-exec` | pass |
| ASan with UBSan, and TSan, every core module | `-fsanitize=address,undefined` and `-fsanitize=thread` | 0 reports |
| Argon2id hash time, default cost (3 passes, 8 MiB) | `p99_user bench` | 41, 17 and 13 ms in three runs on this host |

Limits: the sanitizers were run on the core modules, not on the LVGL harness or
the full executable. Hash time on a Raspberry Pi, typing on a real touchscreen,
a deployed service under its own user, and a power cut during a store write
were not tested.

## Configuration integrity and audit identity (2026-10-22)

WSL2 host (Ubuntu 24.04, gcc 13.3), x86-64, not Raspberry Pi, no real PLC, no
display, no real systemd. Design record: [CONFIG_INTEGRITY.md](CONFIG_INTEGRITY.md).

| Item | Command | Result |
|---|---|---|
| Core tests, all modules | `make -f Makefile.linux test` | 1532 checks, 0 failed (mac 13, cfgsec 223, config 122, writes 280, alarm 103, runtime 68, cli 107, users 131, auth 237) |
| View-model, end to end, config corpus | `test-view`, `e2e`, `test-config-golden` | 163 checks, 75 checks, identical |
| Headless UI scenario, now with the acknowledgement and write recorded under the admin's name | `make -f Makefile.linux ui-host` | 84 assertions, 0 failed |
| Liveness, command line, authentication executables | `liveness`, `cli-exec`, `auth-exec` | pass (all three now run against a signed configuration) |
| Signing tool and every start-up rejection, with a loopback listener counting connections | `make -f Makefile.linux config-exec` | pass: 0 connections in each safe-mode run, 2 in the accepted run |
| Mutation check of the POSIX file-mode rules (key, configuration, signature) | local script | each disabled rule made the suite fail |
| ASan with UBSan, and TSan, every core module | `-fsanitize=address,undefined`, `-fsanitize=thread` | 0 reports |

Limits: no real systemd, no `prometheus` user, no root-owned files; the
sanitizers were run on the core modules, not on the LVGL harness or the full
executable.

## Persistence and storage (2026-10-22)

WSL2 host (Ubuntu 24.04, gcc 13.3), x86-64, not Raspberry Pi, no real PLC, no
display, no real systemd, no power cut. Design record: [STORAGE.md](STORAGE.md).
The source tree was synced to `/root/p99` as before.

| Item | Command | Result |
|---|---|---|
| Core tests, all modules | `make -f Makefile.linux test` | 2635 checks, 0 failed (store 454, auditfmt 60, writes 632, alarm 188, runtime 162, config 137, platform 69, cli 107, users 131, auth 237, cfgsec 223) |
| View-model, end to end, config corpus | `test-view`, `e2e`, `test-config-golden` | 168 checks, 75 checks, identical to the regenerated golden file |
| Headless UI scenario, acknowledgement now pending then settled, audit line with old value and stamp | `make -f Makefile.linux ui-host` | 89 assertions, 0 failed |
| Liveness, command line, authentication, configuration executables | `liveness`, `cli-exec`, `auth-exec`, `config-exec` | pass |
| Kill-loop self test | `make -f Makefile.linux killloop` | 13 checks pass: the verifier accepts a healthy run and refuses each of 11 faults, and accepts one torn last line |
| Kill-loop | `KILL_SEED=7 make -f Makefile.linux killloop KILL_ROUNDS=150` | 150 rounds (125 SIGKILL at random moments between 0.3 and 2.5 s, 25 clean SIGTERM stops), verifier passed after every round, rotation happened 9 times in the retained files |
| ASan with UBSan, and TSan, every core module and the end-to-end test | `-fsanitize=address,undefined`, `-fsanitize=thread`, `setarch -R` | 0 reports |
| Mutation checks of the new rules | local script | 33 of 34 caught; one equivalent change (dropping the partial first journal line, which fails its CRC anyway) |

Windows host (MSYS2 UCRT64 GCC 13.2): `mingw32-make clean all test-all ui-host
check-linux` exit 0; core 2597 checks, view-model 168, end to end 75, corpus
identical, UI scenario 89 assertions, legacy suite 48 checks. The wall-clock
state is always `sync=no` there.

Limits: the kill-loop kills the process, not the power, and the operating
system still writes what the process had handed to it. No unfinished audit line
occurred in the runs; that path is covered by the store tests with injected
torn writes. Whether `fsync` and the rename make records durable on the storage
of a Raspberry Pi was not tested. The clock state (`adjtimex`) was read on WSL2
only. The sanitizers were not run on the LVGL harness, the full executable or
the kill-loop processes.

## Screens, settings and configuration changes (2026-10-22, milestone M4)

WSL2 host (Ubuntu 24.04, gcc 13.3), x86-64, not Raspberry Pi, no real PLC, no display, no real
systemd. The source tree was synced to `/root/p99`. Design records: [SCREENS.md](SCREENS.md) and
[CONFIG_INTEGRITY.md](CONFIG_INTEGRITY.md).

| Item | Command | Result |
|---|---|---|
| Core tests, all modules | `make -f Makefile.linux test` | 3925 checks, 0 failed (cfgedit 868, cli 116) |
| View-model, end to end, config corpus | `test-view`, `e2e`, `test-config-golden` | 313 checks, 75 checks, identical |
| Headless UI scenario | `make -f Makefile.linux ui-host` | 277 assertions, 0 failed |
| Liveness, command line, authentication, configuration executables | `liveness`, `cli-exec`, `auth-exec`, `config-exec` | pass |
| The root helper against real files, as root | `make -f Makefile.linux cfgedit-exec` | passes |
| Production and simulation executables, helper | `make -f Makefile.linux hmi hmi-sim` | build without warnings |
| ASan with UBSan | core tests `cfgedit auth users config cfgsec cli trend alarm store runtime auditfmt`, and `p99_cfg` running `cfgedit-exec` | 0 reports |
| TSan | core tests `cfgedit auth runtime trend` | 0 reports |
| Mutation checks of the new rules | local script | 33 of 34 caught (one equivalent change) |
| Unit files | `systemd-analyze verify` on `prometheus99.service`, `prometheus99-config.service`, `prometheus99-config.path` | no messages (the executables were stand-ins); a syntax check only |

Windows host (MSYS2 UCRT64 GCC 13.2): `mingw32-make test-hmi test-view test-config-golden ui-host`
pass: core 3887 checks, view-model 313, corpus identical, UI scenario 277 assertions. Three
intermittent failures of the `runtime` and `auth` tests on this host were explained and fixed (two
test timing margins and a retry of the file replace in `hmi_platform_win32.c`; see
`ROADMAP_STATUS.md`). `runtime` ran 60 times without a failure afterwards.

Defect found by the Linux build only: `BYTE_ORDER` is a macro in glibc's `endian.h` and collided with
an array name in `hmi_config_write.c`; renamed.

Limits: the units were not run, the restart of the service by the helper was not tried, the
sanitizers were not run on the LVGL harness or the full executable, and nothing was rendered on a
real display.

## Size and memory work, authentication removed (2026-10-23)

Run on WSL2 Ubuntu 24.04 (kernel 6.18.54, gcc 13.3, musl-gcc 1.2.4, aarch64-linux-gnu-gcc 13 with qemu-user 8.2.2).
The measurements and the reading of them are in `docs/ENGINEERING_REPORT.md`; this table is the record of what ran.

| What | Command | Result |
|---|---|---|
| Core tests, dynamic glibc `-O2` | `make -f Makefile.linux test` | 3,262 checks, 0 failed (3,925 before the login layer was removed) |
| View model, config golden, notify | `test-view`, `test-config-golden`, `test-notify` | 197 checks; identical; 13 checks |
| Headless UI scenario, full and lite | `make ui-host`, `make LITE=1 BUILD=build-lite ui-host` | 205 and 127 assertions, 0 failed; both include the RAM gate |
| Screenshot gate | `make ui-shots` | 23 screenshots inside the 11-colour palette (the chart within 1.5 %), 4-bit round trip exact |
| End to end | `make e2e` | 75 checks, 0 failed |
| Executables | `cli-exec`, `config-exec`, `cfgedit-exec` (as root), `liveness` | all pass |
| The same under static musl, `-Os`, LTO | `make STATIC=1 CC=musl-gcc BUILD=build-musl ...` | the same suites pass |
| aarch64 under qemu-user | cross build of the static tree; `test`, `test-view`, `e2e`, `ui-host` | 3,262, 197, 75 and 205, 0 failed (`bench/results/linux/arm64`) |
| AddressSanitizer + UBSan | `make SAN=address ...` | eight suites clean (`bench/results/linux/sanitizers_address`) |
| ThreadSanitizer | `setarch x86_64 -R make SAN=thread ...` | core, end to end and liveness clean; the UI scenario reported a data race in the **test fixture** (`fake_modbus_server*.c`), fixed with relaxed atomics (rerun noted below) |
| Kill-loop | `make killloop KILL_ROUNDS=150` | 150 rounds (125 kills, 25 clean stops), every round verified (`bench/results/killloop_phase3.txt`) |
| Size and memory gates | `bench/gate.sh build-musl full`, `... build-musl-lite lite` | pass on the product executable: 297,088 and 268,416 bytes stripped, 405,640 and 321,760 bytes static data, no Argon2id, no shared library |

Defects found by these runs: UBSan reported a signed overflow inside LVGL's chart (a "no value" point times the height, `lv_chart.c` line
847): the LVGL objects are compiled with `-fwrapv`; ThreadSanitizer's data race in the test fixture, as above; the TSan runtime of this
kernel cannot map memory with the default address-space randomisation, so it runs under `setarch -R` (no system setting is changed).

Limits: the sanitizers ran on the development build, not on the static LTO build; qemu is an emulator, not a measurement; nothing ran on a
Raspberry Pi, a framebuffer, a touch panel, a real PLC or real systemd.
