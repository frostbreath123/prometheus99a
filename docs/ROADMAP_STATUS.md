# Prometheus99 requirements audit and completion roadmap

Date of audit: 2026-10-22. Repository state audited: commit `2ca7c75` on branch
`feature/authorization-hardening` (one commit, local only).

**This audit was read-only. No implementation code was changed, nothing was
committed or pushed, and the frozen legacy prototype was not touched.** The
only changes made after the audit are this file and a pointer plus a state
summary in `PROJECT_RESUME.md` (section R).

Primary requirements baseline: the mentor's document "Prometheus99 Industrial
HMI Runtime Architecture and Engineering Documentation" (design baseline v0.2,
15 October 2026), kept outside the repository at
`C:\Users\Admin\Downloads\Prometheus99 Industrial HMI Runtime Architecture and Engineering Documentation.md`.
Section numbers below (§) refer to that document. `docs/ADR-0001-runtime-boundaries.md`
records the intentional deviations from it.

## Where things stand on 2026-10-23 (read this first)

This file is the requirements audit and completion roadmap. It was written as a read-only audit on
2026-10-22 and then kept current: the "Update" sections below, newest first (M4, then M1, M2, M3),
record what each milestone changed. Sections 1 and 2 describe the state at the time of the audit and are
kept as history; the requirements matrix (section 4) carries the current status of every row. Nothing in
this file was removed; the statuses, evidence, discrepancies, milestones M1 to M9, blockers and acceptance
criteria are all below.

Git: branch `feature/authorization-hardening`, in sync with `origin`
(`https://github.com/frostbreath123/prometheus99a.git`). Milestones M1 to M3 are commits `fe165c2`,
`e858465` and `defa45f`. M4 is commits `b6b7ef9` (the milestone) and `f3ccb68` (the Windows demo
scripts). Uncommitted at the time of writing: `docs/OPTIMIZATION_ROADMAP.md`, the handoff edits to this
file and to `PROJECT_RESUME.md`, and the generated `demo/prometheus99_demo.html` (left out on purpose).

| Milestone | State | Evidence and what is left |
|---|---|---|
| M1 Authentication and sessions | **Removed by decision on 2026-10-23** | the login, user store and Argon2id hash are gone; the role is a claimed `--role` (docs/AUTHENTICATION.md); the code is in `fe165c2` and the later commits |
| M2 Configuration integrity and safe startup | Done on the host | `e858465`; not run under real systemd as the service user, no power-cut test of the serial write |
| M3 Persistence and storage | Done on the host | `defa45f`; no real power cut, no trace ring (row 42) |
| M4 HMI functionality | Done on the host | `b6b7ef9`; screens, trends, priority, accounts, configuration helper `p99_cfg`, units, `--spool`; units never run under real systemd, nothing shown on a real display |
| M5 Raspberry Pi bring-up | Not started, blocked on hardware | everything on target |
| M6 Verification hardening | Partly | sanitizers, kill-loop, mutation checks, an aarch64 run under qemu-user and a CI workflow (never run) exist; cppcheck, clang-tidy, fuzzing and scripted fault injection on a real network are not done |
| M7 Measurement and soak | Host part done, board part blocked on hardware | host measurements with `bench/` scripts (start-up, transitions, touch-to-pixels, CPU, PSS, heavy update, recovery, soak); see ENGINEERING_REPORT.md; nothing on a board |
| M8 Compliance and documentation | Partly | ADR, README, LINUX_VALIDATION, SCREENS, AUTHENTICATION (removal), THIRD_PARTY (musl, font, LGPL note) and `.gitmodules` exist; threat model, IEC 62443 mapping, SBOM, licence review and version 0 tag are not done |
| M9 OPC UA spike | Not started | |

Tally of the 55 rows (2026-10-23): 28 CV, 5 IU, 10 PI, 8 NI, 2 BL, 1 OF, 1 RM (removed by decision: row 30).

New since the M4 update below: a size and memory plan, [OPTIMIZATION_ROADMAP.md](OPTIMIZATION_ROADMAP.md).
The owner wants a final size of a few hundred KB and a whole process under 1 MB. The plan fixes the
metric (PSS from `smaps_rollup`, framebuffer reported separately), gives the measured baseline (stripped
Linux executable 695 KB, static data 499 KB, Argon2id hash 8 MiB per login) and phases 0 to 7. All
"after" figures in it are estimates. It must be read with the mentor's email of 12 October 2026 (Modbus
TCP first with open-source libraries, LVGL and no Qt, develop on a PC then validate on a Raspberry Pi or
ARM board, measure startup, render and transition time, touch latency, CPU and memory, stability and
recovery, keep authentication, secure configuration, validation, timeouts and reconnection). That plan
touches rows 4, 5, 37, 48 and 50, and it needs five owner decisions (static musl or glibc, the password
hash memory cost, a lite profile, OPC UA as a separate build, the Pi model).

Also new: a Windows live demo (`demo/run_live.bat`, `build/hmi_win_demo.exe`, a Win32 window with the real UI
against the fake PLC) and a slideshow (`demo/run_demo.bat`). Both are demonstrations, not the product, and
`demo/README.md` states the differences from the device.

## Update 2026-10-23: size and memory work (phases 0 to 7 of OPTIMIZATION_ROADMAP.md), authentication removed

Status: implemented and verified on the development host only (WSL2 Ubuntu 24.04, Windows MSYS2, aarch64 under qemu-user).
Nothing ran on a Raspberry Pi, a real display, a real touch panel or a real PLC. Nothing is committed. The full account,
with measurements and test conditions, is in [ENGINEERING_REPORT.md](ENGINEERING_REPORT.md).

What changed for the requirements:

- **Row 30 (authentication) is removed by decision.** The owner chose to remove the login, the user store and the
  Argon2id hash instead of shrinking or replacing it. The role is now the claimed `--role` of the process (default viewer)
  in both builds, enforced in the core, not authenticated (`docs/AUTHENTICATION.md`). Rows that depended on accounts
  (login, roles from accounts, lockout, idle logout) are no longer met.
- **Row 7 (screens)** lost the login and accounts screens; **row 9 (pixel format)** went from not implemented to partly:
  RGB565 only, a framebuffer of another depth is refused, no 4-bit target exists in LVGL 9.2.2.
- Matrix tally now: 28 CV, 5 IU, 10 PI, 8 NI, 2 BL, 1 OF, 1 RM.

What exists now (host evidence):

| Milestone-like item | State |
|---|---|
| M6 verification | sanitizers (AddressSanitizer, UBSan, ThreadSanitizer) run clean on the development build, the kill-loop passed 150 rounds, an aarch64 build runs the suites under qemu-user. CI workflow written, **not run**. cppcheck, clang-tidy, fuzzing, scripted fault injection on a real network: not done |
| M7 measurement | host-only measurements of start-up, render, transitions, touch-to-pixels, CPU, PSS, heavy update, soak and recovery with scripts in `bench/`; the board part is not started |
| M8 documents | `.gitmodules` fixed; threat model, IEC 62443 mapping, SBOM, licence review, version tag: not done |
| M5 | not started, blocked on hardware |

Test counts on the final tree: Linux core 3,262 checks, view model 197, headless UI scenario 205 assertions (full) and
127 (lite), end-to-end 75, executable tests (`cli-exec`, `config-exec`, `cfgedit-exec`, `liveness`) pass, config golden
identical, screenshot gate 23 files pass; Windows core 3,240. The earlier totals (3,925 and 3,887) fell because about 620
checks belonged to the removed login layer.

## Update 2026-10-22: milestone M4 (screens, trends, priority, settings) is implemented

Status: steps 4.0 to 4.6 are implemented and verified on the development host only
(Windows with MSYS2, and WSL2 Ubuntu 24.04). Nothing ran on a Raspberry Pi, on a real
display, against a real PLC or under real systemd. Nothing is committed. The screens are
described in [SCREENS.md](SCREENS.md); the helper that applies a configuration change is in
[CONFIG_INTEGRITY.md](CONFIG_INTEGRITY.md); the record of how the work went is
[M4_HANDOFF.md](M4_HANDOFF.md).

Owner decisions on 2026-10-22 (recommended option each): D1 a root helper started by a
systemd path unit, with a narrow whitelist; D2 trend data in RAM only; D3 one alarm
priority per tag; D4 a paged overview; D5 the whitelist (alarm limits, deadband, delays,
priority, PLC host from the allow-list, port, unit); D6 account management on the device
for an Administrator.

What exists (host evidence): per-screen UI files and a tab framework; alarm priority
(`alarm.priority`, `hmi_alarm_list`); paged overview; alarm list with a per-alarm
acknowledgement that refuses an alarm that changed after it was drawn; `hmi_trend`
(fixed rings, min and max per bucket, only GOOD data, gaps otherwise) with a chart screen
and the config fields `trend` and `trend_bucket_ms`; a settings screen with a system view,
an accounts view and a configuration view; account management in `hmi_auth` (Admin only,
audit before effect, `by=<admin>` in the lines, own account protected, a password reset
releases a lock); `hmi_config_serialize` and `hmi_config_equal` (all 58 accepted golden
cases write out and read back identically); `hmi_cfgedit` (request format, whitelist,
bounds, spool hand-off); `p99_cfg` (the root helper); the units
`deploy/prometheus99-config.path` and `.service`; `--spool DIR` in the service.

Row changes in M4 (rows not listed are unchanged): row 7 (screens) PI to CV (H); row 8
(trend charts) NI to CV (H); row 21 (alarm priority) NI to CV (H); row 32 stays CV (H) and
its open part changes (on-device saving now exists, host-tested; the real systemd run is
still open); row 4 text updated for the larger pool. Tally by this count: 29 CV, 5 IU,
9 PI, 9 NI, 2 BL, 1 OF (55 rows).

Fresh host results after M4 (rerun them rather than trusting this list):

| Item | Result |
|---|---|
| Core modules, Windows host (`mingw32-make test-hmi`) | 3887 checks, 0 failed (see the intermittent failures below, now fixed) |
| Core modules, WSL2 (also runs the POSIX file-mode rules and the clock state) | 3925 checks, 0 failed |
| View-model | 313 checks, 0 failed on both hosts; end to end 75 checks, 0 failed |
| Config corpus | identical to the golden file; 58 accepted cases written out and read back identically |
| Headless UI scenario | 277 assertions, 0 failed on both hosts |
| Liveness, command line, authentication and configuration executable tests (WSL2) | pass |
| `cfgedit-exec` (WSL2, as root): `p99_cfg` against real files | passes: applied, stale and future serial, unknown and forbidden keys, host outside the allow-list, limit outside the range, symlink, wrong owner, group-writable file, second hard link, oversized file, named pipe, directory in place of the request, edited configuration, wrong key, missing signature, writable configuration, missing, linked and wrongly owned audit file, unsafe spool directory, serial 4294967295 |
| ASan with UBSan (WSL2) | 0 reports for `cfgedit`, `auth`, `users`, `config`, `cfgsec`, `cli`, `trend`, `alarm`, `store`, `runtime`, `auditfmt`, and for `p99_cfg` itself running the executable test |
| TSan (WSL2) | 0 reports for `cfgedit`, `auth`, `runtime`, `trend` |
| Mutation checks of the new rules (18 in the core, 16 in `p99_cfg`) | 33 of 34 caught; the survivor removes the final read-back assertion in `hmi_cfgedit_apply`, which no input can trigger because the writer is proven by the golden corpus (an equivalent change). The first run had 4 survivors; 3 tests were added for them |
| `systemd-analyze verify` of the three units (WSL2) | no messages; this is a syntax check, not a run |
| LVGL pool, headless harness | 119 KB of 172 KB in use with every screen and the login layer built, peak 122 KB; 0 heap calls in 2 s of steady state |

Intermittent failures seen on the Windows host (explained and fixed afterwards):
1. `runtime` failed at `tests/hmi/test_runtime.c:606` and sometimes crashed (4 of 20 runs). The
   slow-disk test made all 64 queued appends 40 ms each; `Sleep(40)` takes 47 ms on this host, so
   the drain (about 3.0 s) equalled the 3000 ms store stop budget and `hmi_runtime_stop` sometimes
   timed out. A timed-out stop leaves the store thread running, by design, and the next test
   re-initialised the same static memory under it. Fixed in the test: only the first four appends
   after the queue is filled are slow, so the queue is still full at stop and the drain takes about
   1 s. The product code is unchanged.
2. `runtime` failed at line 226 (`same_state_on_disk`, 4 of 40 runs): the test read the state file
   once, but a change after the first save is written only after the minimum interval (50 ms in
   that test). The test now waits up to 2 s for the file to catch up. 0 failures in 60 runs after
   both fixes (it was about 1 in 12 before).
3. `auth` failed at lines 881 to 891 (a user change refused because the account file could not be
   saved). A virus scanner or indexer that holds the file open for a few milliseconds without
   `FILE_SHARE_DELETE` makes `MoveFileEx` fail with access denied or a sharing violation. Reproduced
   with a small program that keeps opening the file: 20 of 30 `auth` runs failed. Fixed in
   `src/hmi/hmi_platform_win32.c`: the replace is retried for at most 200 ms on those errors (a
   code change, Windows development host only; the Linux layer is unchanged). With the same
   program running, 0 of 30 failed after the fix. The real failure on the host was not captured,
   so that it was the same cause is shown by reproduction, not by observation.

Defects found and fixed while finishing 4.5: a glibc macro named `BYTE_ORDER` collided with an
array name in `hmi_config_write.c` and broke the Linux build only; the writer wrote members
that hold their default, which made a 64-tag file overrun the parser's 1024-token budget; the
request parser left `base_serial` set on some refusal paths.

Documentation conflicts found while doing M4 (reported, not hidden):
1. The instruction that repository files use CRLF does not match the repository: the
   index and working tree are LF for the portable code; only the legacy prototype files
   are CRLF. New files were written as LF. `PROJECT_RESUME.md` section U also says "CRLF".
2. The roadmap gives the settings screen to the Engineer role; the mentor's document gives
   Engineer "setpoints, network settings" and Admin "users and security settings". M4 splits
   them: accounts for Admin, configuration changes for Engineer and above.
3. The mentor's document says the configuration holds screen templates; the code has none.
   The screens are fixed and built from the tag table.
4. `LV_MEM_SIZE` was raised from 128 KB to 192 KB. With every screen built the harness uses
   119 KB (host measurement), of which about 20 KB is the CONFIG view.
5. `hmi_cfgedit.h` said the service never holds the key. The deployment gives the service group
   read access to the key (it must verify the signature), so the sentence was wrong and was
   corrected. What protects the configuration is that the service cannot write it.
6. `docs/AUTHENTICATION.md`, `README.md` and `bench/CLAIMS.md` quoted the old state; they were
   updated in 4.6, except that `bench/CLAIMS.md` keeps its measured numbers and carries a note.
   `walkthrough.md` was not checked.

Not verified: the units under real systemd (including the restart of the service by the
helper), the two-rename replacement of the configuration and signature across a power cut or
a restart between the renames, rendering and touch of the CONFIG view on a device, behaviour
when the helper exits with status 3 and leaves a request (the path unit would start it again
until the start limit), and everything that needs a Raspberry Pi or a real PLC.

## Update 2026-10-22: milestone M1 (authentication and sessions) is implemented (superseded: removed on 2026-10-23, see AUTHENTICATION.md)

Decisions given by the project owner on 2026-10-22: roadmap order M1 to M5
approved; Monocypher chosen (D1); a root-owned key file for the configuration
MAC (D2, relevant from M2); coils and discrete inputs out of scope for now (D3);
a Raspberry Pi is expected "soon, date unknown" (D4); GitHub and CI setup is
wanted (D5) but was not started, and nothing was committed or pushed.

M1 result, host evidence only (Windows with MSYS2, and WSL2 Ubuntu 24.04): the
design, parameters, evidence and limits are in
[AUTHENTICATION.md](AUTHENTICATION.md). Rows 29 and 30 below changed from
partial and not implemented to complete and verified on the host. The sections
below this one are the original audit text of the same date; where a statement
there conflicts with this update, this update is the current one. In particular
the sentence in section 1 that the role "is a claimed role from `--role`" now
describes only the simulation build.

Fresh host results after M1 (rerun them rather than trusting this list):

| Item | Result |
|---|---|
| Core modules, Windows host (`mingw32-make test-hmi`) | 1148 checks, 0 failed |
| Core modules, WSL2 (`make -f Makefile.linux test`, also runs the POSIX file-mode tests) | 1168 checks, 0 failed |
| View-model | 163 checks, 0 failed |
| End to end | 75 checks, 0 failed |
| Config corpus | identical to the golden file |
| Headless UI scenario | 77 assertions, 0 failed; 0 flushes in 1.5 s idle |
| Liveness, command line, authentication executable tests (WSL2) | pass |
| ASan with UBSan, and TSan, every core module (WSL2) | 0 reports |
| Legacy prototype suite (Windows) | 48 checks, 0 failed; no legacy source file was changed |

Documentation conflicts found while doing M1 (reported, not hidden):

1. The M1 acceptance wording "a tampered user file is rejected" can only be met
   for corruption and careless edits. Without a key the format cannot stop a
   writer who is the service user (see AUTHENTICATION.md).
2. The roadmap says to store the user file with mode 0600. The service user
   must write it (counters, password changes), so the D2 design of a root-owned
   key file does not apply to it.
3. ADR-0001 rule 1 had to gain a second exception (`hmi_kdf.c` includes
   Monocypher), recorded in an ADR-0001 addendum.
4. The mentor's document asks for no steady-state heap. The password hash needs
   8 MiB of scratch memory per login attempt. It is allocated and freed per
   attempt and is not steady state, but it is a heap use the document did not
   anticipate.
5. The mentor's document lists "Admin (users and security settings)". There is
   no settings screen until M4, so users are managed with the offline tool.
6. The stale text named in discrepancy 14 was fixed in ADR-0001 and README.

Still open for M1: hash time on a Raspberry Pi, typing on a real touchscreen, a
deployed service under its own user, a power-cut test of the store write, and the
audit lines for acknowledgements and writes carrying the user name (M2 and M3).

## Update 2026-10-22: milestone M2 (configuration integrity and safe startup) is implemented

Decisions given by the project owner for M2: HMAC-SHA-512 from Monocypher's
optional files (no new dependency, matches the document's "HMAC"), and the
configuration, signature and key owned by root and only read by the service (the
recommended form of D2). Consequence, reported rather than hidden: the HMI cannot
save configuration changes itself, so "configuration save restricted to Admin" is
deferred to the settings screen in M4 and needs a privileged helper. Design,
ownership table, limits and evidence are in
[CONFIG_INTEGRITY.md](CONFIG_INTEGRITY.md).

Rows 32 and 33 changed from partial to complete and verified on the host. Row 26
stays partial: the audit lines now carry the user, but still no old value, no
wall-clock time, and the write is still synchronous. After M2 the tally is 20 CV,
4 IU, 15 PI, 13 NI, 2 BL, 1 OF.

Fresh host results after M2 (rerun them rather than trusting this list):

| Item | Result |
|---|---|
| Core modules, Windows host (`mingw32-make test-hmi`) | 1492 checks, 0 failed |
| Core modules, WSL2 (also runs the POSIX file-mode rules) | 1532 checks, 0 failed |
| View-model, end to end | 163 and 75 checks, 0 failed |
| Config corpus | identical to the golden file |
| Headless UI scenario | 84 assertions, 0 failed |
| Liveness, command line, authentication and configuration executable tests (WSL2) | pass |
| ASan with UBSan, and TSan, every core module (WSL2) | 0 reports |
| Legacy prototype suite (Windows) | 48 checks, 0 failed; no legacy source file was changed |

Documentation conflicts found while doing M2:

1. The roadmap says "configuration save restricted to Admin". With root-owned
   files the service cannot write the configuration, so on-device saving needs a
   privileged helper and moves to M4. `hmi_config_save_text` remains in the code,
   unused by the product.
2. The roadmap says the key must be readable only by its owner. The service
   must read it, so it is root:prometheus mode 0440. The protection is that the
   service cannot write the files and the directory, not that it cannot read the
   key.
3. The mentor's document says "HMAC"; HMAC-SHA-512 was chosen over keyed BLAKE2b
   so no acceptance question remains.
4. The mentor's document says to load "read-only defaults and show a banner" on a
   rejected configuration. The code shows a banner, makes no PLC endpoint and
   forces writes off (discrepancy 3 below), unchanged.
5. A shared-secret MAC cannot stop anyone who holds the key or is root.

Still open for M2: a run under real systemd as the `prometheus` user with the
root-owned files, a power-cut test of the serial state write, and the
privileged helper for on-device configuration changes (M4).

## Update 2026-10-22: milestone M3 (persistence and storage) is implemented

Decisions given by the project owner for M3: the plan was approved with the
recommended option for each of its five decisions. (1) Writes and
acknowledgements are two-phase: the call returns before the record is durable
and the action takes effect only when it is. (2) The write rate limits are
optional fields of the signed configuration. (3) The PLC timeouts are bounded in
the configuration so that the longest stall fits the service watchdog and stop
timeout. (4) At the size cap the audit file and the journal rotate (1 MiB, four
rotated files, a marker line); a failed rotation refuses the record and sets
the fault flag. (5) The trace ring (row 42) is deferred; only a storage fault
banner and counters were added. Design, formats, limits, evidence and the list
of what is not verified are in [STORAGE.md](STORAGE.md).

Rows 2, 22, 26, 27, 28 and 43 changed to complete and verified on the host. Row
41 changed from not implemented to implemented but unverified. Row 40 stays
implemented but unverified (no power-cut test is possible on the host), and row
42 stays partial. After M3 the tally is 26 CV, 5 IU, 10 PI, 11 NI, 2 BL, 1 OF.

Fresh host results after M3 (rerun them rather than trusting this list):

| Item | Result |
|---|---|
| Core modules, Windows host (`mingw32-make test-hmi`) | 2597 checks, 0 failed |
| Core modules, WSL2 (also runs the POSIX file-mode rules and the clock state) | 2635 checks, 0 failed |
| View-model, end to end | 168 and 75 checks, 0 failed |
| Config corpus | identical to the golden file, which was regenerated (see conflicts) |
| Headless UI scenario | 89 assertions, 0 failed on both hosts |
| Liveness, command line, authentication and configuration executable tests (WSL2) | pass |
| Kill-loop (WSL2) | 150 rounds, 125 SIGKILL and 25 clean stops, verifier passed after each; its self test (13 checks) refuses each fault it is meant to catch |
| ASan with UBSan, and TSan, every core module and the end-to-end test (WSL2) | 0 reports |
| Mutation checks of the new rules | 33 of 34 caught; the 34th is an equivalent change |
| Legacy prototype suite (Windows) | 48 checks, 0 failed; no legacy source file was changed |

Documentation conflicts found while doing M3 (reported, not hidden):

1. The ADR text on acknowledgements says the record is made before the effect
   and the action is refused if it cannot be recorded. That holds, but the call
   now returns `HMI_ACK_PENDING` and the outcome comes later. Recorded in the
   ADR addendum.
2. The roadmap says the UI thread must not block on the audit record. A login
   still waits for its record, at most 2 s, on the UI thread.
3. The roadmap says "all write-safety tests unchanged". The 280 earlier write
   checks are unchanged and pass. The runtime and UI tests had to change because
   acknowledgements are two-phase and the audit lines gained a trailing stamp.
4. The bounds of `response_timeout_ms` and `connect_timeout_ms` were lowered
   from 30000 to 2000 and 3000, which the roadmap did not list. With the old
   bounds the longest stall (92 s) could not fit `WatchdogSec=10` or
   `TimeoutStopSec=15`. One case of the golden corpus (case 040) changed, and 18
   cases were added.
5. The roadmap lists row 42 under M3 without a task for it; only the fault flag
   and counters exist.
6. `PROJECT_RESUME.md` said the working tree was clean while its own START HERE
   block was an uncommitted change. Corrected.

Still open for M3: a real power-cut test and the behaviour of `fsync` and the
rename on the storage of the target, the clock state with a real time source,
the trace ring, and grouping several queued records into one flush (all M5 or
later).

## 1. Executive summary

Genuinely complete, on the development host only (Windows MSYS2, WSL2 Ubuntu
24.04, fake PLC, one real-systemd transient unit under WSL2):

- Modbus TCP runtime through libmodbus with a connection state machine, backoff
  with jitter, N-miss link loss, and a comms-loss alarm.
- Typed tags with scale, offset, byte and word order, per-tag quality and
  staleness.
- Alarm state machine (debounce, deadband, acknowledgement, return to normal
  unacknowledged, persistence of state at clean shutdown).
- Write pipeline: validation, role and range checks, bounded queue, audit
  before send (fail closed), read-back, at-most-once transmission, ambiguous
  outcome UNKNOWN, no replay.
- Strict table-driven JSON configuration with a 184-case characterization
  corpus.
- Real LVGL v9.2.2 rendering, headless, with a pure view-model.
- Liveness supervision: sd_notify READY and WATCHDOG gated on UI, comms and
  supervisor progress; a real-systemd watchdog restart was observed under WSL2.
- Authorization in the core for alarm acknowledgement and tag writes. The role
  is a claimed role from `--role` (default viewer), not an authenticated
  identity. Acknowledgements are audited, fail closed.

Incomplete:

- Nothing has run on a Raspberry Pi or against a real PLC. No requirement is
  verified on target hardware.
- No authentication (login, password KDF, first-boot credentials, lockout, idle
  logout).
- No configuration integrity (CRC or HMAC).
- No storage worker thread, no alarm journal on disk, no periodic alarm-state
  save, no audit rotation or integrity, no wall-clock time marking.
- No trends and only one screen; no alarm priority.
- No fuzzing, no CI, no cppcheck or clang-tidy, no soak test, no Qt baseline, no
  OPC UA spike, no threat model, no IEC 62443 mapping, no SBOM.

## 2. Verified test evidence at the time of the audit

All results were produced during the previous session on the host. None is
target hardware.

| Item | Command or source | Result |
|---|---|---|
| Core test modules (decode, tags, comms, link_health, writes, alarm, config, runtime, watchdog, authz, cli) | `make -f Makefile.linux test`, `mingw32-make test-hmi` | 742 checks, 0 failed (writes 252, alarm 81, config 89, cli 66, runtime 64, authz 41, comms 39, link_health 38, watchdog 29, decode 24, tags 19) |
| View-model | `test-view` | 92 checks, 0 failed |
| End to end against libmodbus and the fake PLC | `e2e` | 75 checks, 0 failed |
| sd_notify sender | `test-notify` (Linux) | 13 checks, 0 failed |
| Config characterization corpus | `test-config-golden` | 184 cases, output identical to the golden file on Windows and Linux |
| Headless LVGL scenario | `ui-host` | 36 assertions, 0 failed; 0 flushes and 0 heap calls in 2 s idle |
| Liveness against the real executable | `make -f Makefile.linux liveness` | 8 of 8 |
| Command line strictness on the real executable | `make -f Makefile.linux cli-exec` | 15 of 15 |
| Real systemd | transient unit, `WatchdogSec=4`, SIGSTOP | watchdog timeout, SIGABRT, restart with a new PID, `NRestarts=1` (WSL2) |
| Sanitizers | ASan+UBSan and TSan over every module, notify and e2e | 0 reports |
| Windows regression | `mingw32-make clean all test-all ui-host check-linux` | exit 0, 0 warnings (legacy suite 48 checks) |
| Tag table access guard | `check-tagdb-access` | ok |

Counts may change as work continues; rerun the suites rather than trusting
this table.

## 3. Status key and evidence key

Status: CV complete and verified; IU implemented but unverified; PI partially
implemented; NI not implemented; BL blocked (hardware, environment, decision);
OF optional or future per the mentor's roadmap.

Evidence: H host unit test; S simulated PLC test; D real systemd (WSL2
transient unit). No row has Raspberry Pi or real PLC evidence.

Tally of the 55 rows at the time of the audit: 16 CV, 4 IU, 18 PI, 14 NI, 2 BL,
1 OF. After M1 (rows 29 and 30 changed): 18 CV, 4 IU, 17 PI, 13 NI, 2 BL, 1 OF.
All CV rows are host, simulated-PLC or WSL2 evidence only.

## 4. Requirements matrix

### Architecture

| # | § | Requirement | Status | Evidence and files | Remaining |
|---|---|---|---|---|---|
| 1 | 2, 4.1 | Layered C99, OS access only through the platform layer | CV (H) | ADR-0001, `src/hmi/hmi_platform.h`, `check-tagdb-access`, builds on Windows and Linux | none |
| 2 | 4.2 | Threads: UI, comms, storage worker | CV (H) | `hmi_store.*`: one storage worker with a bounded queue and a fault flag owns every durable write (M3); TSan clean; see STORAGE.md | Pi |
| 3 | 4.3 | One lock discipline, no IO under a lock | CV (H) | locks never nested; TSan and ASan clean; ack audit runs outside the lock | none |
| 4 | 4.4, 8.3 | Static tables, fixed LVGL pool, no steady-state heap | CV (H, app only) | `ui_headless`: 0 heap calls in 2 s; LVGL pool 192 KB, 119 KB in use with every screen built (it was 20 KB of 128 KB with one screen) | process PSS and Pi (row 50) |
| 5 | 8.7 | Release flags, strip, LVGL logs off | PI | -O2, -Wall -Wextra -Wpedantic, PIE, relro, fortify, `LV_USE_LOG 0` | `-Werror`, stripped release target, `size` report |
| 23 | 4.2, 5.3 | Alarm evaluation in the comms thread | CV (H) | intentional deviation recorded in ADR-0001: a separate supervisor thread evaluates alarms | none |

### UI

| # | § | Requirement | Status | Evidence and files | Remaining |
|---|---|---|---|---|---|
| 6 | 7.3, 8.1 | Real LVGL v9, binder, update on change | CV (H, headless) | `src/ui/hmi_ui.c`, `ui_headless` 36 assertions, 0 idle flushes | on-target behaviour (rows 9, 10) |
| 7 | 1, 7.1 | Screens: overview, alarms, trends, settings | CV (H) | overview (paged), alarms, trends, settings (system, configuration); headless UI scenario 205 assertions, screenshots checked against the palette; the login and accounts screens were removed on 2026-10-23; see SCREENS.md | rendering and touch on a device (row 50) |
| 8 | 8.1 | Trend charts | CV (H) | `hmi_trend`, `ui_trends.c`; min and max per bucket, only GOOD data, gaps; `test_trend` 90 checks, runtime test, UI scenario with the hung-PLC gap; ASan and TSan clean | Pi (rendering) |
| 9 | 8.1 | Pixel-format match, DRM, SDL development backend | PI | RGB565 only is compiled into the drawing code and the fbdev start refuses a framebuffer that is not 16 bits; no 4-bit target exists in LVGL 9.2.2 (SCREENS.md); no SDL or DRM; never run on a real framebuffer | probe bits per pixel on the Pi, add DRM if the Pi needs it (M5) |
| 10 | 6 | fbdev and evdev on a real device | IU, BL | compiled and linked; only `--fb /dev/null` was run | Pi hardware |

### Communications

| # | § | Requirement | Status | Evidence and files | Remaining |
|---|---|---|---|---|---|
| 11 | 6.1 | Modbus TCP, one connection, groups up to 125 registers, 0-based addresses | CV (S) | `src/hmi/plc_libmodbus.c`, e2e 75 checks | real PLC |
| 12 | 5.2 | State machine, backoff with jitter, N-miss, comms-loss alarm | CV (S) | `test_comms` 39, `test_link_health` 38, e2e: link declared lost at 601 ms for 3 x 200 ms | none |
| 13 | 6.2 | Bounded connect, keepalive, NODELAY, no SIGPIPE | IU | libmodbus sets NODELAY and MSG_NOSIGNAL (third_party source read); the connect bound is only a code comment; no keepalive or `TCP_USER_TIMEOUT` | iptables DROP test (M5) |
| 14 | 6.1, 9 | Exceptions, truncated frames, hung PLC | CV (S) | fake server modes | real PLC |
| 15 | 6.1 | Function codes 01, 02, 05, 15 (coils) | NI | holding and input registers only; the tag type list in the document has no boolean | decision D3 |
| 16 | 6.1 | Real-device word and byte order and addressing | BL | assumptions shared with the fake PLC | real PLC or simulator |

### Tags and alarms

| # | § | Requirement | Status | Evidence and files | Remaining |
|---|---|---|---|---|---|
| 17 | 5.1 | Typed tags, scale and offset, orders | CV (H) | `test_decode` 24, config golden | none |
| 18 | 5.1, 5.2 | Quality and staleness (larger of 3 x poll interval or 1 s), per-group restore | CV (H) | `hmi_tag_stale_limit_ms`, `test_comms`, `test_tags` | none (initial state is UNAVAILABLE, not BAD) |
| 19 | 7.2 | Dirty flag with deadband | PI | version counters and label-text comparison; deadband used only for alarms | decide whether the document's mechanism is required |
| 20 | 5.3 | Alarm state machine | CV (H) | `test_alarm` 81 | none |
| 21 | 5.3 | Alarm priority | CV (H) | `alarm.priority` in the signed configuration, `hmi_alarm_list` sorts by priority, then unacknowledged, then newest; golden cases added; alarm list screen | none for the host |
| 22 | 5.3, 8.6 | Alarm persistence and journal | CV (H) | since M3 the alarm state is rewritten on change within an interval and an hourly limit and at shutdown; the journal is on disk with a CRC per line, rotated, and reloaded at start; kill-loop of 150 rounds; see STORAGE.md | power-cut test and flash behaviour on the Pi |

### Writes

| # | § | Requirement | Status | Evidence and files | Remaining |
|---|---|---|---|---|---|
| 24 | 7.4 | Validation, role, range, queue, read-back, at-most-once, UNKNOWN | CV (H, S) | `test_writes` 252, e2e | none |
| 25 | 7.4, 10 | Audit before send, fail closed | CV (H, S) | same | none |
| 26 | 7.4 | Audit content: user, old value, new value, result, time; via storage worker | CV (H) | since M3 the record has user, old value and its quality, new value, raw, read-back, exception code, monotonic ms and a wall-clock stamp with the clock state; written by the storage worker, the UI thread does not wait (a login still waits up to 2 s) | Pi |
| 27 | 10 | Write rate limiting | CV (H) | `write_min_interval_ms` per tag and `write_max_per_minute` in the signed configuration, defaults 1000 ms and 30; `test_writes` | none for the host |
| 28 | 7.4 | Re-validation in the comms thread | CV (H) | the comms thread checks switch, tag, role, range and encoding again before sending (`REJECTED_REVALIDATION`); the login session is not re-checked | none for the host |

### Security

| # | § | Requirement | Status | Evidence and files | Remaining |
|---|---|---|---|---|---|
| 29 | 10 | Roles enforced in the core | CV (H) | `hmi_authz`, `hmi_alarm`, `hmi_writes`, `hmi_cfgedit`; since 2026-10-23 the role is the claimed `--role` of the process (default viewer) in both builds, nothing authenticates it (AUTHENTICATION.md) | none for the host; Pi |
| 30 | 10 | Authentication, password KDF, first-boot credentials, lockout, idle logout | RM (removed by decision, 2026-10-23) | the whole login layer, the user store, the password hash (Argon2id) and `p99_user` were removed on the owner's instruction to meet the memory goal; no replacement; see AUTHENTICATION.md | the requirement is not met; if it is wanted again a design with a small memory cost must be chosen (AUTHENTICATION.md, last section) |
| 31 | 5.4 | Strict configuration validation | CV (H) | config golden 184, `test_config` 89 | none |
| 32 | 5.4, 10 | Configuration integrity (CRC, HMAC), safe defaults and banner | CV (H) | `hmi_mac`, `hmi_cfgsec`, `p99_sign`; detached HMAC-SHA-512 signature, key file, CRC to separate damage from tampering, rollback serial, safe mode with banner; 223 cfgsec checks, `config-exec`; on-device changes through `p99_cfg` (`cfgedit-exec`, 868 `cfgedit` checks); see CONFIG_INTEGRITY.md | run under real systemd with root-owned files, including the helper and its path unit |
| 33 | 10 | Allow-list of PLC addresses | CV (H) | `plc.allowed_hosts` in the signed configuration; the tool will not sign and the device will not run a host that is not in it | none for the host |
| 34 | 10 | State that Modbus TCP is unauthenticated; threat model; IEC 62443 mapping | NI | README does not say it; no threat model | M8 |
| 35 | 10 | Non-root systemd hardening | IU | `deploy/prometheus99.service`; not installed under its user; `systemd-analyze security` not run | M5 |
| 36 | 10 | Device hardening (SSH keys, minimal image) | BL | Pi | hardware |
| 37 | 11 | Licence records and SBOM | PI | `docs/THIRD_PARTY.md`; no SBOM; crypto library undecided | M8 |

### Reliability and operations

| # | § | Requirement | Status | Evidence and files | Remaining |
|---|---|---|---|---|---|
| 38 | 7.1, 9 | READY, WATCHDOG, restart on hang | CV (D) | `tests/hmi/liveness_e2e.sh` 8 of 8; real-systemd watchdog restart under WSL2 | Pi |
| 39 | 9 | Hardware watchdog, read-only root overlay | NI | none | M5 |
| 40 | 9, 8.6 | Power loss during a write | IU | atomic rename and directory fsync implemented; the kill-loop (150 rounds of SIGKILL) and injected torn writes pass on the host, but they are not a power cut | M5 |
| 41 | 9 | Wrong clock, NTP flag | IU | every audit and journal line carries a wall-clock time and `sync=ok` or `sync=no` from the kernel clock state (read on WSL2 only; the Windows host always says `no`); BOOT markers order the runs | real NTP and a device without a clock (M5) |
| 42 | 6 | journald logging, trace ring | PI | stderr lines; since M3 a visible storage fault flag and counters; no trace ring | trace ring (not part of the M3 tasks) |
| 43 | 7.6 | Clean shutdown | CV (H) | the join budget defect is fixed (M3): the stall bound is `max(connect, 2 x response + 2000) + 2000`, one budget for both threads, then the storage queue is drained and the last alarm state written; `hmi_runtime_stop` reports whether everything stopped; the timeouts are bounded so that the stop fits `TimeoutStopSec`; test with a write in flight | real systemd stop on the Pi |

### Verification and performance

| # | § | Requirement | Status | Evidence and files | Remaining |
|---|---|---|---|---|---|
| 44 | 13 | Unit tests and sanitizers | CV (H) | section 2 | none |
| 45 | 13 | cppcheck, clang-tidy | NI | none | M6 |
| 46 | 13 | Fuzzing | NI | none | M6 |
| 47 | 13 | Fault-injection suite | PI | fake-server modes, SIGSTOP, SIGKILL done; no netem, iptables, cable pull, tamper or power-cut tests | M5, M6 |
| 48 | 13 | Headless recovery tests, CI, aarch64 | PI | headless scenarios in `test-all`; no CI, no aarch64 or QEMU, no 50-repetition timing | M6 |
| 49 | 13 | 72 hour soak | NI | none | M7 |
| 50 | 14 | Benchmarks: startup, render, touch, CPU, PSS, recovery x50 | PI, BL | host-only partial numbers in `bench/CLAIMS.md`; Pi numbers are TBD | M7 |
| 51 | 11, 14 | Qt baseline | NI | none | M7 |
| 52 | 16.6 | OPC UA spike | NI | none | M9 |
| 53 | 16.7, 5.4 | Modbus RTU, Modbus/TCP Security, process split, in-house client, CSV import, shelving | OF | none | after core completion |
| 54 | 12 | Deliverable documents: decision log, threat model, porting guide | PI | ADR-0001, README, LINUX_VALIDATION | M8 |
| 55 | 15 | Version 0 preserved and tagged | PI | local repository with one commit, no tag; `third_party/lvgl` and `third_party/libmodbus` are gitlinks without a `.gitmodules` file | small repository tasks |

## 5. Discrepancies between the document and the implementation

These are reported, not resolved. Items 1 and 2 are covered by ADR-0001.

1. Alarms are evaluated on a separate supervisor thread, not the comms thread
   (accepted in ADR-0001).
2. Tag values are stored as an exact raw 32-bit pattern plus a derived double,
   not a single float (accepted in ADR-0001).
3. On a rejected configuration the document says to load read-only defaults and
   show a banner; the code shows a banner, creates no PLC endpoint and forces
   writes off.
4. The document says all tags begin BAD; the code starts them UNAVAILABLE,
   shown as `---`.
5. The document describes a per-tag dirty flag with deadband; the code uses
   version counters and label-text comparison.
6. The document says the comms thread re-checks role and range before sending;
   the code validates only at submission (configuration is immutable).
7. The document's audit record includes user and old value; since M2 the code
   has the user, and still not the old value.
8. The document's function-code table lists coils and discrete inputs; the code
   supports holding and input registers only.
9. The document says Windows is dropped; the repository keeps a Windows host
   build and the frozen legacy prototype, by the project owner's decision.
10. The document says version 0 is tagged; no tag exists.
11. The document lists SDL2 as the development backend; the development harness
    is headless and there is no SDL backend.
12. The document's repository layout (`src/core`, `src/platform`, ...) differs
    from the actual layout (`src/hmi`, `src/ui`, `src/app`); cosmetic.
13. The document names Unity or CMocka; the tests use small in-repo CHECK macros.
14. Stale text in the repository: `docs/ADR-0001-runtime-boundaries.md` still
    says the POSIX platform file is not yet compiled and quotes 521 checks, and
    `README.md` line 77 quotes 535 + 79 + 75 checks and 22 UI assertions
    instead of the current 742 + 92 + 75 and 36.

## 6. Gaps by priority

- **P0, missing required controls with security consequence.**
  - Authentication (rows 29, 30) was missing at the time of the audit and is
    implemented on the host since 2026-10-22 (see the update above).
  - No configuration integrity (row 32). A file edit can enable writes and
    widen write ranges up to the engineering range.
  - No known code defect lets an unsafe or duplicated write bypass the current
    rules; the P0 items are absent controls.
- **P1, core product requirements.** Trends, screens, alarm priority (rows 7, 8,
  21). Persistence and the storage worker (rows 2, 22, 26, 27). Coil scope
  (row 15).
- **P2, deployment and production readiness.** Pi bring-up and display, touch,
  bits-per-pixel (rows 9, 10, 13); non-root hardened service and hardware
  watchdog (35, 36, 39); power-loss, clock and logging (40, 41, 42); shutdown
  join budget (43); comms re-validation (28).
- **P3, evidence.** cppcheck, fuzzing, fault injection, CI (45 to 48); soak,
  benchmarks, Qt baseline (49 to 51); threat model, 62443 mapping, SBOM (34, 37,
  54).
- **P4, optional.** OPC UA spike (52) and the row 53 items.

## 7. Completion roadmap

Order: M1 to M5 first. M5 depends on hardware and should start as soon as a
device exists, in parallel with M3 and M4.

### M1 Authentication and sessions (P0)

Status 2026-10-22: implemented and verified on the host (not on a Raspberry Pi).
See [AUTHENTICATION.md](AUTHENTICATION.md) for what was built, the parameters
that were chosen with the project owner (Argon2id, 3 passes, 8 MiB, 1 lane),
evidence, limits and what is not verified. The task list and acceptance text
below are the original plan.

- Requirements: rows 29, 30; §10 roles, credentials, lockout, idle logout.
- Tasks: user store file (atomic write, mode 0600) with salted password hashes
  from a proper password KDF; first-boot credential creation with no default or
  shared code; lockout with increasing delay after 5 failures; idle
  auto-logout; LVGL login screen with the on-screen keyboard; the session role
  feeds the existing `hmi_authz`; `--role` remains only a test and simulation
  option; audit login, logout and lockout events; never log credentials.
- Dependencies: decision D1 (crypto library). Touch credential entry design.
- Likely files: new `hmi_auth.*` and `hmi_users.*`, a vendored crypto source
  under `third_party/`, `hmi_ui.c`, `main_linux.c`, new `test_auth.c`, UI
  harness login scenario.
- Host versus Pi: all development and tests on the host; KDF cost must be
  re-measured on the Pi (memory and time parameters must suit a small device).
- Acceptance: no state-changing action (write or acknowledgement) without a
  valid session; known-answer KDF vectors pass; lockout timing verified with the
  fake clock; a tampered user file is rejected; the UI harness shows login and
  refusal; regression suites and sanitizers clean.

### M2 Configuration integrity and safe startup (P0)

Status 2026-10-22: implemented and verified on the host (not under real systemd,
not on a Raspberry Pi). See [CONFIG_INTEGRITY.md](CONFIG_INTEGRITY.md). The task
list below is the original plan; "configuration save restricted to Admin" is
deferred to M4 (see the update at the top).

- Requirements: rows 32, 33; §5.4, §10 config integrity, §9 tampered config.
- Tasks: CRC for accidental corruption and a keyed MAC for tampering over the
  configuration; key file with owner-only permissions stored outside the
  repository; a signing tool; an unsigned or invalid configuration gives the
  safe mode and banner with writes off; configuration save restricted to
  Admin; PLC allow-list check; fix the audit identity fields (user) once M1
  exists.
- Dependencies: M1's crypto choice; decision D2 (key storage design: a key
  readable by the service user does not stop code running as that user).
- Host demo: bit flip, truncation, wrong key, missing signature and downgrade
  tests.
- Acceptance: an unsigned or altered configuration can never enable writes;
  safe mode is visible on screen; tamper tests pass.

### M3 Persistence and storage (P1, P2)

Status 2026-10-22: implemented and verified on the host (not on a Raspberry Pi,
no real power cut). See [STORAGE.md](STORAGE.md) for what was built, the
decisions, evidence and limits. The task list and acceptance text below are the
original plan.

- Requirements: rows 2, 22, 26, 27, 28, 40, 41, 42, 43; §4.2, §8.6, §9.
- Tasks: storage worker thread with a bounded queue and a visible fault flag;
  alarm journal on disk; periodic alarm-state save when changed; audit rotation
  with a size cap and a boot marker line; audit records with user and old value;
  wall-clock and "unsynced" marking; write rate limit; comms re-validation;
  fix the shutdown join budget (use `hmi_runtime_worst_stall_ms`).
- Design constraint: audit before send must stay durable before the write is
  sent, and the UI thread must not block on it.
- Host demo: power-cut simulation and a kill-loop (SIGKILL at random times) with
  a valid reload every time; rotation and cap unit tests; soft-cap policy tests.
- Acceptance: no loss beyond the documented window; wear bounded by counters;
  all write-safety tests unchanged.

### M4 HMI functionality (P1)

- Requirements: rows 7, 8, 21; §1, §5.3, §8.1.
- Tasks: screen framework (overview, alarms, trends, settings, login); trend
  ring buffers with min and max decimation and append-only updates; alarm
  priority and an alarm list screen with per-alarm acknowledgement; settings
  screen for the Engineer role.
- Dependencies: M1 for login and settings; M2 for any signed settings change.
- Host demo: headless screenshots and assertions; flush counts stay low; LVGL
  pool tuned from the memory monitor.
- Acceptance: all required screens and trends demonstrable headless; idle flush
  count stays 0; memory within the pool.

### M5 Raspberry Pi bring-up and deployment (P2, blocked on hardware)

- Requirements: rows 9, 10, 13, 35, 36, 39, 40, 41, 47, 50; §6.2, §8.5, §9,
  §10 hardening, §13.
- Tasks: build on the Pi (aarch64); fbdev or DRM, evdev, touch calibration,
  bits-per-pixel match; install the unit under a non-root user and run
  `systemd-analyze security`; connect to a simulator or test PLC over Ethernet
  (never a production PLC); cable pull, iptables DROP (proves the connect
  bound), tc netem; hardware watchdog; read-only root overlay; power-cut script;
  first measured numbers.
- Optional preparation without hardware: an aarch64 build and test run under
  QEMU user mode (requires installing software, which needs the owner's action).
- Acceptance: the walking skeleton runs on the Pi with a real display and touch
  and survives link loss and restart; the service runs as non-root with watchdog
  restart.

### M6 Verification hardening (P3)

- Requirements: rows 45 to 48. Tasks: libFuzzer with ASan on the config parser
  and register decoder (and CSV import if added); cppcheck and clang-tidy clean;
  scripted fault-injection suite; CI definition (needs the owner's repository
  decision); headless recovery timing over 50 repetitions.

### M7 Measurement and soak (P3)

- Requirements: rows 49 to 51; §14. Tasks: 30-run startup (power-on and process
  start to first frame), render and transition time, touch latency confirmed by a
  240 fps camera, CPU, PSS from smaps_rollup, 50-repetition recovery, 72 hour
  soak with scheduled disconnects and per-minute PSS, CPU and allocation count,
  Qt baseline on the same Pi. Every number must link to a script and raw log in
  `bench/results`, and results where Qt wins must be reported.

### M8 Compliance and documentation (P3)

- Requirements: rows 34, 37, 54. Tasks: one-page threat model; IEC 62443-4-2
  mapping table (alignment, not compliance); SBOM; licence review including the
  crypto library; refresh README, ADR-0001 and LINUX_VALIDATION; state openly
  that Modbus TCP has no authentication or encryption; version 0 tag; `.gitmodules`.

### M9 OPC UA spike (P4)

- Requirement: row 52. Time-boxed open62541 spike with measured binary size and
  RAM. Optional items in row 53 only after core completion.

## 8. Blockers

1. Raspberry Pi hardware with a touchscreen. Blocks M5, M7 and every on-target
   claim.
2. Crypto library decision D1: one dependency for the password KDF and the
   configuration MAC (Monocypher, libsodium, or mbedTLS or OpenSSL), plus a
   licence check. The document says HMAC; a keyed BLAKE2b MAC would need the
   mentor's acceptance.
3. Key storage design D2.
4. A real PLC or a simulator (pymodbus, OpenPLC, diagslave) for word order,
   addressing and cable-pull tests.
5. Mentor rulings: coil scope (D3), the discrepancies in section 5, how
   credentials are entered by touch, and CI or GitHub setup (D5).
6. Time and equipment for the 72 hour soak and the 240 fps camera.

## 9. Outstanding decisions (all answered on 2026-10-22; see the update at the top)

D1 Monocypher, D2 root-owned key file (for M2), D3 coils out of scope for now,
D4 Raspberry Pi "soon, date unknown", D5 GitHub and CI wanted but not yet set up
(repository details not given), roadmap order approved. The original wording
follows.

- **D1, crypto library and HMAC approach.** Leaning: Monocypher (one small
  dependency for Argon2id and a keyed MAC), pending a licence check and the
  mentor's view on keyed BLAKE2b versus HMAC. Alternatives: libsodium, mbedTLS
  or OpenSSL (PBKDF2 and HMAC).
- **D2, key storage** for the configuration MAC.
- **D3, coil and discrete-input scope.** Leaning: out of scope for now, because
  the document's tag type list has no boolean type.
- **D4, Raspberry Pi availability** and whether M5 can start early.
- **D5, repository and CI setup.** A local git repository exists with one
  commit; GitHub and CI were not set up.
- Roadmap approval (M1 to M5 order) is also pending.

## 10. Definition of finished

Verifiable on the host:

- Every matrix row that does not depend on a Pi or a PLC is complete and
  verified.
- Authentication and session tests and configuration tamper tests pass.
- Persistence tests and the kill-loop pass; fuzzers for the parser and decoder
  run; cppcheck and clang-tidy are clean; ASan, UBSan and TSan are clean.
- The tag-access guard stays green and all existing regression suites still
  pass.
- The threat model, IEC 62443 mapping and SBOM exist.

Requires a Raspberry Pi-class target:

- The walking skeleton runs on the Pi with a real display and touch.
- The service runs as a non-root user with hardening verified and watchdog
  restart.
- Startup, render, touch latency, CPU and PSS are measured with raw logs.
- The Qt baseline is measured on the same Pi.
- The 72 hour soak shows flat PSS, CPU and allocation count.
- 50 recovery repetitions are recorded.

Requires a real PLC or industrial test environment:

- Word and byte order and addressing verified on a device.
- Cable pull, iptables DROP and tc netem recovery measured.
- Write read-back verified against a real device, using a test setup only.

Optional and not part of core completion: OPC UA spike, Modbus RTU,
Modbus/TCP Security, process split, in-house Modbus client, CSV import, alarm
shelving.

Production readiness must not be claimed until the acceptance criteria above
have actually been met.

## 11. Execution rules agreed with the project owner

- Complete major requirements first; defer minor polish and optional features.
- Inspect first, recommend, and wait for approval before implementing each
  milestone; verify each milestone before the next.
- Do not connect to a real PLC or issue real control writes.
- Do not move, rename, delete or modify the frozen legacy prototype.
- Do not commit or push unless asked. Do not set up GitHub.
- Preserve ADR-0001 and the existing architecture unless a demonstrated defect
  requires a change.
- Report documentation conflicts instead of silently choosing a side.
- Update documentation only with verified facts.
