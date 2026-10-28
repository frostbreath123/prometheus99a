# Engineering report: Prometheus99 size, memory and speed work

Date: 2026-10-23. Scope: phases 0 to 7 of `docs/OPTIMIZATION_ROADMAP.md`, removal of Argon2id, LVGL reduction, screenshot
testing, and the performance categories the mentor asked for. Everything in this report was measured or run on the
development host (Windows 11 with MSYS2; WSL2 Ubuntu 24.04 on x86-64; aarch64 under qemu-user). **Nothing ran on a
Raspberry Pi, a real display, a real touch panel, a real PLC or real systemd.** Nothing is committed or pushed.

## 1. Executive summary

**What was implemented.** The Linux HMI executable was measured properly for the first time (scripts, a file-backed fake
framebuffer, a fake PLC with fault injection), then reduced: authentication and the 8 MiB Argon2id hash were removed on the
owner's instruction; the device build became one static musl file built with `-Os` and link-time optimisation; LVGL was cut
to the object, label, button and chart widgets with its own theme, one 1-bit ASCII font, an 11-colour palette and a 32 KB draw
buffer; the runtime stopped making about 40 KB of unused memory resident (the resident part of its main object fell from 132 KB to 92 KB); two heavy screens are built when first opened; a lite
profile exists; size, memory and screenshot gates, a CI workflow and sanitizer builds were added. The work found and fixed
four visual defects, one UI gap (setpoint buttons enabled for a viewer), one data race in a test fixture and a scripting
error in the first measurements.

**The most important results (host, same scripts before and after):**

| | Before | After |
|---|---|---|
| Stripped executable | about 695 KB for the product (earlier record; 666,008 bytes for the measurement build), plus three shared libraries | 297,088 bytes for the product, one static file with the C library and libmodbus (lite 268,416; the measurement build 284,800) |
| Process PSS without the framebuffer, after the first frame | 1,422 KB | 552 KB (608 KB after ten minutes; lite 484 KB) |
| Peak during a login | +9,896 KB | no login |
| LVGL pool in use after start | 119 KB | 48 KB |
| Idle CPU, one core | 0.60 % | 0.63 % (no change) |
| CPU under heavy update | 2.29 % | 2.17 % |
| Warm start to a drawn first frame | 11 ms | 8 ms |
| Touch to first pixels, application level | not collected | median 8 ms, worst 34 ms (the 33 ms refresh tick) |

The owner's goals were "a whole process under 1 MB" and "a size of a few hundred KB": on the host both are met, with the
framebuffer mapping (752 KB, owned by the display driver) reported separately as planned. With it the process is 1.3 MB.

**Current state of the product.** A host-verified prototype: Modbus TCP runtime, signed configuration, storage, alarms,
four screens (three in the lite profile), roles checked in the core, audit trail. 3,262 core checks, 205 UI assertions (127 in the lite profile),
75 end-to-end checks, the executable tests, a 150-round kill-loop, a screenshot gate and the size gate all pass; the
sanitizers are clean on the development build. A one-hour host run with 11 injected PLC faults ended with no crash, no hang, a flat memory curve after the first eight minutes and a valid journal (this is not a 72-hour soak).

**Most significant remaining limitations.**
1. **There is no authentication.** The role is chosen by whoever starts the process; the audit trail names no person.
2. **Nothing was run on the target.** The 16-bit framebuffer requirement, touch, readability of the 14 px one-bit font, real PLC
   behaviour, systemd, and every timing on a Pi are unproven.
3. **The CI workflow has never run.** Soak length is one hour, not days.
4. The 4-bit-per-pixel framebuffer asked for is not possible with LVGL 9.2.2's software renderer; the 16-colour palette is
   done and the 4-bit conversion is shown on screenshots only.

## 2. Scope, rules and environment

**Request.** Implement phases 0 to 7 of `docs/OPTIMIZATION_ROADMAP.md` completely; remove Argon2id; reduce LVGL to what the
UI needs with a 16-colour scheme where practical; test with screenshots; treat speed and efficiency as requirements; measure
start-up, rendering and transitions, touch latency, CPU and memory, stability and recovery; report honestly.

**Owner decisions taken during the work** (asked once, because a security conflict and a build choice blocked the plan):

1. Remove authentication completely (not shrink and not replace the hash).
2. Static musl build for the device.
Not answered, so not decided: the lite profile (built anyway, optional), OPC UA, the Raspberry Pi model.

**Rules kept.** No commit, push or GitHub change. No real PLC connection and no real control write (the PLC is always a
fake on loopback). The frozen legacy prototype (`src/layer*.c`, `src/main.c`, `include/`, `tests/test_core.c`,
`prometheus99.exe`) was not touched. Documentation conflicts are reported (below), not silently resolved. Repository files are LF.
Saved files are in plain sentences.

**Documentation conflicts found and how they were handled**

- `PROJECT_RESUME.md` and `docs/ROADMAP_STATUS.md` still described the login as done; they were updated and the old
  sections marked superseded, not deleted.
- `docs/OPTIMIZATION_ROADMAP.md` quoted an executable of 695 KB and 499 KB of static data for the product build; the bench
  build measured 666,008 bytes and 648,728 bytes (it carries a 96 KB draw buffer the product allocates with `malloc`).
  Both are kept and labelled; comparisons in this report use the same bench build before and after.
- The earlier note said repository source files were CRLF; the repository is LF (reported earlier, unchanged).
- The roadmap's phase 2 asked for "build one screen at a time" and a "button matrix keypad"; both were examined and only the
  lazy build of two screens was done (section 4). The plan's estimates (its section 4) are set against the results in section 8 of
  `docs/OPTIMIZATION_ROADMAP.md`: size and PSS beat them, static data did not reach them.

**Environment.** Windows 11 Home 10.0.26200; WSL2 Ubuntu 24.04 (kernel 6.18.54, gcc 13.3, musl-gcc 1.2.4, aarch64 cross
compiler with qemu-user 8.2); 12 logical CPUs. All numbers are from this machine. They are not numbers for a Raspberry Pi
and must not be quoted as such. The measurement scripts are in `bench/` (`bench/README.md`).

## 3. Phase-by-phase implementation report

Phase definitions and acceptance criteria are those of `docs/OPTIMIZATION_ROADMAP.md` (sections 3 and 6 to 7); none were
invented. "Status" uses four words: **Done** (criteria met and evidence exists), **Done on the host** (met on the
development host only; the target part is open), **Partly**, **Not done**.

### Phase 0: measure properly

- **Objective:** a script that records PSS, RSS, thread count and mappings of the Linux executable under a fake
  framebuffer, a size report, a baseline table for metrics A (PSS), B (peak during login) and C (size).
- **Acceptance:** baseline table for A, B and C.
- **Changes:** `bench/measure_linux.sh`, `bench/startup_linux.sh`, `bench/size_report.sh`, `bench/touched_symbols.py`,
  `bench/README.md`, `tests/hmi/fake_plc_main.c` (a standalone fake PLC with fault injection by signal), a measurement
  build of the real executable (`make -f Makefile.linux hmi-bench`) that maps a regular file as its framebuffer
  (`-DP99_FILE_FB` in `src/app/main_linux.c`; draw buffer and flush behave as the fbdev driver's).
- **Tests and evidence:** baseline taken on the unmodified code before any other change: `bench/results/linux/baseline/`
  (10-minute run, 15 start-ups, size report, login hash peak).
- **Metric B** (the peak during a login) could be taken only from the hash tool: `p99_user bench`, three Argon2id hashes,
  peak resident 9,896 KB. A login on the screen needs a touch device and was not driven.
- **Status: Done.** Limits: the measured executable has a file instead of `/dev/fb0` and no touch input; the PLC is on
  loopback; x86-64 under WSL2.

### Phase 1: build flags

- **Objective:** `-Os`, LTO, no unwind tables, gc-sections, no build id, keep the stack protector, PIE and relro; static
  musl; libmodbus's TCP subset only.
- **Acceptance:** every suite rerun on the new build; sanitizers rerun.
- **Changes:** `Makefile.linux` option `STATIC=1 CC=musl-gcc` (static PIE, `-Os -flto -fno-unwind-tables
  -fno-asynchronous-unwind-tables`, libmodbus compiled from `third_party/libmodbus`, only `modbus.c`, `modbus-tcp.c`,
  `modbus-data.c`); `docs/THIRD_PARTY.md` (musl, LGPL note), `.gitmodules` (the two submodules had no URL entry, so a
  fresh clone could not fetch them), `.github/workflows/ci.yml`. Host set-up needed: `musl-tools` and links of the kernel
  headers into musl's include directory (documented in the workflow).
- **Tests and evidence:** all Linux suites passed on the static build (`make STATIC=1 ... test test-view ui-host e2e
  cli-exec config-exec cfgedit-exec liveness`). The same code was also built for aarch64 with the Ubuntu cross compiler
  and run under qemu-user: core 3,262 checks, view 197, end-to-end 75 and the UI scenario all pass
  (`bench/results/linux/arm64/`).
- **Sanitizers:** AddressSanitizer plus UBSan clean on eight suites; ThreadSanitizer clean on core, end-to-end and the
  liveness test, and clean on the UI scenario after a race in the test fixture was fixed (section 8). They run on the
  development (glibc, non-LTO) build: musl static has no sanitizer runtime, so **the LTO build itself was not run under
  a sanitizer**; it was run through the functional suites.
- **Status: Done on the host.** Limits: no real cross-built musl aarch64 binary (the cross check used glibc); musl and the
  Pi's kernel were not combined on a real board.

### Phase 2: slim LVGL

Section 4 is the dedicated report. Summary of the criteria of the plan:

| Item of the plan | Result |
|---|---|
| Turn off unused widgets, themes, formats, features | Done (28 widgets, 3 themes, 7 pixel formats, flex, grid, observer, profiler, object ids and more) |
| One screen at a time | **Partly:** TRENDS and the CONFIG view are built at their first visit and kept; the overview and alarm list are built at start. Freeing a screen on leaving was rejected (section 4) |
| Keypad as one button matrix | **Not done**, see section 4: the CONFIG view is now built lazily, which removes its cost for a panel that never opens it |
| Draw buffer from 48 lines to 16 to 24 | Done: 20 lines (32 KB); the CPU cost is in section 6 |
| Lower `LV_MEM_SIZE` | Done: 192 KB to 112 KB; the pool in use after start is 48 KB |
| Fonts | Done: one 1-bit ASCII font |
| 16-colour palette, colour depth | Done as far as LVGL 9.2.2 allows: 11 colours, RGB565 only; no 4-bit target exists (section 4) |

- **Tests and evidence:** UI scenario 190 to 205 assertions (grown as states were added), screenshot palette gate
  (`make ui-shots`), RAM gate in the harness, `bench/gate.sh` size gate. All pass on the final tree.
- **Status: Done on the host** for the items marked Done; the two items marked Partly or Not done are recorded as such.

### Phase 3: runtime RAM diet

- **Objective:** remove static RAM without changing behaviour; keep the durability tests green.
- **Changes, each chosen from a measurement of which pages are really resident** (`bench/touched_symbols.py`):
  1. The 64 slots of the storage queue (49 KB of address space) are no longer cleared when the runtime starts; each slot is written in
     full when a request is queued (`hmi_store_zero`). Queue length unchanged, so the durability design is unchanged.
  2. The trend rings of series that are not used are no longer cleared (about 4 KB per unused series, up to 23 KB with the
     two series of the example configuration).
  3. The journal reload buffer went from 48 KB to 64 lines at their longest (20 KB).
  Measured effect (`bench/touched_symbols.py`, 4 s after start): the resident part of the runtime object fell from 135,168 to
  94,208 bytes (41 KB). The rest of the PSS saving between phases 2 and 3 (688 to 592 KB) also contains the smaller draw buffer.
- **Not changed (and why):** the configuration scratch buffers (about 105 KB of address space, 48 KB resident): sharing
  one area between the parser, the editor check and the serializer touches the parts of the program that guard the
  configuration; the gain (about 20 KB) did not justify the risk in this pass. Tag table size and queue length were left
  as they are; the lite profile halves the tag table. Thread stacks were measured: the touched part is about 28 KB for all
  worker threads; the default size of the mapping does not count in PSS, so no change.
- **Tests and evidence:** core 3,262 checks, ThreadSanitizer and AddressSanitizer clean, and the kill-loop
  (`make killloop KILL_ROUNDS=150`): 150 rounds, 125 SIGKILLs and 25 clean stops, every round verified (audit lines,
  write ids, journal, alarm state; `bench/results/killloop_phase3.txt`).
- **Status: Done on the host.** No power-cut test exists.

### Phase 4: password hash

Became the removal of authentication (section 5). **Status: Done** as specified by the owner.

### Phase 5: feature profiles

- **Changes:** `LITE=1` (compile time): no TRENDS screen, no CONFIG view, no chart widget in LVGL, `HMI_MAX_TAGS` 32,
  nothing recorded for trends; `--spool` is ignored with a note. The harness has a lite mode (`P99_LITE`); CI builds and
  tests it. Kept in every build: Modbus TCP, reconnection, timeouts, strict configuration parsing and signature
  check, roles in the core, audit records, alarms.
- **Evidence:** lite UI scenario 109 assertions pass; lite static binary and its RAM are in section 6.
- **Status: Done on the host.** The trend and journal screens named as optional in the plan: the alarm journal line
  on the overview stays in both profiles.

### Phase 6: gates

- **Changes:** `bench/gate.sh` (stripped size, static data, no Argon2id symbol, no shared library, both profiles),
  the RAM gate in `tests/hmi/ui_headless.c` (pool after start and peak), the screenshot gate (`make ui-shots`),
  `.github/workflows/ci.yml` with three jobs (tests, static with the gates, sanitizers), `SAN=address|thread` in the
  Makefile.
- **Evidence:** the gates pass on the final builds (section 6). **The workflow was never run on GitHub**: nothing was
  pushed. Its commands are the ones that were run by hand.
- **Status: Done on the host, CI not run.**

### Phase 7: validation on the target

No Raspberry Pi, display, touch panel or PLC was available. What a host can do was done and is labelled as host
evidence: start-up, render, transition and touch-to-pixels timing of the application (loop paced as the real one is),
CPU and PSS at idle and under heavy update, a continuous run with scheduled PLC faults, recovery after a hung PLC and a
dropped connection, an aarch64 build run under an emulator. **Not done:** everything with a board, a real panel and
a real PLC; a Qt baseline; a 72-hour soak. **Status: Partly (host part Done, target part Not done).**

## 4. LVGL optimization report

### What was removed or disabled (`config/lv_conf.h`)

| Group | Disabled | Kept |
|---|---|---|
| Widgets | animimg, arc, bar, buttonmatrix, calendar, canvas, checkbox, dropdown, image, imagebutton, keyboard, led, line, list, menu, msgbox, roller, scale, slider, span, spinbox, spinner, switch, textarea, table, tabview, tileview, win (28) | object (the core), label, button, chart (the chart only in the full profile) |
| Themes | default, simple, mono | none; `src/ui/ui_theme.c` styles panels, buttons and the chart |
| Layouts | flex, grid | none (every position is explicit) |
| Software drawing formats | RGB565A8, RGB888, XRGB8888, ARGB8888, L8, AL88, I1 | RGB565 (the display) and A8 (text masks) |
| Fonts | Montserrat 14 with its FontAwesome symbols and kerning (13.4 KB of data) | one font, DejaVu Sans 14 px, 1 bit per pixel, ASCII 0x20 to 0x7E, no kerning, 1.5 KB (`src/ui/font`) |
| Other | observer module, built-in profiler, object ids and property names, label text selection, font placeholder | `LV_DRAW_SW_COMPLEX` stays on because the trend chart draws slanted lines |
| Images, decoders, file systems, vector graphics, animation images | already off | off |
| Memory | `LV_MEM_SIZE` 192 KB | 112 KB |
| Draw buffer of the Linux driver | 60 lines (96 KB) | 20 lines (32 KB) |

`LV_USE_PRIVATE_API` is on (compile time only) so that `ui_theme.c` can define a theme.

### Were the disabled features really excluded?

The build compiles every LVGL source file but the `#if` guards remove the disabled code. This was checked, not assumed: the
linker map attributes the code to groups (`bench/size_report.sh`), and `nm` on the final static executable shows no symbol of
a disabled widget, theme, or pixel format other than the ones listed as kept. `lv_theme_default_init` (12.5 KB in the
baseline), `lv_draw_sw_blend_image_to_argb8888` (13.6 KB), `..._to_al88` (11.7 KB), `..._to_i1` (9.5 KB), `..._to_rgb888`
(8.4 KB), `..._to_l8` (6.7 KB) and the widget event handlers of the removed widgets (`lv_arc_event`, `lv_slider_event`,
`draw_indic`, ...) were in the baseline's list of the 40 largest functions and are not in the final one.

Measured, with the same compiler flags before and after (the development build: dynamic glibc, `-O2`, no LTO; linker map
attributed by source group, `bench/size_report.sh`):

| LVGL part of the executable | Baseline | Final |
|---|---|---|
| Code and read-only data | 424,671 bytes | 182,097 bytes (-57 %) |
| Static data (the memory pool and small tables) | 208,398 bytes | 119,856 bytes (pool 196,608 to 114,688) |
| Whole stripped executable (same builds) | 666,008 bytes | 358,624 bytes (-46 %) |
| Pool in use after the screens are built | 119 KB | 48 KB |
| Draw buffer (heap or static, depends on the driver) | 96 KB (60 lines; the harness used 48 lines, 77 KB) | 32 KB (20 lines) |

By group (code and read-only data): draw 150,730 to 60,872; widgets 107,059 to 17,143; core 63,672 to 55,392; misc 34,931 to
30,131; themes 19,634 to 0; font 16,255 to 2,667; layouts 11,320 to 0. The static musl build with `-Os` and LTO then brings the
whole program, C library and libmodbus included, to 284,800 bytes. A check of the final static executable with `nm` found no
`*_create` or `*_class` symbol of any disabled widget, no theme init function and no blend function of a disabled pixel format;
the only widget classes present are label, button and chart.

### Colour depth and rendering strategy

The three things that were asked to be told apart:

- **A 16-colour visual palette:** done. The interface uses 11 colours (`C_*` in `src/ui/ui_priv.h`), flat fills, one-pixel
  borders, square corners, a one-bit font. A screenshot gate (`make ui-shots`) fails when a screen shows a colour outside
  the palette, except the trend chart, whose slanted lines are anti-aliased (0.6 percent of its pixels).
- **A 4-bit-per-pixel framebuffer:** **not done, and not possible directly with this LVGL version.** In LVGL 9.2.2 the
  software renderer can draw into RGB565, RGB888, XRGB8888, ARGB8888, L8, AL88, A8 and I1; `LV_COLOR_FORMAT_I4` exists
  for images and layers but not as a display format (`grep` of `src/draw/sw`, `src/core` and `src/display` finds no
  render path for it). A 4-bit framebuffer would need a conversion step in the flush callback (RGB565 to a 16-entry
  palette index, two pixels per byte). `tests/hmi/shot_tool.py i4` does exactly that conversion on the screenshots and it
  is exact for every screen except the chart (it needs 192 KB instead of 750 KB for 800 by 480). No 4-bit display driver was
  written because the Raspberry Pi framebuffer is 16 or 32 bits and the program keeps no second copy of the screen, so
  the saving would only exist on a panel that has a 4-bit interface.
- **LVGL's configured colour depth:** `LV_COLOR_DEPTH 16`, RGB565. The program now refuses to start on a framebuffer of
  another depth (exit status 2, with the reason), because only RGB565 drawing is compiled in. This is the lightest
  practical alternative and it breaks nothing silently, but it makes "16 bits per pixel" a deployment requirement.

### Visual defects found by screenshots and fixed

1. The first font tried (LVGL's `unscii` 16) is 16 pixels per character and broke the header and the tables; replaced.
2. A tag name of the longest allowed length (23 characters) ran into the value column of the overview; the name, value,
   quality and alarm columns now have fixed widths and end in dots when too long, and the value column moved right by 24 pixels.
3. The write status line of the overview (for example "WRITE #5: UNKNOWN (sent, no confirmation)") was drawn over the
   journal lines on the right; it now wraps in 284 pixels.
4. The long name in the alarm list likewise ends in dots instead of running into the state column.

### Remaining opportunities

Share one scratch area for configuration (about 20 KB); remove `LV_DRAW_SW_COMPLEX` by drawing the trend as vertical bars
(about 15 KB of code); free screens on leaving them once they keep no state in their widgets (about 30 KB at most); a
button-matrix keypad (a few KB of RAM, but it adds the widget's code). The font could go to 12 px or a hand-made 7 by 12 bitmap
font for another 0.5 KB; not worth the readability.

## 5. Argon2id removal report

### Decision and conflict

The first task message asked to remove Argon2id and not to replace it with a home-made or insecure hash. The product had
a login built on it (milestone M1 of the earlier work), so removing the hash meant either removing authentication or
putting another standard hash in its place. This was a security conflict, so one question was asked
(removal, PBKDF2-HMAC-SHA-512, or a smaller Argon2id). The owner answered: remove authentication. Nothing replaced it.

### What was removed

| Area | Removed |
|---|---|
| Core | `hmi_kdf.c/.h` (Argon2id, constant-time compare, wipe), `hmi_users.c/.h` (user file, password policy, CRC), `hmi_auth.c/.h` (login, lockout, idle logout, first boot, account changes). About 1,450 lines. |
| UI | `ui_login.c`, `ui_users.c` (the login layer and the accounts view), the LOGIN button, the login and accounts test seams of `hmi_ui`, the user wording of `hmi_view` (login, setup, account results, wait text, role colours). About 750 lines. |
| Tools | `src/app/p99_user.c` (offline account tool). |
| Command line and options | `--users`; the production build no longer refuses `--role`; `HMI_ACT_MANAGE_USERS` removed from `hmi_authz`. |
| Tests | `test_kdf.c`, `test_users.c`, `test_auth.c`, `auth_exec_test.sh`; the login, first-boot, lockout, idle-logout and accounts parts of `ui_headless.c`; the user-store parts of the executable tests. Core checks went from 3,925 to 3,262 on Linux and from 3,887 to 3,240 on Windows. |
| Build | `hmi_kdf.c`, `hmi_users.c`, `hmi_auth.c`, `ui_login.c`, `ui_users.c`, `p99_user` and `auth-exec` from both Makefiles; the `kdf users auth` entries of the test lists. |
| Deployment | `p99_user` and `--users` from `deploy/prometheus99.service`. |
| Documents | `docs/AUTHENTICATION.md` rewritten as the record of the removal; notes added to `STORAGE.md`, `CONFIG_INTEGRITY.md`, `M4_HANDOFF.md`, `LINUX_VALIDATION.md`; `README.md`, `SCREENS.md`, `THIRD_PARTY.md`, `ADR-0001` (addendum), `ROADMAP_STATUS.md`, demo texts updated. |

Kept on purpose: `hmi_wipe` and `hmi_crc32`, which other modules use, now in `src/hmi/hmi_util.c`; Monocypher itself
(SHA-512, HMAC-SHA-512, constant-time compare and wipe for the configuration signature). The vendored Monocypher files
were not edited; the linker drops the Argon2id code, and `bench/gate.sh` proves it by failing if an `argon2` or `blake2b`
symbol is found in the executable. The gate passed on the final static builds.

### Build and run-time change

- No 8 MiB allocation at login. Baseline: `p99_user bench`, three hashes, peak resident 9,896 KB (measured before the
  removal with `/usr/bin/time -v`). After: there is no hash.
- No user file, no lockout file, no first-boot screen, no login records in the audit file. The executable no longer reads
  or creates any file for accounts (checked by `cli_exec_test.sh`).
- `--users` is an unknown option and exits with status 2.
- The Monocypher code in the executable fell from 13,648 bytes (baseline linker map; `crypto_argon2` was 4,216 bytes and
  `blake2b_compress` 5,716 bytes) to about 1.5 KB (SHA-512 and HMAC only); the account code and screens went as well.

### How the safeguards were kept

- **No plain-text credentials:** there are no credentials at all. No password is read, stored or compared.
- **No bypass of access restrictions:** the core still decides every acknowledgement, write and configuration request
  from the role (`hmi_authz`), tested by `test_authz` (59 checks), `test_writes`, `test_alarm`, `test_cfgedit`, and by the
  UI scenario (a viewer is refused even when the button is forced). The default role is `viewer`, which can do none of
  them. A role above viewer exists only when the process is started with it.
- **No unauthorised operator writes:** `writes_enabled` in the signed configuration, the per-tag `write.role`, the audit
  record before sending, the second check before sending and the rate limits are unchanged. A rejected configuration
  still means safe mode.

### Security implications (stated plainly)

1. There is no authentication. Whoever can start the process or edit `/etc/systemd/system/prometheus99.service` chooses
   the role, including `admin`. The role is only as trustworthy as root access and the physical device.
2. The audit trail says which role acted, not which person (`user=-`).
3. Per-person accountability, lockout against guessing, idle logout and a separate administrator role at the screen no
   longer exist. The mentor's document asked for authentication in section 10; that requirement is recorded as removed
   by decision (row 30 of the requirements matrix), not as met.
4. One class of attack disappeared with the file: copying a user file and guessing passwords offline.

### Remaining authentication limitations and what to do

Listed in `docs/AUTHENTICATION.md`, last section. If authentication returns, choose the memory cost first (the old 8 MiB
per hash was eight times the whole process goal), use a standard algorithm from a reviewed library and never store a
credential in plain text.

## 6. Performance results

**All figures are host figures** (x86-64, WSL2 Ubuntu 24.04, 12 logical CPUs; fake PLC on loopback; a regular file as the
framebuffer) and are not representative of a Raspberry Pi. "Baseline" is the code as it was at the start of this work
(dynamic glibc, `-O2`, the full LVGL configuration, Argon2id), built and measured with the same scripts on the same
machine. "Final" is the static musl `-Os` LTO build of the final tree. The raw files are in `bench/results/linux/`
(`baseline`, `final_full`, `final_lite`, `final_stress`, `baseline_stress`, `final_recovery`, `final_soak`, `static_suites`).
Every row says what was run, so that it can be repeated.

### Summary table

| Category | Baseline | Final (full profile) | Test conditions | Status |
|---|---|---|---|---|
| **Start-up** (launch to a completely drawn first frame) | cold 20 ms; warm median 11 ms (min 10, max 15; 14 runs) | cold 18 ms; warm median 8 ms (min 7, max 12; 14 runs). Lite: cold 17 ms, warm median 10 ms | `bench/startup_linux.sh`, 15 launches, run 1 after dropping the page cache; the file framebuffer's last pixel row non-zero, polled every 2 ms; includes signature check, runtime threads and the screen build; excludes a real display driver | **Pass** (no budget was set); host only |
| **Rendering** (application level, into memory) | `lv_timer_handler`: mean 96 us, p50 13, p95 244, p99 1,255, max 19,670 us (48-line buffer, a shorter scenario with the login screens) | mean 59 us, p50 8, p95 295, p99 1,091, max 4,114 us (20-line buffer; 2,652 samples) | `tests/hmi/ui_headless.c` scenario, 30 Hz loop, real LVGL and runtime, fake PLC; not comparable one to one (different scenarios) | **Pass**; framebuffer presentation **not measured** (no display) |
| **Screen transitions** | not collected | press to last pixels: median 0.8 ms, p95 33.0 ms, max 67.2 ms (n = 96); first visit of TRENDS (lazy build included) 6.1 ms, of SETTINGS 0.7 ms | same harness, 25 rounds through the four screens with alarm changes in between; the p95 is the wait for LVGL's 33 ms refresh tick | **Pass**; host only |
| **Touch response** (application level) | not collected | press to first pixels of the pressed button: median 8.3 ms, p95 32.8, max 33.8 ms; release to first pixels of the new screen: median 25.8 ms, p95 33.6, max 35.0 ms; 120 of 120 touches registered, 0 missed, 0 on the wrong screen | harness injects press and release through an LVGL pointer device at a random moment of the loop's sleep, with PLC polling and alarm changes going on; loop paced like `main_linux.c` (5 to 33 ms). Excludes the touch controller, the driver and the panel | **Pass** at application level; **Not measured** on hardware |
| **CPU, idle** | 0.60 % of one core (10 minutes, 4 tags, poll 200 to 1,000 ms) | 0.63 % (10 minutes); lite 0.61 % (5 minutes) | `bench/measure_linux.sh`, utime plus stime over the run | **Pass**; the 20-line draw buffer did not raise idle CPU measurably (an early 60 s sample that suggested it was noise) |
| **CPU, heavy update** | 2.29 % of one core (5 minutes) | 2.17 % | fake PLC moving the data every 10 ms at 40 times speed, every tag polled every 50 ms (the minimum), about 60 requests per second | **Pass** |
| **Memory, steady state** | PSS 2,174 KB after the first frame (1,422 KB without the framebuffer mapping), 2,183 KB after 10 minutes (1,431), RSS 4,228 KB, 4 threads | PSS 1,304 KB (552 KB without the framebuffer) after the first frame, 1,360 KB (608) after 10 minutes, RSS 1,364 KB, 4 threads. Lite: 1,236 (484) to 1,276 (524) in 5 minutes | PSS from `smaps_rollup`, the framebuffer mapping reported separately (752 KB); measurement build: the product also links the framebuffer and touch drivers (12 KB more code), so its PSS is at most about 12 KB higher | **Pass** for the goal "process under 1 MB" on the host; the framebuffer is separate by definition |
| **Memory, heavy update** | PSS 2,203 KB, 2,200 KB after 5 minutes (1,451 and 1,448 without the framebuffer) | PSS 1,304 KB, flat for 5 minutes (552 without the framebuffer) | same load as above | **Pass** |
| **Memory, login peak** | +9,896 KB (`p99_user bench`, three hashes: the largest resident size of that process) | none: there is no login | `/usr/bin/time -v` of the old tool | **Not applicable** after the removal |
| **Binary size** | product about 695 KB stripped (the earlier record of this repository); measurement build 666,008 bytes; both plus `libmodbus.so`, `libc`, `libm` | product 297,088 bytes stripped, one static file including the C library and libmodbus (about -57 %); lite 268,416. Measurement build 284,800 (252,032 lite). The product is 12 KB larger than the measurement build because it links the framebuffer and touch drivers | `bench/size_report.sh`, `bench/gate.sh` | **Pass** |
| **Static data (.data + .bss)** | product about 499 KB (earlier record; `.bss`); measurement build 648,728 bytes | product 405,640 bytes (lite 321,760); measurement build 470,416 (386,512) | the baseline measurement build included a 96 KB draw buffer, the final one has 32 KB | **Pass** |
| **LVGL pool in use after start** | 119 KB of 172 KB (peak 122 KB) | 48 KB of 101 KB; peak 74 KB over a scenario that opens every screen and the number pad. Lite 46 KB | the harness prints it; it also fails above 56 KB after start or 88 KB at the peak | **Pass** |
| **Continuous operation** | not collected | one hour (3,604 s) with 11 injected faults (6 hangs and 5 dropped connections, 60 s each, one every 5 minutes), 29,664 Modbus requests; no crash, hang or restart; PSS rose from 1,324 KB to 1,360 KB in the first 452 s and stayed at 1,360 KB for the remaining 52 minutes; 4 threads; 3 to 4 open files; CPU mean 1.03 %, maximum 1.20 % of one core (higher than the idle 0.63 % because of the outages); the alarm journal grew by 219 lines, all valid, the alarm state file valid, 1 run; exit status 0 on SIGTERM; the audit file did not change (no operator action in this run) | `bench/soak_linux.sh`, final static bench build, fake PLC with a fault every 5 minutes | **Pass** for one hour on the host. **Not a 72-hour soak.** Disk writes per process **Not measured** (no `/proc/pid/io`); no touch or screen input during the run |
| **PLC and network recovery** | not collected by script; the end-to-end test has the same cases | see the table below | `bench/recovery_linux.sh`; plus `test_link_health` and `test_modbus_e2e` (75 checks) | **Pass** on the host; real network **Not measured** |

### Recovery in detail (`bench/results/linux/final_recovery/recovery.txt`)

One run of `bench/recovery_linux.sh` on the final static build (about three minutes). The example configuration has four tags
polled every 200 to 1,000 ms and a reply timeout of 500 ms. Times are from the fault signal to the line the program itself wrote
in its alarm journal (a `PLC_LINK` event); counts are what the fake PLC received.

| Step | Result |
|---|---|
| Normal polling | about 10 requests per second |
| The PLC hangs (reads requests, never answers) | link loss recorded after 4,134 ms; **5 requests** reached the PLC in the next 25 s |
| The PLC answers again | link return recorded after 5,399 ms (consistent with a reconnect back-off; the program says the link is back only after a valid reply) |
| The PLC drops every connection | link loss recorded after 1,153 ms; **49 requests** in 25 s (about 2 per second: bounded reconnect attempts, no flood) |
| The PLC answers again | link return recorded after 531 ms |
| Afterwards | polling back to about 10 requests per second; the executable still running; exit status 0 on SIGTERM; 4 journal lines for the link (loss and return twice) |

What this shows: the link state follows protocol replies (a hung server is detected although the socket is connected); retries
during an outage are few and bounded; the program recovers by itself. What it does not show: stale-data handling with a PLC that
answers slowly but correctly, a write pending during the outage (covered by `test_writes`, `test_comms` and the end-to-end
test: one transmission, outcome UNKNOWN), and a real network with loss, delay or a switch that keeps a dead connection open.
The first version of the script reported "NEVER" for the second fault because the link was still down from the first; the script
now restores the link between the faults.

What the protocol-level tests add (they ran on every build, 75 checks in `test_modbus_e2e` plus `test_link_health`):
the link is declared lost within three reply timeouts (601 ms in the end-to-end test against a 200 ms timeout); a Modbus
exception reply is data with quality BAD, never a value; a truncated or mismatched reply is never data; a write whose reply is
lost ends as UNKNOWN with exactly one transmission; a server that closes the connection is reconnected; nothing listening gives
BACKOFF and values stay UNAVAILABLE. A reconnected socket alone does not recover the link: the program waits for a valid reply.

### Not measured

| Item | Why | What is needed |
|---|---|---|
| Real framebuffer presentation, display and touch-controller latency | no display, no touch panel | a Raspberry Pi with the panel; the same harness plus an oscilloscope or a camera for end-to-end latency |
| CPU and memory on an ARM board | no board | the same scripts on the board |
| `write_bytes` and disk activity per process | this WSL kernel has no `/proc/<pid>/io` | the board's kernel; until then the size of the audit file is sampled |
| 72-hour soak | the host run was one hour | a board and three days |
| Qt baseline | not built | the mentor's M7 comparison on the same board |
| Cold start from a real SD card | the host reads from a fast disk | the board |

### Defects the measurements exposed

- The first measurement script run failed because the bench scripts still passed the removed `--users`, and the framebuffer file
  was not created before the probe; fixed.
- The first reading of idle CPU (0.86 % against 0.47 %) looked like a cost of the smaller draw buffer; the ten-minute runs show
  0.63 % against 0.60 %. Short runs of 60 s are too noisy to compare.
- PSS rises from 1,304 to 1,360 KB over the first ten minutes at normal load and is flat at heavy update. The rise is pages
  touched for the first time (the trend rings and the journal fill, the queue slots come into use), not a leak: the soak
  shows whether it settles (above).

## 7. Screenshot and UI verification report

### How it was done

The headless harness (`tests/hmi/ui_headless.c`) runs real LVGL 9.2.2, the real binder and runtime and the real libmodbus
driver against a fake PLC, renders into memory, and writes a PPM of each state. `tests/hmi/shot_tool.py` converts them to PNG,
counts colours, checks them against the palette and does the 4-bit round trip. The PNG copies of the final run are in
`bench/results/screens/final/` (23 files); the PPM originals are in `build-linux/` and `build-musl/` and are not kept in Git.
Screenshots are renderings of the program on the host, not photographs of a device. Nothing was run on a real display.

The same scenario ran on Windows (MSYS2), on WSL2 (glibc, dynamic), on WSL2 (musl, static, `-Os`, LTO), in the lite profile and
for aarch64 under qemu-user. The assertions are about what the UI shows (label texts through test seams), not about pixels,
except where stated.

### States and screens covered

| Screenshot | State | What was checked |
|---|---|---|
| `shot_1_normal` | overview, four tags, valid PLC data, operator | values, units, quality GOOD, link HEALTHY; 0 flushes in 2 s of steady state |
| `shot_2_alarm` | HI alarm on TANK_LEVEL, unacknowledged | alarm word, header summary, colour |
| `shot_3_acked_write` | alarm acknowledged, write confirmed, setpoint panel | value changed on the PLC, write CONFIRMED |
| `shot_4_commloss` | PLC unreachable | `PLC: DOWN`, values `---`, quality BAD COMM, no stale number shown, a comms alarm raised |
| `shot_5_recovered` | PLC back | values good again, the comms alarm waits for the operator (RTN) |
| `shot_6_viewer_denied` | viewer role, forced ACK ALL | ACK ALL and the setpoint buttons dimmed; the core refuses: "ACK DENIED: role not permitted" |
| `shot_9_admin_role` | admin role | header shows the claimed role; acknowledgement and a write are recorded with `role=admin` and `user=-` |
| `shot_10_alarm_list` | alarm list with a per-alarm ACK | order, ACK enabled for operator |
| `shot_11_last_page` | 23 tags, third page, PLC not reachable | paging, `NO DATA` for tags never read, no number shown |
| `shot_12_trend`, `shot_13_no_trend` | trend with a gap left by a hung PLC; a device with no trended tag | gap shown as a gap; the empty state says so |
| `shot_14_settings_system`, `14b` | SETTINGS, SYSTEM view for viewer and engineer | seven lines, the role named and marked not authenticated |
| `shot_17_config_viewer`, `18_config_keypad`, `19_config_apply`, `20_config_result` | CONFIG view: refused for viewer, number pad, two-press APPLY with three edits, the helper's answer | the view explains why it is empty; pending edits shown as `-> new`; the request file content; a rejected answer shown in the failure colour; disabled APPLY and DISCARD |
| `shot_21_write_pending` | a write sent while the PLC does not answer | state SENT (or UNKNOWN once the 200 ms timeout passes) |
| `shot_22_write_no_answer` | the same write seconds later | UNKNOWN, never CONFIRMED |
| `shot_23_write_rejected` | the PLC answers with a Modbus exception | REJECTED |
| `shot_24_write_unknown` | the PLC applies the write and drops the connection | `UNKNOWN (sent, no confirmation)`, sent once |
| `shot_25_long_values_and_range` | longest name, the largest u32, a long float, a negative i16, a unit with a slash, a value out of range | name 23 characters, `4294967295 kWh/m3`, `123456792 l/min`, `-32768 degC`; `120.0 %` with quality `UNCERTAIN RANGE` |
| `shot_26_after_transitions` | the table after 40 rounds through every screen | pixel-identical to the table before (0 differing pixels in the rows compared) |

Roles seen: viewer (6, 11, 13, 17, 14), operator (1 to 5, 10, 21 to 25), engineer (14b, 18 to 20), admin (9). Repeated screen
transitions: 100 tab presses timed (96 in the distribution, the first round is listed apart) and 160 more in the drift test.

### Automated image checks

- **Palette gate** (`make ui-shots`): every screen uses only the 11 palette colours, except the trend chart (0.686 percent of
  its pixels, anti-aliased slanted lines, limit 1.5 percent). Screens use 8 to 10 colours.
- **4-bit round trip:** each screen without chart pixels packs to 4 bits per pixel through the palette and unpacks to the same
  image (192,000 bytes instead of 768,000 for RGB565).
- **Drift:** pixel comparison of the table before and after 40 rounds of screen changes.
- **No golden-image comparison.** Pixel-exact golden images were not kept, because the screens carry text that changes with time
  (heartbeat, ages, journal) and the harness's timing; the text of every cell is asserted instead.

### Defects found and corrected

1. LVGL's `unscii` font (the first try at a tiny font) is 16 pixels per character and overflowed the header and tables: another
   font was generated.
2. The longest tag name ran into the value column of the overview: fixed column widths with an ellipsis, value column moved.
3. The status of a write ("WRITE #5: UNKNOWN (sent, no confirmation)") was drawn over the journal on the right: it wraps in 284 px.
4. The alarm list's point column had no width limit: it ends in dots.
5. The SEND, `+` and `-` buttons were enabled for a viewer: they are now dimmed for a role that may not write the tag (the
   core refused the write already; this is the visible half). Covered by two assertions.

### What was inspected by eye and what only by the gates

Looked at as PNG on the final tree: 6 (viewer, dimmed buttons), 11, 17, 21, 23, 24 and 25. Looked at in the iteration before the last
three changes (which touched only the overview columns, the write status wrap and the setpoint buttons): 2, 4, 10, 12, 14, 18, 19 and
20. Not looked at by eye after the last change: 1, 3, 5, 9, 13, 14b, 22 and 26; they were checked by the assertions, the palette gate
and (26) the pixel comparison. No claim of visual verification is made for them beyond that.

### Not verified

Rendering on a real panel (dot pitch, contrast, glare); readability of a 14 px one-bit font at the real size; finger use of the
number pad (96 by 46 px keys) and of the tab bar (36 px high); touch calibration; a framebuffer deeper than 16 bits (refused);
a rotated display; the behaviour of the CONFIG view's helper under real systemd.

## 8. Outstanding bugs, risks and technical debt

Ordered by severity (impact times likelihood). "Found" means found in this work; everything else is a known limit.

| # | Severity | Finding | Impact | Likelihood | Remediation |
|---|---|---|---|---|---|
| 1 | High | **No authentication.** The role is the `--role` of the process; anyone who can start it or edit the service unit is `admin`. The audit trail names no person. | Any local user with that access can acknowledge alarms, write tags the configuration allows, and ask for configuration changes. | Certain on a device without physical and root control; low on a locked panel | Decide the mechanism (`docs/AUTHENTICATION.md`, last section), write the threat model (M8), keep the device physically and administratively controlled until then |
| 2 | High | **Nothing ran on the target.** No Raspberry Pi, framebuffer, touch panel, PLC or real systemd. | Every on-target property (fps, touch, 16-bit framebuffer, PLC word order, unit sandboxing, helper under systemd) is unproven. | Certain | M5 bring-up on a board; rerun `bench/` there |
| 3 | High | **CI was never run.** `.github/workflows/ci.yml` and `.gitmodules` are new and unexecuted. | The first push may fail for environment reasons. | Likely at least once | Push to a branch and fix; Auto-fix can watch it |
| 4 | Medium | **The framebuffer must be 16 bits per pixel.** Only RGB565 is compiled into LVGL's drawing code; the program refuses to start on another depth (exit 2, with the reason). The Pi's default framebuffer is often 32 bits. | The program will not start on a default Pi image until the depth is set (`framebuffer_depth=16`). | Likely on a Pi | Set the depth in `config.txt`, or compile XRGB8888 back in (about 13 KB) after a decision |
| 5 | Medium | **The LTO static build was not run under a sanitizer**, only through the functional suites; the sanitizers ran on the development build. | A bug that only LTO or musl expose could escape the sanitizer runs. | Low | Build the same code with clang and LTO plus sanitizers in CI, or run the soak on the static build (done for the host: section 6) |
| 6 | Medium | **UBSan found a signed overflow in LVGL's chart** (`lv_chart.c:847`, a "no value" point multiplied by the height). Worked around by `-fwrapv` on the LVGL objects; the result is not used for such a point. | Benign in practice; would reappear if the flag were dropped. | Present in every build | Report upstream or avoid a missing first point; keep the flag and its comment |
| 7 | Low | **Smaller draw buffer, more strips.** A 20-line buffer renders a redraw in more strips than the old 60 lines. No effect was measurable on the host (idle CPU 0.63 percent against 0.60, heavy update 2.17 against 2.29). | A Pi core is slower; the effect there is unknown. | Unknown | Measure on the board; return to 40 lines if CPU matters more than 16 KB |
| 8 | Medium | **Readability and touch size are judged on screenshots only.** DejaVu Sans 14 px at one bit per pixel on a real 800 by 480 panel and a 96 by 46 pixel number pad key were not tried by eye or finger. | The interface may be too small or too thin on the real panel. | Unknown | Test with a person on the panel; the font is one command to regenerate (`src/ui/font/README.md`) |
| 9 | Medium | **LGPL.** The static build puts libmodbus in the executable. | A delivered device needs the relinking material (objects or source) to comply. | Applies on delivery | Process note in `docs/THIRD_PARTY.md`; legal review |
| 10 | Medium | **Soak length.** The longest run was far short of the 72 hours a validation would need (section 6 gives the actual length). | Slow leaks, SD card wear and rare races are not covered. | Unknown | A 72-hour run on the board, with `bench/soak_linux.sh` |
| 11 | Low | **Static scratch buffers for configuration** (about 105 KB of address space, 48 KB resident) are separate. | About 20 KB of RAM that one shared area could save. | Present | Phase 3 follow-up with the parser, editor check and serializer tests |
| 12 | Low | **Two Makefiles** list the sources separately (Windows and Linux). | A removed or added file can be forgotten in one (it happened once in this work and the build caught it). | Recurring | One list in an included file |
| 13 | Low | **LTO "may be used uninitialized" warning** in `hmi_view_write_status_tone` (a false positive: the value is used only when found). | Noise. | Present in LTO builds | Initialise the record in `ui_overview.c` |
| 14 | Low | **Chart lines are anti-aliased** at a slant, so the trend screenshot has 0.6 percent of its pixels outside the palette. | Cosmetic; a true 4-bit panel would show dithering there. | Present | Draw the min and max as vertical bars (no slanted lines) if a 4-bit panel is chosen |
| 15 | Low | **Timing assertions in the UI harness are loose** (all touches must register; the worst touch must be under half a second). | A slow CI machine could still trip the worst-case check. | Low | Keep loose; print the distribution |
| 16 | Low | `bench/CLAIMS.md` and `bench/results/*.png` at the top level describe the legacy prototype and the M1 to M4 state. | Stale numbers can be quoted by mistake. | Present | Mark as history or regenerate |

**Found and fixed in this work:** the screenshot review showed a long tag name running into the value column, a write
status line drawn over the journal on the overview, and (earlier) a UNSCII font that broke the header; all three were
fixed and are covered by the large-value screenshot (`shot_25`) and the screenshot gate. ThreadSanitizer found a data
race in the **test fixture** (`tests/hmi/fake_modbus_server*.c`: register arrays read by the server thread while the test
thread wrote them), fixed with relaxed atomic accesses; no race was found in `src/hmi`. The bench scripts still passed
the removed `--users` option and the new bench build did not create its framebuffer file; both were caught by the first
runs and fixed.

## 9. Future roadmap

Respecting `docs/ROADMAP_STATUS.md` (M5 to M9) and the mentor's email. Nothing here was started.

**A. Before deployment on target hardware (M5, M7)**
1. Build for the Pi (aarch64): the cross build exists; make the static musl aarch64 build (a musl cross compiler) and run `bench/gate.sh` on it.
2. Set the framebuffer to 16 bits, check `evdev` input and touch calibration, run `prometheus99` on the real panel.
3. Run under real systemd as the service user: the two units, `p99_cfg` as root, `--spool`, the watchdog.
4. Interoperate with a real PLC or a simulator (word and byte order, unit id, timeouts on a real network).
5. Repeat every measurement in section 6 on the board with the same scripts; record them in `docs/LINUX_VALIDATION.md`.
6. A 72-hour soak with faults, an SD-card write measurement (`write_bytes` is already sampled) and a power-cut test of the audit, journal and configuration serial writes.

**B. Security and reliability**
1. Decide authentication (`docs/AUTHENTICATION.md`), then the threat model, the IEC 62443 mapping, an SBOM and the licence review (M8).
2. Run the CI on GitHub, add clang-tidy and cppcheck, fuzz the configuration parser and the Modbus reply decoder, add an aarch64 job under qemu.
3. Run the static build under a sanitizer (clang with LTO) or add leak and race checks to the soak.

**C. Further performance and memory work** (only if the board shows a need)
1. Share one scratch area for the configuration parser, editor check and serializer (about 20 KB).
2. Drop `LV_DRAW_SW_COMPLEX` by drawing the trend as vertical bars (removes shadow, mask and arc code, about 15 KB).
3. Replace the C library's float formatting with a small fixed-point formatter (musl's `vfprintf` and `floatscan` are about 17 KB).
4. Tune the refresh period (33 ms now) against the touch latency of section 6.
5. Freeing screens on leaving them, only after the screens keep their state outside their widgets.

**D. Functionality the mentor's email names**
1. Modbus RTU (small; the same library). Needs the owner's answer first.
2. OPC UA as a separate optional build or process (open62541 alone is larger than the whole target).
3. The Qt baseline on the same board (M7), as a measurement.

**E. Production readiness**
Version tag and release process, provisioning guide (keys, signing, allow-list), an operations guide, update strategy, and the items of 8.

## 10. Production-readiness assessment

**The product is not production-ready.** It is a host-verified prototype with a small, tested core.

*Verified on the host (WSL2 Ubuntu 24.04 x86-64, Windows MSYS2, aarch64 under qemu-user):* the Modbus runtime against a fake
PLC including faults; alarms, writes and the at-most-once rule with unknown outcomes; configuration signature, rollback
serial and allow-list; the storage worker with the kill-loop; the screens, the role checks and the audit records in the
headless harness; the static build with its size and memory gates; AddressSanitizer, UBSan and (after the fixture fix)
ThreadSanitizer; a continuous run with scheduled PLC faults and its results (section 6).

*Unproven on the target:* a real display and touch panel (including the 16-bit framebuffer requirement and the readability
of the font), real PLC interoperability, real systemd and the root helper, sustained operation for days, power loss,
fault recovery on a real network, performance and memory on a Pi, and the Qt comparison.

*Security:* there is no authentication and no threat model; the audit trail does not identify people. The configuration
is protected by a signature, and writes by roles checked in the core, but the role is chosen by whoever controls the process.

*Operations:* no release process, no provisioning guide, CI not yet run. These are listed in section 9.

Only the hardware validation, a decision on authentication with its documents, a long soak on the board and a first
green CI run, together, would support a stronger statement.

## 11. Handoff

### Commands

Linux (WSL2 Ubuntu 24.04; `sudo apt install build-essential pkg-config libmodbus-dev musl-tools python3 rsync`; for the
cross check `gcc-aarch64-linux-gnu qemu-user qemu-user-binfmt`; for the font `node`, `npm`; the musl set-up is in
`.github/workflows/ci.yml`):

```sh
make -f Makefile.linux -j8 test test-view test-config-golden ui-host ui-shots e2e cli-exec config-exec cfgedit-exec liveness
make -f Makefile.linux LITE=1 BUILD=build-lite ui-host
make -f Makefile.linux killloop KILL_ROUNDS=150
make -f Makefile.linux SAN=address BUILD=build-asan test e2e ui-host
setarch x86_64 -R make -f Makefile.linux SAN=thread BUILD=build-tsan test e2e ui-host liveness   # -R: this kernel and TSan
make -f Makefile.linux STATIC=1 CC=musl-gcc BUILD=build-musl hmi hmi-bench
make -f Makefile.linux STATIC=1 LITE=1 CC=musl-gcc BUILD=build-musl-lite hmi-bench
bash bench/gate.sh build-musl full ; bash bench/gate.sh build-musl-lite lite
bash bench/size_report.sh build-musl/prometheus99-bench LABEL build-musl/prometheus99-bench.map
bash bench/startup_linux.sh build-musl LABEL 15 ; bash bench/measure_linux.sh build-musl LABEL 600
bash bench/recovery_linux.sh build-musl LABEL ; bash bench/soak_linux.sh build-musl LABEL 3600 300
```

Windows (Git Bash or MSYS2 UCRT64): `mingw32-make test-hmi test-view test-config-golden ui-host` (writes `build/shot_*.ppm`),
`mingw32-make demo-live` for the live window (F3 changes the claimed role).

### Run

`prometheus99 --config hmi.json --audit audit.log --alarms alarms.bin --config-key config.key --config-state config.serial
[--role viewer|operator|engineer|admin] [--spool DIR] [--fb /dev/fb0] [--touch /dev/input/event0]`. The framebuffer must be
16 bits per pixel. The configuration must be signed (`p99_sign`). `deploy/prometheus99.service` is the unit; it carries the
`--role` decision. Do not run with `config/hmi.example.json` as shipped (its PLC address is a LAN address).

### Change summary

Authentication and the Argon2id hash removed; one static musl build with LTO and gates; LVGL reduced to its object, label,
button and chart widgets with its own theme, one 1-bit font and an 11-colour palette; the runtime no longer clears 70 KB
it does not use; a lite profile; measurement, soak, recovery and size scripts; screenshot, palette and RAM gates; CI and
`.gitmodules`; AddressSanitizer, UBSan, ThreadSanitizer and aarch64 runs; documents updated. Nothing is committed or pushed.

### Known limitations and unresolved issues

Section 8. The three that matter first: no authentication, nothing run on the target, CI never run.

### Recommended next phase: M5, Raspberry Pi bring-up

| Priority | Task | Acceptance |
|---|---|---|
| 1 | Run the static aarch64 build on a Pi with the panel | The process starts, draws the overview, shows live values from a real or simulated PLC; `bench/gate.sh` passes on the aarch64 binary |
| 2 | Framebuffer depth and touch | 16-bit framebuffer set; every screen reachable by touch; the number pad usable with a finger (a person confirms) |
| 3 | systemd as the service user | `READY=1` and watchdog work; `p99_cfg` applies a request and the service restarts; a kill is restarted |
| 4 | Repeat section 6 on the board | Start-up, transition, touch, CPU, PSS, 72-hour soak and recovery recorded with the same scripts; every row has a status |
| 5 | Decide authentication and write the threat model | A decision recorded in `docs/AUTHENTICATION.md`; a threat model document exists |
| 6 | First CI run | The three jobs are green on GitHub |

## Appendix A: test results on the final tree (what ran, what it said)

Every line was observed in the output of the run named; none is copied from an earlier report. Files are in
`bench/results/linux/` (`final_dev_suites`, `static_suites`, `sanitizers_*`, `arm64`, `killloop_phase3.txt`).

| Suite | Environment | Result |
|---|---|---|
| Core tests (18 programs) | Windows MSYS2 | 3,240 checks, 0 failed |
| Core tests (18 programs) | WSL2 glibc `-O2`; WSL2 musl static `-Os` LTO; aarch64 under qemu-user | 3,262 checks, 0 failed (each) |
| View model | all of the above | 197 checks, 0 failed |
| Config golden | Windows, WSL2 | identical (58 accepted configurations round-trip) |
| Headless UI scenario | Windows; WSL2 glibc; WSL2 musl static; aarch64 | 205 assertions, 0 failed (each) |
| Headless UI scenario, lite profile | WSL2 glibc; WSL2 musl static | 127 assertions, 0 failed |
| Screenshot gate | WSL2 glibc, musl, aarch64 | 23 screenshots, pass |
| End to end (real libmodbus, fake PLC) | WSL2 glibc, musl static, aarch64 | 75 checks, 0 failed |
| Executable tests (`cli-exec`, `config-exec`, `cfgedit-exec` as root, `liveness`) | WSL2 glibc and musl static | all pass |
| Kill-loop | WSL2 glibc, 150 rounds | pass: 125 kills, 25 clean stops, every round verified |
| AddressSanitizer + UBSan | WSL2 glibc, 8 suites | 0 reports |
| ThreadSanitizer (`setarch -R`) | WSL2 glibc: core, end to end, UI scenario, liveness | 0 reports on the final tree; one race in the test fixture found earlier and fixed |
| Size and memory gates | musl static, full and lite | pass |
| Not run | CI on GitHub; the LTO build under a sanitizer; anything on a Raspberry Pi; Qt baseline; 72-hour soak | |

## Appendix B: what changed in the repository (uncommitted)

- **Deleted (13):** `hmi_kdf`, `hmi_users`, `hmi_auth` (each `.c` and `.h`), `ui_login.c`, `ui_users.c`, `p99_user.c`, `test_kdf.c`,
  `test_users.c`, `test_auth.c`, `auth_exec_test.sh`.
- **New:** `src/hmi/hmi_util.c/.h`, `src/ui/ui_theme.c`, `src/ui/font/` (font and README), `bench/` (seven scripts, README, results),
  `tests/hmi/fake_plc_main.c`, `tests/hmi/shot_tool.py`, `.github/workflows/ci.yml`, `.gitmodules`, `docs/ENGINEERING_REPORT.md`.
- **Changed:** both Makefiles (static, lite, sanitizer, measurement targets), `config/lv_conf.h`, `src/app/main_linux.c` (no login, role
  from `--role`, framebuffer depth check, file framebuffer for measurement), the UI files, `hmi_runtime.c`, `hmi_store.c`,
  `hmi_trend.c` (what is cleared at start), `hmi_authz`, `hmi_cli`, tests, the demo, `deploy/prometheus99.service`, and the documents.
- The frozen legacy prototype was not touched.
