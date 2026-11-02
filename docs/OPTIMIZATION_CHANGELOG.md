# Optimization changelog

Every change of phases 1 to 5 (`docs/MASTER_PROMPT_FINAL.md`) is listed here with the problem and its evidence, the files, the implementation, the checks rerun and the measured effect. The before figures are in `docs/OPTIMIZATION_BASELINE.md`. A change that could not be made safe is listed with "reverted" and the reason. The preservation matrix is `docs/FUNCTIONALITY_PRESERVATION.md`.

## Phase 1

Changes C1 to C5 were made together and measured together on 2026-11-01 and 2026-11-02 in a separate copy of the tree (`~/p99b` in WSL), with the untouched baseline tree (`~/p99`) kept for comparison. The checks and the combined effect are listed after the five entries.

### C1. Powers of ten from a table instead of `pow()`

- **Problem and evidence.** The stripped size had 288 bytes of margin. `nm` showed the musl `pow` code and its tables (`__pow_log_data`, `__exp_data`) in the executable; the only caller in the program was `hmi_view.c`, which needs 10 to the power 0, 1, 2 or 3.
- **Files.** `src/ui/hmi_view.c`.
- **Implementation.** A four-entry table of exact doubles. `hmi_view_decimals` multiplies by the table entry; `hmi_view_setpoint_step` divides 1 by it. The values are identical to `pow(10, d)` for these exponents.
- **Effect.** After the change `nm` finds none of `pow`, `__pow_log_data` or `__exp_data` in `build-musl/prometheus99`. The stripped size fell by 8,192 bytes for C1 to C4 together; C3 and C4 add a few lines of code and remove none, so the saving is C1's. (The two were not built separately.)

### C2. Overview journal text rebuilt only when the journal changed

- **Problem and evidence.** `overview_update` (`src/ui/ui_overview.c`) copied the last four journal entries, built a 64-entry name table and formatted the journal text on every 50 ms binder tick, although the text depends only on the journal entries, which change only on alarm events. The label itself was already skipped when the text was equal (`ui_set_text`), so the waste was the copy, one alarm mutex lock and the formatting.
- **Files.** `src/ui/ui_overview.c`.
- **Implementation.** The text is rebuilt only when `hmi_alarm_state_rev` differs from the value seen at the last rebuild. Every journal entry raises that revision (`hmi_alarm.c`, the journal push). The revision is read before the copy, so an entry added in between causes one more rebuild on the next tick, never a missed one. The remembered value is invalidated whenever the screen is reset (screen change, page change, relayout).
- **Effect.** UI harness, one run each: `lv_timer_handler` mean 34 us before and 32 us after, p95 213 and 208 us. The difference is within run-to-run noise; the change is kept because it removes work without any cost, not because of a measured gain. Idle CPU of the whole process is 0.40 percent before and after (the measurement resolution is one 10 ms tick in 30 s).

### C3. Configuration loaded in place into the runtime

- **Problem and evidence.** `main_linux.c` held the configuration in a static `g_cfg` (11,968 bytes) only until `hmi_runtime_start` copied it into `g_rt.cfg`; afterwards `g_cfg` was never read. Static data had 1,464 bytes of margin.
- **Files.** `src/hmi/hmi_runtime.c`, `src/hmi/hmi_runtime.h`, `src/app/main_linux.c`, `tests/hmi/test_runtime.c`.
- **Implementation.** `hmi_runtime_start_ex` accepts `cfg == &rt->cfg`: the clearing step then keeps the configuration (the first member, checked at compile time) and the copy is skipped. `main_linux.c` lets `hmi_boot_config` write into `g_rt.cfg` and passes that. Callers that pass a separate configuration behave as before. New test `test_runtime_cfg_in_place` (8 checks): the runtime is filled with stale bytes, the configuration parsed into `rt.cfg`, the runtime started; the configuration is unchanged, the tag database has every tag and both worker threads run.
- **Effect.** 11,968 bytes less static data.

### C4. Boot-only file buffers taken from the heap

- **Problem and evidence.** Two static buffers are used only during start: the configuration file bytes in `hmi_cfgsec_load` (32,769 bytes) and the journal read-back buffer in `journal_collect` (20,480 bytes). They were reserved as static data for the whole run.
- **Files.** `src/hmi/hmi_cfgsec.c`, `src/hmi/hmi_runtime.c`.
- **Implementation.** Each is allocated with `malloc` when the function starts and freed when it ends. The program already allocated at start (the tag database check in `hmi_config.c`); the steady state still allocates nothing (the harness counts 0 allocations in 2 s of steady state, as before). Without memory, `hmi_cfgsec_load` returns "configuration unreadable" (safe mode, fail closed) and the journal is not shown again (as when the file is absent; the file is untouched).
- **Trade-off checked.** Moving data from static to heap could hide memory instead of saving it. The peak was measured: VmHWM of the headless run is 1,308 and 1,312 kB after against 1,332 and 1,336 kB before, and Pss without the framebuffer 552 and 556 kB against 576 and 580 kB. The peak did not rise.
- **Effect.** 53,249 bytes less static data.

### C5. Test build: core compiled once, sanitizers in the default `test` target

- **Problem and evidence.** Every core test program compiled all 22 core files again, one program after another; the baseline ASan run took about 27 minutes. The master prompt asks for sanitizers in the default test target (roadmap P4-1, adopted from the friend's `Makefile`).
- **Files.** `Makefile.linux`.
- **Implementation.** The core is compiled once per build directory into `$(BUILD)/core/` with dependency files (`-MMD -MP`), and the test programs (core tests, `test_view`, `test_e2e`, `test_notify`, kill-loop programs) link those objects. The device programs still compile the sources themselves as one link-time-optimised unit, so the product is unchanged by this. `make test` now runs the core tests under AddressSanitizer and UBSan in `$(BUILD)-asan` with `-fno-sanitize-recover=all` (any finding stops the run); `make test-fast` is the previous uninstrumented run. With `SAN=` given, or with `STATIC=1` (musl has no sanitizer runtime, and the CI workflow runs `test` with `STATIC=1`), `test` is `test-fast`. `clean` also removes `$(BUILD)-asan`.
- **Effect.** `make -j8 test` (ASan and UBSan, build and run of the core tests from a clean build directory) took 37 s. The uninstrumented core test build took 3.8 s with `-j8`.

### C6. Strict warnings target

- **Problem and evidence.** Roadmap P4-2 asks for `-Wconversion -Wshadow -Wcast-qual -Wformat=2` on the project's own code, in the build or CI. A survey with those flags over `src/hmi`, `src/ui`, `src/ui/font`, `src/app` and `tests/hmi` found 12 warnings in own files: two `-Wsign-conversion` in `src/app/display_fbdev.c` (the row offset mixed `int32_t` and `uint32_t`, added with the responsive layout), six `-Wcast-qual` in `tests/hmi/test_cli.c`, two `-Wconversion` in `tests/hmi/test_trend.c` and two in `tests/hmi/ui_headless.c`. The other 35 are in third-party headers (18 in `jsmn.h`, 17 in LVGL's `lv_api_map_v8.h`).
- **Files.** `Makefile.linux` (target `warnings`), `.github/workflows/ci.yml` (the `tests` job runs `warnings`), `src/app/display_fbdev.c`, `tests/hmi/test_cli.c`, `tests/hmi/test_trend.c`, `tests/hmi/ui_headless.c`.
- **Implementation.** `make -f Makefile.linux warnings` checks every own file with `-fsyntax-only` and the strict flags (the real builds keep their flags), writes `$(BUILD)/warnings.log`, and fails when any warning or error is located in an own file; warnings located in third-party headers are counted only. The offset in `display_fbdev.c` is now computed in `off_t` throughout (same result for every valid row; no unsigned wrap for a negative intermediate). `test_cli.c` copies its arguments into writable buffers instead of casting `const` away (an argument too long for the buffer aborts the test program, so nothing is truncated and the check count stays 110). The two test files use explicit conversions with the same values.
- **cppcheck.** Not run: it is not installed in WSL or MSYS2, and installing it needs sudo or a package download.
- **Effect.** `make warnings`: 0 in own files, 35 in total. Checked that the target fails: a scratch file with a shadowed variable made it report 2 and exit non-zero (the file was removed).

### C7. Executable tests safe under a parallel make (the `config-exec` flake)

- **Problem and evidence.** README section 5 reported that a parallel `make -j` once failed `config-exec`; the CI workflow uses `-j4`. Running `make -j4 cli-exec config-exec liveness` on the baseline tree failed 3 runs out of 3, with 9 failed checks each ("missing signature: 4 connection(s) made"). Cause: `cli-exec`, `liveness` and `config-exec` all used 127.0.0.1:5502, and `config-exec` counts every connection to that port to prove that a rejected configuration makes none, so the other scripts' executables were counted.
- **Files.** `tests/hmi/cli_exec_test.sh` (port 5503), `tests/hmi/liveness_e2e.sh` (port 5504, also for its hanging server), `tests/hmi/config_exec_test.sh` (comment only; it keeps 5502).
- **Effect.** The same parallel command passed 3 runs out of 3. The three CI jobs were then run locally with `-j4` in clean build directories and all passed (next section).

### C8. LVGL heap measured at six screen sizes (no change to the heap size)

- **Problem and evidence.** The master prompt asks to check the LVGL heap use before changing `LV_MEM_SIZE` (112 KB), and the responsive layout had only been measured at 800x480.
- **Files.** `tests/hmi/ui_headless.c`: an opt-in probe, `P99_HEAP_PROBE=WxH`, builds the UI at that size, visits every screen, prints the heap use and exits before the scenario. Without the variable the harness is unchanged (205 assertions).
- **Result.** At 640x400, 800x480, 1024x600, 1280x800, 1366x768 and 1920x1080 the heap is the same: 48 KB after the build, 55 KB after visiting every screen, `max_used` 56 KB of 100 KB usable. The heap use does not depend on the screen size; the scenario's peak of 74 KB comes from something the scenario does beyond visiting the screens (which step was not isolated), not from the resolution.
- **Decision.** `LV_MEM_SIZE` stays at 112 KB. After C3 and C4 the static data has 66,744 bytes of margin, so shrinking the heap is not needed to make room, and the stripped size (the tighter limit) does not depend on it. If static data becomes tight again, the measurement supports about 96 KB (22 KB above the observed peak of 74 KB); that would need a fresh peak measurement with the login screen built.

### Checks rerun after C1 to C5

| Check | Result after | Before |
|---|---|---|
| Core tests (`test-fast`) | 3,283 checks, 0 failed | 3,275 (the 8 new checks are C3's test) |
| Core tests under ASan and UBSan (`make test`) | 3,283, no report | 3,275, no report |
| View, golden, e2e, UI scenario (ASan build too) | 208, identical, 75, 205 | same |
| ThreadSanitizer, core and e2e | 3,283 and 75, no warning | 3,275 and 75, no warning |
| `cli-exec`, `config-exec`, `liveness` | pass | pass |
| `cfgedit-exec` | not run (needs root) | not run |
| Kill-loop, 150 rounds | PASS (125 kills, 25 clean stops) | PASS |
| Windows `test-hmi-all` and `ui-host` (`C:\p99`) | core 3,261, view 208, golden identical, e2e 75, UI 205 | core 3,253, the rest the same |
| Windows demo exe | 311,296 bytes | 311,296 bytes (the file size did not change; the reason was not investigated) |

### Checks rerun after C6 to C8

| Check | Result |
|---|---|
| `make warnings` | 0 in own files (35 in third-party headers) |
| Core tests (`test-fast`) | 3,283 checks, 0 failed (`cli` 110, `trend` 90, unchanged counts) |
| `ui-host` | 205 assertions, 0 failed; LVGL heap `max_used` 74 KB as before |
| `cli-exec` | pass |
| Gate after the `display_fbdev.c` change | PASS, 313,520 and 373,256 bytes (unchanged) |
| CI job `tests`, locally, `make -j4`, clean build directory `build-ci` | pass in 188 s: tag-table access check, core 3,283 (under ASan, the new default), notify 13, view 208, golden identical, UI scenario 205, 23 screenshots inside the palette, e2e 75, `cli-exec`, `config-exec`, `liveness`; `cfgedit-exec` skipped (needs root) |
| CI step lite headless UI (`LITE=1`) | pass in 129 s, 127 assertions |
| CI job `static` (musl): tests, product and bench builds, both gates, size report | pass in 192 s; full gate 313,520 and 373,256; lite gate 288,944 and 295,432 |
| CI job `san` (`SAN=address`): core, e2e, UI scenario | pass in 180 s: 3,283, 75, 205 |
| Windows `test-hmi-all` and `ui-host` (`C:\p99`) | core 3,261 (`cli` 110, `trend` 90), view 208, golden identical, e2e 75, UI scenario 205 |

The CI workflow itself has still never run on GitHub (pushing is the owner's decision). The local run used the same commands on WSL2 Ubuntu 24.04, not the GitHub runner image.

### Combined effect of phase 1 so far

| Metric | Before | After | Difference |
|---|---|---|---|
| Stripped size, full profile | 321,712 | 313,520 | -8,192 bytes (margin 288 to 8,480) |
| Static data, full profile | 438,536 | 373,256 | -65,280 bytes (margin 1,464 to 66,744) |
| `size` text / data / bss | 307,626 / 7,408 / 431,128 | 299,466 / 7,408 / 365,848 | |
| Lite profile gate | FAIL: 297,136 and 354,824 | PASS: 288,944 and 295,432 | the lite gate passes again |
| Headless Pss without framebuffer (two runs) | 580, 576 kB | 552, 556 kB | about -24 kB |
| Headless VmHWM (two runs) | 1,336, 1,332 kB | 1,308, 1,312 kB | about -24 kB |
| Idle CPU, whole process | 0.40 % | 0.40 % | none measurable |
| Context switches per second | 125.5, 125.7 | 125.5, 126.0 | none (C2 does not change sleeping) |
| UI harness `lv_timer_handler` mean | 34 us | 32 us | within noise |
| LVGL heap `max_used` | 74 KB | 74 KB | none |
| aarch64 static build, stripped | 358,496 | 350,304 | -8,192 bytes (built, not run) |
| ASan core test run, wall time | about 27 min | 37 s | build change C5 |

## Phase 2

The phase 1 tree (C1 to C8) is the "before" of phase 2. Measurements on the same machine, build and workload as in `docs/OPTIMIZATION_BASELINE.md`.

### C9. Comms thread sleeps until something is due, woken at once by a queued write

- **Problem and evidence.** The idle profile (baseline section 3) showed the comms thread as the largest wake-up source: 60 voluntary context switches per second, of which about 49 came from the 20 ms cap on its sleep (`comms_thread` in `hmi_runtime.c`: `hmi_sleep_ms(min(hint, 20))`). The fake PLC received about 11 requests per second, so most passes found nothing to do. The cap existed so that a queued write was found on the next pass; the cost was that a write waited on average half the cap, and the new write-latency bench (C10) measured a median of 18 ms from submit to arrival at the PLC, of which about 6 ms is the durable audit record.
- **Files.** `src/hmi/hmi_writes.h`, `src/hmi/hmi_writes.c` (notify hook), `src/hmi/hmi_runtime.h`, `src/hmi/hmi_runtime.c` (event, wait, stop), `tests/hmi/test_writes.c`, `tests/hmi/test_runtime.c`.
- **Implementation.**
  - `hmi_writes_set_notify` registers a hook that the writes module calls, outside its lock, each time a request enters the send queue (it becomes PENDING in `hmi_writes_audit_done`, the only place where a request is queued, for both the synchronous and the storage-worker path). It is not called for a refused, rejected or repeated report.
  - The runtime owns an auto-reset event (`hmi_event_t comms_wake`, the same primitive the storage worker uses, available on POSIX and Windows). The hook signals it; `hmi_runtime_stop` signals it after setting the stop flag.
  - The comms thread waits on that event for the hint (the time to the next due poll group, or to the end of the back-off, both computed by `hmi_comms_step` exactly as before) but at most `HMI_COMMS_IDLE_MAX_MS` (250 ms). The cap remains so that queued writes that can no longer be sent (5 s time to live, `hmi_writes_expire`, run at the start of every pass) are expired at most 250 ms late instead of 20 ms; that is the trade-off, recorded here.
  - `hmi_comms_step`, the poll scheduling, timeouts, back-off, jitter and the write path itself are unchanged. An early or spurious wake only causes one extra pass, which finds nothing due.
- **New tests.** `test_write_notify` (`test_writes.c`, 18 checks): the hook fires once for a synchronous queue, not for a role refusal, not while RESERVED, once when the RESERVED record becomes durable, not for a repeated report, not for a rejected record, and never after it is removed. `test_runtime_write_wakes_comms` (`test_runtime.c`): with polls 1 s apart, ten writes at different phases of the idle sleep each reach the PLC in under 150 ms (measured: at most 10 to 11 ms; 31 ms under ThreadSanitizer). With the hook disabled in a scratch copy, the same test measured 227 ms and failed, so it detects the loss of the wake-up.
- **Test adjusted.** `test_runtime_cfg_in_place` (added in C3) waited 200 ms for a comms heartbeat; with polls every 200 ms and the longer idle sleep a 200 ms window can hold no pass, and it failed once in a TSan-free run. It now waits `2 * HMI_COMMS_IDLE_MAX_MS + 100` ms. The existing `test_runtime_beats` waits 300 ms and passed unchanged. `test_runtime` then passed 15 runs out of 15 and the e2e test 5 out of 5.
- **Watchdog.** The comms heartbeat now advances at least every 250 ms when idle (before: every 20 ms); the supervisor's stall limit is 4,600 ms, so the liveness check is unaffected (`liveness` passes).

### C10. Write-latency bench

- **Files.** `tests/hmi/bench_write_latency.c`, `Makefile.linux` (`make -f Makefile.linux write-latency`).
- **What it measures.** The real runtime with its threads, storage worker and an audit file, libmodbus and the in-process fake PLC on loopback, one tag polled once a second. 60 writes at irregular intervals of 250 to 450 ms; for each, the time from `hmi_writes_submit_as` to the arrival of the write at the PLC and to the CONFIRMED state after the read-back, polled every 0.2 ms with `CLOCK_MONOTONIC`. Linux only; a measurement, not a test.

### Measured effect of C9

| Metric | Before (two runs) | After (two runs) |
|---|---|---|
| Comms thread voluntary switches per second (idle profile, 30 s) | 60.0, 59.9 | 15.9, 16.0 |
| All threads, switches per second | 125.5, 126.0 | 81.4, 81.7 |
| Comms thread CPU ticks in 30 s | 5, 5 | 2, 3 |
| Whole process CPU, percent of one core | 0.40, 0.40 | 0.33, 0.33 |
| Requests the fake PLC received in 30 s (polling unchanged) | 334, 334 | 334, 334 |
| Write: submit to arrival at the PLC, median | 18.0, 18.6 ms | 7.8, 7.5 ms |
| Write: submit to arrival, p95 | 28.0, 25.6 ms | 8.7, 8.8 ms |
| Write: submit to arrival, max | 28.4, 50.2 ms | 9.4, 9.1 ms |
| Write: submit to CONFIRMED, median | 18.0, 19.0 ms | 7.9, 7.8 ms |
| Pss without framebuffer | 552, 556 kB | 552, 552 kB |
| Static data (full gate) | 373,256 | 373,416 (+160 bytes: the event and the hook pointers) |
| Stripped size (full gate) | 313,520 | 313,520 |

**Recovery** (`bench/recovery_linux.sh`, the static bench build, the example configuration; 12 runs of each tree, run in pairs at the same time). The first single run after the change showed a return after the hang of 5,365 ms against 3,090 ms before, so the comparison was repeated until it rested on 12 runs each. Times are from the fault or restore signal to the link event in the alarm journal file, sampled every 100 ms by the script.

| Metric | Before: median (mean), range | After: median (mean), range | Mann-Whitney U |
|---|---|---|---|
| Loss shown after the PLC hangs | 4,038 (4,000) ms, 3,867 to 4,124 | 4,066 (4,071) ms, 3,916 to 4,150 | 32 |
| Requests during the 25 s hang | 11 (9), 5 to 11 | 11 (9), 5 to 11 | 62.5 |
| Return after the hang | 3,626 (3,802) ms, 2,262 to 5,531 | 4,013 (4,186) ms, 3,010 to 5,365 | 53 |
| Loss shown after connections drop | 1,145 (1,174) ms, 1,133 to 1,250 | 1,194 (1,191) ms, 1,132 to 1,249 | 64 |
| Requests during the 25 s of drops | 48 (49), 47 to 55 | 48 (48), 44 to 52 | 48.5 |
| Return after the drops | 313 (356) ms, 106 to 729 | 364 (347) ms, 105 to 725 | 71 |

For 12 and 12 runs the two-sided 5 percent critical value of U is 37: a smaller U means a difference. The returns, the request counts (no extra retries) and the loss after dropped connections do not differ. The loss after a hang is about 70 ms later on average (U = 32), which is below 37 for one comparison but not significant once the six comparisons of this table are taken into account (a 5 percent level over six tests needs U of about 27 or less). No code path was found that would delay it: the hints of `hmi_comms_step` are unchanged and the wait only ends earlier than the hint, never later. It is recorded as an open observation; the soak and recovery runs of phase 5 should look at it again. In one "after" run the script printed no request count for the hang (its read of the fake PLC's counter file returned nothing; the cause was not investigated), so that column has 11 values.

The README figures (loss 4.1 s and 1.2 s, return 3.4 s and 0.43 s, 49 requests) fall inside both columns.

### C11. `--report` run statistics (measurement builds)

- **Problem.** Benchmarks read the program's behaviour from outside (`/proc`, the fake PLC's counters). The master prompt asks for an optional report written by the program itself, adapted from the friend's `--report`.
- **Files.** `src/hmi/hmi_cli.c`, `src/hmi/hmi_cli.h` (strict option `--report PATH`), `src/app/main_linux.c`, `Makefile.linux` (`-DP99_REPORT` for `prometheus99-bench` and `prometheus99-sdl`), `tests/hmi/test_cli.c` (10 checks), `tests/hmi/cli_exec_test.sh` (the device program refuses the option), `bench/idle_profile.sh` (`P99_EXTRA`, `P99_PLC_ARGS`).
- **Implementation.** At exit, before the runtime stops, the program writes `key=value` lines: run time, main loop passes, `lv_timer_handler` calls with mean and maximum duration, the comms counters (polls, failures, exceptions, connects, links lost, worst poll latency), the write counters and the storage worker counters. Nothing is collected without the option.
- **Size decision.** Compiled into the device program, the report added about 1.3 KB of code and pushed the stripped size over a page boundary: 313,520 to 317,616 bytes (the stripped size grows in steps of 4,096 bytes because each segment is page aligned). That is half of the remaining margin, needed for login, RTU and the audit chain, for a measurement feature. It is therefore compiled only into the measurement builds (`P99_REPORT`); the device program parses the option strictly and exits with status 2 and "available only in the measurement builds", as it does for `--unsigned-config`. Device program after: 313,520 bytes.

### C12. Lock contention measured (no change to the locking)

- **Problem.** The master prompt allows lock work only if a trace shows contention on the tag database or the alarm mutex.
- **Files.** `src/hmi/hmi_platform_posix.c`, `src/hmi/hmi_platform.h` (`HMI_LOCK_STATS`, measurement builds only), `Makefile.linux` (`EXTRA_CFLAGS`).
- **Implementation.** With `-DHMI_LOCK_STATS` every `hmi_mutex_lock` first tries the lock; if it was held by another thread it counts as contended. Two counters per mutex live in the unused part of the opaque mutex storage (checked at compile time), and a table keeps the mutexes in creation order (removed on destroy). The report (C11) lists them. Without the flag the code is the plain `pthread_mutex_lock` as before.
- **Result.** Bench build with the counters, 30 s, example configuration, overview screen. The six mutexes in creation order (`hmi_runtime_start_ex`): tag database, comms, writes, alarms, trend, storage worker.

| Mutex | Idle: taken, contended | Heavy updates (`fake_plc ... 10 40`): taken, contended |
|---|---|---|
| tag database | 2,505, 0 | 2,505, 0 |
| comms | 2,323, 1 | 2,320, 0 |
| writes | 1,072, 0 | 1,072, 0 |
| alarms | 5,940, 0 | 5,913, 0 |
| trend | 0, 0 (no trended tag in the example configuration) | 0, 0 |
| storage worker | 373, 0 | 339, 0 |

- **Decision.** One contended lock in about 12,000 is not a problem worth a change; lock hold times stay as they are. The friend's seqlock design stays rejected (master prompt section 11). The measurement does not cover operator interaction (no touches) or a trended configuration.

### C13. Rendering workload on the application side (evidence, no change)

- **Evidence.** The UI harness (`ui-host`) asserts that nothing is flushed in 2 s of steady state (`flushes=0`, an existing check), and its binder counters show 384 label updates against 10,651 skipped as unchanged in the scenario (C2 run). Screens are built lazily; the heap probe (C8) shows the same 56 KB at every screen size. No redundant invalidation or object creation was found, so nothing was changed. Per-interaction invalidation counts beyond the flush counters were not added.

### Checks rerun after C9 and C10

| Check | Result |
|---|---|
| Core tests (`test-fast`) | 3,307 checks, 0 failed (`writes` 650, `runtime` 193) |
| Core under ASan and UBSan (`make test`) | 3,307, no report |
| View, e2e, UI scenario under ASan | 208, 75, 205 |
| ThreadSanitizer, core and e2e | first run: one data race reported in the new test itself (it read the mock driver's plain write counter while the comms thread incremented it); the test now counts through the atomic probe driver that `test_runtime_sent_before_send` already used; rerun: no warning |
| `test-view`, golden, `ui-host`, `cli-exec`, `config-exec`, `liveness` | pass |
| Kill-loop, 150 rounds | PASS (125 kills, 25 clean stops) |
| Gates | full PASS (313,520 and 373,416); lite PASS (288,944 and 295,592) |
| `make warnings` | 0 in own files |
| Windows `test-hmi-all` and `ui-host` | core 3,285 (`writes` 650, `runtime` 193, including the wake-up test), view 208, golden identical, e2e 75, UI 205 |

## Phase 3

### C14. Keyed hash chain for the audit file (roadmap P1-3)

- **Problem.** The audit file had no tamper evidence: anyone who could write the file could change, remove or add a record without trace (the alarm journal has a per-line CRC against damage, which is not a protection against an editor). The comparative audit named the friend's keyed, hash-chained log as the thing to adopt.
- **Files.** New `src/hmi/hmi_auditchain.c`, `src/hmi/hmi_auditchain.h` (in `sources.mk`). Changed: `src/hmi/hmi_store.c`, `src/hmi/hmi_store.h` (sealing in the worker, rotation marker with `prev=`), `src/hmi/hmi_runtime.c`, `src/hmi/hmi_runtime.h` (`audit_key` in the storage options), `src/hmi/hmi_cfgsec.c`, `src/hmi/hmi_cfgsec.h` (the audit key derived as soon as the key file has passed its checks), `src/hmi/hmi_boot.c`, `src/hmi/hmi_boot.h` (the start-up verdict sealed; the key handed to the caller), `src/app/main_linux.c` (passes the key, wipes its copy), `src/app/p99_cfg.c` (the helper seals `CONFIG_CHANGED`), `src/app/p99_sign.c` (`verify-log`). Tests: new `tests/hmi/test_auditchain.c` (82 checks, in both Makefiles), `tests/hmi/test_boot.c` (7 more checks), `tests/hmi/config_exec_test.sh` (6 more checks on the real executable), the kill-loop child, verifier and self-test.
- **Design.** Described in `docs/STORAGE.md`, section 4, "Keyed hash chain". In short: a seal of 32 hex digits per line (the first 128 bits of HMAC-SHA-512 over the previous seal and the line), a key derived from the configuration key, each writer reading the previous seal from the file, a rotated file naming the seal it continues from, torn lines reported alone. The friend's design was adapted, not copied: HMAC-SHA-512 from the existing `hmi_mac.c` instead of his SHA-256, the existing storage worker as the writer, the existing readable line format with the seal appended, and the verifier in the existing signing tool.
- **Additive.** Without a key (the test build's `--unsigned-config`, the existing tests that start the runtime without one) the file is written exactly as before.
- **Tests and checks.**
  - `test_auditchain`: key derivation, seal format and strictness, a clean chain, the wrong key, an edited line, a deleted line, an inserted unsealed line, an inserted line with a made-up seal, two lines swapped, a removed seal, truncation at the end (passes: the documented limit), a torn line followed by more records, an unfinished last line, and the storage worker end to end: 30 records with at least two rotations checked as one chain and the live file alone, a restart continuing the chain, an outside writer between two records, a tampered rotated file found, and no seal without a key.
  - `test_boot`: the accepted verdict is sealed and verifies, a second start continues the chain, the key is handed out only for an accepted configuration, a rejection is sealed too.
  - `config-exec`: the real executable's audit file is fully sealed; `p99_sign verify-log` reports INTACT, finds one changed character (exit 1, line 1) and a deleted first line, fails with another key, and refuses to run without `--key` (exit 2).
  - Kill-loop, 150 rounds (125 kills): the child now seals its audit file as the device does, and the verifier checks the chain over all generations, allowing no wrong seal and at most one torn line per kill. Final state: 1,501 lines with valid seals, 0 wrong, 0 unsealed. A new self-test case (a sealed record changed so that it is still a valid record) is refused only by the chain check, as intended; the 13 earlier cases are still refused for their own reasons.
- **Other checks.** Core 3,406 checks plain, under ASan and UBSan, and under ThreadSanitizer (no report); e2e 75, UI scenario 205 and view 208, plain and under ASan; `cli-exec`, `liveness`; `make warnings` 0 in own files; Windows core 3,384 (including `auditchain` 82 and `boot` 20), view 208, golden identical, e2e 75, UI 205; full and lite gates pass.
- **Cost.** Device program: text 299,890 to 301,714 bytes (+1,824), stripped size unchanged at 313,520 (inside the page padding), static data +128 bytes (the store's copy of the key and flags). Per audit record: one read of at most 1 KB from the end of the file and 37 bytes more on disk.
- **Limits.** Lines cut off the end and removed whole files are not detectable; the key holder can forge; two writers at the same instant cause a visible fork (`docs/STORAGE.md`). The kill-loop exercises crashes of the running program only, not of `p99_cfg`.

### C15. The lite profile without the root helper `p99_cfg`

- **Problem and evidence.** After C14 the lite profile's code segment crossed a page boundary by 1,406 bytes: stripped 288,944 to 293,040 bytes against the lite limit of 290,000. The lite build linked the root helper `p99_cfg` (`P99_MULTICALL`: `cmd_apply` and its configuration-change code, about 20 KB), although the lite profile has no CONFIG view and so never leaves the requests the helper applies (`main_linux.c` already says "--spool is ignored: the lite profile has no CONFIG view").
- **Files.** `Makefile.linux` (`MULTICALL_SRC`, `MULTICALL_DEF`, empty with `LITE=1`).

### C16. Named-user PIN login (roadmap P1-1, owner decision Q7)

- **Problem.** The product had no authentication: the role was whatever `--role` claimed, and the audit file named nobody. The mentor's requirements include basic authentication; the comparative audit named the friend's PIN hashing, lockout and idle logout as the thing to adapt.
- **Design.** `docs/AUTHENTICATION.md`, first section. Users (at most 8) in the signed configuration; PIN of 4 to 8 digits; `PBKDF2-HMAC-SHA-512(HMAC-SHA-512(pin key, PIN), salt, 10,000)` with a pin key derived from the configuration key; 5 wrong PINs lock for 60 s, doubling to 16 minutes; logout after 5 minutes without a touch; LOGIN, LOGIN_FAILED, LOGIN_LOCKED and LOGOUT records in the audit chain; the logged-in user's name in every write and acknowledgement record; a login refused when its record cannot be made durable; `p99_sign add-user` and `del-user` (PIN on standard input) as the way to manage users and to recover. Adapted from the friend's `security.c` (PIN hash, lockout, idle logout); his shared role PINs, separate vault and factory PINs were not taken (master prompt section 11). A first version used a home-made iterated HMAC; it was replaced by standard PBKDF2 before the change was finished, as the earlier `docs/AUTHENTICATION.md` required ("a standard algorithm, never a home-made one").
- **Additive.** Without users, or in the test build with an unsigned configuration, the claimed role is used exactly as before; every existing test runs unchanged.
- **Files.** New: `src/hmi/hmi_auth.c`, `src/hmi/hmi_auth.h`, `src/ui/ui_login.c`, `tests/hmi/test_auth.c`. Changed: `src/hmi/hmi_config.h`, `src/hmi/hmi_config.c` (the `users` array, strict), `src/hmi/hmi_config_write.c` (users written back, so an on-device settings change keeps them; equality compares them), `src/hmi/hmi_cfgsec.*` and `src/hmi/hmi_boot.*` (the pin key, handed out only for an accepted configuration), `src/ui/hmi_ui.c`, `src/ui/hmi_ui.h`, `src/ui/ui_priv.h` (role and actor from the login, header button, test seams), `src/ui/ui_settings.c`, `src/app/main_linux.c`, `src/app/p99_sign.c`, `src/app/p99_cfg.c` (wipes the pin key), `sources.mk`, both Makefiles, `tests/hmi/ui_headless.c`, `tests/hmi/config_exec_test.sh`.
- **Tests.**
  - `test_auth` (67 checks): PBKDF2-HMAC-SHA-512 against four published vectors (one with two output blocks), confirmed with Python's `hashlib`; the hash depends on key, salt and PIN; the two derived keys differ; PIN format rules; login, wrong PIN, unknown user, a malformed PIN not counted; five wrong PINs lock for 60 s, the right PIN is then not even checked; the lock doubles and stops at the maximum; a success resets everything; input keeps a session alive, 5 idle minutes end it once; a lock logs out whoever was logged in; the module is off without users or without a key; the `users` array parsed and every rejection (empty, not an array, missing hash, a space in the name, unknown role, short or long salt, non-hex hash, unknown key, duplicate name, nine users); the serialize, parse and compare round trip keeps users, and a configuration without users writes none.
  - UI harness (20 more assertions): with users the header says LOGGED OUT, SEND is disabled and ACK ALL is denied even though the program was started with a claimed admin role; a wrong PIN is refused; the right PIN (after a deleted extra digit) logs in, the header says `op1 OPERATOR` and SEND is enabled; a write as op1 is confirmed and its RESERVED record carries `user=op1`; LOGOUT works; five wrong PINs lock (`LOCKED FOR 60 s`) and the right PIN is then refused; the audit file has LOGIN_FAILED, LOGIN, LOGOUT and LOGIN_LOCKED with the names; the login panel screenshot uses only palette colours (`shot_27_login.ppm`).
  - `config-exec` (14 more checks): `p99_sign add-user` adds and signs, the PIN is not in the file, a 2-digit PIN, a duplicate name and an unknown role are refused, the result verifies, the real program starts with login required and says a claimed `--role` is ignored, its audit file is intact, `del-user` refuses an unknown user and removes the last user, and without users the claimed role is used again.
  - Before the PBKDF2 switch the whole regression ran (core 3,469 plain, ASan and TSan; e2e, view, UI under ASan; executable tests; kill-loop 150 rounds; Windows core 3,447 and UI 224); after it, `test_auth`, the core tests (3,473), the UI harness (225), `config-exec`, strict warnings and the gate were rerun.
- **Size.** The login costs about 7 KB of code (separate `-Os` objects: `ui_login.c` 4,130 bytes, `hmi_auth.c` 1,539, parser +1,249, writer +660, `hmi_ui.c` +699). The first build crossed a page because the relocation table grew past 8 KB (the keypad's table of string pointers needs one 24-byte relocation per entry in the static PIE file); turning it into a table of fixed-size strings brought the file back. Device program: stripped 321,712 bytes (gate 322,000: 288 bytes of margin, and 226 bytes before the code segment needs another page), static data 375,496 (+1,952: the user records in the configuration copies). Lite: 268,448 bytes. Windows demo: 356,864 bytes (not size-gated; it is built without per-function sections, so whole objects are linked; the full cause of its growth from 311,296 was not investigated).
- **Limits.** `docs/AUTHENTICATION.md`, "Limits, stated plainly": short PINs are searchable offline with the configuration key; the lock is lost on restart; no encryption on Modbus TCP; nothing measured on target hardware.
- **Effect.** Lite: stripped 264,352 bytes (margin 25,648), static data 295,184.

### C17. Modbus RTU (roadmap P2-1, owner decision Q4): built and tested, not in the default device program

- **Problem.** Only Modbus TCP existed. The mentor's requirement is "Modbus TCP first, RTU next".
- **Size finding first.** libmodbus's RTU backend alone is 4,107 bytes of code (`modbus-rtu.c`, `-Os`). After C16 the device program had 288 bytes of gate margin and 226 bytes before its code segment needs another page, so RTU cannot be added to the default device program without moving the gate. The master prompt says to stop and report in that case, and the owner decides gate moves. RTU was therefore built as an option: `make -f Makefile.linux RTU=1 ...` (`-DHMI_RTU`, links `modbus-rtu.o` in the static build). The default device program is unchanged (TCP only, an RTU configuration is refused as unknown keys).
- **Measured.** `STATIC=1 RTU=1` device program: stripped 329,904 bytes (7,904 over the 322,000 limit), text 314,898, static data 377,480 (within its limit). Default device program: 321,712 bytes, gate PASS.
- **Implementation.**
  - Configuration (`src/hmi/hmi_config.h`, `hmi_config.c`): `plc.transport` `"tcp"` (default) or `"rtu"`; for RTU `device` (a path under `/dev/`, no `..`), `baud` (1,200 to 115,200, default 19,200), `parity` (`none`, `even`, `odd`, default even, the Modbus default), `stop_bits` (1 or 2, default 1), `unit`, the same timeouts, and `allowed_devices`, the RTU counterpart of `allowed_hosts`: a signed configuration must name the device it may open (`hmi_config_has_allow_list`, `hmi_config_host_allowed`, used by the signature check, the on-device editor and `p99_sign`). Host, port or allowed_hosts with RTU, and serial keys with TCP, are rejected.
  - Writer and comparison (`hmi_config_write.c`): the serial settings are written back and compared, so an on-device settings change keeps them.
  - Driver (`src/hmi/plc_libmodbus.c`, `.h`): `plc_libmodbus_create_rtu`; everything else (timeouts, the mapping of errors to timeout and transport classes, one request at a time) is shared with TCP, so the comms state machine, back-off, link health and the rule "no retry after a lost write reply" are unchanged.
  - `main_linux.c` picks the driver from `plc.transport`.
  - Build (`Makefile.linux`): `RTU=1`; the test programs and `p99_sign` are always built with RTU (`TCFLAGS`), so the RTU code is tested in every run; `warnings` checks it too; the Windows core tests are built with it as well.
- **Tests.** `test_rtu` (33 checks, in both Makefiles): parsing, the defaults, every serial setting, the allow-list checks, the round trip, 14 rejections, and TCP unchanged. `rtu-e2e` (17 checks, Linux): the real runtime and libmodbus's RTU code on one end of a pseudo-terminal pair, a fake RTU slave (functions 3, 4, 6 and 16, CRC-16) on the other: the first GOOD value after 20 ms; an audited write confirmed by the read-back and present in the slave's register, sent once; a silent slave (frames read, never answered) gives BAD data after 660 ms with 3 requests and the communication-loss alarm; the slave answering again gives GOOD data again (20 ms with the system libmodbus, 700 ms with the static musl build). It passes with the system libmodbus (dynamic) and with libmodbus compiled from `third_party` (static musl).
- **Checks after C17.** Core 3,506 checks under ASan and UBSan (including `rtu` 33) and under ThreadSanitizer (no warning, with `rtu-e2e` and e2e); `rtu-e2e` 17 and e2e 75 under ASan; UI harness, `cli-exec`, `config-exec`, `liveness`, `test-view`, `make warnings` (0 in own files, RTU code included); kill-loop 150 rounds PASS; config golden identical; full gate PASS (321,712) and lite gate PASS (268,448); Windows core 3,484 (including `auth` 67 and `rtu` 33), view 208, golden identical, e2e 75, UI 225.
- **Not verified.** A real serial line or adapter, line timing at low baud rates (the pty has no baud rate), RS-485 direction control, the emulated Pi's serial port. The friend's malformed-frame fault tests were not adapted yet.
- **Owner decision needed.** To ship RTU in the device program the full gate must move to at least 329,904 bytes (one more page: 330,000 would leave 96 bytes), or RTU stays a separate build. The full profile is unchanged and keeps the helper (`cmd_apply` present in `build-musl/prometheus99`). The decision removes no function from the lite profile that it could use; it is recorded here and in the preservation matrix.
