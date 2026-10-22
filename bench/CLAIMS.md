# Measurements and claims

Every number below links to a raw file in `bench/results/` and the command
that produced it. Targets are listed separately and are NOT results.

## Environment (all measurements)

- Windows 11 Home 10.0.26200, 12 logical CPUs, MinGW-w64 GCC 13.2.0 (MSYS2 UCRT64)
- Host PC measurements only (Windows, plus WSL2 Linux in section 2b). **Nothing here was measured on a Raspberry Pi,
  an ARM CPU, a real display, a touch panel or a real PLC.**
- Windows timer resolution raised to 1 ms in the legacy prototype only.

## 1. Legacy Windows prototype (custom renderer), 20 s each, 500 ms samples

Command: `powershell -File bench/measure_process.ps1 -Exe <exe> -Seconds 20 -Out <csv>`
Raw: `legacy_prototype_orig.csv`, `legacy_prototype_new.csv`

| Metric | Original MSVC exe (30,720 B, shipped) | Rebuilt GCC exe after audit (66,048 B) |
|---|---|---|
| Private bytes (min..max) | 5,680 .. 6,684 KB | 5,016 .. 5,016 KB |
| Working set (min..max) | 60 .. 3,208 KB (oscillating) | 14,964 .. 14,964 KB |
| Peak working set | 15,164 KB | 14,964 KB |
| CPU, % of whole machine (avg / max) | 0.52 / 3.81 | 0.35 / 2.27 |
| Threads | 8 | 5 |
| Handles (min..max) | 241 .. 266 | 160 .. 160 |
| Start to window (ms) | 137 | 128 |

Reading: the old "0.10 MB RAM" number was the *working set after repeatedly
calling `EmptyWorkingSet`*. Its **private bytes were about 5.7-6.7 MB** and
its handle count grew during the run. The fixed build has flat private
bytes and flat handles. One run each: no confidence intervals.

## 2. Portable runtime + real LVGL v9.2.2, headless host harness

Command: `mingw32-make ui-host` (runs `build/ui_headless.exe`; 3 runs saved)
Raw: `ui_headless_host_run1.txt` .. `run3.txt`, screenshots `shot_*.png`
Scenario (~16 s): normal -> idle 2 s -> alarm -> ack + write -> PLC hangs -> recovery.
UI loop 30 Hz, 800x480 RGB565, LVGL partial render mode, 76,800-byte draw buffer.

| Metric (3 runs) | Value |
|---|---|
| `lv_timer_handler` per call (render + binder), mean | 21 / 24 / 21 us |
| same, p50 / p95 / p99 | 10-12 / 72-94 / 231-355 us |
| same, max | 667 - 814 us |
| Flushes while nothing changes (2 s) | 0 |
| Heap allocation calls in 2 s steady state | 0 (link-level count: app + LVGL + libmodbus objects) |
| Heap allocation calls over whole scenario | 1 (the harness's own sample array) |
| LVGL heap used / peak | 20 KB / 21 KB of a 128 KB pool (`LV_MEM_SIZE`), measured with the single screen of that time. Since M4 the pool is 192 KB and the harness reports 119 KB in use (peak 122 KB) with every screen and the login layer built; that later number is from one run, not three (see `docs/SCREENS.md`) |
| Poll latency (loopback fake PLC) | max 203-215 ms, only during the injected hang (= response timeout) |
| Process private bytes at end | about 2.3 MB (includes the 768 KB RGB565 harness framebuffer) |
| Binary size, stripped, `--gc-sections`, LVGL default config | 1,551,872 B (harness + fake server + all default LVGL widgets) |

Optimisation recorded (same harness, before -> after caching text colours so
unchanged labels do not invalidate): steady-state flushes 44 -> 0 per 2 s,
handler mean 182 us -> about 22 us, p99 1003 us -> 231-355 us.
Pre-optimisation numbers were from a single run.

## 2b. Same harness on Linux (WSL2 host, not Raspberry Pi)

Environment: Ubuntu 24.04 on WSL2, kernel 6.18.54.1-microsoft-standard-WSL2,
gcc 13.3.0, libmodbus 3.1.10, x86_64. Command: `make -f Makefile.linux ui-host`.
Raw: `bench/results/linux/linux_ui_host.txt`, `linux_test_e2e.txt`,
screenshots `bench/results/linux/shot_*.png`. One run, no confidence interval.

| Metric (1 run) | Value |
|---|---|
| `lv_timer_handler` per call, mean / p50 / p95 / p99 / max | 16 / 10 / 22 / 152 / 924 us (421 samples) |
| Flushes while nothing changes (2 s) | 0 |
| Heap allocation calls in 2 s steady state | 0 (link-level count, same limits as section 5) |
| LVGL heap used / peak | 20 KB / 21 KB |
| Link declared lost (3 x 200 ms timeout, loopback fake PLC) | 601 ms |
| Core tests / e2e / UI scenario | 521 / 75 / 22 checks, 0 failed |

Sanitizers (same host, `-O1`): ASan+UBSan 0 reports on core and e2e; TSan 0
reports on e2e and, after a test-code fix of 3 initial races, 0 reports on core in 3 runs. See
`docs/LINUX_VALIDATION.md`. These numbers say nothing about Raspberry Pi
performance; the screenshots were rendered into memory, not to a display.

## 3. What was NOT measured

- Raspberry Pi / ARM: any CPU, memory, startup, frame or latency number.
- Real framebuffer flush cost, touch latency, vsync/tearing.
- Real PLC behaviour, long-run soak (72 h), memory growth over days.
- `Sleep`-jitter distribution of the runtime threads, alarm-response latency
  end to end (only deterministic fake-clock tests exist).
- Binary size with a trimmed `lv_conf.h`.

## 4. Targets (design goals, not results)

| Goal | Source |
|---|---|
| Runs in a few tens of MB RSS on a Pi class device | design baseline v0.2 |
| First frame within a couple of seconds of process start | design baseline v0.2 |
| Link loss detected in `miss_limit * response_timeout` | implemented; host-measured 611 ms for 3 x 200 ms |
| 72 h soak with flat PSS / allocations | not run |

## 5. Heap claim, exact wording

"No heap allocation was observed in application, LVGL or libmodbus code
during a 2 s steady-state window on the host build." It does **not** cover
the C runtime or OS libraries, connect/reconnect, config load, or other
operating modes.
