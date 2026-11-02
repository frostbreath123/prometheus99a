# Prometheus99

Prometheus99 is a small industrial HMI (human-machine interface) runtime written in C99. It reads a PLC over Modbus TCP and shows live values, alarms and trends on a touch screen. A permitted operator can acknowledge alarms and write set-points. The device program is one static file of about 318 KB (350 KB for 64-bit ARM) and uses about 0.6 MB of memory. It is built for a Raspberry Pi or a similar Linux board with a framebuffer display, and it also runs as a Windows demo window and a Linux desktop window.

Authors: Team Doomsday (Abhilash L, Adarsh Abhilash, Nikhil Nuguri). Licence: MIT (`LICENSE`). Third-party components: `docs/THIRD_PARTY.md`.

**Status: a working, presentation-ready prototype, not a production product.** Everything was run on a development PC (Windows 11 with MSYS2, Ubuntu 24.04 under WSL2) and in an emulated ARM computer, always against a simulated PLC. Nothing has run on a Raspberry Pi, a real touch panel, a real PLC or real systemd.

This file is also the handoff for a new person or a new coding agent. Section 12 lists what is left to do, and section 11 lists the rules to follow. Read sections 1 to 3 first.

## 1. What exists, in one table

| Target | Program | Size (stripped) | Program memory | Verified |
|---|---|---|---|---|
| Windows demo window (Win32 + real LVGL + built-in fake PLC) | `build-windows/hmi_win_demo.exe` | 304,640 B | 3.5 MB private working set (an empty Windows window is 1.43 MB) | builds, tests pass, scripted tour with 10 screenshots |
| Linux desktop window (SDL2) | `build-sdl/prometheus99-sdl` | dynamic, not a size target | about 9 MB PSS (shared libraries) | tour with xdotool under WSLg |
| Linux device program (static musl, framebuffer) | `build-musl/prometheus99` | 317,616 B | 0.55 to 0.61 MB PSS | suites, 1-hour soak, kill-loop, size gate |
| Raspberry Pi target (static aarch64 musl) | `build-aarch64/prometheus99` | 350,288 B | 0.63 to 0.68 MB PSS in an emulated guest | scripted tour in QEMU, 42-minute soak |

Display memory (the framebuffer) is outside the program: 0.75 MB at 16 bits per pixel, 1.5 MB at 32. The program plus the framebuffer is about 1.3 MB, so "below 1 MB" holds for the program only.

The deliverable folder for the Schneider submission is `..\Prometheus99_Submission` (outside this repository, built by a refresh script, see section 9). The full verification report is `docs/FINAL_REPORT.md`. The older history (milestones M1 to M4, the size work, the removal of authentication) is in `docs/history/` (`PROJECT_RESUME.md`, `ROADMAP_STATUS.md`, `ENGINEERING_REPORT.md`, `M4_HANDOFF.md`, `walkthrough.md`) and `docs/OPTIMIZATION_ROADMAP.md`; parts of those are outdated and say so.

## 2. What the program does

- **Modbus TCP polling** with libmodbus: typed tags (16 and 32 bit integers, 32 bit floats, word and byte order per tag, scale and offset), a quality for every tag (good, uncertain, bad, unavailable), merged poll groups, bounded timeouts, reconnection with back-off, and a link-health state that treats a silent PLC as "no response" even when the socket is connected.
- **Alarms:** high-high, high, low and low-low limits, hysteresis, on and off delays, acknowledgement, escalation, return to normal, and a communication-loss alarm. The journal and state are stored on disk.
- **Writes:** validated (writes enabled, tag allowed, role, range, rate limit), checked again just before sending, sent at most once, read back and verified. A lost reply is marked unknown and never retried. If the audit record cannot be stored, the write is refused.
- **Signed configuration:** HMAC-SHA-512 signature, rollback serial, PLC allow-list. An unsigned, altered or older configuration puts the device in safe mode (banner, no PLC connection, writes off).
- **Storage:** one worker thread owns every durable write (audit file, alarm journal, alarm state), with a bounded queue, atomic writes with checksums, and rotation at 1 MiB per file.
- **Screens (LVGL 9.2.2):** overview, alarms, trends, settings, a number pad for set-points, and an on-device configuration editor. The editor leaves a request that a root helper applies; the helper is the device program started under the name `p99_cfg`.
- **Roles:** viewer, operator, engineer, admin, checked in the core. **There is no authentication:** the role is the claimed `--role` given when the process starts. The audit trail names no person. See section 10.
- **Display backends:** the program draws RGB565 and converts each strip to a 16 or 32 bits per pixel framebuffer (a default Pi framebuffer works), or writes to a file (measurement build), or opens an SDL2 window, or runs without a display with `--fb none`.

## 3. Repository layout

```
src/hmi/           portable core, independent of LVGL and of the operating system
                   (tags, comms, writes, alarms, config, cfgsec, boot, store, runtime, platform_posix/win32)
src/ui/            LVGL screens, theme, the one 1-bit font, hmi_view.c (view model, testable without LVGL)
src/app/           main_linux.c, display.h + display_fbdev.c / display_file.c / display_sdl.c,
                   p99_sign.c (signing tool), p99_cfg.c (root helper, also built into the main program)
config/            hmi.example.json, lv_conf.h (LVGL reduced to object, label, button, chart)
deploy/            systemd units (written, never run under real systemd)
demo/              Windows demo source (hmi_win_demo.c, with --tour mode), run and tour scripts, ui_targets.txt
emulation/pi/      QEMU emulated Pi-class target: prepare, make_initramfs, run_qemu, drive.py, soak_emulated
tests/hmi/         unit, end-to-end, UI-harness and executable tests, fake PLC, screenshot tool
bench/             measurement, soak, recovery, size and RAM-gate scripts (results are history)
third_party/       lvgl, libmodbus (git submodules, see .gitmodules), monocypher, jsmn
docs/              FINAL_REPORT.md first; EMULATION, THIRD_PARTY, SCREENS, CONFIG_INTEGRITY, STORAGE,
                   AUTHENTICATION (record of the removal), ADR-0001; docs/history/ holds the older records
evidence/          screenshots and logs from the running programs (windows, linux, pi_emulated, ...)
sources.mk         the source lists, shared by every Makefile (add a source file here, once)
Makefile.linux     Linux build and tests          Makefile.windows   Windows tests and demo
Makefile           repository-wide checks (test-all, check-linux, clean); includes Makefile.windows
```

The legacy Windows prototype (`src/layer*.c`, `src/main.c`, `include/`, `tests/test_core.c`, `prometheus99.exe` and its batch files) was deleted in the repository cleanup. It remains in git history before that commit. The history documents (`PROJECT_RESUME.md`, `ROADMAP_STATUS.md`, `ENGINEERING_REPORT.md`, `M4_HANDOFF.md`, `walkthrough.md`) are in `docs/history/` and still mention it; they are records of earlier work, not current instructions.

## 4. Build and run

Build directories are separate per target: `build-windows`, `build-linux` (development build), `build-sdl`, `build-musl`, `build-aarch64`.

**Windows** (MSYS2 UCRT64 with `mingw-w64-ucrt-x86_64-gcc` and `make`; build from a short folder path, because LVGL's nested include paths exceed the 260-character path limit otherwise):

```sh
mingw32-make BUILD=build-windows build-windows/hmi_win_demo.exe     # or demo\run_live.bat
mingw32-make BUILD=build-windows test-hmi test-view test-config-golden ui-host build-windows/test_e2e.exe
powershell -ExecutionPolicy Bypass -File demo\win_capture.ps1 -Exe build-windows\hmi_win_demo.exe -Out evidence\windows
```

The demo opens a window: click like a touch screen, F1 help, F2 cut or restore the PLC link, F3 cycle the claimed role, Esc closes. `--tour DIR` runs the scripted tour and saves pictures.

**Linux** (Ubuntu 24.04: `build-essential pkg-config libmodbus-dev libsdl2-dev musl-tools python3 xdotool`):

```sh
make -f Makefile.linux STATIC=1 CC=musl-gcc BUILD=build-musl hmi              # the device program
make -f Makefile.linux BUILD=build-sdl hmi-sdl && bash demo/run_linux_sdl.sh   # live window (WSLg or X11)
bash demo/linux_tour.sh build-sdl evidence/linux                               # scripted tour, pictures and measure.txt
```

**Raspberry Pi target in an emulator** (QEMU, see `docs/EMULATION.md` for what it proves):

```sh
make -f Makefile.linux STATIC=1 CC=/opt/toolchains/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc BUILD=build-aarch64 hmi
bash emulation/pi/prepare.sh && bash emulation/pi/make_initramfs.sh
bash emulation/pi/run_qemu.sh window            # visible emulated screen; or: headless, then drive.py for the tour
```

**Run the device program** (needs a signed configuration):

```sh
p99_sign keygen --key config.key
p99_sign sign --config hmi.json --key config.key        # again after every change
prometheus99 --config hmi.json --audit audit.log --alarms alarms.bin --config-key config.key \
  --config-state config.serial [--role viewer|operator|engineer|admin] [--fb /dev/fb0|none] [--touch /dev/input/event0]
```

## 5. Tests (all pass on the committed tree; run the Linux ones one after another)

```sh
make -f Makefile.linux test test-view test-config-golden ui-host ui-shots e2e cli-exec config-exec cfgedit-exec liveness
make -f Makefile.linux killloop KILL_ROUNDS=150
bash bench/gate.sh build-musl full                       # size and RAM gate
```

- `make test` runs the core tests under AddressSanitizer and UBSan (build directory `build-linux-asan`); `make test-fast` runs them without instrumentation. With `STATIC=1` or `SAN=...`, `test` is the plain run in that build. Figures after phases 1 and 2 (2026-11-02, uncommitted work): core 3,307 checks, stripped 313,520 bytes, static data 373,416 bytes, idle 0.33 percent CPU and 81 thread wake-ups per second, write latency median 7.8 ms; see `docs/OPTIMIZATION_CHANGELOG.md`. `make -f Makefile.linux write-latency` measures the write latency. Phase 3 (uncommitted): the audit file is a keyed hash chain (`p99_sign verify-log`, `docs/STORAGE.md`), named-user PIN login (`p99_sign add-user`, `docs/AUTHENTICATION.md`), and Modbus RTU (`plc.transport "rtu"`, `make -f Makefile.linux rtu-e2e`). RTU is in the default device program since the owner moved the full gate to 330,000 bytes on 2026-11-02 (C18); `RTU=0` builds a TCP-only device program. An optional PLC heartbeat (`plc.heartbeat_register`, C19) writes a counter once a second while the screen loop runs (`docs/CONFIG_INTEGRITY.md`). `make -f Makefile.linux ui-sizes` checks every screen and the login panel at 640x400, 800x480, 1024x600, 1280x800, 1366x768 and 1920x1080 (nothing off the screen or its panel, no overlapping or cut text, palette) and the safe "screen too small" page shown below 640x400 (C20).
- Linux: core 3,275 checks (includes `test_boot`), view 208, config golden identical, UI scenario 205 assertions, end-to-end 75, 23 screenshots checked against the palette, the executable tests, the config helper tested through the single-file program (a link named `p99_cfg`), kill-loop 150 rounds.
- Windows: core 3,253 checks, view 208, golden identical, UI scenario 205, end-to-end 75.
- Size gate (full profile, with RTU, 2026-11-02): stripped 329,904 bytes against a limit of 330,000 (moved from 322,000 by the owner for RTU); static data 377,480 against 440,000. **The stripped margin is 96 bytes;** adding code fails the gate unless room is made elsewhere.
- A parallel `make -j` run of the Linux suites failed `config-exec`: three scripts shared port 5502. Fixed in the uncommitted phase 1 work (each script has its own port); the three CI jobs then passed locally with `-j4`. The CI workflow has still never run on GitHub. `make -f Makefile.linux warnings` checks the own sources with strict warning flags.
- Not run on the final tree: the sanitizer builds (AddressSanitizer, UBSan, ThreadSanitizer), the lite profile (`LITE=1`) and its gate.

## 6. Measurements (host and emulator only; details in `docs/FINAL_REPORT.md`)

- Start to first frame: 9 ms median warm on Linux; Windows window created in about 47 ms.
- Rendering: `lv_timer_handler` mean 116 us (Linux), p99 1.9 ms. Screen transitions 2 to 3 ms median. Touch press to first pixels 8.8 ms median (Linux), p95 about 32 ms (the 33 ms refresh tick).
- CPU: 0.4 to 0.8 percent of one core (Linux, 4 tags); 1.5 to 2.5 percent for the Windows demo during its tour.
- Recovery: PLC silent, link loss shown after 4.1 s and returned 3.4 s after the PLC answered; connections dropped, loss after 1.2 s and return 0.43 s, with bounded retries (49 requests in 25 s).
- Continuous operation: 1 hour on x86 with 11 injected faults (no crash, memory flat after the first minutes); 42 minutes in the emulated Pi with 7 faults, memory 641 to 677 KB. Not a 24-hour or 72-hour soak.
- Baselines before the size work: the Linux product was about 695 KB plus three shared libraries and 1,422 KB PSS without the framebuffer; the Windows demo exe was 523,264 bytes.

## 7. What was done in this work (chronological)

1. **Audit and baseline.** Reran the existing suites; confirmed the Windows and Linux numbers; found one parallel-run flake.
2. **Windows demo.** Shrunk the exe from 523 KB to 305 KB (compile without `-ffunction-sections`, `--gc-sections`, no unwind tables, static link). Added the `--tour DIR` mode (scripted clicks through the real pointer input and the fake PLC, BMP pictures of the window buffer) and `demo/win_capture.ps1`. Screen copying captured the lock screen, so the demo saves its own picture.
3. **Linux window.** New SDL2 backend (`hmi-sdl`) with a SIGUSR1 screenshot hook; `demo/run_linux_sdl.sh`, `demo/linux_tour.sh` (xdotool clicks, signals to the fake PLC).
4. **Raspberry Pi target in QEMU.** Static aarch64 musl build with the musl.cc cross compiler; Alpine arm64 virt kernel and modules, busybox initramfs, virtio GPU (800x480, 32 bits per pixel), virtio tablet, user network to a fake PLC on the host; `drive.py` taps through QMP and takes screendumps. Soak run for 42 minutes.
5. **Display driver.** The old LVGL fbdev driver was replaced by `display_fbdev.c`: draws RGB565, converts to 16 or 32 bits per pixel, writes rows with `pwrite()` (no framebuffer mapping), centres on larger framebuffers, refuses other depths, and accepts `--fb none` for headless runs. The 16 bits per pixel branch has not run on any framebuffer.
6. **One-file device program.** The root helper `p99_cfg` is now a mode of `prometheus99` (started under the name `p99_cfg`, build flag `P99_MULTICALL`), which removed a 96 KB second file. The helper's large buffers moved to the stack to stay inside the static-data gate.
7. **Defects found by running and screenshots.** The "no active alarms" text drew over the first alarm row (fixed); `--fb /dev/null` tests broke when the driver became strict (tests now use `--fb none`, plus two new refusal tests); static data overflowed after the merge (fixed); a background run was killed by a terminal hangup (restart detached); a tour tap was lost once under load (a press that began and ended between two input reads; now latched in the SDL and Windows backends).
8. **Measurements and soaks** on all targets (section 6), with definitions that keep the framebuffer, shared libraries and the OS separate and disclosed.
9. **Architecture and code-quality review**, then the changes it asked for:
   - display backends split into files behind `display.h` (a real seam: three adapters);
   - `hmi_boot.c`: the signed-config-or-safe-mode decision moved out of `main()` into one tested module (`tests/hmi/test_boot.c`, 13 checks);
   - `hmi_view_alarm_row()` moved the alarm row composition into the view module (11 new checks); the alarms screen only draws;
   - the mirror flag `s_none_shown` was deleted (the widget's hidden flag is the single source);
   - `sources.mk` holds the source lists; `Makefile.windows` is a real file; the `Makefile` includes it and keeps only the repository-wide checks;
   - `demo/ui_targets.txt` holds the touch-target coordinates for all three tour drivers; `demo/make_demo_config.sh` builds the demo configuration;
   - stale comments corrected, `.gitignore` extended.
10. **Questions answered for the owner** (no code): the final memory and storage figures per target (section 1); whether the program fits the usual HMI problem list (it is strong on footprint and crash tolerance; it does not address tag scale beyond 64, operator authentication, encryption, OPC UA, web viewing or real-hardware validation); why the Windows demo cannot go below 1 MB (the OS floor); why program plus framebuffer cannot reach 1 MB at 16 bits per pixel; the git commands to push; how to run on Linux; how to measure memory (`grep -E "^(Pss|Rss):" /proc/<pid>/smaps_rollup`).
11. **Reports and delivery.** `docs/FINAL_REPORT.md`, `docs/EMULATION.md`, the evidence folders, a refresh script for the submission folder (binaries verified bit-identical to a clean rebuild), and this README.

## 8. Decisions already made by the owner (do not reopen without asking)

- Authentication, the login, the user store and the Argon2id hash were removed to meet the memory goal (`docs/AUTHENTICATION.md`). The role is the claimed `--role`.
- The device build is static musl, `-Os`, link-time optimisation, with the LVGL configuration cut down (one 1-bit font, an 11-colour palette, 20-line draw buffer).
- A real Raspberry Pi is too expensive for now; an emulator is the accepted substitute, labelled as emulated.
- Modbus RTU ships in the device program (2026-11-02: the full size gate moved to 330,000 bytes for it). OPC UA is not decided; the recommendation is a separate optional build.
- The Windows demo is a demonstration, not the product, and is not held to the 1 MB target (the OS floor is 1.43 MB).

## 9. Environment notes (what cost time)

- **WSL2 Ubuntu 24.04 is used for Linux and QEMU.** The work tree is a copy at `/root/work/p99` (rsync from the repo, excluding `.git`, `build*`, `evidence`); the emulation files are in `/root/work/pi`; the aarch64 compiler is `/opt/toolchains/aarch64-linux-musl-cross`. The first start of WSL after a Windows restart can hang; `wsl --shutdown` or a Windows restart fixes it.
- **Windows path limit:** building LVGL from a deep folder fails with `lv_conf_internal.h: No such file`. Use a short path such as `C:\p99`.
- **Shell quirks:** heredocs and `sed` replacements with backslashes or `\n` get mangled in this environment; write scripts and patches with a file-writing tool, not shell heredocs. A `sed` delimiter that also appears in the replacement breaks the command. A `pgrep -f` pattern can match the command that runs it (a wait loop never ended for that reason).
- **Background runs from a terminal need `setsid nohup`**, or the terminal hangup kills QEMU.
- **The Windows desktop was locked during much of the work:** the programs write their own pictures, so no screen capture is needed, but nothing was watched live.
- **Safety check:** the tooling refuses `rm -rf` on top-level folders. Two temporary build copies, `C:\p99win2` and `C:\p99win3`, still exist; delete them by hand.
- The submission folder is rebuilt by a refresh script kept outside the repository (it copies `src`, `config`, `deploy`, `demo`, `emulation`, `tests/hmi`, the used part of `third_party`, selected docs, the binaries with `SHA256SUMS`, and the evidence). If it is lost, recreate it from the layout in `..\Prometheus99_Submission\README.md`.
- Installing the `improve-codebase-architecture` skill put `.agents/`, `.claude/skills/` and `skills-lock.json` in the repo; they are ignored by `.gitignore`.

## 10. Known limitations and risks

1. **No authentication.** Anyone who can start the process or edit the service unit chooses the role. Modbus TCP is unencrypted. There is no threat model, no IEC 62443 mapping, no SBOM and no legal licence review. The static build puts libmodbus (LGPL-2.1 or later) in the executable, so a delivered device needs the relinking material described in `docs/THIRD_PARTY.md`.
2. **Nothing ran on target hardware.** Unproven: the 16 bits per pixel framebuffer branch, touch calibration, readability of the 14 px 1-bit font on a real 800x480 panel, finger-sized touch targets, real timings and memory on a Pi, real systemd and the root helper under systemd, power loss, SD card behaviour, a real PLC (word order, timeouts), a full disk.
3. **Scale:** the limit is 64 tags (32 in the lite profile); tens of thousands of tags are not supported. Modbus TCP and Modbus RTU exist (RTU tested on a pseudo-terminal only, never on a serial line); there is no OPC UA. There is no web client.
4. **The size gate has almost no margin** (329,904 of 330,000 bytes stripped with RTU; static data 377,480 of 440,000).
5. **CI has never run** (`.github/workflows/ci.yml`, `.gitmodules`). It uses parallel `make -j4`, which can hit the `config-exec` flake.
6. **Soak length** is 1 hour (x86) and 42 minutes (emulated), not days.
7. **The emulated Pi is a generic ARM virtual machine** with a generic kernel, not Raspberry Pi OS or a Pi kernel; its CPU speed is not reported.
8. **Documents with stale text:** `docs/CONFIG_INTEGRITY.md` and `docs/STORAGE.md` still describe logins and accounts from an earlier design (a note at the top says so); several source comments point to documents that are not in the submission folder; the documents in `docs/history/` still describe the deleted legacy prototype and the old login design.
9. The header counter `HB nn` on Linux and the Pi is a deliberate liveness indicator but looks like a debug label.

## 11. Rules for whoever continues

- No commit, push or GitHub change unless the owner asks. (Everything up to commit `ddda58d` was committed by the owner; check `git status`.)
- No real PLC connection and no real control writes. Use the fake PLC. (The owner now wants to connect to the Schneider lab equipment, section 13. That changes this rule only for the scope the owner states in writing: start with read-only, `writes_enabled` false. Never start a real motor or drive without the owner and the lab supervisor present and a hard limit in the configuration.)
- Report documentation conflicts instead of choosing silently; update documents only with verified facts. Never claim a build, test, screenshot or measurement that was not run.
- Repository files use LF line endings. The portable core in `src/hmi` stays independent of LVGL and of the operating system (all OS access is in `hmi_platform_*.c`).
- Chat replies to this owner are written in a terse "caveman" style; saved files use plain full sentences.
- Do not weaken role checks, input validation, auditing, write protections or the configuration signature to save size.

## 12. What is left (suggested order)

0. **The owner's current goal is section 13 (the Schneider lab panels).** Start there; the items below remain open but are secondary.

1. **Run the CI once** (push a branch and watch the three jobs); fix environment problems; consider serial `make` for the executable tests.
2. **Rerun what last ran before the final changes:** the sanitizer builds (`SAN=address`, `SAN=thread` with `setarch x86_64 -R`), the lite profile (`LITE=1`, `bench/gate.sh build-musl-lite lite`), and the kill-loop on the final static build.
3. **Raspberry Pi bring-up (M5), blocked on hardware:** build natively or with the aarch64 compiler, set up `/dev/fb0`, run on the real panel (this exercises the 16 bits per pixel branch), run under systemd with the units in `deploy/`, repeat the `bench/` measurements on the board, record them in `docs/LINUX_VALIDATION.md`, and run a 72-hour soak with faults and a power-cut test.
4. **Authentication decision** and the M8 documents: threat model, IEC 62443 mapping, SBOM, licence review, version 0 tag. A PIN or role-select with the choice recorded in the audit trail is the cheapest option.
5. **Modbus RTU** (about 2 to 3 days: `modbus_new_rtu` branch in `src/hmi/plc_libmodbus.c`, new config keys, regenerate the config golden file, an RTU fake on a pty pair, rerun the size gate). **OPC UA** only as a separate optional build.
6. **Remaining items from the architecture review:**
   - Unify the three tour timelines (the touch targets are already shared in `demo/ui_targets.txt`; the timelines differ per target because the Windows tour drives the PLC directly).
   - Move more screens to the view module: 47 direct `ui_rt->` uses remain in `src/ui`; only the alarm rows were moved to `hmi_view_alarm_row()`.
   - Derive `r->used` and `r->btn_on` in `ui_alarms.c` from the widgets, like `s_none_shown` was.
7. **Further size work if a gate needs room** (none is required today): a lookup table for `pow` (about 8 KB), the trend drawn as bars with `LV_DRAW_SW_COMPLEX 0` (9 to 15 KB), a shared configuration scratch buffer (about 20 KB of touched memory, needs a race test), non-PIE static (rejected: it removes address randomisation).
8. **Documents:** rewrite or retire the stale passages in `docs/CONFIG_INTEGRITY.md` and `docs/STORAGE.md`; the documents in `docs/history/` may be left as records.
9. **A Windows live look:** run `bin\windows\Prometheus99_Demo.exe` on an unlocked desktop and check readability by eye; the same for the Linux window and the QEMU window mode.

## 13. Next goal: the Schneider HMI lab panels (added after the work in section 7)

**What the owner wants.** To get this software onto the five HMI panels of the Schneider Electric HMI lab and have it run there well. The screens must adapt to any screen size by themselves and be fast and easy to use. This section records what was found and the agreed plan. None of it has been started: no code was written for it.

**What the lab photos show** (read from the labels; verify them on site):

| Panel | Setup | Protocol |
|---|---|---|
| 1 | HMIST6600 (about 10 inch, looks 4:3) to M251 PLC and ATV340 drive; the HMI also acts as a web server | Modbus TCP |
| 2 | HMIST6600 to a TM3 bus coupler (TM3BCEIP) and an ATV630 drive, logic in a soft PLC (EAE); push buttons and lamps | Modbus TCP |
| 3 | HMI is an OPC UA server, with a drive and motor behind a PLC | OPC UA (and Modbus TCP behind it) |
| 4 | HMI is an OPC UA client to an M241 PLC; a dashboard with gauges | OPC UA |
| 5 | HMIST6400 (about 7 inch, wider) to a PAS600 panel server and two EM6400 energy meters over RS-485; FTP logging | Modbus TCP |

- IP addresses on the labels: HMIs 192.168.10.1, .10, .20, .30 and .40; the Ethernet switch .11; the TM3 coupler .12; the PAS600 .41; the M241 looks like .31 (hard to read).
- The panels run Schneider's own runtime, configured with EcoStruxure Operator Terminal Expert. The assistant believes, but has not verified, that a native C/LVGL program cannot be installed on them. **Step 1 of the plan is to confirm this with the lab staff or the HMIST6 datasheet.** If it cannot run there, the realistic route is a stand-in device (a laptop, a Raspberry Pi or an industrial PC) on the lab network that talks to the real PLCs, drives and meters.
- What the program can reach today: Modbus TCP through libmodbus, so panels 1, 2 and 5. It cannot reach panels 3 and 4 (OPC UA is not implemented).

**What must change in the code (the gaps found)**
1. **Layout is fixed at 800 x 480** with absolute positions (tab bar y = 62, setpoint panel at the bottom, columns at fixed x). It does not adapt. Needed: compute every position from the real width and height at start-up (the display backend already reads the framebuffer size and the program centres 800 x 480 on a larger one; that is not responsive), add two or three larger font sizes (only one 14 px 1-bit font exists), and add screenshot checks at several sizes (suggested: 800x480, 800x600, 1024x600, 1280x800). LVGL's flex and grid layouts are switched off in `config/lv_conf.h` to save size; either switch them on and measure the cost, or write a small own layout function. A quick alternative (scale one fixed picture) shows borders on 4:3 and was judged not good enough.
2. **One PLC endpoint only.** The configuration has one `plc.host`. The lab needs several devices at once (PLC, drive, meter, panel server), each with its own address and Modbus unit id.
3. **Writes are holding-register writes only.** Drives and PLCs often expect a command word or a coil/bit write (function 05). Needs checking against the ATV340 and ATV630 manuals.
4. **Tags and register maps.** Every panel needs a configuration with the real register addresses, types and scaling. These come from the lab's PLC projects (EcoStruxure Machine Expert or EAE) and the drive and meter manuals; the assistant does not have them.
5. **Optional, large:** OPC UA for panels 3 and 4, only as a separate optional build (open62541 alone is bigger than the whole current program).

**Plan (in order)**
1. **Confirm, half a day:** can the HMIST6600/6400 run third-party software? Does it have a browser? Which register maps exist? Is the owner allowed to connect and, later, write? Which panel first? (Suggested first target: panel 5, because it only reads energy values.)
2. **Stand-in device on the lab network, 1 to 2 days:** a laptop on the lab switch in the 192.168.10.x subnet runs the Linux window build (or a Windows build), connected read-only to the real PLC and meters. Show live values.
3. **Responsive UI, 3 to 5 days:** item 1 above, with screenshot checks at the sizes listed, and a layout that handles both 4:3 and 16:9.
4. **Multi-device and bit writes, about 3 days:** items 2 and 3 above, with tests against an extended fake PLC that serves several unit ids.
5. **Supervised writes** (start, stop, speed reference) only after the owner and the lab supervisor approve, with an emergency stop at hand and a hard speed limit in the configuration. `writes_enabled` stays false until then.
6. **Optional later:** a web version served from a PC if the panel has a browser; OPC UA as a separate build.

**Rules that apply here:** every size and speed claim must be measured; keep the device program under the existing gates (`bench/gate.sh`, margin is thin: 317,616 of 322,000 bytes stripped); do not weaken the role checks, audit trail or signed configuration; no commit or push unless the owner asks.

## 14. First thing to do in a new session

```sh
git status && git log --oneline -3          # expect a clean tree at ddda58d or later
make -f Makefile.linux test test-view ui-host e2e   # a quick Linux sanity check (WSL or Linux)
mingw32-make BUILD=build-windows test-hmi test-view  # a quick Windows sanity check
```

Then read `docs/FINAL_REPORT.md`, pick an item from section 12, and ask the owner before committing or pushing.
