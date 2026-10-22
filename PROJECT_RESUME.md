# PROJECT_RESUME.md: Prometheus99 handoff

## START HERE (handoff for a new session, 2026-10-22)

**M4 is implemented and uncommitted.** Read `docs/ROADMAP_STATUS.md` (its newest "Update"
section is M4), then `docs/SCREENS.md`, then section V at the end of this file. Section U and
the older ones describe M1 to M3 and are still true for them, except where section V says
otherwise.

State, checked on the development host only (Windows MSYS2 and WSL2; nothing on a Raspberry
Pi, a real PLC, a real display or real systemd): steps 4.0 to 4.6 are done. The screens
(overview, alarms, trends, settings with system, accounts and configuration views), alarm
priority, trends, account management, the configuration writer and request format
(`hmi_config_write`, `hmi_cfgedit`), the root helper `p99_cfg`, the two systemd units for it
and `--spool` exist. Last verified numbers (rerun, do not trust): Windows core 3887 checks,
WSL2 core 3925, view-model 313, headless UI scenario 277 assertions, config golden identical,
`cfgedit-exec` passes as root, ASan and TSan clean on the new modules, 33 of 34 mutations
caught. Three intermittent Windows failures (`runtime` at `test_runtime.c:606` and line 226,
`auth` at lines 881 to 891) were explained and fixed: two test timing margins, and a retry of the
file replace in `hmi_platform_win32.c` (a scanner holding the file open); see the M4 section of
`docs/ROADMAP_STATUS.md`.
The three stray files that were in the repository root at the start of M4 (`cs_cfg.json`,
`cs_cfg.json.sig`, `cs_key.bin`) are fixtures of `tests/hmi/test_cfgsec.c`; they were gone after
the 4.6 test runs, so the test's own cleanup removed them. Nothing was deleted by hand.

Exact next steps:
1. The owner reviews M4, then commits when they say so (no commit, push or GitHub change
   unless asked).
2. CI (GitHub Actions on Ubuntu plus a `.gitmodules` fix) was approved by the owner but is not
   started and is outside M4.
3. When a Raspberry Pi and a real systemd are available: run the units, the helper and its
   restart of the service, then the display and touch (the number pad), then row 50 of the
   roadmap.
Rules unchanged: no commit, push or GitHub change unless asked; no real PLC; do not touch the
frozen legacy prototype; chat in caveman style, files in plain sentences. Repository source
files are LF, not CRLF (the older instruction said CRLF; this was reported). Write patch
scripts with the Write tool, not shell heredocs.

Read in this order: `docs/ROADMAP_STATUS.md` (requirements matrix of 55 rows
against the mentor's document, discrepancies, milestones M1 to M9, blockers,
acceptance criteria; the four "Update" sections at its top, for M1, M2, M3 and M4,
are the current state),
then sections S, T and U at the end of this file, then `docs/ADR-0001-runtime-boundaries.md`
(read its addenda), then `docs/AUTHENTICATION.md`, `docs/CONFIG_INTEGRITY.md`
and `docs/STORAGE.md`.
The requirements baseline is the mentor's document, kept outside the repository at
`C:\Users\Admin\Downloads\Prometheus99 Industrial HMI Runtime Architecture and Engineering Documentation.md`.
Sections A to R below are older history and are partly out of date.

Current state, checked against the repository on 2026-10-22:
- The original audit (matrix, discrepancies, roadmap) was read-only: it changed
  no implementation code. It is kept in `docs/ROADMAP_STATUS.md`.
- Since then three milestones were implemented, host-verified only: M1
  (authentication, sessions, lockout, first-boot administrator, `p99_user`), M2
  (signed configuration with HMAC-SHA-512, rollback serial, PLC allow-list,
  `p99_sign`, user name in acknowledgement and write audit lines) and M3
  (persistence and storage: storage worker, audit file with wall-clock stamps and
  rotation, alarm journal on disk, alarm state saved on change, write rate limit,
  second check before sending, shutdown budget fix).
- Git: branch `feature/authorization-hardening`, four commits (`2ca7c75`, `fe165c2`
  for M1, `e858465` for M2, `defa45f` for M3), pushed to `origin`
  (`https://github.com/frostbreath123/prometheus99a.git`); the branch was in sync
  with `origin` and the tree clean when this was written (the owner committed and
  pushed M3; this file's own last edit is the only change since). The four empty
  stray files were deleted on 2026-10-22 with the owner's permission.
  `third_party/lvgl` and `third_party/libmodbus` are still gitlinks without a
  `.gitmodules` file, so a fresh clone lacks them. No CI exists.
- Nothing has run on a Raspberry Pi, against a real PLC, or under real systemd as
  the service user. Every number is a host number.
- Verified at the last run (Windows MSYS2 and WSL2 Ubuntu 24.04, 2026-10-22; rerun,
  do not trust): core modules 2597 checks on Windows and 2635 on WSL2, view-model
  168, end to end 75, config corpus identical (regenerated for M3), headless UI
  scenario 89 assertions, legacy suite 48; WSL2 executable tests `liveness`,
  `cli-exec`, `auth-exec` and `config-exec` pass; kill-loop of 150 rounds passes;
  ASan with UBSan and TSan 0 reports on every core module and the end-to-end test.
- Requirement tally after M3 (55 rows): 26 complete and verified (host evidence),
  5 implemented but unverified, 10 partial, 11 not implemented, 2 blocked, 1 optional.
- The frozen legacy prototype (`include/`, `src/layer*.c`, `src/main.c`,
  `tests/test_core.c`, `tests/stub_layer0.c`) is unchanged.

Decisions: all of the original open decisions were answered on 2026-10-22. Roadmap
order M1 to M5 approved. D1 Monocypher (Argon2id for passwords, HMAC-SHA-512 for the
configuration, so the mentor's "HMAC" is met literally). D2 configuration, signature
and key owned by root and only read by the service. D3 coils and discrete inputs out
of scope for now (the document's tag types have no boolean). D4 Raspberry Pi
"soon, date unknown". Still open: D5, GitHub and CI setup (the remote exists but no
CI definition was requested or written), and the mentor's rulings listed in section 8
of the roadmap. One consequence was reported and accepted: the HMI cannot save its own
configuration, so "configuration save restricted to Admin" moves to M4 and needs a
privileged helper.

Owner answers on 2026-10-22, at the start of the M3 session: the Raspberry Pi is
**not** available yet; CI is wanted and a plan is to be written first. A CI plan
was given in the chat (GitHub Actions on Ubuntu running the Linux tests and the
sanitizers, plus a `.gitmodules` file or vendored sources for the two gitlinks) and
is waiting for approval; nothing was written, committed or pushed for it. M3 was
planned, approved with the recommended option for each of its five decisions, and
implemented (section U).

Exact next steps:
1. Ask the owner whether the CI plan is approved (and the Raspberry Pi status). Do not
   commit, push or touch GitHub without being asked.
2. Plan M4 (screens, trends, alarm priority, settings and the configuration helper;
   `docs/ROADMAP_STATUS.md` section 7) and wait for approval before implementing, as
   for M1 to M3. M5 (Raspberry Pi) waits for the hardware; the power-cut test of the
   storage layer and the flash behaviour belong there.
3. Open M3 items for later: trace ring (row 42), grouping queued records into one
   flush, a power-cut test on the target.

Rules that still apply: complete major requirements first; no real PLC connection or
real control writes; do not modify the legacy prototype; no commit, push or GitHub
change unless the owner asks; keep ADR-0001 and the architecture unless a defect
requires a change; report documentation conflicts instead of choosing silently;
update documentation only with verified facts; chat in caveman style, files in plain
full sentences. Tool notes: run Linux commands with `wsl.exe` from PowerShell and keep
a rsync-to-`/root/p99` script; the shell tool in this environment mangled backslashes
inside heredocs, so write files with the file tools and put patch scripts in files;
`mingw32-make clean` deletes `build/`; TSan needs `setarch $(uname -m) -R`.

Last updated: 2026-10-22 (milestone M3, persistence and storage, implemented; see
section U, sections S and T, section R and `docs/ROADMAP_STATUS.md`). Earlier text: 2026-10-15, after the Linux execution and sanitizer session
(see section K for what changed; sections E, F, G and J were updated to
match). Written for a fresh session / a new
engineer with no access to the earlier conversation. Everything was checked
against the working tree and command output of the session; items marked
**UNVERIFIED / NOT RUN / UNKNOWN** must not be treated as true.

Note on this file: the handoff request that produced this rewrite was cut off
after "B. Current architecture". Sections below follow the evident intent
(objective, architecture, work, evidence, decisions, risks, next steps,
safety). If more headings were wanted, add them; nothing here is lossy.

Project root (Windows): `C:\Users\Admin\Desktop\prometheus99-main\PROMETHEUSS`
(WSL: `/mnt/c/Users/Admin/Desktop/prometheus99-main/PROMETHEUSS`).
Working Linux copy: `/root/p99` inside WSL distro `Ubuntu-24.04`.

---

## A. Objective and requirements

### A.1 Product
A lightweight industrial HMI runtime in **C99**: real **LVGL v9.2.2** UI,
**Modbus TCP via libmodbus**, typed tags with per-tag quality, ISA-18.2-style
alarms, audited operator writes, strict validated configuration; intended for
small Linux hardware (Raspberry Pi OS Lite class, framebuffer + evdev touch).
It reads/controls a PLC; it is **not** a safety function and not a full SCADA.

Origin: a hackathon prototype ("Team Doomsday": Abhilash L, Adarsh P Abhilash,
Nikhil Nuguri) that monitored the *host PC's* sensors on Windows with a custom
renderer. The mentor-derived requirements (from the user's design document
`C:\Users\Admin\Downloads\Prometheus99 Industrial HMI Runtime Architecture and
Engineering Documentation.md`, "design baseline v0.2", 2026-10-15):

| Requirement (source: design doc) | Status |
|---|---|
| Open protocol (Modbus TCP; RTU/OPC UA later), open-source library | Modbus TCP via libmodbus implemented; RTU and OPC UA NOT implemented |
| Develop on PC (Linux/Windows), validate on Raspberry Pi/ARM | PC (Windows + WSL2 Linux) only; Pi NOT done |
| Fast lightweight rendering, LVGL, no Qt | LVGL v9.2.2 used; Qt baseline comparison NOT done |
| Measure startup, render/transition, touch latency, CPU, memory, stability, recovery | host-only partial (see section E); Pi and touch latency NOT measured |
| Basic auth, secure config, input validation, timeouts, auto-reconnect | validation/timeouts/reconnect implemented; **authentication/lockout NOT implemented**; config integrity (HMAC) NOT implemented |
| Low memory, no heap in steady state | measured 0 app/LVGL/libmodbus heap calls in a 2 s steady window on host; process-wide claim NOT made |

### A.2 Explicit vs aspirational
Explicit (implemented + tested on host): typed tags, quality, link health,
write safety rules, alarm FSM, strict config, Modbus TCP, LVGL UI.
Design-doc items NOT implemented (aspirational): storage worker thread
(journal/audit to flash with fsync is synchronous in the calling thread),
systemd unit/sd_notify/watchdog, users/roles with credentials, trend chart,
multiple screens, config reload, HMAC config, OPC UA spike, RTU, TLS, SBOM,
Qt baseline, 72 h soak, CI. Performance goals in the design doc are targets,
not results.

### A.3 Platforms
- Windows 11 + MSYS2 UCRT64 GCC 13.2 (development host, legacy demo, host tests).
- Linux: Ubuntu 24.04 WSL2 + gcc 13.3 (executed this session, no display).
- Raspberry Pi / ARM / real framebuffer / touch: **never touched**.

---

## B. Current architecture

### B.1 Two products in one tree (do not confuse)
| | Legacy Windows prototype | Portable HMI runtime (the product) |
|---|---|---|
| Files | `src/layer0..3`, `src/main.c`, `include/` | `src/hmi/`, `src/ui/`, `src/app/` |
| Data | this PC's CPU/RAM/SSD/ASUS EC | Modbus PLC tags from JSON config |
| Renderer | custom 4-bit renderer behind an LVGL-shaped shim (`include/lvgl.h`, NOT real LVGL) | real LVGL v9.2.2 |
| Failover | a flag only (no second node) | n/a |

The legacy prototype is frozen as a demo; its sensor code is not ported.

### B.2 Portable runtime modules (`src/hmi`)
| File | Responsibility |
|---|---|
| `hmi_types.h` | enums (quality, reason, dtype, word/byte order, role, **`hmi_link_health_t`**), limits (64 tags, 32 groups, queue 8, audit ring 32, journal 64) |
| `hmi_platform.h` + `hmi_platform_win32.c` / `hmi_platform_posix.c` | ONLY OS surface: monotonic clock, sleep, mutex, thread start/join(timeout), atomic flag, atomic file write, fsynced append |
| `hmi_decode.*` | register <-> raw/number, 4 byte/word orders, range-checked; raw 32-bit pattern is the source of truth |
| `hmi_tags.*` | tag table: raw, eng value, quality+reason, monotonic ts, per-tag and global versions, poll-group merge; staleness aging |
| `hmi_driver.h` | PLC driver interface (connect/read/write/close; status OK/TIMEOUT/TRANSPORT/EXCEPTION/PROTOCOL) |
| `hmi_comms.*` | connection FSM (DISCONNECTED/CONNECTING/CONNECTED/DEGRADED/BACKOFF), scheduler (phase-aligned), backoff+jitter, link health owner, executes write jobs |
| `hmi_writes.*` | write lifecycle: validate (enabled/tag/role/range/representable/link), bounded FIFO, audit-before-send (fail closed), expiry, read-back verdict. No transport knowledge |
| `hmi_alarm.*` | HI/HIHI/LO/LOLO, deadband, on/off debounce, ack, escalation re-activation, RTN-unack, comms alarm, journal, CRC persistence |
| `hmi_config.*` | strict JSON (jsmn): rejects whole file on any error; atomic save |
| `hmi_runtime.*` | wires db+comms+writes+alarms, comms thread, supervisor thread, file audit sink |
| `plc_libmodbus.*` | libmodbus driver (only file including `<modbus.h>`) |
| `src/ui/hmi_ui.*` | LVGL binder (50 ms timer), cached labels/colours, setpoint panel, ACK ALL; test seam `hmi_ui_row_text`, `hmi_ui_link_text` |
| `src/app/main_linux.c` | Linux entry: args `--config --audit --alarms --fb --touch --role`; invalid config => banner, no PLC, writes off |

### B.3 Threads and locking
UI thread (LVGL only) · comms thread (`hmi_comms_step`) · supervisor thread
(every 50 ms: `hmi_tagdb_age`, `hmi_alarm_step(link_health)`). Separate
mutexes for tagdb, comms, writes, alarms; **never nested, never held across
driver/file/LVGL calls**. Alarms run on the supervisor thread so they stay
responsive while the comms thread blocks in a Modbus call.

### B.4 Key behavioural rules (decisions)
- **Link health** (`hmi_comms_link_health`) is the single authority:
  DOWN (not CONNECTED/DEGRADED) / NO_RESPONSE (age of last valid reply >
  max(6 x response_timeout, 1000 ms)) / DEGRADED (> 2 x response_timeout or FSM
  DEGRADED) / HEALTHY. Strict `>`. Unsigned wrap-safe; backwards clock fails
  safe. Runtime, alarm engine, UI and write gate all use it. A TCP connection
  that answers nothing is NO_RESPONSE (this was a real bug found by testing).
- **Quality**: UNAVAILABLE (never received) / BAD(reason) / UNCERTAIN /
  GOOD. A successful poll restores only its own group's tags. Modbus
  exceptions are data failures, not link failures. Dead-link detection =
  `miss_limit x response_timeout`, independent of group count.
- **Writes**: transmitted at most once; lost reply => UNKNOWN (final, never
  replayed); read-back decides CONFIRMED vs VERIFY_FAILED; audit intent must be
  persisted before send; without an audit path writes are forced off; queued
  (never sent) writes expire on link loss; refused while link DOWN/NO_RESPONSE.
- **Config**: IPv4 literal only (no DNS), unknown/duplicate keys rejected,
  writable tags need role+min+max within engineering range, viewer cannot
  write, input registers not writable, `writes_enabled` defaults false.
- **Alarms** frozen (not cleared) while tag data is not GOOD.
- Roles are a launch parameter, NOT authentication.

Detailed rationale: `docs/ADR-0001-runtime-boundaries.md` (incl. addendum).

### B.5 Dependencies (vendored in `third_party/`)
LVGL v9.2.2 `7f07a129e8d77f4984fff8e623fd5be18ff42e74` (shallow clone, ~200 MB
with docs/tests/.git); libmodbus v3.1.10
`2cbafa3113e276c3697d297f68e88d112b53174d` (Windows/host builds compile
`modbus.c`, `modbus-tcp.c`, `modbus-data.c` with `third_party/libmodbus_cfg`;
Linux links the **system** library 3.1.10, LGPL, dynamic); jsmn v1.1.0.
`config/lv_conf.h`: derived from the v9.2.2 template, `LV_COLOR_DEPTH 16`,
`LV_MEM_SIZE 128 KB`, fbdev+evdev enabled only with `-DP99_LINUX_TARGET`.
Licences: `docs/THIRD_PARTY.md` (engineering record, not legal review).

---

## C. Work completed (actual)

1. **Legacy prototype audit** (pass 1) and regressions fixed (pass 2): mutex
   state, validity flags, alarm hysteresis, cross-thread watchdog, atomic run
   flag, guarded teardown, stack protector, ASLR/DEP. `README`/`walkthrough`
   rewritten honestly (old "0.10 MB RAM" was a working-set trimming artefact;
   private bytes were 5.7-6.7 MB).
2. **Portable runtime** built end to end (modules in B.2) with a scripted mock
   driver, fake clock, in-process fake Modbus server (Win32 and POSIX),
   headless LVGL harness.
3. **Architecture review** (thermo-nuclear + improve-codebase-architecture
   skills) then implemented: #2 single link health; #1 write pipeline
   extracted to `hmi_writes`.
4. **Linux port** prepared and then **executed in WSL2** (section E).
5. Mutation testing of the new suites (ad hoc `sed` mutants; ~22 mutants;
   survivors were fixed by adding tests or removing dead code).
6. Docs: `README.md`, `walkthrough.md`, `docs/ADR-0001-*.md`,
   `docs/THIRD_PARTY.md`, `docs/LINUX_VALIDATION.md` (**stale**, see G),
   `bench/CLAIMS.md`, `config/hmi.example.json`.

Findings made by running things (all fixed): link "connected" over BAD data;
UI redraw storm from re-applied colours (44 -> 0 flushes/2 s idle); config
rejected empty `units`; audit-path-missing correctly refused writes on Linux
(harness now takes the audit path as an argument); watchdog false positive on
window drag (legacy); teardown-before-join (legacy).

---

## D. Repository state

**Not a Git repository** (`git status` -> "not a git repository"). Nothing is
committed. `find -newer` shows no file changed after the previous checkpoint
save. Recovery copies (outside the project, never delete):
- `..\PROMETHEUSS.bak`: the original project before any change.
- `..\PROMETHEUSS.stage0`: `src/tests/Makefile/README/walkthrough` before the
  architecture-refinement pass.

Layout (excluding `third_party/`, `build/`): `Makefile`, `Makefile.linux`,
`build.bat`, `Run_HMI_Runtime.bat`, `README.md`, `walkthrough.md`,
`PROJECT_RESUME.md`, `LICENSE`, `config/{hmi.example.json,lv_conf.h}`,
`docs/{ADR-0001-runtime-boundaries,LINUX_VALIDATION,THIRD_PARTY}.md`,
`include/*.h` (legacy), `src/layer*.c`, `src/main.c`, `src/hmi/*`,
`src/ui/*`, `src/app/main_linux.c`, `tests/{test_core.c,stub_layer0.c,test_hooks.h}`
(legacy), `tests/hmi/{test_{decode,tags,comms,link_health,writes,alarm,config,runtime}.c,
test_support.{c,h},run_core_tests.sh,test_hmi_view.c,config_golden.{c,txt},test_modbus_e2e.c,ui_headless.c,
fake_modbus_server.{c,h},fake_modbus_server_posix.c,alloc_count.c}`,
`bench/{CLAIMS.md,measure_process.ps1,results/*}`.

Generated/stray artefacts: `prometheus99.exe`, `tests/test_core.exe`,
`build/` (objects, `liblvgl.a`, test exes, Windows screenshots, and a leftover
helper `build/ui_seam.py`, safe to delete but not deleted). `third_party/lvgl`
contains its own `.git` (shallow).

Size reference: runtime+ui+app+harness about 5,000 lines;
`hmi_comms.c` 328, `hmi_writes.c` 298, `hmi_alarm.c` 301,
`hmi_config.c` 406 (before #4; 544 after), `hmi_ui.c` 403 (before #3). The old 1,341-line
`test_hmi_core.c` was split into per-module files (section O).

---

## E. Tests and evidence

### E.1 Executed on **Linux** (Ubuntu 24.04 WSL2, gcc 13.3.0, system libmodbus 3.1.10)
Commands run in `/root/p99`:
| Command | Result |
|---|---|
| `make -f Makefile.linux clean && make -f Makefile.linux test` | **521 checks, 0 failed** |
| `make -f Makefile.linux e2e` | **75 checks, 0 failed** (link declared lost at 601 ms for 3 x 200 ms) |
| `make -j12 -f Makefile.linux ui-host` | **22 scenario assertions, 0 failed**; steady state 0 flushes / 0 heap calls per 2 s; handler mean 12 us, p99 121 us (421 samples; WSL2 host, not a Pi) |
| `make -j12 -f Makefile.linux hmi` | **linked**: ELF 64-bit PIE `build-linux/prometheus99`, 606,448 B unstripped, needs `libmodbus.so.5` |

`ui-host` output (Linux):
```
[ui] link='PLC: HEALTHY' TANK_LEVEL value='50.0 %' quality='GOOD'
[ui] after register 500->700: value='70.0 %'
[ui] after register ->910: value='91.0 %' alarm='HI UNACK'
write submit -> 0 id=1
[ui] write state=4 (CONFIRMED=4) fake-PLC register=600 (expected 600)
[ui] PLC hung: link='PLC: NO RESPONSE' value='---' quality='BAD COMM'
[ui] recovered: link='PLC: HEALTHY' value='60.0 %' quality='GOOD'
scenario assertions: 22 checks, 0 failed
```
This proves, on Linux: simulated register -> libmodbus -> tag DB -> LVGL label
text -> alarm; disconnect/stale indication (value `---`, BAD COMM);
reconnection; alarm recovery (comms alarm RTN_UNACK then ack -> NORMAL); a
write reaching the (loopback fake) PLC exactly once and verified.

Re-run in the later session (same host): core 521, e2e 75, ui-host 22, all 0
failed; ui-host handler mean 16 us, p50 10, p95 22, p99 152, max 924 us over
421 samples (WSL2 host, not Raspberry Pi). The earlier 12 us / 121 us figures
above are from the previous run. Raw files: `bench/results/linux/`.

**NOT executed on Linux**: any run on a framebuffer/evdev device (none exists
in this WSL2). The executable was launched only without a framebuffer, see
section K. Linux screenshots were copied to `bench/results/linux/` as PPM and
PNG and viewed; they are in-memory renderings, not a displayed screen.

### E.2 Executed on **Windows** (MSYS2 UCRT64 GCC 13.2, `mingw32-make`)
| Target | Result | Last run |
|---|---|---|
| `test` (legacy layers 1-2) | 48 checks, 0 failed | before last UI/harness edits |
| `test-hmi` | 521 checks, 0 failed | before last UI/harness edits |
| `test-e2e` (libmodbus + Win32 fake PLC) | 75 checks, 0 failed | before last UI/harness edits |
| `ui-host` | **22 assertions, 0 failed** | after last edits |
| `check-linux` (`-fsyntax-only`) | no errors | before last edits |
| `all` (legacy exe) | built, 66,048 B | before last edits |

**Gap closed**: `mingw32-make clean all test-all ui-host check-linux` was
re-run in the later session, before and again after the `main_linux.c` and
test fixes: exit 0, legacy 48, core 521, e2e 75 checks and 22 UI assertions,
all 0 failed, `check-linux` printed no errors.

### E.3 Measurements (host only; raw files in `bench/results/`, summary in `bench/CLAIMS.md`)
Legacy prototype 20 s runs (Windows, CSV in `bench/results/legacy_*.csv`);
headless LVGL harness runs (3 Windows runs in `ui_headless_host_run*.txt`).
The Linux/WSL2 numbers above are only in this file: add them to CLAIMS with
the label "WSL2 host, not Raspberry Pi". **No Pi measurement exists.**

### E.4 Mutation checks
Ad hoc, not a committed tool. Pattern used: copy `src/hmi`+`tests/hmi`+`jsmn`
to `/tmp/mut`, `sed` a single behaviour break, compile the suite, expect
failures. Stage-1 health: 5 mutants; stage-2 writes: 7; earlier ~10. All
caught after test additions; one redundant guard removed by this evidence.

### E.5 Not tested at all
Real PLC, real display/touch/fbdev/evdev, Raspberry Pi, long-run soak,
fuzzing, `tc netem`, power-cut during write. Sanitizers were run on Linux
(section K) for the core and e2e suites only; the ui-host harness and the
`prometheus99` executable were not run under a sanitizer.

---

## F. Known limitations and risks (UNVERIFIED unless stated)

- Real PLC register map, **byte/word order and 0/1-based addressing** are
  assumptions; only a real device confirms them. The fake PLC shares the
  author's assumptions.
- `LV_COLOR_DEPTH 16` vs a 32 bpp framebuffer: conversion path UNVERIFIED.
- fbdev/evdev code compiled and linked, never executed.
- `hmi_platform_posix.c` thread-start uses a static 8-slot table (works in
  tests; could be simplified by embedding fn/arg in the handle).
- Production gaps: authentication/lockout, config HMAC, systemd, trend UI,
  multi-screen, config reload, storage worker, SBOM/licence review, soak.
- `lv_conf.h` untrimmed (stripped Windows harness binary 1.55 MB).
- A framebuffer path that opens but is not a framebuffer (for example
  `/dev/null`) is not detected: the process starts and LVGL prints ioctl
  errors. Only a path that cannot be opened exits (status 2).
- Audit file is append-only text, no rotation, written synchronously.
- `HMI_AUDIT_LEN >= queue+2` makes "audit ring exhausted" unreachable through
  the public API (defensive; test pokes internals).
- This is not a safety function; must not be used for safety-critical control.

### Remaining architecture recommendations (from the reviews)
Done: link-health ownership; write-lifecycle extraction; (3) pure display
view-model split from the LVGL adapter (section L).
(4) table-driven config: DONE (section M; note the file grew, see there).
Previously open, now done: (4) table-driven config schema (~150 lines removable; many `memset(out,0)`
exits); (5) read-only tag-table accessors (scheduler mutates `groups[]`
through the public struct); (6) decide the legacy prototype's fate
(`layer3_presentation.c` is 1,014 lines); smaller: dead code in `hmi_ui.c`
(`s_mode`, `t_mode`), always-true condition and unused `cur` in
`hmi_alarm_step`, POSIX thread trampoline, split `test_hmi_core.c`, duplicate
fixture code in the harnesses.

---

## G. Immediate next steps (ordered)

Items 1 to 7 of the previous list (WSL check, Windows regression, launching
the Linux executable on loopback, screenshots, sanitizers, doc updates, Linux
re-run) are DONE; results are in section K.

1. View-model split (#3, section L), table-driven config (#4, section M) and
   read-only tag accessors (#5, section N)
   and the test split (section O) are DONE. Next, only on the user's go-ahead:
   the legacy prototype's fate
   (#6).
2. Probe a framebuffer with `FBIOGET_VSCREENINFO` in `main_linux.c` so a
   device that opens but is not a framebuffer also fails fast.
3. Run `ui_headless` and the `prometheus99` executable under a sanitizer (the
   ui-host build uses `-Wl,--wrap=malloc`, which needs separate handling).
4. Then, only if desired: Pi bring-up (real `/dev/fb0`, touch calibration,
   32 bpp check), systemd unit, authentication.

Working method on this machine: Linux commands run through
`wsl.exe -d Ubuntu-24.04 -u root --cd / -e bash <script on /mnt/c>`; put
multi-line commands in a script file instead of inline quoting (nested quotes
broke twice). WSL clears `/tmp` when the distro stops, so keep sanitizer
binaries in `/root`. TSan needs `setarch $(uname -m) -R` on this kernel.
`mingw32-make clean` deletes `build/`, including any log written there.

---

## H. Safety constraints (keep)

- **No real PLC connection or write.** Only the in-repo loopback fake PLC.
  Example config ships `writes_enabled:false`.
- Host/WSL results never prove Raspberry Pi or target-hardware behaviour.
- Do not claim Linux display validation (no framebuffer exists) or Pi results.
- No destructive operations: keep `PROMETHEUSS.bak` and `PROMETHEUSS.stage0`;
  no `rm -rf` of the project; no commit/push unless asked (no repo anyway).
- Do not rewrite working modules without evidence of a defect.
- Installing software/system features requires the user's explicit action
  (WSL install needed elevation and a reboot; the user did it).

---

## I. Working notes for the next session

- The user earlier activated the **caveman** style skill (terse answers;
  plain prose for anything persisted: docs, commits, code comments).
  Skills installed in `~/.claude/skills` via `npx skills`: grill-with-docs,
  improve-codebase-architecture, thermo-nuclear-code-quality-review
  (real name; the `-guhans6` suffix did not exist), improve (shadcn), caveman,
  plus the mattpocock pack (grilling, tdd, domain-modeling, ...).
- Tool gotchas: do NOT embed Python with backslash escapes in bash heredocs
  (several edits silently failed); use the Write/Edit tools. Windows bash uses
  `C:/Users/...` paths; use `mingw32-make` (no `make`). `wsl.exe` output is
  UTF-16 (NULs); `wsl` fails with chdir error if the cwd is unreachable.
  MinGW has no sanitizers. Windows `Makefile` static pattern rule is required
  for `modbus.o`. `check-linux` prints nothing on success.
- Process that worked: one logical change, rebuild, run the relevant suite,
  then mutation-check the new tests; record real outputs; mark gaps.
- Backups exist; the Windows tree is authoritative; `/root/p99` is a copy
  (re-`rsync` before Linux runs; exclude `build*`, `*.exe`, `.git`).

---

## J. Stopping point

At the time of this handoff: nothing running (no make/test/exe processes on
Windows or WSL; WSL distro was Stopped = idle, files persist). The last
completed operation was the Linux `ui-host` run (22/22 assertions) and the
checkpoint update; no edit was in progress and no command was interrupted.

The last completed operation of the later session was the doc update after the
`main_linux.c` and test fixes. Nothing was running and no edit was in
progress. Nothing is committed (no repository). `PROMETHEUSS.bak` and
`PROMETHEUSS.stage0` are untouched.

**First action on resume**: read section K, then ask the user whether to start
section G item 1.

---

## K. Linux execution and sanitizer session (2026-10-15)

Environment: Windows 11 host, WSL2 `Ubuntu-24.04` (VERSION 2), kernel
6.18.54.1-microsoft-standard-WSL2 x86_64, gcc 13.3.0, libmodbus 3.1.10. All
Linux numbers are "WSL2 host, not Raspberry Pi". No framebuffer, no real PLC.

What was verified, with real output:

1. No file was newer than this document at the start of the session. The
   Windows tree was synced to `/root/p99` with rsync.
2. Windows regression: `mingw32-make clean all test-all ui-host check-linux`
   exit 0; 48, 521 and 75 checks and 22 UI assertions, 0 failed.
3. Linux suites: core 521, e2e 75 (link declared lost after 601 ms), ui-host
   22 assertions, `make -f Makefile.linux hmi` linked; all 0 failed.
4. Launch test: `prometheus99` was started with a loopback copy of the example
   config (host 127.0.0.1, port 5502, nothing listening, copy at
   `/root/p99_loopback.json`, not in the project). Before the fix it printed
   `Error: cannot open framebuffer device`, stayed alive headless, and exited
   0 on SIGTERM.
5. Screenshots: five PPMs copied to `bench/results/linux/`, converted to PNG
   and viewed (normal, alarm HI UNACK, acked after write, comms loss with
   `---` and BAD COMM, recovered). They are LVGL renderings into memory; no
   LVGL screen was shown on any display on Linux.
6. ASan+UBSan on core and e2e: 521 and 75 checks, 0 failed, 0 reports.
7. TSan: e2e 0 reports. Core first showed 3 data races, all in
   `tests/hmi/test_hmi_core.c` (`test_concurrent_consumers`: unsynchronised
   mock register write from the main thread, and non-atomic `g_reads` /
   `g_bad_reads` counters). None in `src/hmi`.

Changes made (one at a time, each verified):

- `src/app/main_linux.c`: probes the framebuffer path with `open()` before
  starting anything; on failure prints a message and exits 2. Header comment
  refreshed. Verified: exit status 2 and no alarm file created.
- `tests/hmi/test_hmi_core.c`: relaxed `__atomic` load in the mock read, atomic
  store of the register in the test, `__atomic_fetch_add` on the counters.
  Verified: TSan core 0 reports in 3 runs, 521 checks, 0 failed; Linux and
  Windows suites still pass.
- Docs updated with real results only: `docs/LINUX_VALIDATION.md`,
  `README.md`, `bench/CLAIMS.md` (new section 2b), `walkthrough.md` (pass 4),
  `Makefile.linux` header. Raw outputs are in `bench/results/linux/`.

Still not validated: Raspberry Pi or any ARM device, a real framebuffer,
evdev touch, 16 versus 32 bpp conversion, real PLC interoperability, soak.

---

## L. View-model split (architecture recommendation #3), 2026-10-15

Done in two verified steps.

1. Added `src/ui/hmi_view.{h,c}` (pure functions from runtime state to text
   plus a semantic tone, no LVGL, no globals) and `tests/hmi/test_hmi_view.c`
   with new targets `test-view` in `Makefile` and `Makefile.linux`
   (`test-all` includes it). First run on its own: 79 checks, 0 failed.
2. Rewired `src/ui/hmi_ui.c` to call it. The binder maps a tone to a colour and
   sets label text; it lost about 100 lines, and the dead `s_mode` / `t_mode`
   were removed. `hmi_ui_write_state_text` and `hmi_ui_submit_text` (no
   callers) moved to `hmi_view_write_state_text` / `hmi_view_submit_text`.
   `hmi_view.c` was added to the `ui_headless` and `prometheus99` link lines.

Evidence after step 2: Windows `mingw32-make clean all test-all ui-host
check-linux` exit 0 (48 / 521 / 79 / 75 checks, 22 UI assertions, 0 failed,
no warnings). Linux WSL2: `test-view` 79, `test` 521, `e2e` 75, `ui-host` 22,
`hmi` links, 0 failed. Linux screenshots 1, 2, 3 and 5 are byte-identical to
the pre-refactor ones. Screenshot 4 differs only in the header link text
(`DOWN` versus `NO RESPONSE`); ten repeat runs of the new build gave both
values, so that sample point is timing dependent. This was not confirmed
against a pre-refactor binary because none was kept: keep a copy of the
binary before the next refactor.

Not done: `hmi_ui.c` still owns row caching, the setpoint state and event
handlers (they are LVGL-coupled by design). The binder itself has no unit
test beyond the existing ui-host scenario.

---

## M. Table-driven config schema (architecture recommendation #4), 2026-10-15

Backups and baselines made first, outside the project (never delete):
`..\PROMETHEUSS.pre_config` holds `src`, `tests`, `config`, both Makefiles,
docs, `third_party/jsmn` and `bin/` with the pre-change Windows test binaries
(`test_hmi.exe`, `test_view.exe`, `test_e2e.exe`, `ui_headless.exe`, and
`config_golden_old.exe` built from the old parser).

Steps, each verified before the next:

1. **Characterization corpus** `tests/hmi/config_golden.c` with expected output
   `tests/hmi/config_golden.txt` generated by the OLD parser: 184 cases (43
   accepted, 141 rejected), covering every error class, boundaries, duplicate
   and unknown keys, type errors, byte and token limits. It prints the verdict,
   the exact error text and every parsed field. New targets
   `test-config-golden` in `Makefile` and `Makefile.linux`; `test-all`
   includes it. Windows and Linux outputs are byte-identical (the driver
   switches stdout to binary mode on Windows; an earlier CRLF golden file was
   a test-harness artefact, not a parser difference).
2. **Rewrite** of `src/hmi/hmi_config.c`: field tables for top level, plc,
   tag, alarm and write, one engine `parse_object`, small `finish` code for
   cross-field rules. Output byte-identical to the golden file on Windows and
   Linux and under ASan+UBSan (0 reports). The file is LONGER (406 -> 544
   lines): the earlier estimate of about 150 removable lines was wrong. What
   improved is that the rules exist once and a new key is a table row.
3. **Behaviour fix** found by the corpus: a repeated `writes_enabled` was
   accepted with the last value winning (`false` then `true` enabled writes).
   It is now rejected as `duplicate key: writes_enabled`. Exactly one golden
   case changed; two new checks in `tests/hmi/test_hmi_core.c` fail on the old
   parser (4 failed) and pass on the new one.

Evidence at the end: Windows `mingw32-make clean all test-all ui-host
check-linux` exit 0, no warnings: legacy 48, core 525, view 79, e2e 75,
config golden identical, 22 UI assertions. Linux WSL2 (not Raspberry Pi):
golden identical, view 79, core 525, e2e 75, ui-host 22, `hmi` links without
warnings.

Not done / caveats: TSan and the ui-host harness were not re-run on the new
parser (ASan+UBSan only, over the golden corpus). `hmi_config_save_text` and
file loading are unchanged and covered by the existing core tests.

---

## N. Read-only tag-table accessors (architecture recommendation #5), 2026-10-15

Snapshot first, outside the project (never delete): `..\PROMETHEUSS.pre_tagaccess`
(`src`, `tests`, `config`, both Makefiles, docs, `third_party/jsmn`, and `bin/`
with the pre-change Windows binaries).

Steps, each verified before the next:

1. Added to `hmi_tags.{h,c}`: `hmi_tagdb_is_ready`, `hmi_tagdb_count`,
   `hmi_tagdb_cfg` (NULL when out of range), `hmi_tagdb_group_count`,
   `hmi_tagdb_group` (const view), `hmi_tagdb_group_set_due`,
   `hmi_tagdb_groups_set_all_due`. 10 new checks: core 525 -> 535.
2. `hmi_comms.c` migrated. It was the only code that wrote to the table through
   the public struct (`groups[].next_due_ms`). The phase-aligned reschedule
   arithmetic is unchanged. Core and e2e identical.
3. `hmi_alarm.c`, `hmi_writes.c`, `hmi_ui.c`, `ui_headless.c` and
   `test_modbus_e2e.c` migrated. `hmi_view_journal_text` takes tag names instead
   of the config array (`test_hmi_view.c` updated).
4. Guard `check-tagdb-access` in `Makefile` and `Makefile.linux` (part of
   `test-all`): fails when a file other than `hmi_tags.c` and the white-box
   `test_hmi_core.c` touches `db.cfg/val/groups/order/n_tags/n_groups/
   global_version/lock/ready`. Passes now, fails on the pre-change snapshot.

Evidence: Windows `mingw32-make clean all test-all ui-host check-linux` exit 0,
no warnings: legacy 48, core 535, view 79, e2e 75, config golden identical,
22 UI assertions. Linux WSL2 (not Raspberry Pi): guard ok, golden identical,
view 79, core 535, e2e 75, ui-host 22 assertions, `hmi` links with 0 warnings;
ASan+UBSan and TSan on core 535 and e2e 75: 0 reports. Linux ui-host handler
over 421 samples: mean 13 us, p99 158 us (one run).

Limits: the table is embedded by value in `hmi_runtime_t`, so C cannot make the
members private; enforcement is the grep guard, not the compiler. The core test
still inspects members directly on purpose (white-box). `hmi_tagdb_cfg` is now a
cross-file call in the supervisor-thread alarm loop; no measurable effect was
seen, but alarm-step timing was not benchmarked.

---

## O. Split of `test_hmi_core.c` into per-module tests, 2026-10-15

Snapshot first, outside the project (never delete): `..\PROMETHEUSS.pre_testsplit`
(`src`, `tests`, `config`, both Makefiles, docs, `third_party/jsmn`, the old
`bin/test_hmi.exe` and its baseline output `bin/test_hmi_baseline.txt`). The
old monolith is `PROMETHEUSS.pre_testsplit/tests/hmi/test_hmi_core.c`; it no
longer exists in the project tree.

New layout in `tests/hmi/` (each module file is its own program):

| File | Checks | Covers |
|---|---|---|
| `test_decode.c` | 24 | hmi_decode |
| `test_tags.c` | 19 | poll group building, tag accessors |
| `test_comms.c` | 39 | polling, link loss and recovery, backoff, staleness, no drift |
| `test_link_health.c` | 38 | the single link-health evaluation |
| `test_writes.c` | 252 | write pipeline through comms, and `hmi_writes` directly |
| `test_alarm.c` | 36 | alarm state machine, frozen on bad data, persistence |
| `test_config.c` | 89 | strict parse, rejection, atomic save, shipped example |
| `test_runtime.c` | 38 | lifecycle with real threads, concurrent readers |

Shared code: `test_support.{h,c}` (CHECK/SECTION, fake clock, mock PLC, 4-tag
fixture, `GOOD_CFG`, `parse`). `run_core_tests.sh` runs the modules and prints
one line each plus the total; `make test-hmi` (Windows) and
`make -f Makefile.linux test` use it. Adding a module = add `test_<name>.c`
and its name to `HMI_TESTS` in both Makefiles.

Evidence that nothing was lost: the eight modules sum to exactly 535 checks
(the monolith's count), and the set of SECTION lines is identical to the
monolith's output (29 lines, diffed). Windows `clean all test-all ui-host
check-linux` exit 0, 0 warnings, guard ok, golden identical, view 79, e2e 75,
22 UI assertions. Linux WSL2 (not Raspberry Pi): the same, `hmi` links with 0
warnings; every module run under ASan+UBSan and under TSan: 535 checks total,
0 reports, plus e2e 75 checks, 0 reports.

Caveats: the test code was moved verbatim except for removing `static` from the
shared fixture functions; the split was not mutation-tested again. The
`static hmi_runtime_t g_rt` and reader globals stay local to `test_runtime.c`.

---

## P. Liveness supervision (production-readiness task 1), 2026-10-15

Chosen after reading the code: nothing detected a stuck process, so a frozen
UI could keep showing the last values and nothing restarted it. Snapshot first,
outside the project (never delete): `..\PROMETHEUSS.pre_liveness`.

Changes, each verified before the next:

1. `hmi_runtime`: per-thread heartbeat counters (`beat_comms`, `beat_super`),
   `hmi_runtime_beats()`, `hmi_runtime_worst_stall_ms()` (connect + 2 x
   response timeout + 2 s). Increments use unsigned arithmetic (a signed
   `+ 1` would be undefined after 2^31 iterations).
2. `src/hmi/hmi_watchdog.{h,c}`: pure decision logic plus parsing of
   `WATCHDOG_USEC` / `WATCHDOG_PID`; `tests/hmi/test_watchdog.c`, 29 checks.
   `hmi_notify()` in `hmi_platform_*.c` sends sd_notify datagrams to
   `NOTIFY_SOCKET` (POSIX only; Windows returns false).
3. `src/app/main_linux.c`: READY=1 after the first frame, WATCHDOG=1 from the
   UI loop only when comms and supervisor counters advance, STOPPING=1 on
   shutdown. `hmi_ui_enable_heartbeat()` shows `HB nn` in the header (off by
   default; the headless UI test asserts 0 idle flushes with it off and 2 to 5
   flushes / under 8,000 pixels in 3 s with it on). View-model function
   `hmi_view_heartbeat_text`.
4. `hmi_thread_join` (POSIX): monotonic deadline via a 1 ms `pthread_tryjoin_np`
   poll. `pthread_clockjoin_np` was tried first and rejected: this libtsan does
   not intercept it and reported 29 false races.
5. `deploy/prometheus99.service`: `Type=notify`, `WatchdogSec=10`,
   `Restart=on-failure`, start limit in `[Unit]`, section 10 hardening.

New targets (`Makefile.linux`, in `test-all`): `test-notify`, `liveness`.
New files: `tests/hmi/{test_watchdog.c,test_notify_posix.c,notify_listener.c,
hang_server.c,liveness_e2e.sh}`, `deploy/prometheus99.service`.

Evidence (WSL2 host, not Raspberry Pi): `make -f Makefile.linux test-all`
exit 0, 0 warnings; core modules 570 (watchdog 29, runtime 44), notify 13,
view 84, e2e 75, golden identical, 28 UI assertions. `liveness` 8 of 8 on the
real executable: READY once, cadence, none while SIGSTOPped, resume after
SIGCONT, none after SIGKILL, pings continue against a PLC that never answers,
clean SIGTERM exit, STOPPING sent. Real systemd, transient unit with
WatchdogSec=4: SIGSTOP produced "Watchdog timeout", SIGABRT, restart with a new
PID, NRestarts=1 (repeated). ASan+UBSan and TSan over all modules, notify and
e2e: 0 reports. Windows `clean all test-all ui-host check-linux`: exit 0, 0
warnings, legacy 48, core 570, view 84, e2e 75, 28 UI assertions.

Known limits: not run on a Raspberry Pi; the hardware watchdog and an
installation of the unit under the `prometheus` user with `ProtectSystem=strict`
are untested; the stall limit (about 5 s with the example config) is a
judgement that should be confirmed against a real PLC's response times; when
the config is rejected there are no worker threads, so only UI-loop liveness
is checked. Runner-ups still open, in order: periodic alarm-state save with a
journal flush, audit rotation and size cap, authentication design, framebuffer
probe in `main_linux.c`.

---

## Q. Authorization hardening (security task 1), 2026-10-15

Selected from a requirements-to-implementation gap assessment (mentor document
section 10: "roles enforced in the core, not only hidden in the UI").
Snapshot first, outside the project (never delete): `..\PROMETHEUSS.pre_authz`.

Defects found by tracing the actual path: ACK ALL had no role check anywhere
(the alarm API took no role), and `role_from_name` mapped any unknown `--role`
string, typos included, to Operator.

Changes:
- `src/hmi/hmi_authz.{h,c}`: single decision point; role names and strict
  parse. Acknowledge = Operator or above; tag write = role >= tag minimum
  (existing rule, now routed through it); corrupt role or action denied.
- `hmi_alarm`: `hmi_alarm_ack(a, point, now, role)` and
  `hmi_alarm_ack_all(a, now, role)` return `HMI_ACK_OK / NOTHING / DENIED /
  AUDIT_FAILED`; `hmi_alarm_set_ack_audit` installs the audit hook. Denied:
  nothing changes, no journal entry. Granted: record first, then effect;
  record failure refuses the grant (fail closed). Alarms that changed in
  between are skipped.
- `hmi_runtime`: `file_ack_audit` appends one line per attempt to the audit
  file (`<ms> ALARM_ACK role= action= result=GRANTED|DENIED n= points=`).
  Installed only when an audit path is configured.
- `src/hmi/hmi_cli.{h,c}`: strict parsing; invalid role, unknown option,
  missing value, repeated option or stray argument exit 2; default role is
  viewer. `main_linux.c` prints the claimed role at start.
- UI: ACK ALL dimmed for Viewer, outcome shown; the core is the enforcement.
- Docs: ADR-0001 addendum, README "Roles and authorization", unit file
  comment, walkthrough pass 10, LINUX_VALIDATION section.

Policy decisions to revisit if requirements change: (1) with an audit path a
grant needs a durable record, without one acknowledgements are unaudited;
(2) a denial is audited best effort; (3) "nothing to acknowledge" is not
audited; (4) timestamps are monotonic ms since boot, no wall clock.

Evidence (host: Windows MSYS2 and WSL2; no Raspberry Pi, no real PLC): core
modules 742 checks (authz 41, alarm 81, runtime 64, cli 66, writes 252
unchanged), view 92, e2e 75, config golden identical, UI scenario 36
assertions, Linux `cli-exec` 15 of 15, `liveness` 8 of 8, `make test-all`
exit 0 with 0 warnings, ASan+UBSan and TSan 0 reports, Windows `clean all
test-all ui-host check-linux` exit 0.

Remaining security gaps, in order of consequence: no authentication, lockout
or KDF (the role is whatever the launcher claims); config file has no
integrity protection (a file edit can enable writes and widen ranges up to
the engineering range); audit file has no integrity, rotation or wall-clock
time and acknowledgement/write records are written synchronously on the
calling thread; Viewer still sees setpoint buttons (core refuses); no write
rate limit beyond the queue bound; systemd hardening untested under the real
user; Modbus TCP is unauthenticated and unencrypted and there is no threat
model document; no fuzzing, no SBOM.

---

## R. Handoff for a new session: audit and roadmap (2026-10-22)

Read `docs/ROADMAP_STATUS.md` first. It holds the full requirements matrix
(55 rows against the mentor's document), the discrepancies, the P0 to P4
gaps, milestones M1 to M9, blockers, outstanding decisions and the definition
of finished. This section is only a pointer and a state summary.

**The audit was read-only. No implementation code was changed, nothing was
committed, and the frozen legacy prototype was not touched.** The only edits
made after the audit are `docs/ROADMAP_STATUS.md` (new) and this section plus
the "Last updated" line.

Current state
- Git: local repository, branch `feature/authorization-hardening`, one commit
  `2ca7c75`. Four empty stray files (`BAD`, `DEGRADED`, `backoff`, `recovery`)
  sat untracked in the project root; they were deleted on 2026-10-22 with the
  owner's permission.
  `third_party/lvgl` and `third_party/libmodbus` are gitlinks without a
  `.gitmodules` file. No remote, no push, no CI.
- Verified results (host only: Windows MSYS2 and WSL2; no Raspberry Pi, no
  real PLC): core modules 742 checks, view-model 92, e2e 75, notify 13, config
  golden 184 cases identical, headless UI 36 assertions, liveness 8 of 8,
  command-line 15 of 15, real-systemd watchdog restart under WSL2, ASan, UBSan
  and TSan 0 reports. Rerun the suites; do not trust this list blindly.
- Requirement tally from the audit: 16 complete and verified (host evidence
  only), 4 implemented but unverified, 18 partial, 14 not implemented, 2
  blocked, 1 optional.
- Not done: authentication (login, KDF, lockout, idle logout), config
  integrity (CRC and HMAC), storage worker and journal persistence, audit
  rotation, trends and extra screens, alarm priority, fuzzing, CI, soak, Qt
  baseline, OPC UA spike, threat model, 62443 mapping, SBOM, everything on a
  Raspberry Pi.
- Known stale text to fix when touching docs: `README.md` line 77 quotes old
  test counts; `docs/ADR-0001-runtime-boundaries.md` still says the POSIX file
  is not yet compiled and quotes 521 checks.

Outstanding decisions (waiting for the project owner)
1. Approval of the roadmap order M1 to M5.
2. D1: crypto library and the HMAC approach. Leaning Monocypher (Argon2id and
   a keyed MAC in one small dependency) after a licence check; the mentor must
   accept a keyed BLAKE2b MAC if the document's "HMAC" is to be met that way.
   Alternatives: libsodium, mbedTLS or OpenSSL (PBKDF2 and HMAC).
3. D2: where the config MAC key lives (a key readable by the service user does
   not stop code running as that user).
4. D3: Modbus coil and discrete-input scope. Leaning out of scope for now.
5. D4: Raspberry Pi availability; M5 should start as soon as a device exists.
6. D5: GitHub and CI setup (not requested so far).

Exact next steps
1. Ask the owner for the decisions above (at least approval, D1, D3, D4).
2. Plan M1 (authentication and sessions) in detail and wait for approval:
   module layout (`hmi_auth`, `hmi_users`), user-store format and file
   permissions, KDF parameters for a small device, first-boot flow, lockout
   policy, login screen with the LVGL keyboard, how `--role` is demoted to a
   test option, tests and acceptance.
3. After approval, implement M1 in small verified steps, then M2, M3, M4; start
   M5 when hardware exists.

Rules to keep
- Complete major requirements first; defer minor polish and optional features.
- Do not connect to a real PLC or issue real control writes.
- Do not move, rename, delete or modify the frozen legacy prototype.
- Do not commit, push or set up GitHub unless the owner asks.
- Keep ADR-0001 and the architecture unless a demonstrated defect requires a
  change; report documentation conflicts instead of choosing silently.
- Tool notes: run Linux commands with `wsl.exe` from PowerShell, not from the
  Bash tool (path mangling); put multi-line commands in script files; WSL
  clears `/tmp` when the distro stops (keep work in `/root`); `mingw32-make
  clean` deletes `build/`; TSan needs `setarch $(uname -m) -R`.

## S. Milestone M1 (authentication and sessions) implemented (2026-10-22)

Read `docs/AUTHENTICATION.md` for the design, parameters, evidence and limits.
This section is the state summary. Section R above describes the situation
before M1 and is partly out of date (it says authentication is missing).

Decisions from the project owner: roadmap M1 to M5 approved; Monocypher (D1);
root-owned key file for the config MAC, relevant from M2 (D2); coils out of
scope for now (D3); Raspberry Pi "soon, date unknown" (D4); GitHub and CI wanted
(D5) but not started because no repository details were given. Argon2id with 3
passes, 8 MiB and 1 lane, on-screen first-boot administrator creation, a
compile-time gate for `--role`, and a deferred user-management screen (M4) were
approved.

What exists now (host evidence only, Windows MSYS2 and WSL2):
- `third_party/monocypher` (4.0.2, checksums in `docs/THIRD_PARTY.md`),
  `src/hmi/hmi_kdf.*`, `hmi_users.*`, `hmi_auth.*`, platform additions
  (`hmi_fs_read_all`, `hmi_fs_mode`, `hmi_random_bytes`), login screen and header
  role label in `src/ui/hmi_ui.c`, wording in `src/ui/hmi_view.c`,
  `src/app/p99_user.c`, a `--users` option, the production build refusing
  `--role`, and a simulation build (`hmi-sim`, `-DHMI_TEST_ROLE=1`).
- New tests: `test_kdf`, `test_platform`, `test_users`, `test_auth`, additions to
  `test_cli`, `test_hmi_view`, `ui_headless`, and the scripts `auth_exec_test.sh`
  and `cli_exec_test.sh` (rewritten for the two builds).
- Results: core 1148 checks on Windows and 1168 on WSL2, view-model 163, e2e 75,
  config corpus identical, UI scenario 77 assertions, liveness, `cli-exec` and
  `auth-exec` pass on WSL2, ASan with UBSan and TSan 0 reports on every core
  module. The legacy suite still passes (48 checks) and no legacy file changed.
  Rerun these; do not trust this list blindly.
- ADR-0001 has an addendum for M1 and its stale text was corrected.
  `docs/LINUX_VALIDATION.md`, `docs/ROADMAP_STATUS.md` and the README were updated.

Not verified: hash time on a Raspberry Pi (the host measures 13 to 41 ms, the
target is unknown), typing on a real touchscreen, a deployed service under its
own user, a power-cut test of the store write, sanitizers on the LVGL harness and
the full executable.

Known limits (documented in AUTHENTICATION.md): the user file CRC does not stop a
writer who is the service user; deleting the file returns the device to first
boot; deleting the counter file resets lockouts; the offline tool changes take
effect after a restart; acknowledgement and write audit lines still carry the
role but not the user name.

Working tree at the time of writing: M1 uncommitted. It was committed later as
`fe165c2`. The four empty stray files were deleted on 2026-10-22.

Next steps: GitHub and CI setup when the owner gives repository details; M2
(configuration integrity and safe startup), then M3, M4 and M5 as in
`docs/ROADMAP_STATUS.md`. Tool notes from section R still apply. In addition: this
session's shell tool altered backslash escapes inside heredocs, so write files
with the file tools and keep patch scripts in files.

## T. Milestone M2 (configuration integrity and safe startup) implemented (2026-10-22)

Read `docs/CONFIG_INTEGRITY.md` for the design, the ownership table, limits and
evidence. This section is the state summary. Section S above describes M1 and
says acknowledgement and write lines lack the user name; since M2 they carry it.

Owner decisions for M2: HMAC-SHA-512 from Monocypher's optional files; the
configuration, its signature and the key owned by root and only read by the
service. Reported consequence: the HMI cannot save configuration itself, so
"configuration save restricted to Admin" moves to M4 and needs a privileged helper.

What exists now (host evidence only, Windows MSYS2 and WSL2):
- `third_party/monocypher/monocypher-ed25519.*` (SHA-512 and HMAC, checksums in
  `docs/THIRD_PARTY.md`), `src/hmi/hmi_mac.*`, `hmi_cfgsec.*`, `plc.allowed_hosts`
  in `hmi_config.c`, `src/app/p99_sign.c`, signed-configuration start-up in
  `main_linux.c` with the options `--config-key`, `--config-state` and (test build
  only) `--unsigned-config`, a platform call `hmi_fs_writable_by_me`.
- A configuration is used only if its detached signature verifies; nothing is
  parsed before the MAC check; any failure is safe mode (banner, no PLC
  connection, writes off); rollback is refused by a serial; every start-up
  appends `CONFIG_ACCEPTED serial=N` or `CONFIG_REJECTED reason=...` to the audit file.
- Audit identity: `hmi_alarm_ack_as`, `hmi_alarm_ack_all_as` and
  `hmi_writes_submit_as`; acknowledgement and write lines end with `user=<name>`.
- `deploy/prometheus99.service` has the root-owned install steps. The shipped
  `config/hmi.example.json` has an `allowed_hosts` entry.
- New tests: `test_mac`, `test_cfgsec`, additions to `test_config`, `test_cli`,
  `test_writes`, `test_alarm`, `test_runtime`, `test_users`, `ui_headless`, and
  `config_exec_test.sh`. The other executable tests now run against a signed
  configuration (`sign_fixture.sh`).
- Results: core 1492 checks on Windows and 1532 on WSL2, view-model 163, e2e 75,
  config corpus identical, UI scenario 84 assertions, liveness, `cli-exec`,
  `auth-exec` and `config-exec` pass on WSL2, ASan with UBSan and TSan 0 reports
  on every core module, mutation checks of the new rules all caught. The legacy
  suite still passes (48 checks) and no legacy file changed. Rerun these.
- ADR-0001 has an addendum for M2. README, `docs/LINUX_VALIDATION.md`,
  `docs/ROADMAP_STATUS.md` (rows 32 and 33 complete on the host; row 26 still
  partial) and `docs/AUTHENTICATION.md` were updated.

Not verified: a run under real systemd as the `prometheus` user with root-owned
files, a power cut during the serial state write, timing on a Raspberry Pi,
sanitizers on the LVGL harness and the full executable.

Known limits: anyone who holds the key or is root can sign; the serial state file
can be deleted by the service user; a signed configuration is trusted as written.

Working tree: M1 (`fe165c2`) and M2 (`e858465`) are committed and pushed; the tree
is clean. The four empty stray files were deleted with the owner's permission.

Next: M3 (persistence and storage), M4 (screens, including the on-device
configuration helper), M5 (Raspberry Pi) as in `docs/ROADMAP_STATUS.md`.
GitHub and CI setup still waits for repository details.

## U. Milestone M3 (persistence and storage) implemented (2026-10-22)

Read `docs/STORAGE.md` for the design, formats, limits, evidence and the list of
what is not verified. This section is the state summary. Sections S and T above
say that acknowledgement and write lines have no old value or wall-clock time,
and that the audit file is unbounded; since M3 that is no longer true.

Owner decisions for M3 (the plan was approved with the recommended option for
each): (1) writes and acknowledgements are two-phase, the call returns before the
record is durable; (2) the write rate limits are optional fields of the signed
configuration; (3) the PLC timeouts are bounded (2000 and 3000 ms) so the longest
stall fits `WatchdogSec=10` and `TimeoutStopSec=15`; (4) the audit file and the
journal rotate at 1 MiB with four older files, a failed rotation refuses the
record; (5) the trace ring is deferred. The Raspberry Pi is not available yet.

What exists now (host evidence only, Windows MSYS2 and WSL2):
- `src/hmi/hmi_store.*`: the storage worker (bounded queue of 64, outcome table,
  fault flag, rotation, replace coalescing, torn-line repair). Platform additions:
  `hmi_event_*`, `hmi_fs_size`, `hmi_fs_rename`, `hmi_fs_read_tail`, `hmi_wall_ms`.
- `src/hmi/hmi_auditfmt.*`: line formats (write, acknowledgement, boot, journal with
  a CRC) and the wall-clock stamp `wall=... sync=ok|no`.
- `hmi_writes`: asynchronous RESERVED record (`hmi_writes_set_audit_async`,
  `hmi_writes_audit_done`), old value and its quality in the record, rate limit
  (`hmi_writes_set_limits`), second check before sending (`REJECTED_REVALIDATION`).
- `hmi_alarm`: two-phase acknowledgement (`HMI_ACK_PENDING`,
  `hmi_alarm_ack_audit_done`, `hmi_alarm_last_ack`), the journal sink called outside
  the lock, state as bytes (`hmi_alarm_state_serialize` and `_apply`), journal restore.
- `hmi_runtime`: owns the store; `hmi_runtime_start_ex` takes a storage struct;
  journal on disk and reloaded at start; state saver (10 s interval, 120 saves an
  hour, retry, always at shutdown); `hmi_runtime_stop` returns whether everything
  stopped; stall bound `max(connect, 2 x response + 2000) + 2000`.
- UI: "ACK PENDING: recording" then the result, "RECORDING (not sent yet)" for a
  reserved write, a "STORAGE FAULT: records not saved" banner, "too many writes, wait".
- `main_linux.c`: login records go through the worker once the runtime is up;
  configuration records are stamped.
- Tests: `test_store`, `test_auditfmt`, additions to the writes, alarm, runtime,
  config, platform, view and UI tests; `killloop_child.c`, `killloop_verify.c`,
  `killloop_test.sh`, `killloop_selftest.sh` (`make -f Makefile.linux killloop`).
- Results (rerun, do not trust): core 2597 on Windows and 2635 on WSL2, view-model
  168, end to end 75, UI scenario 89, kill-loop 150 rounds, sanitizers 0 reports,
  33 of 34 mutations caught (one equivalent).
- The golden configuration corpus was regenerated: one earlier case changed (case 040,
  timeout bound) and 18 were added. The old file is in git history.

Not verified: a power cut, `fsync` and rename on the storage of the target, the clock
state with a real time source, the trace ring (not built), sanitizers on the LVGL
harness and the full executable. Known limits are in `docs/STORAGE.md` section 9: a
login still waits up to 2 s on the UI thread; records still queued when the process
dies are lost; each record is flushed on its own.

Defect found on the way: a torn last line would have swallowed the next boot's first
record; fixed in the store (a line break is written first) with tests.

Working tree: M3 is committed as `defa45f` and pushed (see START HERE). Documents updated: README,
`docs/ROADMAP_STATUS.md` (rows 2, 22, 26, 27, 28, 40, 41, 42, 43), ADR-0001 addendum,
`docs/LINUX_VALIDATION.md`, `docs/AUTHENTICATION.md`, `docs/CONFIG_INTEGRITY.md`,
`deploy/prometheus99.service` (comment only), `docs/STORAGE.md` (new).
`walkthrough.md` and `bench/CLAIMS.md` were not updated and may quote older counts.

Tool notes from this session: Git Bash rewrites path arguments, so start Linux scripts
from PowerShell (`wsl.exe -d Ubuntu-24.04 -- bash /mnt/c/.../script.sh`); `/tmp` in WSL
can be cleared between calls, so keep result files under `/root`; shell heredocs
mangle backslashes, so write scripts and patches with the file tools; repository files
use CRLF line endings and the file tools keep them (a Python rewrite does not unless
`newline=''` is used). Helper scripts used for the runs (sync to `/root/p99`, sanitizer
loop, mutation script) were kept in the session scratchpad and are not in the repository.

Next: M4 (screens, trends, alarm priority, settings and the configuration helper),
then M5 (Raspberry Pi) as in `docs/ROADMAP_STATUS.md`. GitHub and CI setup waits for
the owner's approval of the plan given in the chat.

## V. Milestone M4 (screens, trends, priority, settings, configuration changes) implemented (2026-10-22)

Read `docs/SCREENS.md` for the screens and `docs/CONFIG_INTEGRITY.md` (the section on changing
the configuration on the device) for the helper. This section is the state summary. Sections S
and T say that users are managed only with the offline tool and that the configuration is
changed only offline; since M4 an administrator manages accounts on the screen and an Engineer
sends configuration changes to a root helper.

Owner decisions for M4 (the plan was approved with the recommended option for each): D1 a root
helper started by a systemd path unit, with a narrow whitelist; D2 trend data in RAM only; D3
one alarm priority per tag; D4 a paged overview; D5 the whitelist (alarm limits, deadband,
delays, priority, PLC host from the allow-list, port, unit); D6 account management on the
device for an Administrator.

What exists now (host evidence only):
- UI: `src/ui/ui_nav.c`, `ui_overview.c`, `ui_alarms.c`, `ui_ack.c`, `ui_trends.c`,
  `ui_settings.c`, `ui_users.c`, `ui_config.c`, `ui_login.c`, shared header `ui_priv.h`;
  `hmi_ui.c` keeps the shared state and the test seams.
- `hmi_trend` (RAM only), alarm priority (`alarm.priority`, `hmi_alarm_list`), config fields
  `trend` and `trend_bucket_ms`.
- `hmi_auth` account management (add, password, role, remove) with `by=<admin>` in the lines.
- `hmi_config_write.c` (`hmi_config_serialize`, `hmi_config_equal`), `hmi_cfgedit.*`, `p99_cfg`
  (`src/app/p99_cfg.c`), `deploy/prometheus99-config.path` and `.service`, `--spool DIR`.
- Tests: `test_trend`, `test_cfgedit`, `cfgedit_exec_test.sh` (`make -f Makefile.linux
  cfgedit-exec`, as root), additions to the view-model, cli, auth, alarm, config and UI tests.
- Results (rerun, do not trust): see the top of `docs/ROADMAP_STATUS.md`.

Defects found while finishing: a glibc macro `BYTE_ORDER` collided with an array name (Linux
build only); the writer wrote members that hold their default, which overran the parser's
1024-token budget for 64 tags; the request parser left `base_serial` set on some refusals. The
mutation run first had 4 survivors; 3 tests were added; the last survivor is an internal
read-back assertion that no input can reach.

Not verified: the units under real systemd and the restart of the service by the helper, the two
renames across a power cut, the display and touch (the number pad keys are 96 by 46 pixels), the
helper exiting with status 3 and leaving a request (the path unit would start it again until the
start limit), anything on a Raspberry Pi or with a real PLC. The LVGL pool is 192 KB with about
119 KB in use on the host.

Working tree: nothing of M4 is committed. Documents updated: README, `docs/ROADMAP_STATUS.md`,
`docs/SCREENS.md` (new), `docs/CONFIG_INTEGRITY.md`, `docs/AUTHENTICATION.md`, ADR-0001 addendum,
`docs/LINUX_VALIDATION.md`, `docs/M4_HANDOFF.md`, `bench/CLAIMS.md` (a note only),
`deploy/prometheus99.service`. `walkthrough.md` was not checked and may quote older counts.

Tool notes: Git Bash rewrites path arguments, so start Linux scripts with `MSYS_NO_PATHCONV=1
wsl -d Ubuntu-24.04 -u root bash /mnt/c/.../script.sh` and write the scripts with the file tools;
shell and Python heredocs mangle backslashes and `\n`, so write patch scripts with the Write tool.
The helper scripts used for the WSL2 runs, the sanitizer loop and the mutation script were kept in
the session scratchpad and are not in the repository. A user named `prometheus` was created in
the WSL2 distribution only if it did not exist already, to run `systemd-analyze verify`.
