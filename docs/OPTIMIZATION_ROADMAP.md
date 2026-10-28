# Optimization roadmap: a small binary and a process under 1 MB

Date: 2026-10-22, updated 2026-10-23. Status: phases 0 to 6 were carried out on the development host on 2026-10-23 and
phase 7 was done only as far as a host can do it; the results, the measurements with their test conditions and the
limits are in section 8 and in [ENGINEERING_REPORT.md](ENGINEERING_REPORT.md). Sections 1 to 7 are the plan as written
on 2026-10-22; where a decision or an estimate changed, section 8 says so. The baseline numbers were measured on the
development host (WSL2 Ubuntu 24.04, x86-64, gcc 13.3). Every figure marked "estimate" in sections 3 and 4 is a guess made
before the work; none of them is a result.

## 1. Goal and how it is measured

The project owner asks for two things: a final size of a few hundred KB, and a whole HMI runtime
process that uses less than 1 MB. The mentor's email sets the frame, and this plan keeps it:
LVGL and no Qt; open industrial protocols with open-source libraries, Modbus TCP first; develop on a
PC, then validate on a Raspberry Pi or an ARM board; measure startup, render and transition time,
touch latency, CPU and memory, long-run stability and recovery after a disconnection; keep basic
authentication, secure configuration, input validation, communication timeouts and automatic
reconnection.

"Less than 1 MB" is only meaningful with a fixed definition, so this plan fixes one:

- **Metric A (the target):** PSS from `/proc/<pid>/smaps_rollup` of the running `prometheus99` process on
  Linux, measured after the first frame and again after 10 minutes of normal operation, with the
  framebuffer mapping reported separately. The framebuffer of an 800 by 480 RGB565 display is 750 KB
  by itself and belongs to the display driver, so counting it would make the target impossible for any
  program. Both numbers will be reported, never only the better one.
- **Metric B:** the peak of Metric A during a login, because the password hash allocates memory (see 3.4).
- **Metric C (size):** the stripped executable in bytes, and separately the shared libraries it needs.
  A statically linked build is one file, which makes the size honest.
- The target counts the process only: not the kernel, not the framebuffer driver, not systemd.

## 2. Baseline (measured, host only)

| Item | Now |
|---|---|
| `prometheus99` Linux executable, built | 782 KB |
| same, stripped | 695 KB (`.text` 533 KB, `.rodata` 36 KB, `.eh_frame` 72 KB plus 12 KB header) |
| shared libraries needed | `libmodbus.so.5`, `libm`, `libc` (not counted in the 695 KB) |
| helper tools, stripped | `p99_cfg` 64 KB, `p99_sign` 51 KB, `p99_user` 43 KB |
| static data (`.bss`) | 499 KB |
| Windows live demo, idle | 16.4 MB working set, 4.6 MB private (includes the fake PLC; not the target platform) |
| LVGL pool in use, every screen built | 119 KB of a 192 KB pool |
| Argon2id password hash, default parameters | 3 passes, **8 MiB**, 1 lane: allocated at every login and every account change |

Largest static objects (bytes): the LVGL pool 196608; the runtime `g_rt` 131408; the journal reload
buffer `tail` 49152; the configuration scratch buffers 32769 twice; the JSON token table 16384; two
configuration structures 11968 each.

The runtime `g_rt` (131 KB) is made of: store queue 49 KB, trend rings 31 KB, tag database 15.5 KB,
alarms 13 KB, writes 9 KB, configuration copy 12 KB.

Two figures are not measured yet and are the first task (phase 0): the real PSS of the process, and the
size of the draw buffer and of the thread stacks that are actually touched.

## 3. What can be done, in order

### Phase 0: measure properly (no code risk)

- Add a script that starts the Linux executable under a fake framebuffer, waits for the first frame,
  and records PSS, RSS, `VmData`, `VmStk`, thread count and the size of every mapping from `smaps`,
  then repeats after 10 minutes and during a login. Keep the output in `bench/results`.
- Add a size report: `size`, `nm --size-sort`, and a per-object breakdown, for the stripped executable.
- Run both on WSL2 now and on the Pi when it exists. Without this phase every later number is a guess.
- Exit: a baseline table for Metrics A, B and C, committed.

### Phase 1: build flags (estimated saving 100 to 180 KB of file size, no RAM change)

| Change | Why | Estimated effect |
|---|---|---|
| `-Os` and `-flto` for the HMI and LVGL | smaller code, cross-module inlining of tiny functions | 10 to 20 percent of `.text` |
| `-fno-unwind-tables -fno-asynchronous-unwind-tables` | the executable never unwinds C++ exceptions or backtraces | up to 80 KB (`.eh_frame` is 84 KB now) |
| `-Wl,--gc-sections -Wl,--build-id=none`, keep `-fstack-protector-strong`, PIE and relro | already partly used; keeping the hardening costs little | small |
| Static link against musl (aarch64 and x86-64) | one file, no `ld.so`, no libc mappings counted in PSS; libmodbus is linked with gc-sections so only the TCP functions stay | adds about 60 KB for libc pieces but removes the shared mappings from Metric A |
| Keep libmodbus (open source, LGPL) | the mentor asked for open-source libraries | its TCP subset links in at an estimated 30 to 50 KB |

Risk: LTO can hide undefined behaviour that the sanitizers found nothing in; rerun ASan, UBSan and
TSan on the LTO build. LGPL static linking has licence consequences (relinking must stay possible);
`docs/THIRD_PARTY.md` must say so. Decision for the owner: static musl, or dynamic glibc as now.

### Phase 2: slim LVGL (estimated saving 100 to 180 KB of file size and 100 KB or more of RAM)

LVGL is the largest part of the executable. `config/lv_conf.h` currently enables almost every
widget (calendar, menu, message box, roller, spinbox, tabview, tileview, window, list, table, dropdown,
scale, span, slider, switch, canvas, animation image, image button, LED, arc and more), several
themes, the object-id and property-name tables, the observer module and the profiler. The screens use
buttons, labels, a chart, a text area, a keyboard, a button matrix, plain objects and flex layout.

- Turn off every widget, theme, format and feature that no screen uses; rebuild and let the compiler
  list what is still needed. Software drawing of other pixel formats (ARGB8888, AL88, I1, L8, RGB888)
  is present although the display is RGB565: switch off the unused ones. Switch off shadows, image
  transform and vector graphics if no screen needs them (the test screenshots will show if one does).
- Build one screen at a time instead of all five at start. `ui_nav.c` already has `reset`, `build` and
  `update` per screen; building on first visit and freeing on leave keeps the pool near the size of the
  largest screen (the CONFIG number pad alone is 16 buttons).
- Replace the 16 separate keypad buttons by one LVGL button matrix: one object instead of 16, and the
  pad costs a few KB instead of about 20 KB.
- Reduce the draw buffer from 48 lines to 16 or 24 (from about 77 KB to 25 to 38 KB). Cost: more flushes
  per frame; measure render time.
- Fonts: only Montserrat 14 is enabled now; keep it unless a screen needs a second size.
- Then lower `LV_MEM_SIZE` from 192 KB to the measured need plus margin (estimate 48 to 64 KB with lazy
  screens). The harness already reports pool use and heap-call counts, so the effect is visible.

Risk: touch targets and legibility must not shrink to save memory; the number pad keys are 96 by 46
pixels today and should not get smaller. Each removed LVGL feature is a chance for a compile or a
runtime failure that only a screen test shows; the 277-assertion UI scenario and the screenshots are
the safety net.

### Phase 3: runtime RAM diet (estimated saving 150 to 200 KB of static data)

| Object | Now | Plan | Estimate after |
|---|---|---|---|
| Journal reload buffer `tail` in `hmi_runtime.c` | 48 KB static | read the journal in small chunks and parse line by line | about 2 KB |
| Configuration scratch: `cfg.0` 32 KB, `s_chk_text` 32 KB, JSON tokens 16 KB, `s_chk` 12 KB, a second 12 KB copy | about 105 KB | one shared scratch area used by the parser, the editor check and the serializer, never at the same time (they are all on the UI or the start-up path) | 45 to 50 KB |
| Store queue (`HMI_STORE_QUEUE_LEN` 64) | 49 KB | the length is a safety margin chosen in M3; shorten to 16 and re-run the storage and kill-loop tests; the queue-full path already exists and is tested | about 12 KB |
| Trend rings (`HMI_TREND_POINTS` 480) | 31 KB | 240 points, or fewer trended series; both are configuration choices | 15 KB |
| Tag database and configuration (`HMI_MAX_TAGS` 64) | about 28 KB | make the maximum a compile-time profile value (32 for the small profile) | 14 KB |
| Thread stacks | default 8 MB virtual each | set 32 to 64 KB stacks for the four worker threads; measure the real high-water mark first | touched stack becomes small and bounded |
| Alarm and write tables | 22 KB | keep, they scale with tags | 11 KB with 32 tags |

Every change here keeps the same behaviour; the existing 3925 core checks are the regression gate. The
store queue and the journal change touch the durability design of M3, so the kill-loop (150 rounds)
and the injected torn-write tests must be rerun, not only the unit tests.

### Phase 4: the password hash (decision for the owner, large effect on Metric B)

The Argon2id default of 8 MiB is eight times the whole target. It exists once per login or account change
(the buffer is freed afterwards), but it is a peak that Metric B would show, and on a board with little
RAM it can fail. Options:

1. Argon2id with a small memory cost, for example 64 to 256 KiB and more passes. Resistance against an
   offline attack on a copied user file drops, because the attacker needs less memory per guess. The device
   already limits online guessing (lockout), and the user file is mode 0600 and owned by the service user.
2. PBKDF2-HMAC-SHA512, which needs almost no memory; the HMAC code already exists for the configuration
   signature. Weaker against GPUs than Argon2id at the same time cost, and adds a second hash design.
3. Keep 8 MiB and report Metric B honestly as a documented exception.

Recommendation: option 1 at 128 KiB, tuned so a login takes about the same time on the Pi (the time on the
target is still unmeasured), with the cost stored per account as it is now, so existing accounts keep
working. This weakens a security property to meet a size goal; the owner must decide and the change belongs in
`docs/AUTHENTICATION.md`.

### Phase 5: feature profiles

To keep the mentor's core small, add compile-time switches: a full build (everything today) and a lite
build. The core that must stay in every build: Modbus TCP, reconnection, timeouts, strict configuration
parsing and signature check, authentication and roles, audit records, alarms. Optional in the lite build:
the trend screen and the trend rings, account management on the device, the CONFIG view and `p99_cfg`, the
journal view. The helper tools stay separate programs and are already small (43 to 64 KB each).

### Phase 6: gates, so the size does not grow back

- A CI job (the CI plan is already approved and not started) that builds the stripped static binary, fails
  if it exceeds a budget (start with the Phase 1 result plus 5 percent), and records `size` output.
- A RAM budget test in the headless harness: it already reports LVGL pool use and heap calls; add a failing
  limit for pool use and for static data.
- Every phase must keep the full test set green (core, view-model, UI scenario, executable tests, the
  sanitizers) and must not remove a check to make room.

### Phase 7: validate on the target (the mentor's measurement list)

Needs a Raspberry Pi or an ARM board, a display and touch, and a test PLC or simulator. This is milestone M5
and M7 of `docs/ROADMAP_STATUS.md`, combined here with the size goals: startup time, render and transition
time, touch latency, CPU and memory, a long soak, recovery after PLC or network loss, and a Qt baseline on
the same board. Nothing in sections 1 to 6 counts as validated until these run on the board.

## 4. Expected result, stated carefully

These are estimates, not promises:

| Metric | Now | Estimate after phases 1 to 3 | Notes |
|---|---|---|---|
| Stripped executable | 695 KB (plus shared libraries) | 300 to 400 KB, one static file | under 250 KB would need the lite profile or a smaller UI |
| Static data (`.bss`) | 499 KB | 150 to 220 KB | LVGL pool 48 to 64 KB, runtime 40 to 60 KB, scratch 45 KB |
| Process PSS after the first frame | not measured | 0.6 to 0.9 MB is plausible | the first measurement decides; if it lands near 1.2 MB the lite profile is the way |
| Login peak | +8 MiB | +128 KiB | needs the phase 4 decision |

Why the 1 MB target may still be missed even after all this: LVGL keeps a draw layer and caches outside its pool in
some configurations, the C library and libmodbus add their own heap use, and the touched part of the stack and
of the code pages counts toward PSS. Only the Phase 0 script will say. If the full profile cannot be brought
under 1 MB, the lite profile is the fallback, and the result must be reported as it is.

## 5. What the mentor's email does and does not allow

- Modbus TCP stays first and uses open-source libmodbus; the in-house client in the roadmap (row 53) would
  save a little size but the email asks for open-source libraries, so it is not planned.
- LVGL stays; the Qt comparison is a measurement (M7), not a size-saving step.
- Modbus RTU and OPC UA are in the email. OPC UA with open62541 is likely to be larger than the whole target
  by itself, so it cannot be inside a few-hundred-KB executable; it would be a separate optional build or a
  separate process. RTU is small (it reuses the same library) and can be added without hurting the budget.
- Authentication, secure configuration, input validation, timeouts and reconnection are not reduced by any
  phase here, except the memory cost of the password hash, which is an explicit decision (phase 4).

## 6. Decisions needed

Answers (2026-10-23, in chat; the owner answered the first two questions, the others are open):

1. **Static musl build:** chosen. Done (phase 1).
2. **Password hash:** the owner chose to remove authentication entirely, not to shrink the hash and not to replace it
   (phase 4 became a removal, `docs/AUTHENTICATION.md`). The recommended option 1 (Argon2id at 128 KiB) was not taken.
3. **Lite profile:** not decided by the owner. It was built anyway (phase 5, `LITE=1`) because the full profile
   already meets the memory goal on the host and the lite profile costs little to keep; it is optional.
4. **OPC UA as a separate build:** not decided, not started.
5. **Raspberry Pi model:** not given. Nothing was measured on a board; aarch64 was only cross-compiled and run under
   an emulator.

Questions the work raised and nobody has answered: is Modbus RTU expected in the first prototype; is a framebuffer
of 16 bits per pixel acceptable on the target (the build now requires it); is it acceptable that the device has
no authentication (the requirement of the mentor's document that assumed accounts is unmet).


## 7. Order of work and rough effort

| Step | Work | Effort (estimate) | Needs hardware |
|---|---|---|---|
| 0 | measurement scripts, baseline | 1 to 2 days | no |
| 1 | build flags, static musl build, sanitizer rerun | 2 to 3 days | no (an aarch64 build needs a cross compiler or QEMU) |
| 2 | LVGL slimming, lazy screens, button matrix keypad, smaller draw buffer | 4 to 6 days | no |
| 3 | runtime RAM diet and its tests, kill-loop | 3 to 5 days | no |
| 4 | password hash change and docs (after the decision) | 1 to 2 days | no |
| 5 | feature profiles | 2 to 3 days | no |
| 6 | CI size and RAM gates | 1 to 2 days | no |
| 7 | validation on the board | 1 to 2 weeks | yes |

## 8. Results of the first pass (2026-10-23)

Everything below is a host measurement (x86-64, WSL2 Ubuntu 24.04), made with the scripts in `bench/` on the unmodified code
first (`bench/results/linux/baseline`) and on the final tree. The full report, with test conditions and what was not
measured, is [ENGINEERING_REPORT.md](ENGINEERING_REPORT.md).

| Metric | Baseline | After phases 1 to 3 (full profile) | Estimate of section 4 | Verdict |
|---|---|---|---|---|
| A: PSS after the first frame, without the framebuffer mapping | 1,422 KB | 552 KB (608 KB after ten minutes) | 600 to 900 KB "plausible" | beaten |
| A: the same with the 752 KB framebuffer mapping | 2,174 KB | 1,304 KB | not given | reported separately, as planned |
| B: peak at a login | +9,896 KB | no login (removed) | +128 KiB | not applicable |
| C: stripped executable | about 695 KB product (666,008 bytes measurement build) + three shared libraries | 297,088 bytes product, one static file (measurement build 284,800) | 300 to 400 KB | met |
| Static data | about 499 KB product (648,728 bytes measurement build) | 405,640 bytes product (470,416 measurement build) | 150 to 220 KB | **not reached**: the 112 KB LVGL pool, the 131 KB runtime and the configuration scratch remain; resident pages matter more than this figure (see Phase 3) |
| LVGL pool in use after start | 119 KB | 48 KB | 48 to 64 KB of pool | reached |
| Lite profile: executable / PSS without framebuffer | none | 268,416 bytes product (252,032 measurement build) / 484 KB | "under 250 KB would need the lite profile" | not reached for the product (18 KB above 250 KB) |

What the plan got wrong or changed:

- Phase 4 was not a change of the hash cost but the removal of authentication, by the owner's decision.
- The plan expected the largest saving in RAM from shortening the store queue and the trend rings. Measuring which pages were
  resident (`bench/touched_symbols.py`) showed that the cost was in clearing them at start, not in their length: the same queue
  and rings are kept and no longer cleared, so durability and history are unchanged.
- The plan's estimate of the draw buffer saving (to 25 to 38 KB) was met (32 KB), with no measurable CPU cost over ten minutes.
- "Build one screen at a time" was done for the two heaviest screens only; freeing screens was rejected because the screens keep
  state (pending configuration edits, the alarm and write status) in their widgets.
- A button-matrix keypad was not built: with lazy building the number pad costs RAM only for a panel that opens the CONFIG view.
- The unwind-table flags removed `.eh_frame` (68 KB in the baseline, 4 bytes in the final file); LTO, `-Os`, the static C library
  (only the functions used) and the disabled LVGL features carried the rest of the size reduction.

Phases 0 to 6 are done on the host. Phase 7 (the board) is not started; its host part is in the report, section 6.
