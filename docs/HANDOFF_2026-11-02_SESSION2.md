# Handoff for the next session (second session of 2026-11-02)

This file is the starting point for the next session. It continues `docs/HANDOFF_2026-11-02.md` (the first handoff of the day), which still holds the background: the environment, the work method, the commands and changes C1 to C17. Read that file's sections 2, 5 and 7 as well. Section 10 of this file is a prompt to paste into the new session.

## 0. Latest status (end of the session of 2026-11-02)

After this file was first written, a short session resumed, confirmed that the working tree matches this file (HEAD `c1392f5`, branch `main`, 35 changed or new files, no stash), and was about to run the check script for section 4 when the owner stopped it. So nothing has changed since this file was written: section 4 (the RTU PLC line on the settings screen) is still written but not size-measured and not fully tested, and everything else is exactly as described below. Nothing is running in WSL. The helper scripts used for the checks are in the old session's scratchpad and are not needed: the commands are in section 8 and in the first handoff, section 7.

## 1. Where things stand

- Branch `main`. The last commit is `c1392f5` ("P1 TO P3 DONE"), the owner's commit of everything up to C17. Everything described below is **uncommitted** in the working tree: 34 modified files, about 710 lines added and 150 removed. The owner commits, stashes and rewrites history himself. **Do not commit, stash or pop.**
- Work in this session: C18 (RTU in the default device program, full gate moved to 330,000 bytes), C19 (optional PLC heartbeat) and C20 (responsive layout checks at six sizes, the fixes they found, a safe page below 640 x 400). All three are finished, tested and written up in `docs/OPTIMIZATION_CHANGELOG.md` and `docs/FUNCTIONALITY_PRESERVATION.md`.
- One change was being made when the owner paused the work: a fix for the PLC line of the settings screen with an RTU configuration (section 4). It is written and two suites passed, but it is not size-measured, not fully tested and not documented.
- Item 7 of phase 3 (module boundaries) was started and one attempt was reverted (section 5).

## 2. What was done in this session

| Id | Change | Result |
|---|---|---|
| C18 | Owner decision: full stripped-size limit 322,000 to 330,000 bytes; `RTU ?= 1` in `Makefile.linux` (RTU is in the device program by default, `RTU=0` builds the old TCP-only program) | full 329,904 of 330,000; lite 276,640 of 290,000; `RTU=0` 321,712; note in `docs/OPTIMIZATION_ROADMAP.md` section 6 |
| C19 | Optional PLC heartbeat: `plc.heartbeat_register`; the comms thread writes a counter once a second while the UI loop calls `hmi_comms_ui_alive`; a documented, configuration-gated exception to the audited write path (`docs/CONFIG_INTEGRITY.md`, section "PLC heartbeat") | stripped size unchanged; core +25 checks (3,531 Linux, 3,509 Windows), e2e +6 (81) |
| C20 | `make ui-sizes` (in CI): every screen, the CONFIG view and keypad, and the login panel at 640x400, 800x480, 1024x600, 1280x800, 1366x768 and 1920x1080, checked for objects off the screen or outside their parent, overlapping texts and cut labels, plus the palette check; the layout defects it found were fixed; a safe "SCREEN TOO SMALL" page below 640 x 400; the framebuffer backend accepts 320 x 240 and up; two helpers `ui_mk_panel` and `ui_label_box` paid for the code | stripped size unchanged at 329,904 (code segment 98 bytes before the next page); 76 pictures PASS; all suites pass |

The changelog entries C18, C19 and C20 have the details, the measured numbers and what was not verified.

### Checks that passed on the final C20 tree

On Linux (WSL, `~/p99b`): core 3,531 checks plain, under AddressSanitizer and UBSan, under ThreadSanitizer (with `e2e` and `rtu-e2e`) and in the static musl build; `e2e` 81; `rtu-e2e` 17; `test-view` 208; config golden identical; UI harness 225 (plain, ASan, static); `ui-shots` 24 PASS; `ui-sizes` 8 sizes and 76 pictures PASS; `cli-exec`, `config-exec`, `liveness` PASS; `make warnings` 0 in own files; kill-loop 150 rounds PASS; full and lite gates PASS. On Windows (`C:\p99`): core 3,509, view 208, golden identical, e2e 81, UI 225, the demo exe builds (357,376 bytes).

## 3. Current numbers

| Metric | Value |
|---|---|
| Full device program, stripped (limit 330,000) | 329,904 at C20 (the RTU line fix of section 4 is not measured) |
| Code segment slack before the next 4,096-byte page | 98 bytes at C20 |
| Static data (limit 440,000) | 377,640 |
| Lite stripped (limit 290,000) / static data (limit 350,000) | 276,640 / 298,032 at C20 |
| Core tests Linux / Windows | 3,531 / 3,509 |
| UI harness / `ui-sizes` | 225 / 8 sizes, 76 pictures |
| e2e / rtu-e2e | 81 / 17 |
| Windows demo exe (not gated) | 357,376 |

## 4. The change in progress: the RTU PLC line on the settings screen

- **Defect.** With `plc.transport "rtu"` the SYSTEM view of the settings screen builds its first line from `host` and `port`, which are empty for RTU, so it shows "PLC :0  unit 3". Found while reading `src/ui/ui_settings.c` for item 7.
- **Fix as written (uncommitted, not measured).** `hmi_view_rtu_plc_text(device, unit, buf, cap)` in `src/ui/hmi_view.h` and `hmi_view.c` writes "PLC /dev/ttyUSB0 (Modbus RTU)  unit 3"; `system_update` in `src/ui/ui_settings.c` calls it under `#ifdef HMI_RTU` when `cfg->transport == HMI_TRANSPORT_RTU`; `tests/hmi/test_hmi_view.c` (`test_settings_wording`) has one new check.
- **Checked so far:** `test-view` 209 checks (was 208) and the UI harness 225 pass.
- **To do:** run `ui-sizes`, `make warnings`, the full gate (the code segment had 98 bytes of slack; if the fix pushes it over a page, the stripped size jumps to 334,000 and the gate fails: then make room or put the wording in a form that costs less), the lite gate, and the Windows tests (`hmi_view.c` is shared). Then write changelog entry C21, update the matrix row "LVGL rendering and screens" or "Modbus polling and decode", and decide whether the CONFIG view's PLC target (which edits host and port) needs the same care for RTU; it was not looked at.

## 5. Item 7 (module boundaries): what was learned

- There are 49 direct `ui_rt->` uses in `src/ui` (`ui_overview.c` 17, `ui_settings.c` 7, `ui_trends.c` 7, `ui_ack.c` 6, `ui_config.c` 5, `ui_alarms.c` 4, `hmi_ui.c` 2, `ui_login.c` 1). Most pass `&ui_rt->db`, `&ui_rt->alarms` or `&ui_rt->trend` to the thread-safe APIs, which is acceptable. The ones that read runtime internals are `ui_rt->writes_effective` (2), `ui_rt->store_started` (1), `ui_rt->audit_path` (1) and `ui_rt->cfg.*` (7).
- **Attempt reverted:** a view model for the settings SYSTEM view (`hmi_view_system_line` with an input struct, so the screen file only gathers values and draws). It passed its tests but cost about 400 bytes of code, which pushed the full device program one page over the gate (334,000 bytes). Per the method (keep only what is beneficial and inside the gate) it was removed again; the files are back to their C20 state except for the RTU fix of section 4.
- **Lesson for the next attempts:** with under 100 bytes of slack, item 7 work must be size-neutral or paid for in the same step. Cheap forms: `static inline` getters in `hmi_runtime.h` for the internal fields (`hmi_runtime_writes_effective`, `hmi_runtime_store_started`, and so on), which cost nothing after inlining; moving tone decisions into existing `hmi_view` functions only where the code shrinks. Measure each step with the full gate before writing more.

## 6. Open issues and decisions for the owner

1. **Lite RAM check fails (decision needed).** `make -f Makefile.linux LITE=1 BUILD=build-lite ui-host` fails one check: the LVGL pool peak is 58 KB against the lite limit `POOL_PEAK_MAX_KB` of 56 KB (`tests/hmi/ui_headless.c`). The committed tree `c1392f5` fails it the same way, so it is not caused by this session. The peak is the lite screens (46 KB) plus the login panel opened over them (C16, about 11 KB for 12 keypad buttons, the user buttons and labels); before C16 the lite peak was 47 KB (C15). The C16 and C17 checks ran the UI harness for the full profile only. The CI workflow runs this step, so CI would fail. Options: a lighter login panel (fewer objects; costs code in the full build too) or moving the lite limit to 64 KB, which needs the owner's approval. `LV_MEM_SIZE` is not affected: the pool is 102 KB, so the device does not run out; the limit is a regression check.
2. **Size room.** The full device program has 98 bytes of code slack. Alarm presentation (item 5) and every phase 4 visual need room first: either code removed elsewhere without losing function (C20 showed that folding repeated LVGL set-up code into helpers can give back a few hundred bytes; other repeated patterns are worth a search) or another gate decision.
3. **Sync caution.** `rsync -a` keeps the Windows modification times. A source file edited before a WSL build of the old copy ran, and synced after it, is older than the objects built from the old copy, so `make` does not rebuild them (this happened once in C19). After syncing, `touch` the changed files in `~/p99b`, or delete the affected binaries, before building. The helper scripts used in this session did `touch src/ui/*.c src/ui/*.h src/hmi/*.c src/hmi/*.h src/app/*.c tests/hmi/*.c` after each sync.
4. **Shell quirk.** Commands passed through PowerShell to `wsl -e bash -lc '...'` lose their `|` pipes inside quoted grep patterns. Put multi-step WSL work into a script file (written with the Write tool) and run `wsl -e bash /mnt/c/.../script.sh`.
5. Earlier open items still stand (first handoff, section 8 item 4): `cppcheck` not run; `cfgedit-exec` and the emulated Pi need root; the +70 ms hang-loss observation; the Windows demo exe growth from 311,296 bytes. C20 adds: the framebuffer backend at a size other than 800 x 480 in the emulated Pi, a real 320 x 240 panel, and a storage-fault line shown together with the claimed-role line on the overview panel were not verified.
6. A `build-musl/prometheus99 --fb none` demo process (pid 38183 earlier in this session, not started by this session) was running in WSL and was left alone.

## 7. Files changed since `c1392f5` (all uncommitted)

- Build and gates: `Makefile.linux` (`RTU ?= 1`, `ui-sizes`), `bench/gate.sh` (330,000), `.github/workflows/ci.yml` (`ui-sizes`).
- Runtime: `src/hmi/hmi_comms.c`, `hmi_comms.h`, `hmi_config.c`, `hmi_config.h`, `hmi_config_write.c`, `hmi_runtime.c` (heartbeat); `src/app/main_linux.c` (`hmi_comms_ui_alive`); `demo/hmi_win_demo.c` (the same call).
- Display: `src/app/display_fbdev.c` (320 x 240 and up).
- UI: `src/ui/hmi_ui.c` (header layout, safe page, `ui_mk_panel`, `ui_label_box`), `ui_priv.h`, `ui_overview.c`, `ui_login.c`, `ui_settings.c`, `ui_alarms.c`, `ui_config.c`, `ui_nav.c`, `ui_trends.c`, and for section 4 `hmi_view.c`, `hmi_view.h`.
- Tests: `tests/hmi/test_comms.c`, `test_config.c`, `test_modbus_e2e.c`, `ui_headless.c` (layout mode, safe page, shared `test_users`), and for section 4 `test_hmi_view.c`.
- Documents: `docs/OPTIMIZATION_CHANGELOG.md` (C18 to C20), `docs/FUNCTIONALITY_PRESERVATION.md`, `docs/CONFIG_INTEGRITY.md` (heartbeat), `docs/OPTIMIZATION_ROADMAP.md` (gate note), `README.md`, `docs/HANDOFF_2026-11-02.md` (section 0), and this file.

## 8. Commands added in this session

```sh
# in WSL, ~/p99b
make -f Makefile.linux ui-sizes                     # layout check at 6 sizes + safe page at 480x320 and 320x240, palette
P99_LAYOUT=1024x600 ./build-linux/ui_headless build-linux/size build-linux/a.log   # one size, prints [layout] lines
python3 tests/hmi/shot_tool.py png build-linux/size_640x400_login.ppm /tmp/x.png  # look at a picture
make -f Makefile.linux STATIC=1 CC=musl-gcc RTU=0 BUILD=build-musl-tcp hmi       # TCP-only device program
readelf -lW build-musl/prometheus99 | grep 'R E'   # code segment size; the next page starts at the next 0x1000
```

## 9. Next steps, in order

1. Finish section 4 (the RTU PLC line): size, the remaining suites, Windows, changelog C21, matrix.
2. Ask the owner the lite RAM question (section 6 item 1), or try a lighter login panel first and measure it in both profiles.
3. Item 7 (module boundaries) in size-neutral steps (section 5), tests first, the gate measured after each step.
4. Make size room (section 6 item 2), then item 5 (alarm presentation: return-to-normal-unacknowledged shown clearly, severity by shape and word as well as colour, guidance text from the configuration).
5. RTU follow-ups: adapt the friend's malformed-frame fault tests to the pty fake slave; a heartbeat over RTU in `rtu-e2e`.
6. Phase 4 (UI, Tier 2 visuals, each measured against the gate) and phase 5 (documents and final validation, including the aarch64 rebuild and the emulated soak), as listed in the first handoff, section 8.

## 10. Prompt for the next session (paste this)

---

You are continuing work on Prometheus99, an industrial HMI runtime in C99 (Modbus TCP and RTU through libmodbus, LVGL 9.2.2 screens, signed configuration, keyed-hash-chained audit file, named-user PIN login, audited writes, optional PLC heartbeat). The project is at C:\Users\Admin\Desktop\prometheus99-main\PROMETHEUSS (WSL copy for builds: ~/p99b; untouched baseline: ~/p99; Windows build copy: C:\p99). Much of the work is uncommitted, so read before changing anything.

Read in this order: docs/HANDOFF_2026-11-02_SESSION2.md (start here), docs/HANDOFF_2026-11-02.md sections 2, 5 and 7 (environment, method, commands), docs/MASTER_PROMPT_FINAL.md (the brief), docs/OPTIMIZATION_CHANGELOG.md (C1 to C20), docs/FUNCTIONALITY_PRESERVATION.md.

Non-negotiables: preserve every function; LVGL 9.2.2 unchanged; C99; libmodbus kept; no test weakened; never commit, stash or pop (the owner does git himself); no real PLC or hardware; every number measured; the size gate honoured (ask me before moving a limit). Requested behaviour changes are additive. Work method: master prompt section 4 (evidence, smallest change, build, test, compare, keep or revert; update the changelog and the matrix in the same step). Build and test in WSL ~/p99b, run suites one at a time, touch synced files before building (rsync keeps Windows times), put WSL pipelines in script files, use the file tools, keep LF line endings. Replies to me terse; saved files plain full sentences.

Start: run git status and confirm the tree matches the handoff (HEAD c1392f5 or a later owner commit, about 34 modified files), sync to ~/p99b, then finish section 4 of the handoff (the RTU PLC line: size, suites, Windows, changelog C21). Then ask me the lite RAM question (section 6 item 1) and continue with section 9 without asking me about individual engineering decisions inside the constraints.

---
