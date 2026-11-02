# Final master prompt: controlled optimization and improvement of Prometheus99 (phases 0 to 5)

This file replaces the need to read `docs/MASTER_PROMPT.md` first. It merges four sources: the owner's optimization brief (preserve everything, LVGL 9.2.2 unchanged, improve internals), the earlier master prompt (10-day finale, mentor requirements, Tier 1 to 3 features), the improvement roadmap from the comparative audit (`C:\Users\Admin\Desktop\prometheus99-comparative-audit\IMPROVEMENT_ROADMAP.md`), and what was verified in this repository during the audit. Read `README.md` sections 1 to 3, 10 and 11 first.

Status when this file was written: phase 0 had started. A snapshot of the working tree was taken to `C:\Users\Admin\Desktop\prometheus99-baseline-snapshot` and the baseline script was running in WSL (it writes into `docs/baseline/`). No source file had been changed for optimization yet. Nothing is committed.

## 1. Goal

Make the existing Prometheus99 measurably better in CPU, memory, rendering workload, reliability, code quality and maintainability, and finish the mentor's missing requirements, without rebuilding it and without losing any behaviour. The mentor's requirements are: open protocols (Modbus TCP first, RTU next), Linux or Windows development then an embedded board (emulated Raspberry Pi is accepted for now), fast rendering without Qt (LVGL), measured performance and reliability, and basic authentication, secure configuration, input validation, timeouts and automatic reconnection. The finale is in 10 days and judges expect a production-grade product. Honest, measured limits matter more than claims.

## 2. Non-negotiable constraints

1. **Preserve every existing major function.** No feature removed or reduced, no test deleted or weakened to pass, no external behaviour changed without a backward-compatible reason. Existing interfaces and command-line options keep working.
2. **LVGL 9.2.2 stays exactly as it is.** No upgrade, downgrade, replacement, fake layer, or edits to `third_party/lvgl`. The LVGL integration, framebuffer and input drivers stay. Only the application's use of LVGL may be optimized. LVGL calls stay on the UI thread, as the code does today.
3. **C99 stays.** Real libmodbus 3.1.10 for Modbus. The HAL, typed tags with quality, the alarm engine, the connection state machine, audit-before-send writes, the signed configuration and the Windows and Linux builds all stay.
4. **Industrial HMI language stays.** Do not turn the UI into a generic web-style dashboard. Keep information clarity, colour semantics (normal, warning, alarm, fault), alarm visibility and priority, consistent navigation and predictable operation. Use ISA-101 and ISA-18.2 as references only; never claim certification or compliance.
5. **Do not rebuild.** Small, targeted, reversible changes. No new frameworks, languages or dependencies. No new threads unless a measurement demands one.
6. **Nothing is claimed that was not run.** A build that compiles is not proof of equivalence. Anything not verified is listed as unverified. Raspberry Pi or ARM readiness is only claimed for what was actually built or run (the emulated Pi counts as emulated).
7. **No commit, push or GitHub change** unless the owner asks. No real PLC and no real control writes: use the fake PLC and the fake RTU server.
8. **Do not weaken** role checks, input validation, auditing, write protection or the configuration signature to save size or CPU.
9. Behaviour changes that the owner has asked for (login, RTU, heartbeat) are additive: the existing paths keep working, and new behaviour is off or compatible until it is configured.

## 3. Verified facts about the current repository

These were checked in this repository and should be re-verified in phase 0 before relying on them.

- Branch `feature/authorization-hardening`, last commit `ddda58d`, about 125 uncommitted entries, including the responsive layout work.
- Size: static musl build 321,712 bytes stripped (text 307,626, data 7,408, bss 431,128). Gate limits 322,000 and 440,000 bytes (static data measured 438,536). Margin is 288 and 1,464 bytes. Any feature that adds code needs room made first.
- Responsive layout is built: display backends read the real size (`display.h`: `p99_display_size`, `p99_display_take_resize`), SDL window resizes live with a 640x400 minimum, screens use an 800 x 480 design grid mapped by `ui_x()` and `ui_y()` in `src/ui/hmi_ui.c`, three font sizes (14, 18, 24 px). Checked on Linux at several sizes; Alarms, Trends, Settings were only checked at 800x480.
- Tests on Linux passed after that work: core 3,275, view 208, golden identical, UI scenario 205, end to end 75, executable and liveness tests. Sanitizer builds, the lite profile, the kill-loop and the Windows build were not rerun after it.
- Idle behaviour measured: headless static build 620 kB Rss, 464 kB Pss, 4 threads, 0.50 percent of one core over 30 s. SDL window build 9.6 MB Pss.
- Warnings: none in the project's own files under `-Wall -Wextra -Wshadow -Wconversion -Wcast-qual -Wformat=2`; 18 sign-conversion warnings come from third-party `jsmn.h`.
- Makefiles track `config/lv_conf.h` for the LVGL objects (`Makefile.linux:76,80`, `Makefile.windows:41`). The earlier claim that they do not was wrong.
- Wake-up sources at idle (read from the code, not yet measured): the main loop sleeps at most 33 ms (`main_linux.c`); the LVGL binder timer runs every 50 ms (`hmi_ui.c`); the supervisor thread sleeps 50 ms per pass and takes a full tag snapshot for the trend each pass (`hmi_runtime.c:supervisor_thread`); the comms thread sleeps `min(hint, 20)` ms between steps and uses blocking libmodbus calls with bounded timeouts. Poll groups are already merged and phase-aligned (`hmi_comms.c:execute_poll`). The UI already skips unchanged labels (`ui_set_text`, `ui_set_color`), and the harness counts `labels_updated` against `labels_skipped_unchanged`.
- Candidate waste to confirm or reject by measurement: `overview_update` copies the alarm journal, builds a name table and formats journal text on every 50 ms tick even when nothing changed (`ui_overview.c`); the supervisor snapshots all tags every 50 ms although the trend bucket is 1 s by default; the comms thread's 20 ms cap on sleeping hints.

## 4. Method for every change (applies to all phases)

1. State the specific problem and the evidence (a measurement, a code path, a test).
2. Record the files affected.
3. Make the smallest effective change.
4. Build on Linux; build on Windows when the file is shared.
5. Run the relevant tests, then the whole suite at the end of a phase.
6. Compare with the baseline in `docs/baseline/` using the same workload and build flags.
7. Keep the change only if it is correct and beneficial. If it regresses, fix it; if it cannot be made safe, revert that one change from the snapshot and note it in the changelog.
8. Update `docs/OPTIMIZATION_CHANGELOG.md` and the preservation matrix in the same step.

Never trade one metric for another without writing the trade-off down: lower RAM with much higher CPU, faster rendering with stale widgets, smaller binary through removed features, or fewer polls with stale values are not improvements.

## 5. Phase 0: baseline and safety net

**Tasks**
1. Snapshot the working tree outside the repository (done: `C:\Users\Admin\Desktop\prometheus99-baseline-snapshot`, 383 files, source, tests, config, docs, scripts; third-party and build directories excluded because they are unchanged or generated).
2. Run the baseline script (`docs/baseline/`): clean static musl build and its warnings, stripped size and the gate output, `size` sections, the full Linux test list (`make -f Makefile.linux test test-view test-config-golden ui-host e2e cli-exec config-exec cfgedit-exec liveness`), the UI harness performance and heap lines, environment (kernel, compilers, CPU).
3. Add measurements the script does not yet make: headless static run memory and CPU over 30 s (Rss, Pss, VmHWM, threads); per-thread voluntary context switches over 30 s to find the real wake-up sources (`/proc/<pid>/task/*/status`); a syscall count by thread (`strace -c -f`) if available; the SDL window run at 800x480 for CPU; the Windows build and tests (`mingw32-make`, short path such as `C:\p99`).
4. Run once, record and keep the existing results of: the sanitizer builds (`SAN=address`, `SAN=thread` with `setarch x86_64 -R`), the lite profile with its gate (`LITE=1`, `bench/gate.sh build-musl-lite lite`), the kill-loop (`KILL_ROUNDS=150`), and the emulated Pi tour if the QEMU setup is present. Any that cannot run is written down as "not run, reason".
5. Break down the static data: `size -A`, `nm --size-sort -S` for `.bss` and `.data` objects (LVGL heap `LV_MEM_SIZE` 112 KB at `config/lv_conf.h:56`, draw buffer 32,000 bytes, tag database, storage queue, stacks), and the text size by object.
6. Write `docs/OPTIMIZATION_BASELINE.md` (all numbers with commands) and `docs/FUNCTIONALITY_PRESERVATION.md` with the matrix below, filled with the "before" status.

**Functionality preservation matrix (rows)**: startup and boot decision (signed config, safe mode); configuration load and validation; LVGL rendering and each screen (overview, alarms, trends, settings, config editor, number pad); touch and pointer input; framebuffer, file and SDL display backends; Modbus polling and decode (typed tags, word and byte order, scale and offset); tag quality and staleness; alarm engine (limits, hysteresis, delays, escalation, acknowledge, comm-loss); writes (validation, rate limit, second check, audit-before-send, read-back, unknown outcome); connection state machine, timeouts, back-off and recovery; storage worker (audit, journal, state, rotation); trends; on-device configuration editing through `p99_cfg`; roles and authorization; Linux build; Windows build; static musl and lite profiles; aarch64 emulated build; tests and gates. Each row has columns: existing behaviour, tests that cover it, changes made, checks rerun, final status (verified, unverified, changed by request).

**Done when**: the baseline file exists with real numbers, every unrunnable item is listed with the reason, and the snapshot is confirmed.

## 6. Phase 1: low-risk, evidence-first

**Evidence tasks first**
1. Wake-up and work profile of the idle runtime from phase 0 item 3. Decide from the numbers which of the candidate wastes in section 3 are real.
2. Size analysis from phase 0 item 5. Write down what each large static object is for and what can shrink without losing function. Example: the LVGL heap comment says about 81 KB is used at most, against 112 KB reserved; check the harness `max_used` before changing it, and keep a margin. The goal is room in the gate for login, RTU and the audit chain.

**Changes, each only if the evidence supports it**
- Skip the overview journal work when the alarm journal has not changed (use an existing revision counter such as `hmi_alarm_state_rev`, or add a small journal sequence). Expected effect: fewer mutex locks and less formatting per tick. Verify with the harness counters and the CPU measurement.
- Avoid rebuilding the tag-name table on every tick (build once, or only when the table changes).
- Supervisor snapshot cadence matched to the trend bucket while keeping alarm aging and link-health evaluation at their current cadence. Keep trend correctness: the trend step must see enough samples for min and max; prove it with the trend tests.
- Remove copies and temporary buffers on frequently executed paths where the profile shows them. Do not force static allocation anywhere that loses a limit or a feature.
- Sanitizers in the default test target (roadmap P4-1): make the Linux `test` target build with AddressSanitizer and UBSan and fix what they find without suppressing; keep a fast non-sanitized target. Adopted from the friend's `Makefile`.
- Stricter warnings and a static analyser in the build or CI (`-Wconversion -Wshadow -Wcast-qual -Wformat=2`, cppcheck if installed) (roadmap P4-2). Keep third-party code out of the warning count.
- Run the CI workflow once and fix environment problems; make the executable tests safe to run in parallel or keep them serial (README flake in `config-exec`) (roadmap P4-3).

**Verification**: full test list, the gate, the harness heap line, a 30 s idle CPU and memory run compared with phase 0.

## 7. Phase 2: measured performance improvements

Only items that phase 0 and phase 1 evidence justify.

- **Communication scheduling.** Review the wake-up cost of the comms loop (the `min(hint, 20)` ms cap) and consider sleeping until the soonest due group or until a write is queued, with a bounded cap, so the thread does not wake needlessly. Constraints: no stale values, timeout handling and recovery unchanged, write latency unchanged or better, jitter and back-off unchanged. Verify with the recovery measurements in README section 6 (link loss shown after about 4.1 s, return about 3.4 s after the PLC answers; with dropped connections loss after about 1.2 s and return about 0.43 s, 49 requests in 25 s) and the kill-loop.
- **Optional runtime statistics.** A `--report <path>` option writing loop and poll statistics on exit (adapt the idea from the friend's `runtime_stats.h` and `--report`), so benchmarks stop depending on external scripts. Output format documented; no effect when unused.
- **Lock contention.** Instrument first. Only if a trace shows contention on the tag database or alarm mutex, reduce lock hold times (copy under lock, process outside). Do not adopt a lock-free seqlock design; there is no evidence of a problem and the memory-ordering risk is not justified.
- **Rendering workload on the application side.** Count invalidations per second at idle and per interaction with the harness; remove redundant style changes and object creation; keep lazy screen building. Do not change LVGL configuration beyond what is already compiled in, except for measured heap sizing in phase 1.
- Re-measure CPU, memory, UI handler latency, screen transition time and touch latency with the harness and compare with the baseline table in section 10.

## 8. Phase 3: architecture and the roadmap features

These are the items the owner asked to add from the master prompt and the roadmap. Each is additive and each goes through the same method. Make gate room first (phase 1) and measure the size cost of each item.

1. **Keyed hash chain for the audit log (roadmap P1-3).** Replace the per-line CRC-32 as the integrity mechanism with `HMAC(key, previous mac || line)` using the HMAC code already in `hmi_cfgsec.c` and a key derived from the configuration key. Keep the line format readable and keep CRC if useful for corruption. Add a verifier tool or option (`--verify-log`). Reference: the friend's `alarm_log.h:1-33` and `alarm_log.c` (header carries the previous mac so rotation stays verifiable; chain resumes on open; honest note about truncation). Files: `hmi_auditfmt.c`, `hmi_store.c`, `main_linux.c`, `p99_sign.c` or a new tool. Tests: forgery, deletion, insertion, reorder, truncation, rotation, restart resume, kill-loop with the existing atomic-write logic.
2. **Named-user authentication (roadmap P1-1, owner decision Q7).** Named users with a PIN of 4 to 8 digits and a role each; salted, iterated keyed hash stored in the signed configuration; lockout after 5 failures; automatic logout after 5 idle minutes; user name in every audit line; a login screen with a touch keypad. Reference: the friend's `security.c:189-200` (PIN hash), `:272-306` (lockout), idle logout, elevation on demand; use HMAC-SHA-512 and existing key material, not new code, and reject his shared role PINs, separate vault and factory PINs. Additive: when the configuration has no users, the existing claimed-role behaviour continues (needed by the tests and the dev demo) and the screens say so; when users exist, login is required. `ui_current_role` and `ui_current_actor` (`hmi_ui.c`) return the logged-in role and user; the core still re-checks the role on every privileged call. The first user is created with `p99_sign`. Update `docs/AUTHENTICATION.md`. Tests: lockout timing, wrong PIN, idle logout, role elevation, unsigned user list goes to safe mode, UI harness scenario, executable test.
3. **Modbus RTU (roadmap P2-1, owner decision Q4).** `modbus_new_rtu` branch in `plc_libmodbus.c`; configuration keys for device, baud, parity, stop bits and unit id; one PLC link per configuration, `tcp` or `rtu`; same rules as TCP (bounded timeouts, silent line shown as no response, reconnect with back-off, no retry after a lost write reply). Test against a fake RTU server on a pty pair; try the emulated Pi if its virtual serial port can carry it. Regenerate the config golden file, rerun the gate. Model the fault tests on the friend's `tests/test_modbus_faults.c`.
4. **Optional PLC heartbeat (roadmap P2-4).** A configured register that the supervisor increments once a second only while the UI loop is alive, so a frozen HMI lets the PLC go to a safe state. It goes through the same validated write path or is an explicit, configuration-gated exception recorded in the docs. Adapted from the friend's heartbeat idea. Test with the fake PLC: the counter stops when the UI loop is stalled.
5. **Alarm presentation (roadmap P2-3).** Show return-to-normal-unacknowledged clearly, severity by shape and word as well as colour, per-alarm guidance text from the configuration, and keep the acknowledge-what-you-saw behaviour. Do not change severity meanings or ISA-18.2-style semantics already implemented. Files: `hmi_alarm.c`, `ui_alarms.c`, `hmi_view.c`.
6. **Responsive layout checks (roadmap P4-5).** Check every screen and the login screen at 640x400, 800x480, 1024x600, 1280x800, 1366x768 and 1920x1080; add screenshot checks at those sizes to the tests; add a safe single screen below the minimum size (adapted from the friend's `HMI_LAYOUT_TOO_SMALL`); consider replacing the non-uniform stretch with a uniform scale and extra room where the aspect ratio differs; check the framebuffer backend at a non-800x480 size in the emulated Pi.
7. **Module boundaries.** Continue the view-module split (roadmap P4-4): move more screens to the view model and reduce the 48 direct `ui_rt->` uses in `src/ui`. Small steps, tests first. Optionally prepare a slot-based device table design note for several devices (roadmap P2-2) without building it unless phases 1 to 5 are complete.
8. **Later Tier 3 items from the earlier master prompt** (several devices, coil writes, tags above 64, OPC UA as a separate optional build) only after everything above is verified. They stay behind build flags so the small device profile stays within its gate.

## 9. Phase 4: UI side

- Optimize LVGL usage only on the application side: redundant updates, invalidations, object creation, data binding frequency, event callbacks.
- Tier 2 visuals from the earlier master prompt, in this order, measuring the size cost of each and stopping when the gate has no room: alarm banner with severity colours, groups and blinking until acknowledged; per-tag bars with limit marks drawn as plain rectangles; touch targets of at least 48 px at the design scale with a pressed state and a confirm step for writes; alarm history screen from the existing journal; a day and night theme using a second palette of the same eleven colours (the screenshot palette check must pass for both); trend export to CSV; recipes applied through the checked write path with role, range, audit and confirmation; a string table for all text; a mimic page with one tank.
- Visual finish observed in the friend's build (anti-aliased fonts, cards, a tank widget, bars with limit ticks) is a model for clarity, not a style to copy. Anti-aliased fonts cost flash; test whether LVGL's existing font options fit the size budget before choosing. Keep the ISA-101 colour discipline: amber and red only for abnormal states and stop, neutral controls.
- Every visual change must leave alarm meanings, process-state meanings and operator interactions unchanged.

## 10. Measurement methodology and the comparison table

Use the same machine, the same build flags and workload for before and after. State the environment (kernel, compiler, CPU), the workload, how each number was taken and its limits. One run is a data point, so repeat runs where the difference is small. Fill this table in `docs/PERFORMANCE_COMPARISON.md`:

| Metric | How | Before | After | Difference |
|---|---|---|---|---|
| Stripped binary size | `strip`, `stat` | | | |
| Static data (data + bss) | `size`, gate | | | |
| Resident memory headless (Rss, Pss, peak) | `/proc/<pid>/smaps_rollup`, `VmHWM` | | | |
| Idle CPU over 30 s | `/proc/<pid>/stat` ticks | | | |
| Thread wake-ups per second | task context switches | | | |
| Startup time to first frame | harness | | | |
| UI handler latency, screen transition, touch response | harness `[perf]` lines | | | |
| Communication latency and recovery | fake PLC, recovery scripts | | | |
| LVGL heap used (max) | harness | | | |
| Build time | `time make` | | | |

Framebuffer memory is reported on its own line (screen width x height x bytes per pixel). Never claim a total under 1 MB with the framebuffer included.

## 11. Reference from the friend's project: ADOPT, ADAPT, REJECT

| Idea | Decision | Note |
|---|---|---|
| Keyed hash-chained audit log | ADAPT | Use existing HMAC code, a worker-thread writer, readable format |
| PIN hashing, lockout, idle logout | ADAPT | Named users in the signed config, HMAC-SHA-512 |
| Sanitizers in the default test target | ADOPT | Fix findings, do not suppress |
| Strict warning flags and cppcheck | ADOPT | Third-party excluded |
| `--report` runtime statistics | ADAPT | Small, optional |
| PLC heartbeat register | ADAPT | Through the validated write path or a documented exception |
| Alarm return-to-normal-unack display and guidance text | ADAPT | Without changing existing severity meanings |
| Safe screen below minimum size | ADOPT | In the layout code |
| Single-instance guard per data folder | ADAPT | Small, later |
| Horn and GPIO output | ADAPT | Code path only in the emulator |
| Fault-injection tests for malformed frames | ADAPT | For the pty RTU fake |
| Device slot table and driver modules | ADAPT, later | Thread per slot, no active discovery |
| Own 4 bpp renderer | REJECT | LVGL is fixed |
| Lock-free seqlock and SPSC rings | REJECT | No measured problem |
| Fixed 1 kHz loop | REJECT | Event-driven idle is five times cheaper in the measurement |
| Active network discovery | REJECT | Risk on live networks |
| Shared role PINs, 16-bit tag saturation, UI-only role check | REJECT | Weaker than the current design |
| ESP32 and MCU profile | REJECT | Out of scope |

## 12. Phase 5: closing quality work and validation

1. Documentation: `docs/OPTIMIZATION_BASELINE.md`, `OPTIMIZATION_CHANGELOG.md` (every change, files, reason, implementation, the friend's idea where relevant), `FUNCTIONALITY_PRESERVATION.md`, `PERFORMANCE_COMPARISON.md`, `ARCHITECTURAL_IMPROVEMENTS.md`, `FINAL_VALIDATION_REPORT.md`. Also: threat model (include PIN limits, HMAC-based hash, no encryption on Modbus), IEC 62443 mapping at the level of "covered and not covered", SBOM and licence review (libmodbus is LGPL, see `docs/THIRD_PARTY.md`), production roadmap, `GLOSSARY.md` (tag, user, role, safe mode, link health, design grid, stand-in device), and corrections of the stale text in `docs/CONFIG_INTEGRITY.md`, `docs/STORAGE.md`, `docs/SCREENS.md` and the README.
2. Final validation: clean builds on Windows (short path) and Linux (including static musl, lite, sanitizer builds and the aarch64 cross build); all test suites plus the new tests; the gate; the kill-loop; the harness performance run; a soak of 4 to 6 hours in the emulated Pi with injected faults, started at least two days before the finale and labelled "emulated" (not 72 hours); recovery measurements for TCP and RTU.
3. Re-check the preservation matrix row by row and state anything unverified.

## 13. Evidence package and demo for the finale

- All suites green on Linux and Windows, the gate passing, sanitizer builds and the kill-loop on the final tree.
- The seven mentor measurements as one table with how each was measured: startup, render and transition, touch latency, CPU, memory (program and framebuffer separately), stability, recovery (TCP and RTU).
- Screenshots at the six sizes and a short recording of live resizing.
- An honest list of what is not proven: no real hardware, PLC or touch screen; the emulated Pi is a generic ARM virtual machine; soak length; PIN strength; no encryption on the field bus.
- Demo (about 5 minutes): start `demo/run_linux_sdl.sh`; show live values; log in; raise and acknowledge an alarm; trends and alarm history; send a setpoint and show the read-back; apply a recipe if built; cut the link with `kill -USR1`, restore with `kill -HUP`; repeat on RTU; edit the configuration without signing to show safe mode; drag the window from 640x400 to full screen; close with the memory table.

## 14. Acceptance criteria

- Every existing major function verified or listed as unverified with the reason.
- LVGL 9.2.2 and C99 unchanged; libmodbus, HAL, typed tags, alarms, connection state, audited writes and configuration intact.
- Industrial HMI principles kept; alarm meanings unchanged.
- Each optimization backed by a before and after measurement; no fabricated numbers.
- Existing tests pass; new tests cover each improvement; the gate passes or any move is reported with measured numbers and the owner's approval.
- Windows and Linux builds validated where the environment allows; ARM only as far as actually run.
- All changes documented; no feature silently removed; nothing committed.

## 15. Stop conditions and risks

- If a change breaks a test that cannot be fixed safely, revert that change from the snapshot and continue with the others.
- If the gate cannot hold the planned features, stop adding and report the measured sizes; propose a lite profile or a gate move to the owner.
- Login is the highest-risk behaviour change (a lockout can block the only operator): keep an engineer recovery path through the signed configuration tool.
- Audit chain changes touch the storage worker and rotation: test crash and restart with the kill-loop before keeping them.
- RTU timing at low baud rates and long blocking reads in the comms thread need explicit tests.
- Windows and framebuffer paths remain unverified on real hardware.

## 16. Environment notes (what cost time)

- Linux builds and QEMU run in WSL2 Ubuntu 24.04. Work in a copy in the Linux home folder (`~/p99`), not on `/mnt/c`: it is faster and gives real file permissions (the configuration key file is rejected when it looks world-writable, which `/mnt/c` cannot avoid).
- Windows: build LVGL from a short path (the 260-character limit). MinGW is at `C:\msys64\ucrt64\bin`.
- The environment mangles backslashes in shell heredocs and `sed`: write scripts and patches with the file tools. Python scripts that rewrite files on Windows turn LF into CRLF unless they open files with `newline=''`; repository files are LF. A multi-line replace through Python once silently did nothing; use the Edit tool and check the result.
- Run the Linux test suites one after another, not in parallel.
- `pkill` patterns longer than 15 characters match nothing; use `pkill -f`.
- Chat replies to this owner are terse ("caveman" style). Saved files use plain, full sentences.
# AMENDMENT & EXTENSION DIRECTIVE: SCHNEIDER ELECTRIC PRODUCTION HMI SPECIFICATION
# Target: Prometheus99 Production Hackathon Submission (Schneider Electric Track)
# Scope: Layer on top of MASTER_PROMPT_FINAL (Phases 0–5). Do NOT replace existing safety/audit foundations.

---

## 1. REVISED RESOURCE GATES & HARD BOUNDARIES
- Flash / Binary Gate: Maximum stripped static musl binary < 500 KB. Formally increase `bench/gate.sh` limit from 330,000 bytes to 450,000 bytes (giving ~120 KB of headroom for 2bpp fonts, Schneider faceplates, and mimic drawing).
- RAM / Memory Gate: Maximum runtime Pss < 1,000 KB (1 MB). Update `POOL_PEAK_MAX_KB` in `Makefile.linux` and test harnesses from 56 KB to 64 KB to immediately clear CI/lite harness failures.
- Non-Negotiable Core Retentions:
  * Retain signed JSON configuration (`p99_sign`) with serial anti-rollback and safe mode.
  * Retain core-enforced role checks and pre-send audit logging in `src/hmi/hmi_writes.c`.
  * Retain write read-back verification (CONFIRMED / REJECTED / UNKNOWN).
  * Retain typed tags (`u16`, `i16`, `u32`, `f32`) — NEVER clamp floats to 16 bits.
  * Retain surviving unacknowledged alarms across comm outages (`HMI_AL_RTN_UNACK`).

---

## 2. SCHNEIDER ELECTRIC INDUSTRIAL PROFILE & REGISTER MAP
Focus the HMI runtime and tag mapping on standard Schneider Electric automation architectures:
1. Altivar Variable Speed Drive (ATV340 / ATV630):
   * Standard CiA 402 drive state machine mapping:
     - ETA (Statusword, 40001 / Modbus 3201): Bit 0 (Ready to switch on), Bit 1 (Switched on), Bit 2 (Operation enabled), Bit 3 (Fault), Bit 5 (Quick stop).
     - CMD (Controlword, 40002 / Modbus 8501): Shutdown, Switch On, Enable Operation, Fault Reset.
     - RFRD (Output Speed Reference, 40003, signed rpm / 0.1 Hz).
     - LCR (Motor Current, 40004, 0.1 A).
     - THD (Motor Thermal State, 40005, % nominal).
2. Modicon M340 / M580 Process Skid:
   * %MW100–%MW120: Process Analog Values (Tank Level 0.1%, Inflow Rate 0.1 L/min, Discharge Pressure 0.1 bar, Coolant Temp 0.1 °C).
   * %M0–%M31: Process Digital Interlocks (Feed Pump Run, Discharge Valve Open, High-Level Interlock Latch, Emergency Stop Loop).
3. PowerLogic PM5350 / PM8000 Feeder Meter:
   * Real energy metrics via 32-bit floats (Voltage L-L, Active Power kW, Total PF, Frequency Hz).

---

## 3. UI/UX "TIER-2" ELEVATION (STEAL & REFINE)
Bridge the presentation gap while complying with ISA-101 / ISA-18.2 standards:

### A. Typography & Visual Hierarchy
- Upgrade fonts from 1bpp to 2bpp anti-aliased (4-level blending) using `lv_font_conv` for the 3 core sizes (12px, 18px, 24px) in DejaVu Sans / Inter.
- Implement card containers (`ui_mk_panel`) with high-contrast status pills (Running [Green], Stopped [Neutral], Tripped [Red], Interlocked [Amber]).
- Support a Day/Outdoor high-contrast theme toggle alongside the industrial dark theme.

### B. ISA-18.2 Compliant Alarm Banner & Guidance
- Fixed top banner beneath the status bar, active whenever highest severity unacknowledged alarm exists.
- Content:
  * Severity Shape + Word: Critical (Red Diamond), Warning (Amber Triangle), Diagnostic (Blue Square).
  * Tag & Value: e.g., `TK1_LEVEL: 96.2 % (Trip Limit: 95.0 %)`.
  * Operator Action Steps: Pull `action1` and `action2` from the signed configuration (e.g., `1. Stop Feed Pump P-101` | `2. Open Emergency Spill Valve V-102`).
  * Direct action button: Large touch-friendly `[ ACKNOWLEDGE ]`.

### C. Explicit Interlock & Refusal Feedback
- Never leave a button silently disabled or greyed without explanation.
- Query `hmi_writes.c` error states to display clear inline operator feedback:
  * `"Blocked: Drive Fault Latch Active"`
  * `"Blocked: Requires Supervisor Role"`
  * `"Rejected: Value Exceeds Setpoint Range (0–1500 RPM)"`

### D. Industrial Mimic Widgets
- **Tank Mimic Widget:** LVGL canvas or styled panel showing vertical fluid column with dynamic percentage fill, deadband tick, and warning/trip limit lines (80% High, 95% High-High).
- **Altivar Drive Faceplate:** CiA 402 state badge (`SWITCH_ON_DISABLED`, `READY`, `RUNNING`, `FAULT`), speed setpoint slider with `+`/`-` nudge, motor current gauge, and explicit Forward / Reverse / Coast Stop controls.

---

## 4. ACOUSTIC ANNUNCIATOR (ALARM SOUND SYSTEM)
Add non-blocking, silenceable alarm audio feedback:
- Architecture: Background audio worker using SDL2 audio queue / PCM tone synthesizer (or Linux `/dev/snd`, fallback to terminal bell `\a` in headless).
- Tone Profiles:
  * Critical / Trip: Dual-tone alternating 800 Hz / 1000 Hz warble (200 ms pulse).
  * Warning: Single 600 Hz chirp every 2 seconds.
- Operator Controls:
  * Top bar `[ Silence Horn ]` button immediately stops audio synthesis without acknowledging the alarm state in the log or PLC.
  * Audio automatically re-sounds if an alarm escalates or a new unacknowledged alarm arrives.

---

## 5. STANDALONE SCHNEIDER PROCESS SIMULATOR (`tools/sim/`)
Create a dedicated, scriptable process simulator outside `src/` (0 byte impact on production binary).
- File: `tools/sim/schneider_sim.c` (compiles to `tools/sim/schneider_sim`).
- Device Simulation:
  * Port 1502: Altivar ATV340 Drive (CiA 402 state machine logic, motor ramp acceleration, thermal modeling).
  * Port 1503: Modicon M340 Tank Skid (Physics: Inflow Pump A speed fills tank; Outflow Valve drains tank via Torricelli's law; level high interlock automatically trips pump).
  * Port 1504: PowerLogic PM5350 (Simulates load current scaling proportionally with drive speed).
- Real-Time Scripting & Fault Injection Channel:
  * Interactive CLI and socket listener (`tools/sim/sim_ctl`):
    - `fault freeze <port>`: Stop responding to Modbus TCP requests (tests comm-loss).
    - `fault trip <dev> <error_code>`: Inject Altivar Overload (OL2) or Drive Overheat (OHF).
    - `fault exception <port> <code>`: Return Modbus Exception 02 (Illegal Address) or 04 (Slave Device Failure).
    - `nudge <tag> <delta>`: Manually drive level or pressure up/down to trip limits during live demos.
    - `heartbeat latch`: Stop answering HMI heartbeat to trigger PLC safety latch.

---

## 6. VALIDATION & ACCEPTANCE CRITERIA
1. `make -f Makefile.linux LITE=1 BUILD=build-lite ui-host` must pass with `POOL_PEAK_MAX_KB=64`.
2. `bash bench/gate.sh build-musl` must pass with stripped size < 450,000 bytes.
3. Live Cross-Run Demo:
   * Run `tools/sim/schneider_sim` on ports 1502–1504.
   * Start `prometheus99-sdl` with Schneider demo profile.
   * Inject Altivar drive trip from CLI -> HMI displays red diamond, sounds horn, shows CiA 402 fault text, and displays operator guidance `Check Motor Ventilation & Reset Fault`.
   * Operator silences horn -> Sound stops, banner remains active.
   * Operator sends Reset -> Audit logs `RESET_SENT`, drive transitions CiA 402 state, writes confirmed via read-back.