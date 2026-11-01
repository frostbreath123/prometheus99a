# Prompt for the next session

Paste the text between the lines into a new session.

---

You are continuing work on Prometheus99, an industrial HMI runtime in C99 (Modbus TCP through libmodbus, LVGL 9.2.2 screens, signed configuration, audited writes). The project is at `C:\Users\Admin\Desktop\prometheus99-main\PROMETHEUSS` (in WSL: `/mnt/c/Users/Admin/Desktop/prometheus99-main/PROMETHEUSS`). Earlier sessions did a lot of work that is not in the git history, so read before you change anything.

Read these, in this order, before doing anything:

1. `README.md`, sections 1 to 3, 10 and 11 (what exists, limits, rules). Note that the README does not yet describe the responsive layout or the audit work below.
2. `docs/MASTER_PROMPT_FINAL.md`. This is the brief for the work: constraints, verified facts, and phases 0 to 5 in detail. It supersedes `docs/MASTER_PROMPT.md`, which you may skim for the Tier 1 to 3 feature list.
3. `docs/baseline/` (`size_gate.txt`, `tests_summary.txt`, `ui_perf.txt`, `build_musl.txt`, `env.txt`, `tests_full.txt`). This is the phase 0 baseline measured on the current tree. Do not overwrite it.
4. The comparative audit in `C:\Users\Admin\Desktop\prometheus99-comparative-audit\`. Start with `EXECUTIVE_SUMMARY.md`, then `IMPROVEMENT_ROADMAP.md`, `FILE_LEVEL_FINDINGS.md`, `WEAKNESS_AND_RISK_REGISTER.md`, `PERFORMANCE_BENCHMARKS.md` and `FRIEND_VISUAL_AND_WINDOWS_OBSERVATIONS.md`. The friend's repository is cloned unmodified in `friend_repo\` there and is a read-only reference. Benchmark scripts are in `bench\`.
5. `docs/AUTHENTICATION.md`, `docs/CONFIG_INTEGRITY.md`, `docs/STORAGE.md`, `docs/SCREENS.md` (some passages are stale; see the master prompt).

What has been done, and is not committed (about 125 changed entries on branch `feature/authorization-hardening`, last commit `ddda58d`):

- **Responsive layout.** Display backends read the real screen size (`src/app/display.h`: `p99_display_size`, `p99_display_take_resize`). The SDL window resizes live, minimum 640x400 (`P99_SIZE=WxH` sets the start size). Screens use an 800 x 480 design grid mapped by `ui_x()` and `ui_y()` in `src/ui/hmi_ui.c`, with `hmi_ui_relayout()` rebuilding on resize. Text uses three sizes (`src/ui/font/p99_font.c`, `_18.c`, `_24.c`). LVGL flex and grid stay off. Verified on Linux at several sizes; Alarms, Trends and Settings were only checked at 800x480.
- **Phase 0 baseline** (`docs/baseline/`): static musl build stripped 321,712 bytes against a gate of 322,000; static data 438,536 against 440,000. All Linux suites pass (core 3,275, view 208, UI scenario 205, end to end 75, executable and liveness). Idle headless run earlier: about 620 kB resident, 0.50 percent CPU.
- **Snapshot** of the tree before optimization: `C:\Users\Admin\Desktop\prometheus99-baseline-snapshot`. Use it to revert a single failed change.
- **Comparative audit** of the friend's `prometheus99_custombuild` (no LVGL, own renderer, PIN login, signed audit log chain). Main lessons: add named-user authentication, a keyed hash chain for the audit log, sanitizers in the default test target; keep your own write safety, signed configuration and low idle CPU.

What is not done yet:

- Phase 0 leftovers: headless memory, CPU and thread wake-up counts on the current tree; a Windows build and tests; sanitizer, lite and kill-loop runs; the static-data breakdown by object; `docs/OPTIMIZATION_BASELINE.md` and the functionality preservation matrix.
- Phases 1 to 5 of the master prompt (nothing has been optimized yet).
- Authentication, Modbus RTU, the keyed audit chain, the PLC heartbeat, alarm presentation changes, and the Tier 2 visuals.

Non-negotiables (full list in the master prompt, section 2): preserve every existing function, LVGL 9.2.2 unchanged, C99, libmodbus kept, no feature removed, no test weakened, no commit or push unless I ask, no real PLC or hardware, every number measured and no claim that was not run, and the size gate honoured (report any change with measured numbers and ask me before moving a limit). Login and other requested behaviour changes are additive.

How to work: use the method in section 4 of the master prompt (evidence, smallest change, build, test, compare with the baseline, keep or revert). Make room in the size gate before adding features. Update the docs as you go. Use the Linux copy in WSL (`~/p99`) for builds and tests, not `/mnt/c`, and run the Linux test suites one at a time. Write files with the file tools, not shell heredocs, and keep LF line endings. Chat replies to me are terse; saved files use plain full sentences.

Start by confirming the current state (`git status`, run the baseline again quickly to confirm nothing drifted), finish the phase 0 leftovers, then go to phase 1. Do not ask me to approve individual engineering decisions inside the constraints.

---
