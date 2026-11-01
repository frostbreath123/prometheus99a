# Master prompt for the hackathon finale (10 days)

This file is the brief for the coding agent. Read `README.md` first (sections 1 to 3, 10 and 11), then `docs/FINAL_REPORT.md`. Everything here was decided by the owner in a design interview. Do not reopen a decision without asking. Where this file and an older document disagree, this file wins, and the older document should be corrected.

## 1. Goal

Prometheus99 is a small industrial HMI runtime in C99 (Modbus polling, alarms, validated writes, signed configuration, LVGL screens). The owner won a hackathon and must show a finale in 10 days. The judges expect something close to a production-level product that can be scaled into a real product afterwards. The mentor's requirements must all be met:

1. Open industrial protocols only, no proprietary Schneider drivers: Modbus TCP first, then Modbus RTU. OPC UA only as an optional separate build.
2. Develop on Linux or Windows first, then validate on an embedded board. Real hardware is out of reach for now: the Linux build and the emulated Raspberry Pi (QEMU, `emulation/pi`) are the accepted targets. Label every emulated result as emulated.
3. Fast, light rendering without Qt (LVGL).
4. Measure startup time, screen render and transition time, touch latency, CPU and memory, stability in continuous operation, and recovery after PLC or network loss.
5. Basic authentication, secure configuration, input validation, communication timeouts and automatic reconnection.

Judges trust honest, measured limits more than claims. Never state a number that was not measured, and say plainly what is not proven.

## 2. Scope: implement everything except the web viewer

The web viewer (remote viewing in a browser) is out of scope. It goes on the roadmap slide only. Everything else below is in scope. Work in the tiers in section 3, finish and verify one tier before starting the next, and drop work from the bottom of the list if time runs out.

### Tier 1: must be done

1. **Authentication (named users).**
   - Named users, each with a PIN of 4 to 8 digits and a role (viewer, operator, engineer, admin).
   - The user list lives in the signed configuration as salted PIN hashes. Use the HMAC-SHA-512 code already in the program with many iterations. Do not bring back Argon2id (it was removed for memory); note it as the production upgrade in the threat model.
   - A login screen with a touch keypad and the user list. Lockout after 5 wrong tries. Automatic logout after 5 idle minutes. The user's name is written into every audit line. The core still checks the role on every action, as before.
   - The first user is created with the `p99_sign` tool, not on the device.
   - Remove the "NOT authenticated" wording and the claimed `--role` path from the product screens. Keep a clearly marked development option only if tests need it, and say so in the docs.
   - Update `docs/AUTHENTICATION.md` (it is currently the record of the removal) and add tests in the core, the view module, the UI harness and the executable tests.
2. **Modbus RTU.**
   - One PLC link per configuration, `tcp` or `rtu`, chosen in the signed configuration. RTU adds the serial device, baud rate, parity, stop bits and the Modbus unit id. Tags and alarms are unchanged.
   - Use libmodbus (`modbus_new_rtu`) in `src/hmi/plc_libmodbus.c`. Same rules as TCP: bounded timeouts, a silent line shows as "no response", reconnect with back-off, a write is never retried after a lost reply. The new part is the frame timing at the chosen baud rate.
   - Test against a fake RTU server on a pty pair (a virtual serial cable) on Linux. Also try the emulated Pi if its virtual serial port can carry it, and drop that if it costs time.
   - Regenerate the config golden file, rerun the size gate, and record the RTU limits and timings.
3. **Finish the responsive layout.** The window rebuilds itself for any size from 640x400 up to the largest laptop resolution, and the SDL window resizes live (already built, see section 4). Check every tab (Overview, Alarms, Trends, Settings, config editor, login) at 640x400, 800x480, 1024x600, 1280x800, 1366x768 and 1920x1080, fix what breaks, and add screenshot checks at those sizes to the tests. Check the framebuffer backend at a size other than 800x480 in the emulated Pi.
4. **Evidence for the finale** (section 6).

### Tier 2: the look for Schneider

Do these in this order. Each is small. Measure the size cost of each and report it.

1. Alarm banner with severity colours, alarm groups, and blinking until acknowledged.
2. Per-tag bars with alarm limit marks, drawn as plain rectangles (no extra LVGL widget if it can be avoided).
3. Touch targets of at least 48 px at the design scale, a pressed state, and a confirm step for writes.
4. Alarm history screen (the journal already exists on disk and in the core).
5. Day and night theme: a second palette of the same 11 colours, switchable from Settings. Keep the screenshot palette check working for both.
6. Trend export to a CSV file.
7. Recipes: named sets of setpoints that are applied through the existing checked write path (`hmi_writes`), never around it. Role-checked, range-checked, audited, and shown with a confirm step.
8. A string table for all screen text so languages can be added, with English complete and a second language added if the font range allows it. Accented glyphs cost about 1 KB per font size.
9. A mimic page: one tank drawn from rectangles, filled to follow `TANK_LEVEL`, with limit marks, flow and temperature values, and pump or valve symbols that change colour with state. Which tag drives which shape must be set in the signed configuration, not hard-coded.

### Tier 3: production roadmap items that the owner now wants built

Do these only after tiers 1 and 2 are verified, and keep each behind a build flag so the small device profile stays small.

1. Several PLC devices at once, each with its own address (or serial line) and Modbus unit id.
2. Coil and bit writes (function 05), for drives and PLCs that expect a command word or bit.
3. Tag scale above 64 tags.
4. OPC UA, only as a separate optional build, never in the small device profile. If it cannot be done in the time left, write the design and leave it.

### Documents (do alongside tiers 1 and 2)

- A threat model, including the PIN limits (short PIN, HMAC-based hash, no encryption on Modbus, role claimed before login was removed).
- An IEC 62443 mapping at the level of "what we cover, what we do not".
- An SBOM and a licence review (libmodbus is LGPL, see `docs/THIRD_PARTY.md`).
- A production roadmap (what is built, what is next, what the web viewer would need).
- `GLOSSARY.md` at the repository root, created as terms are settled: **tag**, **user**, **role**, **safe mode**, **link health**, **design grid**, **stand-in device**. Keep it free of implementation detail. Offer an ADR only when a decision is hard to reverse, surprising without context and a real trade-off.
- Correct the stale text in `docs/CONFIG_INTEGRITY.md`, `docs/STORAGE.md` and `docs/SCREENS.md`, and update the README sections that this work changes.

## 3. Order of work and priorities

Authentication, then the responsive checks, then RTU, then the Tier 2 items in order, then the documents, then the evidence run, then Tier 3. Authentication and RTU are the two mentor requirements that are not met today, so they come before polish. A 4 to 6 hour emulated-Pi soak must be started early enough to finish, so schedule it two days before the finale at the latest.

## 4. Where things stand (checked in this session, not committed)

- The branch is `feature/authorization-hardening`. The last commit is `ddda58d`. A large amount of earlier work and this session's work are uncommitted.
- Authentication was removed earlier to meet the memory goal. The role is the claimed `--role`.
- The responsive layout is built and tested on Linux:
  - The display backends read the real size. `src/app/display.h` declares `p99_display_size()` and `p99_display_take_resize()`. The SDL backend (`display_sdl.c`) has a resizable window with a minimum of 640x400, a `P99_SIZE=WxH` start option, and a live rebuild through `hmi_ui_relayout()`. The framebuffer backend uses the whole framebuffer, 640x400 up to 2048 pixels wide. The shared draw buffer is a fixed 32 KB.
  - Screen files are written on an 800 x 480 design grid and mapped to the real size by `ui_x()` and `ui_y()` in `src/ui/hmi_ui.c`. The header and tab bar use computed positions. LVGL flex and grid stay off (the owner chose this own-layout approach to keep the size gate).
  - Text uses the largest of three sizes of the one font (14, 18 and 24 px, files `src/ui/font/p99_font*.c`) that fits the scale. A 32 px size was dropped to stay inside the gate.
- Linux checks after that work: core 3,275, view 208, golden identical, UI scenario 205, end-to-end 75, executable tests and liveness passed, size gate passed (stripped 321,712 of 322,000 bytes, static data 438,536 of 440,000). Live resize through several sizes ran without a crash.
- Not checked after that work: the Windows build and demo, the sanitizer builds, the lite profile, the kill-loop, the emulated Pi, Alarms/Trends/Settings at sizes other than 800x480, and the screenshots of the larger sizes (the image viewer rejected them). Treat all of these as to be done.
- Memory, as measured: program about 0.6 MB (0.55 to 0.61 MB static build, no display). Always add the framebuffer on its own line: 0.75 MB at 800x480 and 16 bits per pixel, 1.5 MB at 32 bits per pixel, and more on larger screens. The "below 1 MB" claim holds for the program only, never for the total.

## 5. Rules

- No commit, push or GitHub change unless the owner asks.
- No real PLC connection and no real control writes. Use the fake PLC and the fake RTU server. The Schneider lab and a real Raspberry Pi are out of scope for now.
- Do not weaken role checks, input validation, auditing, write protections or the configuration signature to save size or time.
- The portable core in `src/hmi` stays independent of LVGL and of the operating system. All OS access stays in `hmi_platform_*.c`.
- Keep the size and RAM gates (`bench/gate.sh`). Every added feature gets its size measured. Where a gate must move, say so, give the measured numbers, and ask the owner before changing the limit. Prefer a build flag, a lite profile, or savings elsewhere (a `pow` lookup table is about 8 KB, README section 12, item 7).
- `config/lv_conf.h` is a dependency of the LVGL objects in `Makefile.linux` and `Makefile.windows`, so a change rebuilds them. A first-run segfault of the SDL build during this work was caused by a missing allocation in `display_sdl.c`, not by a stale build.
- Every size, speed and memory claim must be measured and labelled with what it was measured on (PC, emulator). Never claim a build, test, screenshot or measurement that was not run.
- Report documentation conflicts instead of choosing silently. Update documents only with verified facts.
- Repository files use LF line endings. Edit with the file tools, not shell heredocs or `sed` with backslashes (the environment mangles them). On Windows, scripts that write files must not turn LF into CRLF.
- On Windows, build LVGL from a short path (for example `C:\p99`) because of the 260-character limit. Linux builds and QEMU run in WSL2 Ubuntu 24.04. A work tree copy in the Linux home folder (`~/p99`) is faster and gives real file permissions (the configuration key file must not be group- or world-accessible, which `/mnt/c` cannot provide).
- Run the Linux test suites one after another, not with `make -j` across them (a known flake in `config-exec`).
- Chat replies to this owner are terse ("caveman" style). Saved files use plain, full sentences.

## 6. Evidence to show at the finale

- All test suites green on Linux and Windows, the size and RAM gates passing, and the sanitizer builds and the kill-loop rerun on the final tree.
- A soak of 4 to 6 hours in the emulated Pi with injected PLC and network faults (not 72 hours), with memory over time, labelled "emulated".
- The seven measurements from the mentor's list, as a table with how each was measured: startup time, render and transition time, touch latency, CPU, memory (program and framebuffer separately), stability, and recovery after PLC or network loss. Include RTU recovery.
- Screenshots at the six sizes in Tier 1, item 3, and a short screen recording of live resizing.
- An honest list of what is not proven: nothing ran on real hardware, a real PLC or a real touch screen; the emulated Pi is a generic ARM virtual machine; soak length; PIN strength; no encryption on the field bus.

## 7. Demo script (about 5 minutes)

Start `demo/run_linux_sdl.sh`. Show live values and the green link. Log in as an operator. Show an alarm raised, the banner, and acknowledgement. Show the trend screen and the alarm history. Send a setpoint and show the read-back result. Apply a recipe. Cut the fake PLC link with `kill -USR1`, show the red link, restore it with `kill -HUP`, and show the automatic recovery. Run the same on the RTU configuration. Edit the configuration without signing it and show safe mode. Drag the window from 640x400 to full screen. Close with the memory table (program, framebuffer, total) and the limits.

## 8. First thing to do in a new session

```sh
git status && git log --oneline -3
```

Read the files named at the top, then confirm the plan with the owner before writing code. Start with Tier 1, item 1 (authentication). Use the `tdd` skill for the core logic where it fits, and finish each item with the tests, the size gate and a short note in the README.
