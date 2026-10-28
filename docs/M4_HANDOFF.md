# M4 handoff: state at the end of the milestone (2026-10-22)
> **Note, 2026-10-23:** the login, the user store and the password hash described in the parts of this file that mention logins or accounts were removed from the product (`docs/AUTHENTICATION.md`). Those passages are kept as the record of M1 to M4 and no longer describe the program: there are no login or account records in the audit file and no user file, and the audit line shows `user=-`.

This file records the state of milestone M4 so a new session can continue without the earlier
conversation. Nothing was committed or pushed. The owner approved the M4 plan with the
recommended option for decisions D1 to D6. Everything below was checked on the development host
only (Windows with MSYS2, and WSL2 Ubuntu 24.04). Nothing ran on a Raspberry Pi, a real PLC, a
real display or real systemd.

## Plan and progress

| Step | State |
|---|---|
| 4.0 Split the UI into one file per screen | Done. |
| 4.1 Alarm priority | Done. |
| 4.2 Screen framework, paged overview, alarm list | Done, with a per-alarm ACK that refuses an alarm that changed after it was drawn. |
| 4.3 Trends | Done: `hmi_trend` (RAM only), chart screen. |
| 4.4 Settings screen and accounts | Done. |
| 4.5 Configuration helper and editing | Done: `hmi_config_write`, `hmi_cfgedit`, `p99_cfg`, `cfgedit_exec_test.sh`, the units, `--spool`, the CONFIG view. |
| 4.6 Verification and documentation | Done on the host (sanitizers, mutation checks, both hosts, documents). |

## What changed since the earlier handoff

- The 6 failing checks of `tests/hmi/test_cfgedit.c` were three code defects (the writer wrote
  members that hold their default, which overran the token budget for 64 tags and wrote
  `"priority":"medium"`; the parser left `base_serial` set on some refusals) and one wrong test
  (`m.host[0] = '1'` did not change a host that starts with `1`).
- The Linux build found a fourth defect: `BYTE_ORDER` is a glibc macro.
- Mutation checks first left 4 survivors; 3 tests were added (host named in the refusal, a base
  without an allow-list, serial 4294967295). One survivor is left on purpose: the final read-back
  assertion in `hmi_cfgedit_apply`, which no input can reach.

## Where things are

- Screens: `docs/SCREENS.md`. Helper, request, limits and evidence: `docs/CONFIG_INTEGRITY.md`.
  State and results: the top of `docs/ROADMAP_STATUS.md` and section V of `PROJECT_RESUME.md`.
- Run the helper test as root on Linux: `make -f Makefile.linux cfgedit-exec`.

## Known issues and notes

- Three intermittent Windows failures were explained and fixed after the milestone work: the
  slow-disk `runtime` test drained in the same time as the stop budget (and a timed-out stop
  left a thread that crashed the next test), the state-file check in `runtime` read the file
  once instead of waiting for the 50 ms interval, and a scanner holding the account file made
  the Windows file replace fail in `auth`. The last one needed a code change: a bounded retry
  in `hmi_platform_win32.c`. Details are in the M4 section of `docs/ROADMAP_STATUS.md`.
- Repository line endings: the instruction said files use CRLF, but the git index and working tree
  are LF for the portable code (only the legacy prototype files are CRLF). New files are LF.
  `PROJECT_RESUME.md` section U still says CRLF. Reported to the owner.
- The stray files `cs_cfg.json`, `cs_cfg.json.sig` and `cs_key.bin` that were in the repository
  root at the start are fixtures of `tests/hmi/test_cfgsec.c`; they were gone after the 4.6 runs,
  so the test's own cleanup removed them (left behind by an earlier interrupted run).
- Python or shell heredocs mangle backslashes: write patch scripts with the Write tool.
- Account-view limits: the two-press REMOVE lapse after 5 s is not covered by a test; an Operator
  or Engineer cannot change their own password on the device.
- CONFIG view limits: the lapse of the APPLY question after 5 s is not covered by a test; a
  request that was sent and not answered stays "waiting" until the helper runs.
- Not verified: see the end of the M4 section of `docs/ROADMAP_STATUS.md`.
