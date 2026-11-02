# Optimization baseline (phase 0)

This file records the state of Prometheus99 before any optimization of phases 1 to 5 (`docs/MASTER_PROMPT_FINAL.md`). Every number below was measured on the tree of 2026-11-01 (branch `feature/authorization-hardening`, last commit `ddda58d` plus the uncommitted responsive-layout work), with the command that produced it. The raw outputs of the first baseline script are in `docs/baseline/` and are not overwritten. The snapshot of the tree is `C:\Users\Admin\Desktop\prometheus99-baseline-snapshot`.

## Environment

- Host: Windows 11 Home, 12th Gen Intel Core i5-12450H, 12 logical processors.
- Linux: WSL2 Ubuntu 24.04, kernel 6.18.54.1-microsoft-standard-WSL2, gcc 13.3.0, musl-gcc (Ubuntu package). Builds and tests run in `~/p99` (a copy of the repository on the Linux file system), not on `/mnt/c`.
- Windows: MSYS2 UCRT64 gcc and `mingw32-make` from `C:\msys64\ucrt64\bin`, building in the short path `C:\p99`.
- One machine, one run per measurement unless stated. All numbers are host measurements (x86-64), not Raspberry Pi numbers.

## 1. Size and static data

Command: `make -f Makefile.linux STATIC=1 CC=musl-gcc BUILD=build-musl hmi`, then `bench/gate.sh build-musl`.

| Item | Value | Limit | Margin |
|---|---|---|---|
| Stripped size of `build-musl/prometheus99` | 321,712 bytes | 322,000 | 288 |
| Static data (.data + .bss) | 438,536 bytes | 440,000 | 1,464 |
| `size`: text / data / bss | 307,626 / 7,408 / 431,128 | | |
| Compiler warnings in the clean static build | 8 | | |

The rerun of 2026-11-01 17:40 gave the same sizes as `docs/baseline/size_gate.txt`, so nothing drifted between the baseline script and the start of phase 1.

The 8 warnings of the clean build (`docs/baseline/build_musl.txt`): one in the program's own code, a `-Wmaybe-uninitialized` reported by link-time optimisation in `src/ui/hmi_view.c` (the write record copied in `hmi_view_write_status_text`), and seven in LVGL (`lv_evdev.c` two `-Woverflow` on ioctl numbers, `lv_tlsf.c` three unused parameters, `lv_chart.c` one `-Wmaybe-uninitialized`). The LVGL files are not edited (constraint 2).

### Sections (`size -A build-musl/prometheus99`)

| Section | Bytes |
|---|---|
| .text | 268,363 |
| .rodata | 31,912 |
| .rela.dyn | 7,248 |
| .data.rel.ro | 6,384 |
| .data | 568 |
| .bss | 431,128 |

### Largest static objects (`nm --size-sort -S -r`)

| Object | Bytes | What it is | Needed for the whole run? |
|---|---|---|---|
| `g_rt` (`hmi_runtime_t`, `main_linux.c`) | 131,408 | The whole runtime: configuration 11,968, tag database 15,520, comms 264, writes 8,888, alarms 13,288, storage worker 49,424 (of which the queue is 47,104 and is never cleared, so its pages stay untouched until used), trend history 31,152 | yes |
| `work_mem_int` (LVGL heap, `LV_MEM_SIZE` in `config/lv_conf.h`) | 114,688 | LVGL objects, styles, draw tasks | yes; the harness measured `max_used` 74 KB of 101 KB usable |
| `s_chk_text` (`ui_config.c`) | 32,769 | Text buffer for the local check of an on-device configuration change | only while the CONFIG view applies a change |
| `cfg` in `hmi_cfgsec_load` (`hmi_cfgsec.c`) | 32,769 | The configuration file bytes while the signature is checked | only at boot |
| `s_draw` (display backend) | 32,000 | LVGL draw buffer, 20 lines of 800 pixels at 16 bits | yes |
| `tail` in `journal_collect` (`hmi_runtime.c`) | 20,480 | The end of the alarm journal read back at start | only at boot |
| `toks` in `hmi_config_parse` (`hmi_config.c`) | 16,384 | JSON tokens of the parser | at boot and when the CONFIG view checks a change |
| `s_chk` (`ui_config.c`) | 11,968 | Configuration parsed back by the CONFIG view check | only while the CONFIG view applies a change |
| `g_cfg` (`main_linux.c`) | 11,968 | The configuration as loaded at boot; copied into `g_rt.cfg` by `hmi_runtime_start` and not used afterwards | only at boot (a duplicate) |
| `req` (`hmi_cfgedit`) | 4,136 | Configuration change request | CONFIG view |

### Largest code (`nm --size-sort -S -r`, text symbols)

The largest functions are LVGL drawing and layout (`img_draw_core`, `layout_update_core`, `lv_draw_sw_blend`), the musl formatted I/O (`fmt_fp` 4,935, `printf_core` 3,852, `decfloat` and `__floatscan` for `strtod`), the program's own `main` 9,891 and the CONFIG helper (`dispatch`, `cmd_apply`, `hmi_cfgedit_apply`). The read-only data contains the musl `pow` and `exp` tables (`__pow_log_data` 4,168, `__exp_data` 2,160) next to `pow` itself (1,916 bytes of code); the only caller of `pow` in the program is `hmi_view.c` (powers of ten for 0 to 3 decimals).

## 2. Tests (Linux)

Command, one suite after another: `make -f Makefile.linux test test-view test-config-golden ui-host e2e cli-exec config-exec cfgedit-exec liveness`. Output: `docs/baseline/tests_full.txt`, summary `docs/baseline/tests_summary.txt`.

| Suite | Result |
|---|---|
| Core (19 programs) | 3,275 checks, 0 failed |
| View model | 208 checks, 0 failed |
| Configuration golden file and round trip | identical; 58 configurations round-tripped |
| UI scenario (real LVGL, headless) | 205 assertions, 0 failed |
| End to end (runtime, libmodbus, fake PLC) | 75 checks, 0 failed |
| `cli-exec`, `config-exec`, `liveness` | pass |

## 3. Idle behaviour of the headless executable

Command: `make -f Makefile.linux STATIC=1 CC=musl-gcc BUILD=build-musl hmi-bench`, then `bash bench/idle_profile.sh build-musl phase0_idle 30` (new script, added in phase 0). Workload: the fake PLC on loopback, the example configuration, the overview screen, no input, 30 s after 3 s of settling. The framebuffer is a regular file (800 x 480 x 2 = 768,000 bytes) and is reported on its own line. Two runs:

| Metric | Run 1 | Run 2 |
|---|---|---|
| Rss | 1,336 kB | 1,332 kB |
| Pss | 1,332 kB | 1,328 kB |
| Pss of the framebuffer file mapping | 752 kB | 752 kB |
| Pss without the framebuffer | 580 kB | 576 kB |
| VmHWM | 1,336 kB | 1,332 kB |
| Threads | 4 | 4 |
| CPU, whole process, percent of one core | 0.40 | 0.40 |
| Context switches per second, all threads | 125.5 | 125.7 |

Per thread (run 1; run 2 within 0.3 per second):

| Thread | CPU ticks in 30 s (1 tick = 10 ms) | Voluntary switches per second | Wake-up source in the code |
|---|---|---|---|
| main (UI loop) | 4 | 39.9 | `main_linux.c`: sleeps what `lv_timer_handler` returns, clamped to 5..33 ms |
| store (storage worker) | 0 | 5.7 | `hmi_store.c` worker: waits on its queue event with a 200 ms timeout (5 per second) and wakes for each queued record |
| comms | 6 | 60.0 | `comms_thread`: sleeps `min(hint, 20)` ms between steps; the fake PLC answered 334 requests in 30 s (about 11 per second), so about 49 of the 60 wake-ups per second are the 20 ms cap |
| super (supervisor) | 1 | 19.9 | `supervisor_thread`: fixed 50 ms sleep |

Earlier figure for comparison: the README gives 0.55 to 0.61 MB Pss for the device program and 0.4 to 0.8 percent CPU; the audit measured 620 kB Rss and 0.50 percent. This run agrees.

Not measured: a syscall count per thread (`strace` is not installed in the WSL image and installing it needs the owner's password).

## 4. UI harness (performance and heap)

Command: `make -f Makefile.linux ui-host` (the harness prints `[perf]` and heap lines). Output: `docs/baseline/ui_perf.txt`.

| Metric | Value |
|---|---|
| LVGL heap after the UI build | 48 KB used of 101 KB |
| LVGL heap at the end, `max_used` | 74 KB |
| Heap allocations in 2 s of steady state | 0 |
| `lv_timer_handler` (render and binder), 2,692 samples | mean 34 us, p50 10 us, p95 213 us, p99 452 us, max 1,386 us |
| Screen transition, press to last pixels (96) | median 0.4 ms, p95 33.8 ms |
| Touch press to first pixels of the pressed button (120) | median 11.4 ms, p95 31.3 ms |
| Binder | 1,190 ticks, 380 labels updated, 11,369 skipped as unchanged |
| Harness process at the end | RssAnon 2,272 kB, VmRSS 5,064 kB |

## 5. Windows

Command (PowerShell, in `C:\p99`, a fresh copy of the tree without build directories): `mingw32-make -f Makefile.windows BUILD=build test-hmi-all ui-host`. Clean build plus all suites took 404 s. Output: `docs/baseline/windows_tests.txt`.

| Suite | Result |
|---|---|
| Core (19 programs) | 3,253 checks, 0 failed (22 fewer than Linux: 3 fewer in `platform`, 20 fewer in `cfgsec`, 1 more in `auditfmt`; the same counts as in the README) |
| View model | 208 checks, 0 failed |
| Configuration golden file and round trip | identical; 58 configurations round-tripped |
| End to end | 75 checks, 0 failed |
| UI scenario | 205 assertions, 0 failed |
| UI harness `lv_timer_handler` (1,957 samples) | mean 62 us, p50 11 us, p95 352 us, p99 1,093 us |
| UI harness touch press to first pixels (120) | median 27.5 ms, p95 46.4 ms |
| LVGL heap at the end | `max_used` 74 KB (same as Linux) |
| `build/hmi_win_demo.exe` (`mingw32-make -f Makefile.windows build/hmi_win_demo.exe`) | 311,296 bytes |

The demo exe is 6,656 bytes larger than the 304,640 bytes in the README, which was measured before the two extra font sizes of the responsive layout. The Windows timings are slower than the Linux ones on the same machine; the cause was not investigated.

## 6. Sanitizers, lite profile, kill-loop, emulated Pi

| Run | Command | Result |
|---|---|---|
| AddressSanitizer and UBSan | `UBSAN_OPTIONS=halt_on_error=1 make -f Makefile.linux SAN=address BUILD=build-asan test test-view e2e ui-host` | core 3,275, view 208, e2e 75, UI scenario 205; no sanitizer report |
| ThreadSanitizer | `setarch x86_64 -R make -f Makefile.linux SAN=thread BUILD=build-tsan test e2e` | core 3,275, e2e 75; no ThreadSanitizer warning |
| Lite profile and its gate | `make -f Makefile.linux STATIC=1 CC=musl-gcc LITE=1 BUILD=build-musl-lite hmi`, `bench/gate.sh build-musl-lite lite` | **gate FAILS**: stripped 297,136 > 290,000 and static data 354,824 > 350,000. The lite gate had not been run since the responsive layout added two font sizes. |
| Kill-loop | `make -f Makefile.linux killloop KILL_ROUNDS=150` | PASS: 150 rounds (125 kills, 25 clean stops), no damaged audit or journal line, state file valid; 3 min 36 s |
| aarch64 cross build | `make -f Makefile.linux STATIC=1 CC=/opt/toolchains/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc BUILD=build-aarch64 hmi` | builds; 358,496 bytes stripped (README: 350,288, measured before the responsive layout); text 346,285, data 7,128, bss 430,472 |
| `cfgedit-exec` | `make -f Makefile.linux cfgedit-exec` | not run: the script needs root (it uses `chown`) and the WSL user has no password-free sudo |
| Emulated Pi tour | `emulation/pi/run_qemu.sh` | not run: the guest kernel and initramfs were prepared under `/root/work/pi`, which needs root; rebuilding them with `emulation/pi/prepare.sh` downloads an Alpine kernel, which was not done without the owner's approval |
| Syscall count per thread | `strace -c -f` | not run: `strace` is not installed and installing it needs sudo |

The ASan run of the baseline compiled the whole core again for each of the 19 test programs, one after another, and took about 27 minutes of wall time (17:45 to 18:12 UTC by the file times, while other builds shared the machine). That is the reason for the Makefile change in phase 1 (changelog C5).

## 7. Candidate waste named in the master prompt, judged from these numbers

- **Overview journal work every 50 ms.** Real but small: the main thread used 4 ticks (40 ms) of CPU in 30 s in total, so the whole UI loop is about 0.13 percent of one core. The journal copy, the 64-entry name table and the text formatting run on every binder tick although the journal changes only on alarm events. Worth removing because it is cheap to remove and touches nothing else; the expected gain is a few microseconds per tick.
- **Supervisor snapshot of all tags every 50 ms.** Not worth changing: the supervisor used 1 tick in 30 s (0.03 percent of one core). Changing the cadence would risk the trend minimum and maximum for no measurable gain. Rejected for phase 1.
- **Comms thread 20 ms cap.** Real: it is the largest wake-up source (about 49 of 125 switches per second). It is kept so a queued write is picked up within 20 ms. Removing it needs a wake-up on write queueing, which belongs to phase 2 (communication scheduling) and must keep write latency unchanged.
- **Size.** The binding limit is the stripped size (288 bytes of margin). Three changes make room without removing any function: the `pow` call in the view module (pulls in about 8 KB of libm code and tables), the duplicate `g_cfg` (11,968 bytes of static data), and the two boot-only buffers (`cfg` in `hmi_cfgsec_load`, 32,769 bytes, and the journal `tail`, 20,480 bytes).
