# Project comparative audit: Prometheus99 against the teammate's Prometheus HMI

Date: 2026-11-02. This is an audit. No source file of either project was changed for it, nothing was committed, and the only file added to the Prometheus99 repository is this report. It reads the earlier audit in `C:\Users\Admin\Desktop\prometheus99-comparative-audit` as input, checks that audit's claims against the source, and does not replace or overwrite it.

## How to read the labels

Every finding carries one of five labels.

- **Verified by running**: I ran it in this session and saw the result.
- **Source evidence**: read in the source of the named file, not run.
- **Test-only evidence**: shown only by a test suite, not by operating the program.
- **Inferred**: concluded from the above, not directly observed.
- **Untested**: neither run nor read closely enough to claim.

A polished look is not treated as proof of technical strength anywhere in this report.

## 1. Executive summary

The two projects are not variants of one design. Prometheus99 is a core plus LVGL plus libmodbus runtime with a signed configuration, audited and read-back writes, a keyed-hash-chained audit file, named-user PIN login, RTU, a PLC heartbeat and a four-profile build system. The teammate's project ("his project" below) is a self-contained product slice: its own 4-bit renderer, its own Modbus TCP codec, plug-and-play device discovery, nine device driver modules, per-device faceplates, an ISA-18.2 alarm banner with operator guidance, a software STOP ALL, and a Win32 simulator application with fault injection.

Which is technically stronger. Prometheus99 is stronger on the properties that decide whether a write or a stored record can be trusted: configuration integrity with safe mode (source evidence, plus verified by running in the `liveness`, `config-exec` and `e2e` targets), the write path that re-checks the role in the core and reads the value back (source evidence in `src/hmi/hmi_writes.c`, test evidence in `e2e` 81 checks), a typed tag model with 32-bit and float values, a second transport (RTU), and sanitizer, thread-sanitizer and kill-loop coverage. His project is stronger on the number of assertions executed (22,387 against 4,063, though many are table-driven loops, so this is not a measure of coverage), on static data and size of the program that draws the screen (260,304 bytes stripped static musl and 157,400 bytes of static data, against our 329,904 and 377,640, both measured here), on the number of device types, and on code that is portable to a Windows panel PC without a toolkit. In the same headless idle measurement, ours used 0.45 percent of a core and 604 kB of Pss against his 3.45 percent and 1,292 kB; the builds and workloads differ, so I treat that as direction only. His role check lives only in the UI thread and his tag value is a 16-bit integer that saturates (both source evidence, section 4.4).

Which has the better experience. His, clearly. In the hands-on run his HMI showed a device card for each machine, a tank widget, a priority banner with a two-step guidance text, status pills in the header, a role-gated keypad that prints the allowed range, a day/night theme, a diagnostics screen with a register map, and a "Blocked: Stop latch" label on an interlocked control. Prometheus99's screens are a plain table in a one-bit font with an 11-colour palette: correct and legible, but an operator has to read rows to understand the machine. The gap is mostly in presentation and in operator-facing explanations, not in the runtime underneath.

The most important improvements for Prometheus99, in order:

1. Decide the colour and font policy (owner decision, section 10.5 item 1). The 11-colour palette and the one-bit font are self-imposed limits (`docs/SCREENS.md`, `src/ui/font/README.md`) and are the main reason the screens look plain. His anti-aliased Inter font set compiles to about 26 KB of text and data (measured, section 9); our three one-bit fonts are 6.7 KB, and moving them to two or four bits per pixel would cost roughly 3 to 15 KB (inferred). The binary gate has 50 bytes of room, so any visual work needs a gate decision from the owner.
2. Finish alarm presentation (MASTER_PROMPT_FINAL phase 3 item 5): a banner with guidance text, severity by shape and word, and return-to-normal-unacknowledged shown clearly. The engine already has the state (`HMI_AL_RTN_UNACK` in `src/hmi/hmi_alarm.c`); only presentation and configuration of guidance text are missing.
3. A device or tag-group faceplate and a mimic with limit marks (phase 4, "Tier 2 visuals").
4. A scriptable simulator for demonstrations and for tests of fault injection (section 13, recommendation 5), kept outside the device binary.
5. Explain refusals: show why a control is blocked or a write was refused, as his faceplate does.

Recommended roadmap changes, in short: the existing phase and item ids stay. Phase 3 item 5 should be expanded (section 11), phase 4 should start with a font and palette decision instead of with the first visual, and two small items should move earlier because they cost no binary size: the lite RAM limit decision and the simulator control channel (test code only).

What I built and tested is listed in section 9 and what I could not do in section 14.

## 2. Evaluation approach

1. Inventory both trees and compare documentation with source.
2. Read the source of the features that decide trust (writes, alarms, authentication, configuration, threading) in both projects and cite file and function.
3. Build both in copies. His project in a scratch directory under the session scratchpad (then a second copy in the WSL home directory `~/fb`), ours in the existing WSL build copy `~/p99b`. Run each project's own suites.
4. Operate his project: his Win32 simulator and his Windows HMI built from source in scratch. I drove the HMI by posting window messages (keyboard and mouse) and captured the window with `PrintWindow`. I could not use a physical display, so every "I clicked" below means a posted message to the window, and every screenshot is a window capture.
5. Operate ours: the documented SDL window tour (`demo/linux_tour.sh`) under WSLg, the headless harness (`ui-host`, `ui-shots`, `ui-sizes`) and the end-to-end suites.
6. Cross-drive: our HMI against his simulator and his HMI against our fake PLC.

Environment: Windows 11 host, WSL2 Ubuntu 24.04 (kernel 6.18.54.1-microsoft-standard-WSL2), gcc 13.3.0, 12 logical processors, MinGW-w64 gcc under `C:\msys64\ucrt64`. Single runs, WSL2, x86-64.

## 3. Inventory

| Item | Prometheus99 (ours) | Teammate's project |
|---|---|---|
| Path | `C:\Users\Admin\Desktop\prometheus99-main\PROMETHEUSS` | `C:\Users\Admin\Desktop\prometheus99_custombuild-main\prometheus99_custombuild-main` |
| Git | branch `main`, HEAD `c1392f5`, 35 changed or new files uncommitted (not touched by this audit) | not a Git repository (the folder has no `.git`); the parent directory resolves to the user's home, which is a repository with no commits, so there is no history to read |
| Language, standard | C99 (gnu99 in the Makefile) | C99 |
| Display | LVGL 9.2.2 (fixed by the brief), SDL2, Linux framebuffer, file and Win32 GDI back ends | own band renderer (19,200-byte frame buffer in 10 bands), Win32 GDI, X11 loaded at run time, framebuffer, ESP32 port written but not compiled |
| Field bus | libmodbus 3.1.10: TCP and RTU | own Modbus TCP codec: function codes 01, 03, 05, 06 and 43/14 (`include/fieldbus/modbus_proto.h`); no RTU, no function 16 |
| Devices | one PLC link per configuration, tags configured in JSON | up to 8 devices, nine driver modules and a generic one, discovery by FC 43 identity plus explicit `--plc` |
| Tag model | typed tags: u16, i16, u32, f32 with word order, scale, offset, quality | 64 tags of `int16_t` value and setpoint (`include/core/tags.h`) |
| Configuration | signed JSON, safe mode when the signature fails | plain `hmi.cfg` options and compiled driver profiles |
| Authentication | named users with PIN, PBKDF2-HMAC-SHA-512 10,000 rounds, 5 failures lock 60 s doubling, 5 minute idle logout (`src/hmi/hmi_auth.h`) | four roles with one shared PIN each, 2,000 hash rounds, 3 attempts and a 30 s lock (`include/core/security.h`); factory PINs in bench mode |
| Audit | keyed hash chain over the audit file, verifier option | HMAC-SHA-256 chained alarm log, `--verify-log` |
| Threads | UI, supervisor, comms, storage worker, mutex-protected shared state | fieldbus thread at 1 kHz, UI, discovery thread, seqlock snapshot, single-producer ring and atomic command bits |
| Platforms built here | Linux glibc, static musl, lite profile, ASan, TSan, SDL, Windows demo (GDI), emulated Pi (QEMU) | Linux dynamic, Linux static glibc, Windows exe; Pi targets and installers not built here |
| Docs | README, ~30 documents in `docs/`, changelog C1 to C21 | README, user manual, lab deployment note, per-folder READMEs |
| Licence | per repository | MIT |
| Prebuilt binaries in the tree | none relevant | three `.exe` files, an `alarms.log` and a `hmi.key` file are present in his folder; I did not run the prebuilt files and built from source instead. The `hmi.key` file is the device key that signs the audit log and the PIN vault (his README says it must never be in version control), so it should be removed from anything that is shared (source evidence, `hmi-runtime/hmi.key` and `README.md`) |

Documentation claims checked against source or by running:

- "11,513 / 8,999 / 558 / 74 / 1,243 checks": verified by running (section 9).
- "No LVGL, no libmodbus": source evidence (`Makefile` `CORE_SRC`).
- "Roles with least privilege, PIN lockout, keyed hashes": source evidence (`src/core/security.c`, `include/core/security.h`).
- "Audit log signed and checkable with `--verify-log`": the log header and `--verify-log` option exist (source evidence, `src/core/alarm_log.c`, `src/app/app.c` header comment); I did not run `--verify-log` against a tampered file. Untested.
- "About 0.3 MB executable": verified for the Windows exe (352,768 bytes built here). The Linux static build is larger, see section 9.
- "Under 1 MB of private memory": untested here on Windows; Linux headless resident figures are in section 9.
- "Tested on Raspberry Pi targets under qemu, Wine, cppcheck clean": untested by me.
- The earlier audit's claim "His role check lives only in the UI thread": source evidence confirmed. `src/app/app.c` lines 809, 872 and 1138 call `hmi_security_required_role`, and the fieldbus side (`src/fieldbus/device_manager.c`) never checks a role; its write step sends what the UI queued.
- The earlier audit's claim "active network discovery on by default" (its finding F-5): not supported. The built-in discovery targets are loopback only (`src/fieldbus/discovery.c:162-165`); a subnet is scanned only when `--scan` is given. Source evidence. The caution about scanning a live network stands, as an option.
- The earlier audit's claim "16-bit tag saturation": source evidence confirmed in `src/fieldbus/device_manager.c:137`, where a float result is clamped into `int16_t`.
- The earlier audit's claim "no read-back": confirmed. A write is confirmed only by the FC05/FC06 echo (`MB_ERR_ECHO` in `modbus_proto.h`), not by reading the register again.
- The earlier audit's roadmap items P1-1 (named users), P1-3 (audit chain), P2-1 (RTU), P2-4 (heartbeat), P4-1, P4-2 and P4-5 have been carried out in this repository since, as the changelog records them: audit chain C14, named-user login C16, RTU C17 and C18, heartbeat C19, layout checks C20, sanitizers in the default test target C5, strict warnings C6. The changelog was read; the code for each was spot-checked by the suites in section 9. P2-3 (alarm usability, which is MASTER_PROMPT_FINAL phase 3 item 5) is the main open feature item, and P2-2 (several devices) is not started.

## 4. Architecture comparison and source audit

### 4.1 Module boundaries and runtime architecture

| Area | Prometheus99 | His project |
|---|---|---|
| Shape | Library-style modules in `src/hmi` (pure logic: `hmi_decode.c`, `hmi_tags.c`, `hmi_alarm.c`, `hmi_writes.c`, `hmi_authz.c`, `hmi_auth.c`, `hmi_config.c`, `hmi_cfgsec.c`, `hmi_store.c`, `hmi_auditchain.c`), a platform layer (`hmi_platform_posix.c`, `hmi_platform_win32.c`), the Modbus driver (`plc_libmodbus.c`), the runtime that owns threads (`hmi_runtime.c`, `hmi_comms.c`), and the UI (`src/ui`: LVGL screens plus a toolkit-free view model `hmi_view.c`). Source evidence. | `include/` and `src/` split into `core` (tags, alarms, alarm log, security, SHA-256, seqlock), `devices` (one file per machine type), `fieldbus` (codec, device manager, discovery, driver host), `hal` (OS interfaces, two implementations each), `renderer`, `platform` (Windows and Linux shells), and `app/app.c`, a 2,000-line file that holds the UI state machine, the dialogs, the command dispatch and the fieldbus thread glue. Source evidence. |
| Direct UI reach into runtime state | 49 direct `ui_rt->` uses in `src/ui` (`HANDOFF_2026-11-02_SESSION2.md` section 5), most of them passing `&ui_rt->db` and similar to thread-safe APIs. Source evidence. | `app.c` owns both sides in one file, with the UI thread and the fieldbus thread separated by a seqlock snapshot and an atomic command word. Source evidence (`app.c` header comment, `ui_take_snapshot`). |
| Rendering pipeline | LVGL 9.2.2, partial redraw into a 32,000-byte draw buffer, RGB565 to a 16-entry palette in the file and framebuffer back ends, 1-bit text. Labels update only when the text or colour changed (`ui_set_text`, `ui_set_color`). | Token bytecode plus a band renderer. `hmi_layout_*` functions compute geometry once and serve both drawing and hit-testing, so a tap and a drawn button cannot disagree (`include/renderer/layout.h`, `src/app/app.c` `app_pointer_up`). Source evidence. |
| PLC communication | One PLC link. libmodbus with bounded timeouts, a blocking comms thread, merged poll groups, back-off and a link-health state machine (`src/hmi/hmi_comms.c`, `plc_libmodbus.c`). TCP and RTU. | Non-blocking sockets in one fieldbus thread at 1 kHz serving up to 8 devices (`src/fieldbus/device_manager.c`), a discovery thread that probes a range with FC 43 (`discovery.c`). TCP only. |
| Tag model | `hmi_tags.c`, `hmi_decode.c`: typed, scaled, with word and byte order, quality and staleness. | `core/tags.c`: 64 tags of 8 bytes, `int16_t` value, raw limits. Float and 32-bit device values are converted by the driver and clamped into 16 bits (`device_manager.c:137`). |
| Alarm engine | `hmi_alarm.c`: limits, hysteresis, on and off delays, escalation, acknowledge-what-you-saw, communication loss, return-to-normal-unacknowledged state `HMI_AL_RTN_UNACK`. Journal in memory and in the store. | `core/alarms.c`: per-point state machine NORMAL, UNACK, ACKED, RTN_UNACK (`alarms.h` diagram), warn and trip limits only, deadband of 1 percent of span, operator guidance per tag (`action1`, `action2`), horn with silence. |
| Writes and validation | `hmi_writes.c`: validate range and rate, check the role at submit and again at send, write the audit record before the send, send, read back, record a confirmed, rejected or unknown outcome. Source evidence; test evidence in `test_writes.c`, `test_modbus_e2e.c`. | UI asks for the role and the value (`ui_request`, keypad range text), the fieldbus thread sends FC05 or FC06 and checks the echo. No audit before the send, no read-back, no unknown state (source evidence). Commands are logged after the fact ("Tank skid 1: Valve V-101 -> 0 %" appeared in the signed log after I sent it; verified by running). |
| Configuration | Signed JSON, serial number against rollback, safe mode with a visible reason, on-device editor behind a root helper `p99_cfg` (`hmi_config.c`, `hmi_cfgsec.c`, `hmi_boot.c`, `hmi_cfgedit.c`). | Text options in `hmi.cfg` and the command line (`app.c` header comment); device behaviour is compiled in. A commissioned panel is a panel with site PINs; there is no integrity protection on the configuration. Source evidence. |
| Persistence | Storage worker thread, atomic writes, rotation, hash-chained audit file, trend and alarm state; kill-loop tested. | One alarm log file written on the UI thread (`alarm_log.c`), HMAC chain per record, header carries the previous MAC for rotation. |
| Threading and ownership | Mutex-protected shared state with a documented lock order, measured contention (C12: no change needed), ThreadSanitizer run clean. | Lock-free: seqlock snapshot with CRC, SPSC ring for audit events, atomic command word. Reported 5 seqlock retries in one 20 s Windows run (earlier audit; I saw `seqlock_retries=0` in a 15 s Linux run). |
| Error handling and recovery | Timeouts, reconnect with back-off, quality bits per tag, writes with unknown outcome never retried, safe mode. | Per-device link state and removal after 10 s offline (verified by running), reconnect through discovery. |
| Platform dependencies | libmodbus (LGPL), LVGL, SDL2 for the desktop window, Monocypher (vendored). | None beyond the OS. X11 loaded with `dlopen` at run time, which cannot work in a fully static glibc build (linker warning observed, section 9). |
| Deployment | systemd-style service design, static musl, deploy folder, emulated Pi. | NSIS installer, Linux install script and desktop files, Raspberry Pi archives (written; I did not build them). |
| Test quality | Core 3,531, view 209, UI harness 225, e2e 81, rtu-e2e 17, TSan, ASan and UBSan, kill-loop, golden files, screenshot palette check, 76-picture layout check. | 11,513 core checks (ASan/UBSan), 8,999 in the small-device profile, 558 integration checks against the simulator's models, 74 fault-injection checks, 1,243 model checks. Test code is 2,832 lines against 14,698 lines of product code (section 9.5); ours 12,872 against 13,263. |
| Technical debt | Plain screens; lazy single PLC; 49 `ui_rt->` reaches; gate with 50 bytes of room; lite RAM limit failing (58 KB against 56 KB). | `app.c` and `micro_renderer.c` are 2,000 and 2,110 lines; hand-baked fonts and token tables; one 16-bit value type; no config integrity; active network discovery. |

### 4.2 Real functionality against simulated, mock or partial

Ours (source evidence unless stated):

- Real: Modbus TCP and RTU through libmodbus (RTU tested against a pty fake slave only, never on a serial line), signed configuration, audit chain, login, writes, alarms, trends, storage, LVGL screens.
- Test stand-ins: the fake PLC (`tests/hmi/fake_plc_main.c`, `fake_modbus_server*.c`) answers a fixed register image and is steered by signals; the Windows demo uses an unsigned configuration through a stand-in for `p99_cfg`.
- Partial: one PLC link only; no device concept.
- Never run on real hardware, a real PLC or a real touch screen (our own documentation says so).

His (source evidence unless stated):

- Real: the Modbus TCP client and the device manager (exercised in the integration and fault suites over real sockets, and by me against his simulator and our fake PLC), discovery, the audit log chain, the PIN vault, the renderer, the Windows window.
- Simulated: every device register map other than the Altivar status and command words is an assumption (his README says so).
- Partial: the ESP32 port is written but not compiled; Linux GPIO horn output cannot be verified without hardware; "Telemetry" shows "No host telemetry on this platform" on Windows (earlier audit, section 6 of its visual document; not re-checked).

### 4.3 What the two simulators are

- His simulator (`plc-simulator/`) is a separate application. Each simulated device is a Modbus TCP server on its own port with a 100 ms process model (`src/plc_models.c`): a tank filled by a pump and drained by a valve, an Altivar drive with a CiA 402 state machine, a meter with floats, a conveyor, a boiler, pump station, compressor, air handler and ABB drive. It models interlocks, an HMI heartbeat that latches a stop when the HMI stops talking, RUN and STOP, and three injectable faults (freeze link, answer with exception 04, trip an overload). Its Windows window lets a person add and remove devices, nudge values and see an activity log; its Linux and console build only lists values (`plc_simulator_cli`), and only a window user can inject faults. Source evidence and verified by running (section 5).
- Our fake PLC (`tests/hmi/fake_plc_main.c`) is a single-client test helper: one register image that matches `config/hmi.example.json`, a sine-like motion, signal-driven faults (USR1 drops the link, USR2 hangs, HUP restores), and a stats file. It is also a measurement tool. It has no process behaviour, no second device and no operator interface (source evidence; verified by running in the tour).

### 4.4 Source findings in his project that matter for the comparison

1. Role enforcement is only in the UI thread (labels above): `app.c:809`, `app.c:872`, `app.c:1138` call `hmi_security_required_role`; `device_manager.c` has no role check. Source evidence. Consequence: nothing in the core stops a command if a different front end ever queues it.
2. The tag value is a 16-bit integer and float or 32-bit device values saturate silently (`device_manager.c:137`). Source evidence.
3. Write confirmation is the Modbus echo only (no read-back, no unknown outcome). Source evidence.
4. Unacknowledged alarms of a removed device are discarded. Verified by running (section 5.2): after the three devices had been offline for 10 seconds, the three unacknowledged critical communication-loss alarms (log rows 17, 18 and 20) were gone when the devices came back, and the log holds no acknowledge or return-to-normal record for them. Source evidence for the cause: `remove_slot` in `src/fieldbus/device_manager.c:530-548` calls `hmi_alarms_clear_range`, which clears the device's alarm points together with its tags. The state machine in `include/core/alarms.h` keeps a cleared unacknowledged alarm as RTN_UNACK, and that did happen when the link dropped and recovered within a few seconds (log rows 36 to 39, "back to normal", state RTN-UNACK). So an outage longer than the removal time (default 10 s) erases the evidence that an operator never acknowledged. For Prometheus99 the lesson is the opposite design: a single permanent link that is never removed, with its comm-loss alarm kept until acknowledged (this is how our engine behaves, source evidence in `hmi_alarm.c`).
5. A frozen link makes the simulated PLC latch a stop through the missing HMI heartbeat, and after the freeze ends the HMI shows "Tank skid 1: Stop latch active" with "Blocked: Stop latch" on the pump control (verified by running). This is correct behaviour for a heartbeat design and is a good demonstration, but it means a short outage stops the machine until someone acknowledges and resets: an operator-experience point worth copying only with a clear message (his guidance text does give one).
6. `hmi.key` and prebuilt executables sit in the delivered folder (section 3).

## 5. Hands-on: his project

All of this section is verified by running unless it says otherwise. Windows binaries: `plc_simulator.exe` (78,336 bytes), `plc_simulator_cli.exe` (46,592 bytes) and `industrial_hmi.exe` (352,768 bytes), built from source in scratch with `build.bat` (MinGW gcc, `-std=c99 -Wall -Wextra -Wshadow -Werror -Os`, no warnings). Screenshots are in the session scratchpad under `shots\` and are not part of the repository.

### 5.1 Simulator

- Start-up: `plc_simulator.exe --add tank --add atv340 --add meter` opens a window with buttons to add each device type, a card per device with its rows (value, a `-` and `+` button, a label "HMI sets" for values the HMI may write), switches for pumps and latches, buttons Unplug, STOP, Freeze link, Error replies and Remove, and an activity pane.
- Tags exposed per device are the rows of `plc_models.c`. The tank exposes level, pump A speed, valve V-101, line pressure, coolant temperature, level setpoint, control mode, an "HMI heartbeat" row that shows the counter the HMI writes, and coil switches (feed pump A, coolant pump B, stop latch, fault reset, pump A overload).
- Dynamics: the tank level moved by itself without any input (72.0 percent at start, rising to 100 percent with the valve closed). The HMI's valve write changed the pressure from 129 to 155 kPa and the level climbed until the high alarm (85.0 percent) and the high-high alarm (95.0 percent) raised on their own, at 15 and 32 seconds after the valve was set to 0 percent (alarm log rows 12 and 13).
- Fault injection: Freeze link, Error replies and Unplug are buttons; Pump A overload is a toggle. I used Freeze link and the overload toggle (below). I did not use Error replies, Unplug or STOP.
- Ease of use: high for a person with a mouse. Only a window user can inject faults; the console build cannot, which makes scripted fault tests impossible without code changes (source evidence, `main_console.c` options).
- Limits seen: the window has a fixed layout; at a 1,100 by 1,000 pixel request the card is cut off at the activity pane and the lower buttons are partly hidden (screenshot `sim_2_tall`); a stopped simulator process leaves the HMI to discover it again, and a restarted one starts again from default values.

### 5.2 HMI scenarios and results

Operated by posted keyboard and mouse messages to the window (`R` role dialog, digits, Enter, Esc, tab keys `1` to `5`, `A` acknowledge), because a posted mouse click on the faceplate buttons did not always register in my first tries while the window was minimised by the host; after restoring it every posted click and key worked.

| Scenario | Result |
|---|---|
| Start and discovery | `industrial_hmi.exe --config examples\bench.cfg` (simulator already running) found three devices on 127.0.0.1:1502 to 1504 at 371 ms of HMI uptime ("Tank skid 1 found", "Altivar drive 1 found", "Energy meter 1 found") and each was "online" about 20 to 80 ms later (alarm log rows 4 to 9). |
| Read a device | The process screen showed one card per device. The tank faceplate listed 11 values and 6 controls ("Start pump", "Stop pump", "Set valve...", "Set level...", mode switch, overload reset). |
| Sign in | Key `R`, then `2`, then `1111` and Enter: header changed from "Viewer" to "Operator", a toast "Signed in as Operator" appeared, and the log recorded "Signed in: Operator". The factory PIN worked because the bench configuration allows it; the settings screen shows "Factory PINs - BENCH MODE" in amber. |
| Write with validation | Set valve: the keypad printed "Now 65 % - allowed 0 to 100". Entering 150 printed "Out of range: allowed 0 to 100 %" and refused the entry. Entering 0 and Set sent the write; the faceplate showed 0 percent and the log row 11 is "Tank skid 1: Valve V-101 -> 0 %". The prompt that a control press as Viewer should open ("a control asks for the PIN of the role it needs", shown on the faceplate footer) was not seen: my posted clicks on the faceplate buttons did nothing while the window was minimised by the host, and I did not repeat that exact step after restoring it. Untested. |
| Alarm | See 5.3. |
| Acknowledge | Key `A` acknowledged the alarm: the banner button turned into the word "Acknowledged" and the log row "Operator acknowledged 1 alarm(s)" appeared. |
| Link loss and recovery | Killing the simulator: within 6 seconds the header changed to "PLC offline", the alarm pill to "3 unack.", the cards to "--" values with "Offline - removing in 3 s", and the banner to "Meter comm loss active" with the guidance "Check the RS-485 wiring and the gateway". After 10 seconds the three devices were removed from the dashboard. A restarted simulator was discovered again within the 8 s I waited (log row 24, "found" at 15:41:48.6, four seconds after the removal rows) and the header returned to "HMI OK / No alarms" with no acknowledgement step. |
| Frozen link | Freeze link on the tank in the simulator: "PLC comm loss active" banner within 4 seconds, card "No data" and "Unknown"; after unfreezing, the HMI came back online in 5 seconds, but the PLC had latched a stop in the meantime (finding 4.4 item 5). |
| Overload fault | Pump A overload toggle in the simulator: the alarm list showed "Warning Tank skid 1: Pump A overload, New" with a per-row Ack button and an amber triangle, next to the "Stop Tank skid 1: Stop latch" row marked "Acknowledged". |
| Day/outdoor theme | Key `C`: white background, dark text, dark-red STOP ALL; legible. |
| STOP ALL | Not pressed. Source evidence only (`app.c` `ui_handle_key`: key `E` is handled before any dialog or role check). |
| Window size | The window reported a 1003 by 591 pixel client area, and in my `PrintWindow` captures the drawn content filled about 810 by 470 of it. I infer the program is not DPI aware (the host scales by 125 percent) and that the capture shows its logical pixels; not confirmed. I did not test other sizes in this session (the earlier audit did, and found clipping and empty areas at the extremes: carried as that audit's finding, not re-verified). |

### 5.3 What the alarm raise looked like

When the tank passed 95 percent the HMI showed, with no interaction: a red banner under the header "Tank skid 1: Tank 01 level high-high 95.0 % (limit 95.0 %)", the two guidance steps "1. Open outlet valve V-101 to drain" and "2. Stop feed pump A if the level keeps rising", a large Acknowledge button, a header pill "1 unack." with a diamond shape, a red badge with the count on the Alarms tab, and the tank card value in red. The Alarms screen listed "Critical", the tag, the value, "New", the time and a per-row Ack button, with buttons "Reset stop", "Silence horn" and "Acknowledge all". The sound was disabled with `--no-sound`; I did not listen to the horn. Source evidence says the horn is a two-tone signal with a wired output option.

### 5.4 Interface evaluation

- Layout. A header with the product name and a device count, an HMI health pill, an alarm pill, the role pill and a red STOP ALL; a bottom tab bar with icons and a count badge; content in rounded cards. Hierarchy is clear: the alarm banner is the largest coloured area; normal state is neutral. The home page showed three sparse cards at 810 by 470 and richer cards (large number, status pill, a pressure bar with limit ticks, two buttons) after the window refreshed; the density of a card depends on the available size and on the number of devices.
- Typography. An anti-aliased Inter-style font in several sizes; numbers are large; units are set in a smaller size next to the number.
- Colour. Green for normal and running, amber for warning, red only for alarm and stop; controls are neutral grey; the stop button is red. Priority is shown by shape (diamond, triangle) and word as well as colour, so the screen survives a colour-blind reader (source evidence in README, verified by running in the screenshots).
- Navigation. Five tabs; a device opens a full faceplate with the values on the left and the controls on the right; Esc closes dialogs; one-key shortcuts (`A`, `E`, `R`, `C`, `1` to `5`).
- Feedback. A toast for sign-in; the keypad shows the range; interlocked controls say why ("Blocked: Stop latch"); a modal says which role is needed.
- Weak points seen. The device address line "127.0.0.1:1502 - BMX P..." is truncated at 810 pixels; the first screen with three devices showed three sparse cards until the layout refreshed; a device that is offline leaves the dashboard after 10 seconds along with its unacknowledged alarms (finding 4.4 item 4); the Telemetry tab is empty on Windows (earlier audit, not re-checked).
- What makes it feel more polished than ours: anti-aliased type, the card and pill vocabulary, a banner that explains what to do, per-device identity (name, address, online dot), the diagnostics screen, and explicit state words ("Running", "Stopped", "Unknown", "No data") instead of a bare value.

## 6. Hands-on: Prometheus99

### 6.1 The documented SDL tour (verified by running)

Commands (in `~/p99b` after the sync and `touch` of synced files): `make -f Makefile.linux BUILD=build-sdl -j8 hmi-sdl`, then `bash demo/linux_tour.sh build-sdl <out>`. It starts `fake_plc`, signs a demonstration configuration with `p99_sign`, starts `prometheus99-sdl` as operator with `--role operator` and drives the window with `xdotool` under WSLg. Result: ten pictures, `measure.txt`: Pss 9,137 to 9,170 kB, CPU 1.53 percent of one core during the fault and recovery part, 8 audit lines, one write (`RESERVED`, `SENT`, `CONFIRMED` for 66.1 percent, with the old value 80.8 percent and `rb=0x00000295`, the same as the raw value sent).

| Scenario | Result |
|---|---|
| Live values | Overview table of four tags with value, quality ("GOOD") and alarm column. |
| Alarm | TANK_LEVEL passed 80 percent: "ALARMS: 1 UNACK" in red in the header, "HI UNACK" in red in the row, and an event list "TANK_LEVEL RAISED" at the lower right. Later "TANK_LEVEL ESCALATED". |
| Acknowledge | ACK ALL: "ACKNOWLEDGED" in green, the event list "TANK_LEVEL ACKED"; the audit file got `ALARM_ACK role=operator action=ACK_ALL result=GRANTED`. |
| Write | Setpoint panel with `-`, `+` and SEND: "WRITE #1: CONFIRMED" in green; audit `RESERVED`, `SENT`, `CONFIRMED` in the order that proves the audit record comes before the send. |
| Disconnect and restore | `kill -USR1` on the fake PLC: header "PLC: DOWN", every tag "---" with quality "BAD COMM", the event list "PLC LINK RAISED"; `kill -HUP`: "PLC: HEALTHY" again within the 8 s wait; Pss and CPU as above. |
| Viewer | In the viewer run the `-`, `+` and SEND buttons are greyed, the ACK ALL button is greyed and the line next to them reads "ACK DISABLED: viewer role" (picture 10). The same picture shows an alarm already in "RTN UNACK" (amber word in the alarm column) and a five-line event list, so the return-to-normal-unacknowledged state is already visible as a word on the overview; what item 5 still lacks is guidance text, shape and a banner. |
| Observation | Picture 03 of the tour shows the Overview screen although the script had tapped ALARMS 1.7 s earlier. Pictures 06 (Trends) and 07 (Settings) show the right screens, so the tour's first tab tap seems to have missed or the screen switch was slow; the cause was not investigated. Labelled: observation, cause unknown. |
| Role line | The role line says "Role: operator, set when the program was started, NOT authenticated" in amber because the tour starts with `--role` and no users in the configuration (login is off when the configuration has no users, `hmi_auth.h`). The login panel itself is exercised by `ui-host` and `ui-shots` (section 9), not by this tour. |

### 6.2 What an operator sees

The overview is a table: tag name, value with unit, quality, alarm word. State is carried by colour (green quality, red alarm, amber role line), and by the words "HI UNACK", "BAD COMM", "PLC: DOWN". The trends screen draws a min and max band for one tag with a period selector and one line of help text. The settings screen is a list of text lines. All text is one font size family in one bit per pixel, in eleven colours. The screens contain no icon, no card, no bar and no mimic. An operator who knows the tags can read it; a visitor cannot grasp the machine state from it at a glance.

### 6.3 Cross-driving the two simulators (verified by running)

| Direction | Command and result | Cost |
|---|---|---|
| Our HMI, his simulator | `plc_simulator_cli --add tank --port 1502`; our configuration `config/hmi.example.json` with host 127.0.0.1 and port 1502 made by `demo/make_demo_config.sh` and signed with `p99_sign`; `prometheus99-sdl ... --role operator --report`. Result: the HMI connected (`connects_ok=1`), `polls_ok=64`, TANK_LEVEL showed 72.1 percent (his level), and the two tags whose addresses his tank does not map (BATCH_COUNT at input 100, FLOW at holding 10) came back as Modbus exceptions: `exceptions=7`, quality "BAD PLC-EXC", `protocol_errors=0`. TEMP read his "Pump A speed" register as 145.0 C and raised a HIHI alarm: a mapping mismatch, not a defect. | None: our `holding 0` at scale 0.1 matches his tank level register by coincidence. A real mapping needs a hand-written configuration with his register table (`plc_models.c` `sim_type_info_t`). His simulator's Windows window cannot be driven from the command line. |
| His HMI, our fake PLC | `fake_plc 60 100 1` (prints its port) and `industrial_hmi --headless --no-config --no-discovery --plc 127.0.0.1:<port>@tank --no-sound ...`. Result over 15 s: the device attached as "Tank skid 1", `ok165` polls, `to0` timeouts, `fieldbus_loop_hz=1001`; our fake PLC counted 318 requests and 15 writes (his HMI's heartbeat writes). Our fake PLC answers function codes 3, 4, 6 and 16 only (source evidence, `fake_modbus_server.c:80-103`): no coils and no FC 43 identity, so his discovery cannot find it and the explicit `--plc ...@tank` form is required. Whether his tank driver's coil reads received exceptions was not examined; the run showed no timeouts and no alarm. | One command line. His tank driver reads holding registers that fit our register image. |

So the two simulators are interoperable at the protocol level, which gives a cheap external check of each HMI's Modbus client against an independent server. It is not a replacement for either side's own tests.

## 7. Feature matrix

"Stronger" is a judgement on the function and its evidence, not on appearance. Evidence labels as in the introduction.

| Feature | Prometheus99 | Teammate's HMI | Stronger | Evidence | Recommendation |
|---|---|---|---|---|---|
| Modbus TCP client | libmodbus, bounded timeouts, back-off, link-health state machine | Own codec, strict frame checks, FC 01, 03, 05, 06, 43; 74 fault-injection checks | Tie: ours has the broader and maintained library, his has the denser malformed-frame tests | Source; test (`test_comms.c`, his `test_modbus_faults.c`, 74 checks reproduced) | Keep libmodbus; adapt his malformed-frame cases for the pty RTU fake (existing handoff item) |
| Modbus RTU | Yes, libmodbus RTU, pty fake slave | No | Ours | Test-only: `rtu` 33 and `rtu-e2e` 17 checks pass; never on a serial line | Keep; finish fault tests; see section 13 recommendation 10 for the settings screen |
| Several devices and discovery | One PLC link | Up to 8, FC 43 discovery, driver modules, per-device cards | His | Source; verified by running (three devices found, removed and rediscovered) | Later and bounded (MASTER_PROMPT_FINAL phase 3 item 8, roadmap P2-2); no active scan by default |
| Tag model | u16, i16, u32, f32, word order, scale, offset, quality, staleness | 64 tags of 16 bits; floats saturate (`device_manager.c:137`) | Ours | Source; test (`test_decode.c`, `test_tags.c`) | Keep; add a test that a value outside 16 bits is never clipped (section 13 recommendation 7) |
| Alarm state machine | Limits, hysteresis, delays, escalation, comm loss, RTN_UNACK, acknowledge what the operator saw | Per-point NORMAL, UNACK, ACKED, RTN_UNACK; warn and trip; deadband; guidance | Ours for timing and semantics; tie on states | Source; test (`test_alarm.c`) | Keep |
| Alarm presentation | Red word in a table column, header count, event list; "RTN UNACK" in amber as a word | Banner with title, value, limit, two-step guidance, big Acknowledge; pill with diamond; tab badge; per-row Ack; shape and word per priority | His | Verified by running (both) | Phase 3 item 5 expanded (section 11) |
| Writes | Role at submit and send, range and rate checks, audit before send, read back, unknown outcome | Role in UI only, range in keypad, echo check, log after | Ours | Source; test (`test_writes.c`, `e2e` 81) and verified by running (audit order `RESERVED`, `SENT`, `CONFIRMED`) | Keep; show refusal reasons as he does |
| Refusal and interlock explanation | A result line ("WRITE #1: CONFIRMED", "ACK DISABLED: viewer role"); greyed controls | "Blocked: Stop latch", role-needed dialog, keypad range text | His | Verified by running | Recommendation 3 |
| Authentication | Named users in the signed configuration, PBKDF2-HMAC-SHA-512 10,000 rounds, 5 failures then a 60 s lock that doubles, 5 minute idle logout | Four roles, one shared PIN each, 2,000 rounds, 3 failures then 30 s, elevation on demand, factory PINs in bench mode | Ours | Source (`hmi_auth.h`, `security.h`); login verified in `ui-host` (225 checks) and `test_auth.c` (67), his in `test_core` | Keep; consider adopting his "elevation on demand" (a control asks for the role it needs) as a presentation, not a change in checks |
| Audit and tamper evidence | Hash chain over the audit file; records for writes, acks, config, boot | HMAC chain over the alarm log with previous-MAC header | Tie; ours covers more event kinds | Source; test (`test_auditchain.c` 82) | Keep |
| Configuration integrity | Signed, serial against rollback, safe mode | Plain text options | Ours | Source; verified in `config-exec`, `liveness` | Keep |
| Persistence and crash recovery | Storage worker, atomic writes, rotation, kill-loop 150 rounds | One log file; discarded on removal of a device (alarms) | Ours | Source; test (kill-loop passed in the previous session; not rerun here) | Keep |
| Trends | Configurable buckets, min and max bands, 2, 4, 8 minute views, stored | 60 second sparkline for the tank | Ours | Source; verified by running (ours picture 06) | Keep; style with limit lines in phase 4 |
| PLC heartbeat | Optional register, config-gated exception, stops when the UI loop stalls | Profile-defined register; the simulated PLC latches a stop when it stops (verified by running) | Tie | Source; test (`e2e`, his integration) | Keep; copy the "stop latch active" explanation text idea |
| STOP ALL | None | Always visible, never blocked, `E` key; stop latch, reset needs role, acknowledge and confirmation; not labelled as an emergency stop | His (a feature we do not have) | Source; not pressed in the run | Do not copy as written: our write path is audited and read-back; a stop command would be a new write class that needs a design and a safety statement. Candidate Addition, owner decision |
| Responsive layout | Design grid stretched to the window, six sizes checked automatically (76 pictures), safe page below 640 by 400 | Layout functions shared by drawing and hit-testing, content changes with size, small-screen paging, safe screen | Tie: ours is verified by tooling, his gives better content at each size | `ui-sizes` verified by running (section 9); his: source and earlier audit | After the font decision, vary content by size (recommendation 4) |
| Visual finish | One-bit text, flat panels, 11 colours | Anti-aliased text, cards, pills, icons, tank, bars, themes | His | Verified by running (screenshots) | Recommendations 1, 2, 4 |
| Diagnostics | Settings lines; `--report` counters on exit | Diagnostics screen: loop rate, jitter, CRC counts, per-device latency, register map with raw hex | His | Verified by running (screenshot) | Recommendation 6 |
| Simulator | Fake PLC: one register image, signal faults, no UI | Win32 simulator with models, interlocks, heartbeat latch, three faults, console build | His | Verified by running | Recommendation 5 |
| Test volume and kinds | 3,531 core + 209 view + 225 UI + 81 e2e + 17 rtu-e2e (4,063) + sanitizers + TSan + kill-loop + golden + screenshots; 12,872 test lines | 11,513 + 8,999 + 558 + 74 + 1,243 (22,387, with reruns); 2,832 + 414 test lines | His in assertion count; ours in kinds of test and in test code volume | Verified by running (both), section 9.5 | Keep; no weakening |
| Footprint | Stripped static musl 329,904 bytes, static data 377,640; lite 276,640 and 298,032; LVGL heap 49 KB used, 74 KB max in the harness | Static musl 260,304 stripped, static data 157,400; dynamic musl 215,096; Windows exe 352,768 | His for the drawing runtime | Verified by running | Do not trade function for bytes; the gate is the owner's |
| Idle cost, headless | 0.45 percent of a core, 604 kB Pss, 4 threads | 3.45 percent, 1,292 kB Pss, 3 threads (1 kHz loop) | Ours for CPU, ours for memory in this run | Verified by running, builds and workloads differ | Keep event-driven loop |
| Windows | Demo only (GDI, fake PLC); device program is Linux | Native exe, installer script | His | Ours: verified in the previous session; his: built and run here | Out of scope for the device brief |
| Delivery hygiene | No keys in the source tree (a demo key sits in an ignored build folder) | `hmi.key`, `alarms.log` and `.exe` files in the tree | Ours | Source (file listing) | Keep |
| Honest limits | README and handoffs list what is unverified | README has a "not implemented" column and a status section | Tie | Source | Adopt the table form in our threat model (phase 5) |

## 8. UI and UX evaluation

### 8.1 Screens compared

| Task | Prometheus99 | His project |
|---|---|---|
| See machine state | `src/ui/ui_overview.c`: a four-column table (tag, value, quality, alarm word); `src/ui/hmi_ui.c`: header with link, heartbeat, alarm count, role | `app.c` and `renderer/layout.c` `hmi_layout_*`: device cards (name, address, online dot, one headline value, status pill, bar), header with health pill, alarm pill, role pill, STOP ALL |
| Notice an alarm | Red "ALARMS: 1 UNACK" in the header, red word in the row, event list at the lower right | Full-width banner with guidance, header pill with a diamond, tab badge with the count |
| Acknowledge | ACK ALL in the header; per-row ACK on the alarm screen | Acknowledge in the banner, per-row Ack, Acknowledge all |
| Operate a value | Setpoint panel: `-`, `+`, SEND on the overview; result line | Faceplate controls, keypad with "Now 65 % - allowed 0 to 100", confirm step for critical actions |
| Understand a refusal | One line (for example "ACK DISABLED: viewer role") | Role dialog naming the role needed, "Blocked: <reason>" on the control, "Out of range" in red in the keypad |
| See history | Trends screen with min and max band; alarm journal lines | History tab of the alarm screen; 60 s sparkline |
| Diagnose | Settings: PLC line, writes, configuration, storage, clock, tag counts, claimed role | Diagnostics: loop rate and jitter, integrity, per-device latency and counts, register map |
| Sign in | Login panel with user buttons and a keypad, only when users exist (`ui_login.c`) | Role dialog (`R`) and keypad |
| Theme | One dark theme | Dark and outdoor theme (key `C`, button in Settings) |

### 8.2 Design assessment (his project)

- Hierarchy: the banner and the pills carry the state; the cards are quiet. An operator sees "1 unack." in the header, a banner telling what to do, and a red value. Verified by running.
- Density: sparse at the first screen, dense in the faceplate and the diagnostics. A fixed card size leaves empty space at large sizes (earlier audit, not re-checked).
- Typography: Inter-style anti-aliased type in several sizes, with the value in the largest size.
- Colour: a single rule: green normal, amber warning, red alarm and stop only; grey controls. Meaning does not rest on colour alone.
- Navigation: shallow (five tabs, one dialog level), with keyboard shortcuts.
- Cost: his fonts and the renderer are 26,077 and 42,538 bytes of object text and data (section 9). A comparable set in LVGL costs more flash and the gate has no room (recommendation 1).

### 8.3 Design assessment (our project)

- Strengths: every screen is checked at six sizes and 76 pictures for overlaps, cut text and off-screen objects (`ui-sizes`); the palette check keeps colour meaning; the login, write and acknowledge results are always shown as a line; the view model (`hmi_view.c`) lets wording be tested without LVGL (209 checks).
- Weaknesses: the overview is one table; there is no object that represents a machine; alarm severity is a colour and a word with no shape; guidance text for the operator does not exist; there is no diagnostics view; the one-bit font and eleven colours are the limit, not LVGL. Our screenshot palette check (`shot_tool.py check`, seen in the `ui-sizes` output) allows 0.0 percent of pixels outside the palette on most screens and 1.5 percent on the trends chart, so anti-aliased text would fail it as written; that check is a policy, not a technical limit.
- Where the architecture is stronger but the experience weaker: writes, configuration and storage are better than his, and the screens do not show it. The operator never sees that a write was read back or that the configuration was signed, except in two lines of text.
- Where we over-engineered: the three documents with phases, the lite profile and its RAM limit, and the palette discipline are the main items (section 13, "what to remove or simplify").
- Where the implementation is incomplete: guidance text, shape-coded severity, the diagnostics view, alarm history as its own view, the CONFIG view for an RTU configuration (handoff section 4), and the tour's first tab tap.

## 9. Verified test results and measurements, with the exact commands

All verified by running in this session unless noted. The first block is the Windows side; everything else ran in WSL2 Ubuntu 24.04 (gcc 13.3.0, musl-gcc for the static builds). Single runs.

### 9.1 Prometheus99 (WSL `~/p99b`)

Sync, then touch, because `rsync -a` keeps the Windows file times (the handoff's method): `rsync -a --exclude .git --exclude "build*" --exclude Prometheus99_Submission --exclude evidence --exclude "bench/results" /mnt/c/Users/Admin/Desktop/prometheus99-main/PROMETHEUSS/ ~/p99b/` and `touch src/ui/*.c src/ui/*.h src/hmi/*.c src/hmi/*.h src/app/*.c tests/hmi/*.c`. Suites ran one after another from `scratchpad\verify.sh`.

| Command | Result |
|---|---|
| `make -f Makefile.linux -j8 test-fast` | exit 0; core total 3,531 checks (auditchain 82, auth 67, rtu 33 among them), 0 failed |
| `make -f Makefile.linux test-view` | 209 checks, 0 failed |
| `make -f Makefile.linux test-config-golden` | 58 configurations written out and read back identically, 0 failed |
| `make -f Makefile.linux e2e` | 81 checks, 0 failed |
| `make -f Makefile.linux rtu-e2e` | 17 checks, 0 failed |
| `make -f Makefile.linux ui-host` | 225 scenario assertions, 0 failed; touch to first pixels n=120 min 1.4, median 24.6, p95 34.2, max 36.3 ms; 120 touches, 0 wrong; table identical after 40 rounds; LVGL heap used 49 KB, max 74 KB; 9 allocations, 9 frees after the UI build |
| `make -f Makefile.linux ui-shots` | 24 pictures, palette check PASS |
| `make -f Makefile.linux ui-sizes` | 76 pictures, layout rules and palette check PASS |
| `make -f Makefile.linux liveness` | PASS (clean exit on SIGTERM against a hung PLC; STOPPING=1 sent) |
| `make -f Makefile.linux cli-exec` | PASS |
| `make -f Makefile.linux config-exec` | PASS |
| `make -f Makefile.linux warnings` | 0 warnings in own files, 36 in third-party headers |
| `make -f Makefile.linux LITE=1 BUILD=build-lite ui-host` | exit 2: 147 assertions, 1 failed: `FAIL line 1548: mon.max_used / 1024 <= POOL_PEAK_MAX_KB`; LVGL max used 58 KB against the limit 56 KB. Same failure the handoff records, not caused by this audit |
| `bash bench/gate.sh build-musl` | PASS: stripped 329,904 bytes (limit 330,000), static data 377,640 (limit 440,000), no Argon2id code, no shared library |
| `bash bench/gate.sh build-musl-lite lite` | PASS: stripped 276,640 (limit 290,000), static data 298,032 (limit 350,000) |
| `bash demo/linux_tour.sh build-sdl <out>` | ten pictures, Pss 9,137 to 9,170 kB, 1.53 percent CPU during fault and recovery, 8 audit lines (section 6.1) |

Not run in this session: the sanitizer and thread-sanitizer builds, the kill-loop, the Windows build, the emulated Pi and the soak (run in the previous session; see the changelog).

### 9.2 His project (scratch copies)

| Command | Result |
|---|---|
| `cd plc-simulator && make && make test` (WSL `~/fb`) | `plc_simulator_cli` built with `-Wall -Wextra -Wshadow -Werror`; 1,243 passed, 0 failed (his README says 1,243 in the table and 1,229 in the simulator README: the two documents disagree) |
| `cd hmi-runtime && make` | built, 270,528 bytes, no warnings (`-Werror`) |
| `cd hmi-runtime && make test` | `test_core_san` 11,513 passed; `test_core_mcu` 8,999 passed; `test_integration` 558 passed; `test_modbus_faults` 74 passed; all 0 failed, under AddressSanitizer and UBSan |
| `build.bat` in `plc-simulator` and `hmi-runtime` (Windows, MinGW gcc, `PATH=C:\msys64\ucrt64\bin` only) | `plc_simulator.exe` 78,336; `plc_simulator_cli.exe` 46,592; `industrial_hmi.exe` 352,768 bytes, build 20261102.210637, no warnings. Note: with `C:\msys64\usr\bin` on the PATH, `cmd` resolves to an MSYS file and the batch file cannot start |
| `make STATIC=1` (glibc) | 1,280,856 bytes (1,153,896 stripped); linker warning "Using 'dlopen' in statically linked applications requires at runtime the shared libraries from the glibc version used for linking" (his X11 loader) |
| `make CC=musl-gcc` | 249,576 bytes unstripped, 215,096 stripped, dynamically linked against musl; `size`: text 188,746, data 16,722, bss 137,120 |
| `make CC=musl-gcc STATIC=1` | 306,240 bytes, 260,304 stripped; text 232,019, data 16,328, bss 141,072 (static data 157,400) |

### 9.3 Measurements side by side (headless, 20 seconds after a 3 second start, `/proc`)

| Metric | Prometheus99 (static musl, `--fb none`, our fake PLC) | His (dynamic glibc, `--headless`, his tank simulator) |
|---|---|---|
| Stripped size | 329,904 bytes (gate 330,000) | 260,304 static musl; 215,096 dynamic musl; 1,153,896 static glibc |
| Static data (data + bss) | 377,640 bytes | 157,400 bytes (static musl) |
| Pss | 604 kB | 1,292 kB |
| VmHWM | 608 kB | 1,296 kB |
| Threads | 4 | 3 |
| CPU over 20 s | 9 ticks = 0.45 percent of one core | 69 ticks = 3.45 percent of one core |
| Fieldbus loop | event driven | 1,001 Hz, 3 late iterations in 15,003 loops (`--report`) |

These are not like-for-like: his figure is a glibc dynamic build against a tank simulator at one poll cadence, ours a static musl build against our fake PLC at another. The result agrees in direction with the earlier audit (idle CPU about one fifth for ours; resident memory in the same range), and I do not claim more. The memory column does not include a framebuffer or an LVGL heap for either (ours is headless).

### 9.4 Drawing code and fonts (`size` on single objects, musl-gcc `-Os -ffunction-sections`)

| Object | text | data |
|---|---|---|
| His `font_inter.c` (anti-aliased fonts) | 26,077 | 256 |
| His `micro_renderer.c` | 42,122 | 416 |
| His `layout.c` | 8,998 | 0 |
| Our `p99_font.c` (one bit) | 1,574 | 104 |
| Our `p99_font_18.c` | 2,060 | 104 |
| Our `p99_font_24.c` | 3,037 | 104 |

Our fonts total 6,671 bytes of text. The LVGL library itself is not in this comparison.

### 9.5 Size of the code and the tests (lines, `cat | wc -l`)

| | Product | Tests |
|---|---|---|
| Prometheus99 | 13,263 lines in `src/app`, `src/hmi`, `src/ui` (not counting `src/ui/font` and third-party code) | 12,872 lines in `tests/hmi` (C, shell, Python) |
| His HMI | 14,698 lines in `hmi-runtime/src` and `include` (not counting the generated `font_inter.c` or the icon header) | 2,832 lines in `hmi-runtime/tests` |
| His simulator | 2,065 lines | 414 lines |

His check counts (13,388 distinct, 22,387 with the small-device rerun) against ours (4,063 in the suites listed above) are not a measure of coverage: many of his checks are table-driven loops. The earlier statement that his test density is higher per line holds for the number of assertions executed; it does not show that more behaviour is covered. Ours has more kinds of test (thread sanitizer, kill-loop, golden files, screenshots, layout at six sizes).

### 9.6 Cross-driving (section 6.3)

`cross.sh` in the scratchpad: our `prometheus99-sdl` against his `plc_simulator_cli --add tank --port 1502`, report `polls_ok=64 exceptions=7 connects_ok=1`; his `industrial_hmi --headless --no-config --no-discovery --plc 127.0.0.1:<port>@tank` against our `fake_plc 60 100 1`, report `device0=Tank skid 1|...|tank|state2|ok165|to0`, 318 requests and 15 writes at our fake PLC.

## 10. Strengths, weaknesses and decisions for the owner

### 10.1 Prometheus99: strengths

1. Trustworthy writes: the role is checked in the core at submit and at send, the audit record precedes the send, and the value is read back and the outcome (confirmed, rejected, unknown) is recorded. Verified by running in the tour (`RESERVED`, `SENT`, `CONFIRMED` with `rb` equal to the raw value) and by `e2e` (81 checks).
2. Configuration integrity: a signed configuration with a serial number, safe mode with a stated reason, an on-device editor behind a root helper. Verified by running (`config-exec`, `liveness`) in the previous session and by the signed demonstration configuration in this one.
3. Typed tags (u16, i16, u32, f32, word order, scale, offset), quality and staleness, so a value is never silently clipped at 16 bits.
4. Two transports (TCP and RTU) behind one tag model.
5. Test breadth: sanitizers, thread sanitizer, kill-loop, golden files, screenshot palette check, layout check at six sizes.
6. Honest records: a changelog with a measured effect for each change, a preservation matrix, and written limits.
7. Alarm engine with on and off delays, escalation, acknowledge-what-you-saw, and an RTN_UNACK state that survives a long outage (verified by running in the tour: "RTN UNACK" visible after recovery).

### 10.2 Prometheus99: weaknesses

1. The screens do not show the machine: a table with no device object, no guidance, no shape-coded severity (verified by running).
2. A single PLC link and no device concept (source evidence).
3. No simulator with behaviour and no way to inject faults except by signals to a single-client helper (source evidence).
4. No diagnostics view and no register view; the facts are in `--report` output and in logs.
5. The binary gate has 50 bytes of room and the lite RAM check fails (58 KB against 56 KB): both block visual work (handoff section 6).
6. The unfinished RTU follow-ups: the CONFIG view's PLC target edits host and port and was not examined for an RTU configuration (changelog C21, "Not done").
7. Eleven colours and a one-bit font are policy limits that the brief did not require.
8. The tour's first tab tap did not show the Alarms screen in picture 03 (cause unknown).

### 10.3 His project: strengths

1. Operator vocabulary: priority by shape and word, banner with two guidance steps, role dialog, keypad range text, "Blocked: <reason>" on controls, state words. Verified by running.
2. Many device types with a driver-module pattern and plug and play (nine modules, discovery by FC 43), verified by running for three devices.
3. A simulator with process models, interlocks, a heartbeat latch and three fault injections. Verified by running.
4. Test density: 11,513, 8,999, 558, 74 and 1,243 checks reproduced here, run under ASan and UBSan.
5. Small, toolkit-free drawing runtime with portable Windows and Linux shells (352,768-byte Windows exe built here).
6. Standards table that states what is not implemented.

### 10.4 His project: weaknesses (a polished look is not proof of strength)

1. Role check in the UI thread only; the fieldbus thread does not check (source evidence).
2. 16-bit tags; saturating conversion (source evidence).
3. No read-back, no unknown outcome; audit written after the event (source evidence; the signed log row appeared after the write).
4. Unacknowledged alarms of a removed device are discarded (section 4.4 item 4, verified by running and source).
5. No configuration integrity; shared role PINs; factory PINs in bench mode; `hmi.key` and executables in the delivered folder.
6. Active scanning is available and risky on a live network, but it is not on by default: the built-in targets are loopback only (`hmi_discovery_add_defaults`, `src/fieldbus/discovery.c:162-165`: `127.0.0.1:502` and `127.0.0.1:1502-1509`), and a subnet is probed only when `--scan` names one. This corrects the earlier audit's finding F-5 ("active network discovery on by default"), which the source does not support.
7. No RTU, no function 16; Modbus coverage is four function codes plus FC 43.
8. `app.c` at 2,000 lines holds the UI state machine, the dialogs and command dispatch; the renderer and fonts are hand-built, so every visual change touches generated and hand-written tables.
9. The simulator's fault injection exists only in the window application.

### 10.5 Decisions needed from the owner (not taken in this audit)

1. **Colour and font policy.** May the screens use an anti-aliased font and more than eleven colours? This needs either a gate move or code removed elsewhere. Measured costs in section 9.
2. **Lite RAM limit.** Raise `POOL_PEAK_MAX_KB` from 56 to 64 KB, or make the login panel lighter (handoff section 6 item 1). Unchanged from the handoff.
3. **A STOP ALL command.** Whether a software stop that writes to the PLC belongs in this product at all. His project has it with an explicit "not an emergency stop" statement. It would be a new write class.
4. **Simulator location.** Whether a simulator tool lives in this repository (outside the device binary and outside the gate) or stays an external tool.
5. **Several devices.** Whether it is in scope before the finale (MASTER_PROMPT_FINAL phase 3 item 8 says only after everything else).

## 11. Roadmap integration

### 11.1 Which document is authoritative

Three documents carry phases or item ids, and they are not one numbering.

- `docs/MASTER_PROMPT_FINAL.md` ("Final master prompt: controlled optimization and improvement of Prometheus99 (phases 0 to 5)"): phase 0 baseline, phase 1 low-risk evidence-first, phase 2 measured performance, phase 3 architecture and the roadmap features (items 1 to 8), phase 4 UI side, phase 5 closing quality work and validation. `docs/HANDOFF_2026-11-02_SESSION2.md` section 9 follows these phases and calls this file "the brief". I treat it as authoritative for the feature, UI and quality work in this report.
- `docs/OPTIMIZATION_ROADMAP.md` ("Optimization roadmap: a small binary and a process under 1 MB", phases 0 to 7: measure, build flags, slim LVGL, runtime RAM diet, password hash, feature profiles, gates, validate on target). It is the size and memory plan, carried out on 2026-10-23, with the gate decisions in its section 6 and a note for C18. I treat it as authoritative for size, memory and gate decisions only. It carries no item ids such as P1-1: I searched it and found none.
- `C:\Users\Admin\Desktop\prometheus99-comparative-audit\IMPROVEMENT_ROADMAP.md` (earlier audit): items P1-1 to P5, cited inside MASTER_PROMPT_FINAL as "roadmap P1-3" and so on. Those ids are quoted below exactly as MASTER_PROMPT_FINAL writes them.

The two phase lists use the same words with different meanings (OPTIMIZATION_ROADMAP "Phase 4: the password hash" against MASTER_PROMPT_FINAL "Phase 4: UI side"; both have a "Phase 3"), so every reference below names the document. This ambiguity is not resolved here; I recommend the owner either keep naming the document in every reference, or add a one-line index at the top of each file (a documentation change that renumbers nothing).

### 11.2 Audit Findings that map to the roadmap

| # | Audit Finding | Source in this report |
|---|---|---|
| F1 | The screens are correct but plain; an operator cannot read machine state at a glance | 5, 6, 8 |
| F2 | Alarm banner, guidance and shape-coded severity exist in his project and not in ours; RTN_UNACK already exists in our engine and shows as a word | 5.3, 6.1 |
| F3 | Refusals and interlocks are explained on his controls and not on ours | 5.2, 8.1 |
| F4 | The palette and the one-bit font are policy limits; the gate has 50 bytes of room; his fonts compile to 26,077 bytes | 9, 10.5 |
| F5 | His simulator has models and faults; ours is a signal-driven helper | 4.3, 5.1 |
| F6 | The two simulators already interoperate with the other project's HMI over Modbus TCP | 6.3 |
| F7 | His design discards unacknowledged alarms with the device; ours does not | 4.4 |
| F8 | His design has no core role check, 16-bit tags and no read-back | 4.4 |
| F9 | A diagnostics view exists in his project and not in ours | 5.2, 8.1 |
| F10 | The lite RAM check fails and the CONFIG view for RTU was not examined | handoff section 6, changelog C21 |

### 11.3 Improvement Tracks

Broad areas, not phases. A track can contain items from several phases.

- **Track A: Operator understanding** (alarm presentation, refusal reasons, machine overview, diagnostics).
- **Track B: Visual system** (fonts, palette, themes, size-dependent content).
- **Track C: Simulation and demonstration tooling** (control channel, process model, interop checks).
- **Track D: Runtime strength to keep** (writes, configuration, alarms, tags: protect with tests, do not regress).
- **Track E: Footprint, gates and decisions** (gate, lite RAM, size room).
- **Track F: Documentation and delivery** (standards table, index of roadmaps, hygiene).

### 11.4 Recommended Roadmap Adjustments

Every adjustment names an existing phase and item and keeps its id.

| # | Existing reference | Adjustment |
|---|---|---|
| A1 | MASTER_PROMPT_FINAL phase 3 item 5 (roadmap P2-3) | Fits, needs the item expanded: add the alarm banner with guidance text, shape-coded severity, and an explicit statement that RTN_UNACK already shows as a word. Keep the existing sentence "Do not change severity meanings". |
| A2 | MASTER_PROMPT_FINAL phase 4, "Tier 2 visuals ... in this order" and the paragraph "Visual finish observed in the friend's build" | Move one decision earlier: the colour and font policy (finding F4) must be decided before the first visual, because it changes how the screenshot palette check is written. The item order of the list stays as written. |
| A3 | MASTER_PROMPT_FINAL phase 4 Tier 2 item "touch targets ... a pressed state and a confirm step for writes" | Fits, expanded by one sentence: show the reason when a control is blocked or a write is refused (F3). |
| A4 | MASTER_PROMPT_FINAL phase 4 Tier 2 items "per-tag bars with limit marks" and "a mimic page with one tank" | Fits as written; add a size-dependent content note from the responsive item (phase 3 item 6, "consider replacing the non-uniform stretch"). |
| A5 | MASTER_PROMPT_FINAL phase 3 item 6 (roadmap P4-5) | Done in C20; the remaining clause "consider replacing the non-uniform stretch with a uniform scale and extra room" should move to after A2, since content by size depends on the font decision. |
| A6 | MASTER_PROMPT_FINAL phase 3 item 7 (module boundaries, roadmap P4-4) | Unchanged. Not on the critical path of any audit finding. Keep size-neutral. |
| A7 | MASTER_PROMPT_FINAL phase 3 item 8 and roadmap P2-2 (several devices) | Unchanged: later. His device manager is a reference, with a thread per slot and no active discovery, as the existing ADAPT row says. |
| A8 | MASTER_PROMPT_FINAL phase 5 item 1 (documents, IEC 62443 mapping, threat model) | Fits: use his "implemented / not implemented" table form. Add the roadmap index note of 11.1. |
| A9 | MASTER_PROMPT_FINAL phase 5 item 2 (final validation) and section 13 (demo) | Fits: the demo script in section 13 would gain from a simulator with a control channel (Candidate Addition C-1). |
| A10 | OPTIMIZATION_ROADMAP section 6 (decisions) and `docs/HANDOFF_2026-11-02_SESSION2.md` section 6 item 1 | The lite RAM decision is a blocking item for CI (the CI workflow runs the lite harness step); it moves ahead of all visual work. |
| A11 | MASTER_PROMPT_FINAL section 11 (ADOPT, ADAPT, REJECT) | Add rows only as notes: "refusal reasons on controls: ADAPT", "diagnostics view: ADAPT", "removal of alarms with the device: REJECT". No existing row changes. |

No item is unnecessary on the evidence of this audit. Item 3 (RTU) and item 4 (heartbeat) are done; item 1 (audit chain) and item 2 (login) are done.

### 11.5 Candidate Additions (unapproved; no id assigned)

| Id-less candidate | Suggested placement | Why |
|---|---|---|
| Simulator control channel and tank process model, test-only | next to MASTER_PROMPT_FINAL section 13 (demo and evidence package), or phase 5 item 2 | scripted fault injection and a demo that does not depend on signals (F5) |
| Read-only diagnostics page | MASTER_PROMPT_FINAL phase 4 after the Tier 2 list | F9 |
| Interop check against the teammate's simulator, documented as a manual step | phase 5 item 2 | F6; no code, no dependency |
| A test that an unacknowledged communication-loss alarm survives an outage longer than any timeout | phase 5 item 3 (re-check of the preservation matrix) | F7; protects a property he lacks |
| A test that a device value outside 16 bits is never clipped | phase 5 item 3 | F8; earlier audit P3-2 |
| STOP ALL (software stop) | owner decision only; no placement | not a roadmap item today |
| Elevation on demand: a control asks for the role it needs | phase 4, with the confirm step | presentation only |

### 11.6 Table: Improvement Track, finding, change, reference, priority

Priority: P0 correctness or core, P1 high value, P2 polish, P3 optional.

| Improvement Track | Audit Finding | Recommended Change | Existing Phase / Item Reference | Priority | Dependencies | Acceptance Criteria |
|---|---|---|---|---|---|---|
| E | F10 | Decide the lite RAM limit (64 KB) or lighten the login panel | HANDOFF_2026-11-02_SESSION2 section 6 item 1; OPTIMIZATION_ROADMAP section 6 | P0 | owner decision | `make -f Makefile.linux LITE=1 BUILD=build-lite ui-host` passes; CI step passes |
| E | F10 | Examine the CONFIG view's PLC target for an RTU configuration | HANDOFF_2026-11-02_SESSION2 section 4 (C21 "Not done") | P0 | none | a test in `test_hmi_view.c` or `ui_headless.c` shows an RTU configuration is neither shown nor edited as a host and port |
| B | F4 | Colour and font policy: measure a 2 or 4 bits-per-pixel set of the three existing sizes and decide | MASTER_PROMPT_FINAL phase 4 (fits; moves earlier, A2) | P1 | gate decision; lite RAM decision | measured size and pool peak in both profiles; palette check rewritten for the chosen policy; decision recorded in the changelog |
| A | F2 | Alarm banner, guidance text per tag from the signed configuration, severity by shape and word | MASTER_PROMPT_FINAL phase 3 item 5 (roadmap P2-3) (needs expansion, A1) | P1 | size room; B decision for the shape glyphs; config schema and golden file | `test-view` cases for each state; `ui-sizes` passes at six sizes; guidance text round-trips through the signed configuration; `bench/gate.sh` passes or a decision is recorded |
| A | F3 | Show why a control is blocked or a write refused | MASTER_PROMPT_FINAL phase 4 Tier 2 touch-target and confirm item (A3) | P1 | `hmi_writes` result codes (exist) | each refusal code has a wording test; `ui-sizes` passes |
| A | F1 | Machine overview objects: one card per tag group, a tank with limit marks, bars | MASTER_PROMPT_FINAL phase 4 Tier 2 "per-tag bars with limit marks" and "mimic page" (A4) | P1 | B decision; item 5 | pictures at six sizes; palette check; LVGL heap peak within the profile limits |
| A | F9 | Read-only diagnostics page from the counters the runtime already collects | Candidate Addition (after the phase 4 Tier 2 list) | P2 | `hmi_runtime` stats accessor; size room | page shows link state, poll and error counts, latency, raw register values; no lock held across LVGL calls; TSan clean |
| C | F5, F6 | Simulator control channel and a tank process model, outside the device binary | Candidate Addition (MASTER_PROMPT_FINAL section 13) | P1 for the demo | none in the device program | a script can freeze, drop, raise an exception and nudge a value; the demo of section 13 runs from one script |
| C | F6 | Manual interop check with the teammate's simulator CLI | Candidate Addition (phase 5 item 2) | P3 | his repository licence (MIT) respected if any file is copied; none is | the commands of section 6.3 produce `polls_ok` greater than 0 |
| D | F7 | Test: an unacknowledged comm-loss alarm survives a long outage | Candidate Addition (phase 5 item 3) | P2 | none | new check in `test_alarm.c` or `e2e` |
| D | F8 | Test: a value outside 16 bits is not clipped | Candidate Addition (phase 5 item 3; earlier audit P3-2) | P2 | none | new check in `test_decode.c` |
| B | F1 | Content by size: fewer rows at small sizes, richer cards at large sizes | MASTER_PROMPT_FINAL phase 3 item 6 remaining clause (A5) | P2 | B decision; the cards | `ui-sizes` pictures show different content per size and still pass the layout rules |
| B | F1 | Second palette (day theme) | MASTER_PROMPT_FINAL phase 4 Tier 2 "day and night theme" | P2 | B decision | palette check passes for both |
| F | F3 | Standards table with "not done" column; roadmap index note | MASTER_PROMPT_FINAL phase 5 item 1 (A8, 11.1) | P2 | none | documents updated; no id renumbered |
| E | F4 | Size room for everything above | HANDOFF_2026-11-02_SESSION2 section 6 item 2; MASTER_PROMPT_FINAL phase 1 ("make room") | P1 | owner decision | gate passes; slack stated |
| C | none | A software STOP ALL | no existing item | P3 | owner decision, safety statement | not specified until the decision |

## 12. Target architecture, grounded in the current code

The aim is a runtime that stays as it is and a presentation layer that explains it. Nothing below changes the runtime threads, the write path, the configuration signature, the audit chain or LVGL 9.2.2.

### 12.1 What stays

`hmi_comms.c`, `plc_libmodbus.c`, `hmi_writes.c`, `hmi_authz.c`, `hmi_auth.c`, `hmi_config.c`, `hmi_cfgsec.c`, `hmi_boot.c`, `hmi_store.c`, `hmi_auditchain.c`, `hmi_alarm.c`, `hmi_trend.c`, the HAL, the build profiles and the gates. The view model `hmi_view.c`, which already holds wording and tone decisions and is tested without LVGL (209 checks).

### 12.2 What is redesigned

- **The alarm path from engine to screen.** Today `hmi_alarm.c` produces states and `ui_overview.c` and `ui_alarms.c` print words and colours. The redesign adds, in `hmi_view.c`, one function that turns the top alarm and its configured guidance into a banner model (title, value, limit, two steps, severity word and glyph kind, state). `ui_*.c` draws it. The engine does not change; severity meanings do not change.
- **The configuration schema** gains optional fields per tag: `group` (a label such as "Tank skid", used to put tags on one card) and `guidance` (two short lines for the alarm). They are optional, so an existing signed configuration stays valid; the golden file and the signing tool need the new keys. The check that rejects unknown keys must accept them. Everything stays inside the signed file, so safe mode still covers them.
- **The overview screen** becomes a set of cards built from `group` (one card per group, falling back to one card for ungrouped tags), each with the headline value, state word, a bar with limit marks drawn as plain rectangles, and the alarm word. The table view stays available as a second page for engineers. LVGL objects are created lazily as today (`TRENDS` and `CONFIG` already are).
- **Write feedback** reuses the existing result codes of `hmi_writes.c`: one wording function per code in `hmi_view.c` and a line or a toast on the control.

### 12.3 What is added

- **A diagnostics page** in `src/ui`, fed by a read-only copy of counters the runtime already keeps for `--report` (polls, failures, exceptions, latency, writes by outcome, storage counts). The copy is taken under the existing lock in the supervisor pass, as the trend and alarm snapshots are, never inside an LVGL call.
- **A simulator tool** outside `src/`: for example `tools/sim/` with a process model for a tank (level rising and falling with a pump and a valve, the same model idea as his `plc_models.c` but written for our register image), a text control socket (`freeze`, `drop`, `exception 4`, `set <reg> <value>`, `latch`), a console front end, and the existing fake server code reused. It is test and demonstration code, not part of the device binary or the gate.
- **A font and palette decision** recorded in `src/ui/font/README.md` and `docs/SCREENS.md`.

### 12.4 What is simplified

- The two parallel phase documents get an index note (documentation only).
- The palette check becomes a check against the chosen policy rather than a fixed list of eleven colours.
- Nothing in security, correctness, reliability or test coverage is removed.

### 12.5 How modules interact

```
hmi_comms (thread) -> hmi_db (typed tags, quality) -> hmi_alarm (supervisor) -> alarm journal
                                   |                         |
                                   v                         v
                      hmi_view (wording, banner model, card model, refusal text)   [no LVGL, tested]
                                   |
                                   v
                      ui_overview / ui_alarms / ui_diag / ui_login ... (draw only)   [LVGL, UI thread]
UI actions -> hmi_authz + hmi_writes (role, range, audit, send, read back) -> hmi_comms
```

A better simulator attaches at the right edge: it speaks Modbus TCP to `hmi_comms` exactly as the fake PLC does, so nothing in the runtime changes.

### 12.6 Dependencies to add or avoid

- Add: none at run time. At development time, an offline font converter (LVGL's `lv_font_conv`, which needs Node.js) would regenerate font files; the generated C is committed, as for the current three font files, so the build does not need it.
- Avoid: his renderer, his lock-free structures, his discovery, a general widget library, any LVGL change, any new thread.

## 13. Recommendations in detail

Each recommendation states the problem, where it is, why it matters, what changes, how to implement it, what could go wrong and how to verify it.

### Recommendation 1. Decide the font and palette policy with a measurement

- **Problem.** The screens are plain because text is one bit per pixel and the whole interface is limited to eleven colours inside a 16-entry palette (`docs/SCREENS.md` lines 30 to 41, `src/ui/font/p99_font*.c` `.bpp = 1`). The screenshot check allows 0.0 percent of pixels outside the palette on most screens.
- **Where.** `src/ui/font/`, `src/ui/ui_theme.c`, `src/ui/ui_priv.h` (`C_*` colours), `tests/hmi/shot_tool.py check`, the display flush (RGB565 to a 16-entry palette in the file and framebuffer back ends).
- **Why it matters.** The experience gap to his project is mostly type quality and component vocabulary. His anti-aliased font set compiles to 26,077 bytes (measured, section 9) and his renderer to 42,538 bytes. Our three one-bit fonts compile to about 1.6, 2.1 and 3.0 KB of text.
- **What changes.** A policy: either keep one bit per pixel and improve the look only with shapes and layout, or move to a 2 bits per pixel font (four levels) at the three existing sizes, and widen the palette to the 16 entries the display path already provides (five entries are unused today).
- **How.** (1) Generate 2 bits per pixel versions of the three sizes with `lv_font_conv` and compile them in a scratch build. (2) Measure stripped size and the LVGL pool peak in both profiles. (3) Run `ui-shots` and `ui-sizes` with a palette check that tests membership of a 16-entry palette. (4) Compare pictures. 
- **What could go wrong.** The size gate (50 bytes of room): the owner has to decide. Blended edge colours must map to a 16-entry palette, so the edges may look coarse; this is untested here (inferred risk). The lite profile has no room either (pool peak already 58 KB against 56 KB).
- **How to verify.** Measured numbers in both gates, 76 pictures passing the new check, an owner review of two pictures side by side. The three existing fonts are DejaVu Sans, ASCII 0x20 to 0x7E, generated with `--bpp 1 --no-compress --no-kerning` (the `Opts:` line at the top of each `p99_font*.c`), and their object text is 1,574, 2,060 and 3,037 bytes (measured, section 9), 6,671 bytes together. Glyph bitmaps scale with bits per pixel, so at two bits per pixel the three fonts grow by at most 6.7 KB (a bound, because the table part does not grow; I expect 3 to 5 KB) and at four bits per pixel by at most 20 KB (I expect 10 to 15 KB). These are inferred bounds from the measured one-bit sizes, not measurements of converted fonts. Either way the 50 bytes of slack are far too small, so this is a gate decision.

### Recommendation 2. Alarm banner, guidance text and shape-coded severity (MASTER_PROMPT_FINAL phase 3 item 5)

- **Problem.** The alarm is a red word in a table; there is no instruction and no shape. Return-to-normal-unacknowledged is already a word ("RTN UNACK" in amber, verified by running).
- **Where.** `src/ui/hmi_ui.c` (a place under the header), `ui_overview.c`, `ui_alarms.c`, `hmi_view.c`, `hmi_config.c` and `p99_sign` (the optional `guidance` key), `tests/hmi/config_golden.*`.
- **Why it matters.** An operator learns what to do from the screen. In his run the first two steps appeared at the moment the alarm raised.
- **What changes.** A banner with title, value, limit, the two configured lines, a severity glyph and word, and an Acknowledge button that calls the existing acknowledge path with the state the operator saw.
- **How.** Add the optional keys; add `hmi_view_alarm_banner()` returning a struct of strings and a tone; draw it with existing helpers (`ui_mk_panel`, `ui_label_box`); draw the glyph from LVGL line or arc objects, or two rectangles, so no image data is needed.
- **What could go wrong.** Size (the item needs room first, as the handoff says). A long guidance text overflowing at 640 by 400; the layout check will catch it. The configuration golden file changes and must be regenerated deliberately.
- **How to verify.** `test-view` cases for each alarm state; `ui-sizes` at six sizes; a config round-trip with and without the keys; the existing alarm tests unchanged; `bench/gate.sh`.

### Recommendation 3. Say why a control is blocked or a write was refused

- **Problem.** Our line says what happened ("WRITE #1: CONFIRMED", "ACK DISABLED: viewer role"); his control says why it cannot be used ("Blocked: Stop latch") and a keypad says the allowed range.
- **Where.** `ui_overview.c` (setpoint panel), `hmi_view.c` (existing outcome wording), `hmi_writes.h` (result codes `HMI_WRS_*`).
- **Why it matters.** An operator who cannot write needs the reason without reading a manual.
- **What changes.** One wording per refusal code, shown beside the control; the allowed range and the current value shown when the setpoint is changed (the data exists in the tag descriptor).
- **How.** Extend the existing outcome wording function; add test cases per code.
- **What could go wrong.** Wording that disagrees with the code; the tests prevent that.
- **How to verify.** `test-view` for each code; `ui-sizes`; no change in `e2e`.

### Recommendation 4. Machine cards, bars with limit marks and content by size (phase 4 Tier 2, phase 3 item 6 remaining clause)

- **Problem.** There is no object that stands for a machine.
- **Where.** `ui_overview.c`, `hmi_view.c`, the `group` key of Recommendation 2.
- **What changes.** Cards by group; a bar per analog tag with limit ticks as rectangles; fewer rows at 640 by 400 and more at 1280 by 800.
- **How.** Build on `ui_mk_panel` and `ui_label_box`; use the row count from the layout mode; keep lazy creation.
- **What could go wrong.** LVGL pool peak in the lite profile; the layout check must pass at all six sizes.
- **How to verify.** Pictures at six sizes, `ui-host` heap lines in both profiles, gate.

### Recommendation 5. A simulator with a control channel (Candidate Addition, test-only)

- **Problem.** Our fake PLC has no behaviour and is steered by signals; his simulator has models and faults but only its window can inject faults, and it cannot be scripted. Neither supports a repeatable end-to-end demo with a fault sequence.
- **Where.** New `tools/sim/` (outside `src/`), reusing `tests/hmi/fake_modbus_server*.c`.
- **Why it matters.** The finale demo (MASTER_PROMPT_FINAL section 13) and the soak need scripted faults and a process that reacts to the HMI's writes.
- **What changes.** A tank model (level moves with pump and valve), a socket or stdin command set, and a stats line.
- **How.** Model after `plc_models.c` `SIM_TANK`; keep it C99 and outside the gate; do not copy his code (MIT licence would require attribution; copying is unnecessary).
- **What could go wrong.** Becoming a second product to maintain; keep it under a few hundred lines.
- **How to verify.** The demo script runs from one command; the existing `e2e` can use the same server.

### Recommendation 6. A read-only diagnostics page

- **Problem.** The facts (poll counts, latency, errors) exist only in `--report` output.
- **Where.** `hmi_runtime.c` (snapshot of counters), a new `src/ui/ui_diag.c`.
- **What changes.** A fifth tab or a Settings sub-view with link state, counts, latency and the raw register values of the polled tags.
- **How.** Copy the counters under the existing lock in the supervisor pass.
- **What could go wrong.** Lock order and size; ThreadSanitizer run is the check.
- **How to verify.** `ui-host` plus `SAN=thread` `e2e`, gate.

### Recommendation 7. Protect what we do better with two tests

- **Problem.** The properties his design lacks should not regress.
- **Where.** `tests/hmi/test_alarm.c` or `test_modbus_e2e.c` (an unacknowledged comm-loss alarm still visible after a long outage), `test_decode.c` (a 32-bit or float value above 65,535 or beyond int16 is shown exactly).
- **How to verify.** The checks fail if the property is broken; run in `test-fast`.

### Recommendation 8. Unblock the build before any visual work

- **Problem.** The lite RAM check fails and the gate has 50 bytes of room.
- **What changes.** The owner's decisions in section 10.5 items 1 and 2.
- **How to verify.** Both gates and `ui-host` in both profiles pass.

### Recommendation 9. Documentation and delivery

- **Problem.** Two phase lists share words; our README has no "not implemented" column; his folder ships a key.
- **What changes.** An index note in both roadmap files; a standards table in the threat-model document; a check that no `*.key` or `*.sig` file is in a release folder (our `.gitignore` already ignores `build-*`).
- **How to verify.** Documents read; `find . -name "*.key"` over the release folder returns nothing.

### Recommendation 10. Finish the RTU settings follow-up

- **Problem.** The CONFIG view's PLC target edits host and port; with `plc.transport "rtu"` those fields are empty (changelog C21 "Not done").
- **Where.** `src/ui/ui_config.c` lines 128 to 129 and 352 (`HMI_CE_PLC_HOST`, `HMI_CE_PLC_PORT`).
- **What changes.** For an RTU configuration show the serial device and refuse host and port edits, or show them read-only.
- **How to verify.** A harness case with an RTU configuration; the golden file unchanged.

### What to adopt (summary)

| What | Why | Where in his code | How in ours | Dependencies | Risks | How to verify |
|---|---|---|---|---|---|---|
| Banner with guidance, shape and word | operator knows what to do | `alarms.h` (`hmi_emergency_action_t`), tag descriptors `action1`, `action2`, README alarm sections | Recommendation 2 | size room; config keys | size, text overflow | section 13 item 2 |
| Refusal reasons | removes guessing | faceplate action table in `devices/*.c` (`block_tag`, `log_name`), `device_profile.h` | Recommendation 3 | none | wording drift | test-view |
| Faceplate content ideas | machine-level view | `renderer/layout.c`, `micro_renderer.c` | Recommendation 4 (idea only, not code) | font decision | pool peak | pictures, heap lines |
| Diagnostics idea | engineers see health | the Diagnostics screen in `renderer/layout.c` | Recommendation 6 | counters | lock order | TSan |
| Standards table form | honest scope | README "Standards" | Recommendation 9 | none | none | review |
| Malformed-frame cases | RTU faults | `tests/test_modbus_faults.c` | existing handoff item | pty fake | none | rtu-e2e |

### What to avoid

His renderer and fonts as code, his lock-free structures, a fixed 1 kHz loop (his run: loop 1001 Hz, 3 late iterations in 15,003 loops on Linux; idle CPU 3.45 percent of a core against our 0.45 in section 9.3), a UI-only role check, shared role PINs, 16-bit tags, removal of unacknowledged alarms with the device, factory PINs outside bench mode, a key file in a delivered folder, and active subnet scanning by default (his default is loopback only, so this is an option to leave off, not a defect).

### What to remove or simplify (never security, correctness, reliability or test coverage)

- The eleven-colour and one-bit-font limits are policy; relax them if the owner decides (Recommendation 1).
- Overlapping phase vocabularies in two documents: add an index, renumber nothing.
- The first-tab tap in `demo/linux_tour.sh` (picture 03): investigate before the finale; the script is evidence tooling.
- Nothing else qualifies on the evidence collected. In particular the lite profile, the hash chain, the signed configuration and the kill-loop stay.

## 14. Unverified findings and limits

- I could not use a physical display or a mouse. His HMI was operated by posted window messages and captured by `PrintWindow`; our SDL window by `xdotool` under WSLg through the existing tour script. No touch screen, no panel hardware.
- Not run: his prebuilt executables (I built from source), his Linux packages and installers, his Pi builds, his ESP32 port, cppcheck, Wine, QEMU for his project, his `--verify-log` against a tampered file, STOP ALL, the alarm sound on either project (his run used `--no-sound`; ours has no sound), his simulator's Error replies, Unplug and STOP buttons.
- Not re-measured: his idle CPU on Windows, his window sizes other than one, his earlier-audit figures (seqlock retries on Windows, 5.05 percent CPU), our Windows demo, our kill-loop, TSan and sanitizer builds (run in the previous session), the emulated Pi and the soak.
- All numbers are single runs on WSL2 and Windows 11, x86-64. Workloads differ between the two programs, so ratios are indicative only.
- `PrintWindow` captures show the content his program draws in logical pixels (about 810 by 470) inside a larger client area; I did not resolve whether his program is DPI aware.
- The estimate of the cost of anti-aliased fonts in our build (Recommendation 1) is inferred from one measured set of his and our measured one-bit fonts; it is not a measurement of a converted font in our build.
- The cause of the tour's picture 03 showing the Overview screen is unknown.
- The earlier audit's findings that I did not re-check are carried as that audit's claims; the ones I checked are listed in section 3.
- The working tree of our repository contains the earlier session's uncommitted changes, including the unfinished RTU settings-line fix and changelog entry C21 written before this audit began; the audit changed no source file and did not continue that work.

## 15. Evidence and file references

Ours: `src/hmi/hmi_writes.c` (`hmi_writes_submit_as`, line 218 role check at 228 and again at 348), `src/hmi/hmi_alarm.c` (RTN_UNACK at 122, 148), `src/hmi/hmi_auth.h`, `src/ui/ui_overview.c`, `ui_alarms.c`, `ui_settings.c`, `ui_config.c` (128, 352), `ui_login.c`, `hmi_view.c`, `hmi_ui.c`, `src/ui/font/p99_font*.c`, `tests/hmi/fake_plc_main.c`, `fake_modbus_server.c` (80 to 103), `demo/linux_tour.sh`, `demo/make_demo_config.sh`, `config/hmi.example.json`, `docs/MASTER_PROMPT_FINAL.md`, `docs/OPTIMIZATION_ROADMAP.md`, `docs/OPTIMIZATION_CHANGELOG.md`, `docs/HANDOFF_2026-11-02_SESSION2.md`, `docs/SCREENS.md`.

His: `hmi-runtime/src/app/app.c` (809, 872, 1138 role checks; 1146 key handling; 1384 pointer), `src/fieldbus/device_manager.c` (137 saturation, 399 to 407 writes, 530 to 548 `remove_slot`, 690 removal time), `src/fieldbus/discovery.c` (162 to 165 default targets), `src/fieldbus/modbus_proto.h` (function codes), `include/core/alarms.h`, `include/core/tags.h`, `include/core/security.h` (28 to 35), `include/devices/device_profile.h` (heartbeat at 173), `plc-simulator/include/plc_models.h`, `src/main_console.c`, `README.md`.

Scratch artefacts (not in either repository), under the session scratchpad: `friend_src\` (copy of his tree and Windows builds), `shots\` (his window captures), `ours_shots\` (our tour pictures and `measure.txt`), `cross\` (cross-drive logs), `verify_log.txt`, and the helper scripts `ui.ps1`, `fb_build.sh`, `ours_tour.sh`, `cross.sh`, `verify.sh`, `fonts.sh`, `fbmusl.sh`.
