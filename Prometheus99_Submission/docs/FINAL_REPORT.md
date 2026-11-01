# Prometheus99: final verification report (2026-10-24)

This report states what was built, run and measured on 2026-10-23 and 2026-10-24, and what was not. Every figure comes from a run of the real programs on the development PC (Windows 11 with MSYS2, WSL2 Ubuntu 24.04 on x86-64, and an aarch64 guest under QEMU). Nothing ran on a Raspberry Pi, a real display or touch panel, a real PLC, or real systemd. Nothing was committed or pushed.

## 1. Result by requirement

| Requirement | Status | Evidence and limits |
|---|---|---|
| Windows demo with a visible window | VERIFIED (program level) | `bin/windows/Prometheus99_Demo.exe` opens an 800 x 480 window with the real LVGL screens and runtime. A scripted tour clicks through every screen and saves the pictures the window held (`evidence/windows`). I did not watch it on screen: the desktop was locked during the work. |
| Native Linux version that builds and launches | VERIFIED (window build), PARTIALLY VERIFIED (device build) | The SDL2 window build ran under WSLg and was clicked with xdotool (`evidence/linux`). The device build writes to `/dev/fb0`; it ran only on the emulated display (32 bits per pixel). Its 16 bits per pixel branch has not run on any framebuffer. |
| Separate Raspberry Pi-targeted build with an emulated environment and a visible display | VERIFIED (aarch64 in QEMU), BLOCKED (real Pi) | The static aarch64 musl binary runs in an emulated Cortex-A53 guest with a virtual display and touch device, talking Modbus TCP to the fake PLC over a virtual network (`evidence/pi_emulated`). The GTK window mode starts and renders (checked with a screendump) but I did not watch it. It is not Raspberry Pi OS and not a Pi kernel; `docs/EMULATION.md` says what it proves. |
| Runtime memory below 1 MB | VERIFIED for the program, NOT MET if the display memory is counted, NOT MET for the Windows demo | The program uses 0.55 to 0.68 MB (section 4). The display memory belongs to the kernel and is 0.75 MB at 16 bits per pixel or 1.5 MB at 32; with it the total is about 1.3 MB. The Windows demo uses 3.5 MB; an empty Windows window already uses 1.43 MB on this PC. |
| Storage below 500 KB | VERIFIED | Windows demo 304,640 bytes; Linux x86-64 program 317,616 bytes; aarch64 program 350,288 bytes (section 3). |
| Polished, clear interface that keeps the functionality | PARTIALLY VERIFIED | All 29 screenshots from the three targets were reviewed; no overlap, clipping or artefacts remain after the fix in section 6. Readability on a real panel and finger-sized touch targets are unverified. |
| Existing functionality preserved | VERIFIED | Section 5: the full suites pass on Windows and Linux after every change. |
| Modbus through the real implementation, not injected values | VERIFIED | The programs use libmodbus over TCP to a fake PLC that is a separate TCP server (a thread in the Windows demo, a separate process on Linux and in the emulation). The PLC request counters in the evidence files show the traffic. |

The product is a presentation-ready prototype, not a validated production deployment. It has no authentication (the role is the claimed `--role`), Modbus TCP is unencrypted, and the configuration signature protects only the configuration file.

## 2. Build and run

Dependencies: MSYS2 UCRT64 (`mingw-w64-ucrt-x86_64-gcc`, `make`) for Windows; Ubuntu 24.04 or similar with `build-essential pkg-config libmodbus-dev libsdl2-dev musl-tools python3 xdotool` for Linux; `qemu-system-arm qemu-system-gui curl squashfs-tools cpio kmod` and an aarch64 musl cross compiler (https://musl.cc/aarch64-linux-musl-cross.tgz) for the emulation. Build from a short folder path on Windows (the 260-character path limit breaks LVGL's nested includes).

| Target | Build directory | Build | Run |
|---|---|---|---|
| Windows demo | `build-windows` | `mingw32-make BUILD=build-windows build-windows/hmi_win_demo.exe` (or `demo\run_live.bat`) | double-click `bin\windows\Prometheus99_Demo.exe`; `demo\win_capture.ps1` runs the scripted tour and saves pictures |
| Linux window (SDL2) | `build-sdl` | `make -f Makefile.linux BUILD=build-sdl hmi-sdl` | `bash demo/run_linux_sdl.sh` (live) or `bash demo/linux_tour.sh` (scripted tour) |
| Linux device program | `build-musl` | `make -f Makefile.linux STATIC=1 CC=musl-gcc BUILD=build-musl hmi` | `prometheus99 --config hmi.json --audit audit.log --alarms alarms.bin --config-key config.key --config-state config.serial --fb /dev/fb0 --touch /dev/input/event0` |
| Raspberry Pi-targeted (aarch64) | `build-aarch64` | `make -f Makefile.linux STATIC=1 CC=<aarch64-linux-musl-gcc> BUILD=build-aarch64 hmi` | emulated: `bash emulation/pi/prepare.sh`, `bash emulation/pi/make_initramfs.sh`, `bash emulation/pi/run_qemu.sh window` |

The demonstration sequence (normal operation, changing PLC values, a high alarm, acknowledgement, an authorised setpoint write that is read back and confirmed, trends, settings, the PLC link cut, recovery, a viewer who cannot write) is the scripted tour in all three targets: `demo/win_capture.ps1`, `demo/linux_tour.sh`, `emulation/pi/drive.py`. The fake PLC is `tests/hmi/fake_plc_main.c` (signals: `SIGUSR1` drops the connection, `SIGUSR2` hangs, `SIGHUP` restores). Configurations: `config/hmi.example.json`; `demo/make_demo_config.sh` makes the demo variant. `--fb none` runs the program without a display.

## 3. Storage (bytes, stripped)

| Item | Windows demo | Linux x86-64 | aarch64 (Pi-targeted) |
|---|---|---|---|
| The program | 304,640 | 317,616 | 350,288 |
| Shared libraries needed | none (Windows system DLLs only) | none (static musl) | none (static musl) |
| Configuration, signature, key | not used (built in) | about 1.4 KB (1,269 + 88 + 32) | about 1.4 KB (1,269 + 88 + 32) |
| Signing tool (workstation, not on the device) | not used | 71,808 | 79,864 |
| Deployment package | 304,640 | about 319 KB | about 352 KB; 432 KB with the signing tool |

The program contains the C library, libmodbus, the LVGL widgets used, the one 1-bit font and the root helper (started under the name `p99_cfg`, which makes a second file unnecessary). Not counted: the operating system, the emulator, the kernel, and the framebuffer. Baselines: the Linux product before the 2026-10-23 size work was about 695 KB plus three shared libraries; the Windows demo was 523,264 bytes when first linked statically on 2026-10-24.

## 4. Memory

Definitions. Linux and the emulated guest: PSS from `/proc/<pid>/smaps_rollup` (the headline figure), measured after the first frame and again over time. The device program does not map the framebuffer; it writes each finished strip with `pwrite()`. The x86 measurement build maps a file as its framebuffer, so its mapping (752 KB) is reported on its own line. Windows: private working set (the "Memory" column of Task Manager) and private bytes.

| Measurement | Linux x86-64 (host, measurement build) | aarch64 guest (emulated, device program) | Windows demo |
|---|---|---|---|
| After the first frame, process only | 552 KB | 633 KB | 3,392 KB private bytes at start, 3,908 KB after the UI is built (UI harness) |
| After 5 minutes | 592 KB | 641 to 661 KB | 3,492 to 3,528 KB private working set (28 s tour, two runs) |
| Over the long run | 1 hour: first 1,324, last 1,360 KB with the 752 KB framebuffer mapping, so 572 to 608 KB for the process; 4 threads, 6 file descriptors | 2,520 s: 641 to 677 KB (492 samples), 5 to 6 file descriptors | not run |
| Display memory (outside the process) | 752 KB file mapping (16 bits per pixel) | 1.5 MB in the kernel (32 bits per pixel, 800 x 480) | in the Windows window system |
| LVGL pool in use | 48 KB of 101 KB after start; 74 KB peak (UI harness) | same code | same code |

Headline: the device program uses 0.55 to 0.68 MB on Linux and the emulated ARM guest, below 1 MB. The program plus the display memory is about 1.3 MB at 16 bits per pixel, which is not below 1 MB. The Windows demo is not below 1 MB: an empty 800 x 480 Win32 window measured 1,432 KB private working set and 2,024 KB private bytes on this PC before any of this program's code ran. The demo also carries a screen bitmap for the Windows window and the fake PLC threads, which the device does not. Baseline of the device program before 2026-10-23: 1,422 KB (after the first frame, without the framebuffer).

## 5. Performance, reliability and tests

All figures are host figures; the emulated guest runs on an emulated CPU, so its speed is not reported.

| Measure | Linux x86-64 | Windows |
|---|---|---|
| Start to first drawn frame | cold 21 ms; warm median 9 ms (min 8, max 13; 14 runs) | window created about 47 ms after launch (median of 7 runs) |
| Rendering (`lv_timer_handler`, scripted scenario) | mean 116 us, p50 13 us, p99 1.9 ms, max 3.1 ms | mean 180 to 199 us, p99 2.9 to 3.0 ms, max 9.8 ms |
| Screen transition, press to last pixels | median 2.0 ms, p95 67 ms (the 33 ms refresh tick) | median 3 ms, p95 81 ms |
| Touch, press to first pixels of the button | median 8.8 ms, p95 32 ms (120 of 120 touches registered) | median 17 to 32 ms, p95 48 ms (Windows timer granularity) |
| CPU | 0.78 percent of one core over 5 minutes; 0.36 percent mean over the 1-hour run with faults | 1.5 percent over the 28 s tour |
| Recovery, PLC answers nothing | link loss shown after 4.1 s; link return 3.4 s after the PLC answers; 11 requests in 25 s | not measured |
| Recovery, PLC drops connections | loss after 1.2 s; return 0.43 s after the PLC answers; 49 requests in 25 s (bounded) | not measured |
| Continuous operation | 3,604 s, 11 injected faults (6 hangs, 5 drops), no crash; exit status 0 on SIGTERM; alarm journal valid | 28 s tour |
| Kill-loop (storage) | 150 rounds (125 SIGKILL, 25 clean stops): audit, alarm journal and alarm state valid every time | not run |

The continuous runs lasted 1 hour (x86) and 42 minutes (emulated guest, 7 faults, program still running at the end). This is not a 24-hour or 72-hour soak and is not claimed as one.

Test suites, final tree (all passing; the Linux suites were run one after another because a parallel `make -j` run produced a one-off `config-exec` failure that did not repeat):

- Linux: core 3,275 checks (including the new boot module test); view 208; configuration golden file identical; UI scenario 205 assertions; end-to-end 75; screenshot palette gate 23 files; executable tests (`cli-exec`, `config-exec`, `cfgedit-exec`, `liveness`); the config helper also tested through the single-file program (`p99_cfg` link); size gate (317,616 bytes, 438,024 bytes of static data, no password-hash code, no shared libraries).
- Windows: core 3,253 checks; view 208; configuration golden file identical; UI scenario 205; end-to-end 75.
- Not run on the final tree: the sanitizer builds (AddressSanitizer, UBSan, ThreadSanitizer) and the lite profile; they last ran before today's changes.

## 6. Failures found, fixes applied, retests

- Alarm list: the "no active alarms" text stayed drawn over the first alarm row when the screen was first drawn while an alarm was active (found in the Windows tour). Fixed in `src/ui/ui_alarms.c`; checked in a new screenshot; all suites rerun.
- The Linux program accepted only a 16-bit framebuffer, but a default Pi framebuffer is 32 bits. A small display backend (`src/app/display_fbdev.c`) now draws RGB565 and converts each strip to 16 or 32 bits per pixel. Checked on the emulated display at 32 bits.
- The new backend broke three executable tests that used `--fb /dev/null`. Instead of tolerating non-framebuffer devices, the program now refuses them, and the tests use the explicit `--fb none`; two new tests check the refusals.
- Merging the root helper into the device program raised static data by 130 KB and broke the size gate; the helper's buffers moved to the stack. Gate passes again, with little margin (438,024 of 440,000 bytes).
- A background run started from a terminal was killed by a hangup signal when the terminal closed; the emulated soak was restarted detached. A first screenshot approach for Windows (copying the screen) captured the lock screen; the demo now saves its own picture.
- Review (architecture and code quality): display backends split into files, one demo-config script, one shared source list for the Makefiles (`sources.mk`, `Makefile.windows`), stale comments corrected.

## 7. What still needs physical hardware or real systems

- A Raspberry Pi with its real display and touch panel: the 16 bits per pixel path, touch calibration, readability and touch-target size, real timings, real memory.
- Real systemd as the service user (watchdog, root helper), power loss, SD card behaviour.
- A real PLC or independent simulator (word and byte order, timeouts on a real network).
- Authentication (none), a threat model, an SBOM and a licence review (the static build puts libmodbus under LGPL-2.1 into the executable; relinking material is described in `docs/THIRD_PARTY.md`).
- Multi-day soak, and sanitizer reruns on the final tree.
- Further size reductions that were considered and not made: a lookup table for `pow` (about 8 KB), drawing the trend as bars and turning off the complex-drawing code (9 to 15 KB), a shared configuration scratch buffer (about 20 KB of touched memory, needs a race test). The targets were met without them.
