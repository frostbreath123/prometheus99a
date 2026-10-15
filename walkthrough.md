# Prometheus99 Walkthrough

`README.md` is the current status. This file records what was done in each
engineering pass and how to reproduce it.

## Pass 1: prototype audit (Windows prototype)

Fixed data races (mutex-guarded Layer 2), invented temperature/fan values
(validity flags, N/A), non-persistent test alarm, missing alarm hysteresis and
debounce, tautological watchdog, misleading claims, leaks and cleanup order.
48 tests. Details are in git-less history: compare with `../PROMETHEUSS.bak`.

## Pass 2: verification of pass 1, then the real HMI path

### 2.1 Regressions found in pass 1 and fixed
- The cross-thread watchdog could report a false UI stall while the window was
  being dragged/resized (Win32 modal loop starves the message pump). Fixed with
  a timer inside the modal loop; the UI heartbeat is also refreshed before the
  sensor thread starts.
- Early-exit paths in `main.c` skipped hardware teardown, and teardown could
  run while the sensor thread was still alive (use-after-free risk). Teardown
  now runs only after a confirmed join.
- Stack protection was claimed in design notes but not applied;
  `-fstack-protector-strong` is now in the build and verified in the binary
  (12 `__stack_chk_fail` call sites). ASLR/DEP verified via PE header
  (`DllCharacteristics 0x160`, `.reloc` present).
- Mutation-checked the legacy suite: removing the snapshot lock, the
  ack-needs-alarm rule, or fault latching each fails the tests.
  Caveat: the stress test alone is not proof of race freedom.

### 2.2 New portable runtime
Layout and rules: `docs/ADR-0001-runtime-boundaries.md`. Mutation checks on
the new suite (10 mutations: no expiry on link loss, replay, skipped
read-back, no debounce, no hysteresis, exception treated as link miss, audit
ignored, unknown config keys accepted, ack on normal) all fail the suite.
Two mutants initially survived (link-loss expiry; unknown key inside a tag);
tests were added for both.

### 2.3 Findings made by the new tests / harness (and fixed)
- A TCP connection that accepts but never answers kept the state "connected",
  so the comms-loss alarm never raised and the UI said CONNECTED over BAD
  data. Link health now uses time since the last valid reply.
- UI redrew every 50 ms because text colours were re-applied each tick;
  colours are now cached (44 -> 0 flushes per 2 s idle).
- Config rejected empty `units` strings; legitimate for counters.

### 2.4 How to reproduce
```sh
mingw32-make clean
mingw32-make test-all ui-host check-linux all
powershell -File bench/measure_process.ps1 -Exe prometheus99.exe -Seconds 20 -Out bench/results/x.csv
```

## Pass 3: architecture refinements (from the two reviews)

Staged, each stage rebuilt and re-tested before the next
(snapshot of the pre-pass sources: `../PROMETHEUSS.stage0`).

1. **Single link health** (`hmi_comms_link_health`). Removed the duplicated
   staleness formula from runtime and UI; the UI no longer reads `rt->cfg`.
   `hmi_alarm_step` takes the health value instead of a bool.
   Tests: thresholds and exact boundaries, delayed replies, connected-but-silent
   socket, disconnect/reconnect, alarm/predicate/UI-text consistency, unsigned
   wrap and backwards clock step. 5 mutants of the health function all fail
   the suite; one guard (explicit backwards-clock check) was found redundant
   by mutation and removed.
2. **Write pipeline extracted** into `hmi_writes`. `hmi_comms.c` 508 -> 328 (+ new `hmi_writes.c` 298)
   lines; the write module is tested directly (no comms, no transport) as
   well as through the existing comms scenarios and the libmodbus end-to-end
   run. 7 mutants (always-confirm, send without durable intent, expiry
   boundary, accept while lost, no link-loss expiry, 32-bit verify, ...) all
   fail the suite.
3. Behaviour change worth noting: writes are now refused while the link is
   `DOWN` or `NO_RESPONSE` (previously only while the TCP state was down).

## Pass 4: Linux execution and sanitizers (2026-10-15, WSL2 host, not Raspberry Pi)

Environment: Ubuntu 24.04 on WSL2, gcc 13.3.0, libmodbus 3.1.10. Details and
raw files: `docs/LINUX_VALIDATION.md`, `bench/results/linux/`.

1. Windows regression re-run (`mingw32-make clean all test-all ui-host
   check-linux`): exit 0; legacy 48, core 521, e2e 75 checks, UI 22
   assertions, 0 failed. This closes the earlier "not re-run" gap.
2. Linux suites: core 521, e2e 75, ui-host 22, all 0 failed; executable links.
3. The Linux executable was launched against a loopback config with no
   framebuffer. It printed `Error: cannot open framebuffer device`, kept
   running headless, and exited 0 on SIGTERM. Finding: `main_linux.c` did not
   check the framebuffer open result. Fixed: it now probes with `open()` and
   exits 2 before starting the runtime (verified, exit status 2). A device
   that opens but is not a framebuffer is still not detected.
4. ASan+UBSan: core and e2e clean. TSan: e2e clean; core first reported 3 data
   races, all in the test code (`test_concurrent_consumers`: unsynchronised
   mock register write, non-atomic read counters). None in `src/hmi`. Fixed
   in the test with atomics; TSan core now 0 reports in 3 runs. TSan needed
   `setarch -R` on this kernel.
5. After both fixes the Windows regression passed again (48 / 521 / 75 checks,
   22 UI assertions, `check-linux` clean).
6. No LVGL screen was displayed on any display on Linux; screenshots are
   in-memory renderings.

## Pass 5: display view-model split (2026-10-15)

Goal: take the presentation policy out of the LVGL binder so it can be tested
without a display.

1. New `src/ui/hmi_view.{h,c}`: pure functions from runtime state to text and
   a semantic tone (value cell, quality, alarm cell, link, alarm summary,
   setpoint step and text, write outcome wording, journal text). No LVGL, no
   globals, no I/O.
2. New `tests/hmi/test_hmi_view.c`, target `test-view` (Windows and Linux):
   79 checks, 0 failed. Run first on its own, before the binder was touched.
3. `src/ui/hmi_ui.c` now only maps a tone to a colour and sets label text. It
   shrank by about 100 lines; dead `s_mode` / `t_mode` were removed. The
   unused `hmi_ui_write_state_text` / `hmi_ui_submit_text` exports moved to
   `hmi_view_*`.
4. Verification after the rewire, same session: Windows
   `clean all test-all ui-host check-linux` exit 0 (48 / 521 / 79 / 75 checks,
   22 UI assertions, 0 failed, no warnings). Linux (WSL2, not Raspberry Pi):
   `test-view` 79, `test` 521, `e2e` 75, `ui-host` 22 assertions, `hmi`
   links; 0 failed, no warnings. Steady state still 0 flushes and 0 heap
   calls in 2 s; handler mean 13 us, p99 121 us (421 samples, one run).
5. Equivalence check: the five Linux scenario screenshots were compared
   byte for byte with those from before the refactor. Four are identical.
   `shot_4_commloss` differs only in the header link text. Ten repeat runs of
   the new build produced `PLC: DOWN` eight times and `PLC: NO RESPONSE`
   twice at that sample point, so this scenario sample is timing dependent
   (reconnect backoff with jitter against a hung server) and the harness
   accepts either. This was not isolated against a pre-refactor binary: no
   pre-refactor build was kept, so it rests on the repeat-run spread and the
   four identical frames.

## Pass 6: table-driven config schema (2026-10-15)

Safety net first, then the rewrite, then one behaviour fix, each verified.

1. Snapshot before any change: `../PROMETHEUSS.pre_config` holds the sources,
   tests, Makefiles, docs and the pre-change Windows test binaries.
2. Characterization corpus `tests/hmi/config_golden.c`: 184 cases (43
   accepted, 141 rejected) covering every error class, boundaries, duplicate
   keys, type errors, size and token limits. Its output (verdict, exact error
   text, every parsed field) was generated by the OLD parser and stored as
   `tests/hmi/config_golden.txt`. Targets: `test-config-golden` in both
   Makefiles. The first Windows output used CRLF; the driver now writes
   binary-mode LF so Windows and Linux compare byte for byte.
3. Rewrite of `src/hmi/hmi_config.c`: every object (top level, plc, tag,
   alarm, write) is described by a field table and walked by one engine
   (`parse_object`); cross-field rules remain as short `finish` code beside
   each table. Result: the golden output is byte-identical on Windows and on
   Linux, and under ASan+UBSan with 0 reports; all older suites unchanged.
   The file did not get shorter: 406 lines before, 544 after, because the
   tables and engine are longer than they look. The gain is that the
   rules (unknown key, duplicate key, range, type) exist once and a new key is
   one table row.
4. Behaviour fix found by the characterization: a repeated `writes_enabled`
   was accepted and the last value won, so `false` followed by `true` enabled
   writes. All other keys already rejected duplicates. It is now rejected
   (`duplicate key: writes_enabled`). The golden diff showed exactly one case
   changed (case 011); two new checks in `test_hmi_core.c` fail on the old
   parser (4 failed) and pass on the new one.
5. Verification after step 4: Windows `clean all test-all ui-host
   check-linux` exit 0, no warnings (48 / 525 / 79 / 75 checks, golden
   identical, 22 UI assertions). Linux WSL2: golden identical, 79 / 525 / 75,
   22 UI assertions, `hmi` links with 0 warnings.

## Pass 7: read-only tag-table accessors (2026-10-15)

Problem: the tag table's members (`cfg[]`, `groups[]`, `n_tags`, `ready`)
were read directly by the UI, the alarm engine, the write pipeline and the
comms scheduler, and the scheduler wrote `groups[].next_due_ms` through the
public struct. Snapshot before the change: `../PROMETHEUSS.pre_tagaccess`
(sources, tests, docs, Makefiles and the pre-change test binaries).

Done as separate verified steps:
1. `hmi_tagdb_is_ready / count / cfg / group_count / group / group_set_due /
   groups_set_all_due` added to `hmi_tags.{h,c}`, with 10 new checks
   (core 525 -> 535). Nothing else changed.
2. `hmi_comms.c` migrated: the scheduler now reads a `const` group view and
   writes the due time through `hmi_tagdb_group_set_due`. The phase-aligned
   reschedule arithmetic is unchanged (core and e2e identical).
3. `hmi_alarm.c`, `hmi_writes.c`, `hmi_ui.c` and the e2e and headless
   harnesses migrated. `hmi_view_journal_text` now takes tag names, not the
   config array, so the view-model no longer sees table internals.
4. Guard: `make check-tagdb-access` (in `test-all`) fails if any file outside
   `hmi_tags.c` and the white-box core test touches `db.cfg`, `db.groups`,
   `db.n_tags` and the other members. It passes now and fails on the
   pre-change snapshot (10 hits in the UI and harnesses alone).

Limits, stated plainly: C cannot hide a struct that is embedded by value in
`hmi_runtime_t`, so this is enforced by a grep guard, not by the compiler.
Verification: Windows `clean all test-all ui-host check-linux` exit 0, no
warnings (48 / 535 / 79 / 75, golden identical, 22 UI assertions). Linux
WSL2: the same suites, `hmi` links with 0 warnings; ASan+UBSan and TSan on
core and e2e: 0 reports.

## Pass 8: per-module core tests (2026-10-15)

The 1,366-line `tests/hmi/test_hmi_core.c` was split. Snapshot before:
`../PROMETHEUSS.pre_testsplit` (it still holds the monolith and its baseline
output).

1. Shared fixtures moved to `tests/hmi/test_support.{h,c}`: check counters,
   fake clock, mock PLC, the 4-tag fixture, `GOOD_CFG`, `parse`.
2. Eight programs: `test_decode`, `test_tags`, `test_comms`,
   `test_link_health`, `test_writes`, `test_alarm`, `test_config`,
   `test_runtime`, each with its own `main` and summary line. Test bodies
   were moved verbatim.
3. `tests/hmi/run_core_tests.sh` runs them and prints per-module and total
   counts; `make test-hmi` and `make -f Makefile.linux test` use it.
4. Check that nothing was lost: modules sum to 535 checks (same as the
   monolith) and the 29 SECTION lines are identical to the monolith's output.
5. Verification: Windows `clean all test-all ui-host check-linux` exit 0, no
   warnings. Linux WSL2: same suites, `hmi` links with 0 warnings; ASan+UBSan
   and TSan over every module: 535 checks, 0 reports.

## Pass 9: liveness supervision (2026-10-15)

Problem: nothing detected a stuck process. If the UI, comms or supervisor
thread stalled, the screen froze on its last values and nothing restarted the
program. Snapshot before the change: `../PROMETHEUSS.pre_liveness`.

Steps, each verified before the next:
1. `hmi_runtime` counts completed loop iterations of the comms and supervisor
   threads (`hmi_runtime_beats`) and reports the longest legitimate stall
   (`hmi_runtime_worst_stall_ms` = connect timeout + 2 x response timeout +
   2 s). The counters use unsigned arithmetic so they wrap without undefined
   behaviour. Runtime tests 38 -> 44 checks.
2. `hmi_watchdog` (pure logic): a ping is allowed only while every counter
   changed within the stall limit; it also parses `WATCHDOG_USEC` and
   `WATCHDOG_PID`. 29 checks. `hmi_notify` sends `READY=1`, `WATCHDOG=1` and
   `STOPPING=1` to `NOTIFY_SOCKET` (no libsystemd dependency; harmless when
   unset; Windows build returns false). 13 POSIX checks.
3. `main_linux.c`: READY after the first frame, ping from the UI loop only when
   the watchdog allows it, STOPPING on shutdown. The header shows an `HB nn`
   mark that changes once a second; it is off by default so the idle-flush
   assertion stays at 0 (the UI harness now asserts 0 idle flushes, then 2 to 5
   flushes and under 8,000 pixels in 3 s with the mark on).
4. `hmi_thread_join` now uses a monotonic deadline. The first version used
   `pthread_clockjoin_np`, which this ThreadSanitizer does not intercept, and
   TSan then reported 29 false data races; it was replaced by a 1 ms
   `pthread_tryjoin_np` poll and TSan is clean again.
5. `deploy/prometheus99.service`: `Type=notify`, `WatchdogSec=10`,
   `Restart=on-failure`, start limit, and the section 10 hardening options.

Verification (WSL2 host, not Raspberry Pi):
- `make -f Makefile.linux test-all`: 0 warnings; core modules 570, notify 13,
  view 84, e2e 75, golden identical, 28 UI assertions.
- `make liveness`, real executable against a fake notify socket, 8 of 8:
  READY once; pings at cadence; none while SIGSTOPped; resume after SIGCONT;
  none after SIGKILL; pings continue against a PLC that accepts and never
  answers (no false positive); clean SIGTERM exit; STOPPING sent.
- Real systemd (a transient unit, nothing installed, WatchdogSec=4): after
  SIGSTOP of the main process systemd logged "Watchdog timeout", killed it and
  restarted it with a new PID (`NRestarts=1`). Repeated after the final change.
- `systemd-analyze verify` on the unit file: only the expected "binary not
  installed" message.
- ASan+UBSan and TSan over every core module, the notify test and e2e: 0
  reports.
- Windows `clean all test-all ui-host check-linux`: exit 0, 0 warnings.

Not validated: a Raspberry Pi, the unit file installed under its real user and
paths, the hardware watchdog, and what the heartbeat mark looks like on a real
display.

## Pass 10: authorization in the core, strict role handling (2026-10-15)

Defects found by tracing the real execution path: ACK ALL had no role check
anywhere (the API took no role, so a Viewer could acknowledge), and
`role_from_name` turned any unknown `--role` string, including a typo, into
Operator. Snapshot before the change: `../PROMETHEUSS.pre_authz`.

1. `hmi_authz` is the single decision point (acknowledge needs Operator; a tag
   write needs the tag's minimum role). The existing write rule now goes
   through it with identical behaviour, and a corrupt role value is denied.
2. `hmi_alarm_ack` / `hmi_alarm_ack_all` take the role and return OK, NOTHING,
   DENIED or AUDIT_FAILED. Denied requests change no alarm state and add no
   journal entry. An alarm that changes while its record is being written is
   left unacknowledged.
3. Every acknowledgement attempt goes to the same audit file as PLC writes.
   A granted acknowledgement is recorded before it takes effect and is
   refused if the configured file cannot be written (fail closed); a denial
   is recorded best effort; with no audit path acknowledgements work and are
   not recorded. "Durable" means `fsync` reported success; nothing is queued.
4. `hmi_cli`: strict parsing. Unknown role, unknown option, missing value,
   repeated option or stray argument exits with status 2. The default role is
   viewer. The role is a claimed role: command-line selection is not
   authentication.
5. UI: ACK ALL is dimmed for a Viewer and the outcome is shown in the bottom
   panel; the core remains the enforcement point.

Verification (host only; no Raspberry Pi, no real PLC):
- Core modules 742 checks (authz 41, alarm 36 -> 81, runtime 44 -> 64,
  cli 66), view 92, e2e 75, config golden identical, UI scenario 36
  assertions, write checks (252) unchanged and passing.
- Linux real executable, 15 of 15: ten invalid invocations exit 2; no
  `--role` starts as viewer; viewer, operator, engineer and admin start.
- ASan+UBSan and TSan over every module, notify and e2e: 0 reports.
- Windows `clean all test-all ui-host check-linux`: exit 0, 0 warnings.

Remaining gaps: no authentication, lockout or KDF; the role is whatever the
launcher claims; config and audit files have no integrity protection; audit
timestamps are milliseconds since boot; the setpoint buttons are still shown to
a Viewer (the core refuses the write); no audit rotation; Modbus TCP itself is
unauthenticated and unencrypted.

## Interactive controls of the legacy prototype
`1`-`5` screens, `A` ack, `F` failover flag, `C` contrast, `T` test alarm,
`W` inject 1 s sensor-loop stall.

## Open items
See "Known gaps / next work" in `README.md`.
