# ADR-0001: Separate the Windows prototype from the portable HMI runtime

Status: accepted (2026-10-15). Context: the repository held a Windows-only
host-monitoring prototype with a custom renderer. The intended product is a
C99 industrial HMI that uses real LVGL, talks Modbus TCP to a PLC and runs on
small Linux hardware.

## Decision

Keep the prototype as a development/demo target and add the product as
separate, layered modules. Nothing in the product core includes a Windows
header.

```
src/hmi/        PORTABLE CORE (C99, no OS headers)
  hmi_types.h     quality/dtype/role enums, compile-time limits
  hmi_decode.*    register <-> typed value, byte/word order, scaling limits
  hmi_tags.*      tag table: raw value, quality+reason, monotonic ts, versions, poll groups
  hmi_driver.h    plc driver interface (connect/read/write/close + status classes)
  hmi_comms.*     connection FSM, scheduler, PLC link health (step function)
  hmi_writes.*    write lifecycle: validate, queue, audit-before-send, expiry, verify outcome
  hmi_alarm.*     ISA-18.2-style alarm engine, journal, persistence
  hmi_config.*    strict JSON config (jsmn)  [file I/O via platform layer]
  hmi_runtime.*   threads + audit sink wiring
  hmi_kdf.*       Argon2id password hash (the only file that includes monocypher.h, see the 2026-10-22 addendum)
  hmi_users.*     user store: accounts, strict file format
  hmi_auth.*      login, lockout, idle logout, first-boot administrator
  hmi_store.*     storage worker: every durable file write, bounded queue, rotation
  hmi_auditfmt.*  audit and journal line formats, wall-clock stamp (pure functions)
  hmi_platform.h  the ONLY OS surface: clock, mutex, thread, event, atomic flag, file operations
src/hmi/hmi_platform_win32.c   host tests / Windows demo
src/hmi/hmi_platform_posix.c   Linux target (built and tested on WSL2; not run on a Raspberry Pi)
src/hmi/plc_libmodbus.*        driver backed by libmodbus 3.1.10 (the only file that includes <modbus.h>)
src/ui/hmi_ui.*               LVGL v9.2.2 presentation; UI thread only
src/app/main_linux.c          Linux entry point (fbdev + evdev)
src/app/p99_user.c            offline user management tool (Linux)

include/, src/layer*.c, src/main.c   LEGACY Windows prototype (unchanged boundary)
tests/ (legacy), tests/hmi/          host tests, fake PLC, headless UI harness
```

## Rules

1. `src/hmi/*` (except `hmi_platform_*.c` and `plc_libmodbus.c`) may include
   only the C standard library and `hmi_platform.h`.
2. LVGL is called from one thread only (the one running `lv_timer_handler`).
   `hmi_ui.c` reads runtime state exclusively through thread-safe getters.
3. No lock is held across a driver call, file write, or LVGL call.
4. Lock graph is flat: tag-table lock, comms lock, alarm lock are never nested.
   The alarm engine copies a tag snapshot first, then takes only its own lock.
5. Data never "becomes" something it was not: quality is per tag, restored
   only by a successful read of that tag's own poll group.
6. The legacy prototype is not extended further. Its sensor code
   (ASUS ATKACPI, PhysicalDrive0 IOCTL, GetSystemTimes) is Windows-specific
   and is not ported.

## Consequences

- The portable core is host-testable with a scripted mock driver and a fake
  clock and with real libmodbus against an in-process fake PLC, with no
  hardware. The check counts change as tests are added; the current numbers
  are in docs/LINUX_VALIDATION.md.
- The POSIX platform file has been built and run on WSL2 (Ubuntu 24.04). Linux
  behaviour on a Raspberry Pi has not been verified at all.
- At the time of writing roles were a launch parameter and not authentication.
  This was superseded on 2026-10-22: see the last addendum.

## Addendum (2026-10-15): comms / writes split

- `hmi_comms` is the only module that decides whether the PLC is alive
  (`hmi_comms_link_health`). Consumers must not derive their own thresholds.
- `hmi_writes` has no transport knowledge: comms asks it for a job
  (`hmi_writes_take`) and reports the outcome
  (`finish_exception / finish_unknown / finish_verified`). The audit sink is
  its only seam (file adapter in production, scripted adapters in tests).
- `hmi_audit_fn` and the write state types moved from `hmi_comms.h` to
  `hmi_writes.h`; `hmi_comms_submit_write/write_status/set_audit` no longer exist.

## Alternatives rejected

- Port the prototype layers 0-3 to Linux: the sensors are not PLC data and the
  renderer is not LVGL, so the port would carry the wrong abstractions.
- Float as the stored tag type: loses exact 32-bit integers (counters,
  totalisers). Raw bit patterns are stored; engineering values are derived.
- Lock-free state exchange between threads: unnecessary complexity, and the
  prototype's version of it was racy.

## Addendum 2026-10-15: authorization in the core, claimed role

Context: roles existed only as a launch flag, the UI was the only place that
limited alarm acknowledgement (a Viewer could press ACK ALL), and an
unrecognised `--role` silently became Operator.

Decision:
- `hmi_authz` is the single decision point for privileged actions:
  acknowledging alarms needs Operator or above; a tag write needs a role at or
  above that tag's configured minimum (the existing rule, now routed through
  the same function). A corrupt role value is denied.
- `hmi_alarm_ack` and `hmi_alarm_ack_all` take the caller's role and return
  `HMI_ACK_OK / NOTHING / DENIED / AUDIT_FAILED`. A denied request changes no
  alarm state and writes no journal entry.
- The role is a CLAIMED role. It comes from `--role` of whoever starts the
  process. It is not authenticated and nothing here makes it so; login,
  lockout and a password KDF remain unimplemented.
- `hmi_cli` parses the command line strictly: an unknown role, unknown option,
  missing value, repeated option or stray argument exits with status 2. The
  default role is viewer.
- Acknowledgement attempts are audited to the same file as PLC writes (the
  claimed role, the action, the alarm names, GRANTED or DENIED, monotonic
  milliseconds since boot). A granted acknowledgement is recorded BEFORE it
  takes effect; if an audit path is configured and the record cannot be
  durably appended the acknowledgement is refused (fail closed, the alarm
  stays annunciated). A denied attempt is recorded best effort. With no audit
  path acknowledgements are allowed and not recorded.
- "Durable" means `hmi_fs_append_sync` returned true (written and flushed to
  stable storage as the OS reports it). Nothing is buffered in this module.
- An alarm that changes while its acknowledgement record is being written is
  not acknowledged (the operator must see the new state first).

Not changed: PLC write authorization, audit-before-send and fail-closed
behaviour for PLC writes (252 write checks and the e2e suite pass unchanged).

## Addendum 2026-10-22: authentication and sessions (superseded on 2026-10-23: removed, see the last addendum)

Context: roles were claimed on the command line (addendum 2026-10-15), which is
not access control. Roadmap milestone M1 replaces it with a login. Details,
parameters and limits are in docs/AUTHENTICATION.md.

Decision:
- Roles come from a session. `hmi_auth_role(auth, now)` is the only source of the
  role that the UI passes to the unchanged `hmi_alarm_ack*` and
  `hmi_writes_submit`. It returns Viewer when logged out and after an idle
  timeout, without needing any state change to happen first.
- `hmi_authz`, the write pipeline, audit-before-send and fail-closed behaviour
  are unchanged. A successful login is itself recorded before the session starts
  and is refused if a configured audit file cannot be written.
- New modules: `hmi_kdf` (Argon2id and constant-time helpers over Monocypher
  4.0.2), `hmi_users` (strict user file), `hmi_auth` (sessions, lockout,
  asynchronous hash job). `hmi_auth` is owned by the UI thread. Only the
  password hash runs on a short-lived worker thread, and it communicates through
  one atomic flag and a thread join.
- Rule 1 above (core files include only the C standard library and
  `hmi_platform.h`) gets a second exception, the same kind as `plc_libmodbus.c`:
  `hmi_kdf.c` includes `monocypher.h` and nothing else outside the portable core
  does. The platform layer gained `hmi_fs_read_all`, `hmi_fs_mode` and
  `hmi_random_bytes` so that no other core file touches the operating system.
- The Argon2id scratch memory (8 MiB at the default cost) is allocated for one
  hash and freed afterwards. This is a transient allocation made only while a
  password is checked, not a steady-state allocation.
- `--role` is parsed in every build but the production executable refuses it
  (exit status 2). A separate build compiled with `-DHMI_TEST_ROLE=1` accepts it,
  announces that it is a test build and shows a permanent banner. It is for
  tests and simulation only.

Consequences:
- The user file is protected by its owner and mode, not by its format. A CRC
  detects corruption but not an attacker who can write the file as the service
  user (see docs/AUTHENTICATION.md).
- The existing acknowledgement and write audit lines still carry the role and
  not the user name. (Done in M2: see the 2026-10-22 addendum on configuration integrity.)
- Nothing here was run on a Raspberry Pi. The hash time on the target is
  unmeasured.

## Addendum 2026-10-22: configuration integrity and safe startup

Context: the configuration decides which PLC the device talks to and whether and
how far it may write, and until now any file edit could change that. Roadmap
milestone M2 adds an integrity check. Details, file ownership, limits and
evidence are in docs/CONFIG_INTEGRITY.md.

Decision:
- A configuration is used only if its detached HMAC-SHA-512 signature verifies
  (`hmi_cfgsec`). Nothing is parsed before the MAC check succeeds. A failure is
  the existing safe mode (banner, no PLC endpoint, writes off). The strict
  parser, the table-driven schema and the golden corpus are unchanged apart from
  one new optional field, `plc.allowed_hosts`, which a signed configuration must
  contain with `plc.host` in it.
- The signature carries a serial number and the device refuses a lower one than
  it has accepted (rollback protection).
- The files are owned by root and only read by the service. The HMI therefore
  does not write its configuration. The roadmap item "configuration save
  restricted to Admin" is deferred to milestone M4 and needs a privileged helper.
- Rule 1 above gets a third exception of the same kind as the earlier two:
  `hmi_mac.c` includes Monocypher's `monocypher-ed25519.h` and nothing else
  outside the portable core does. `hmi_cfgsec.c` and `hmi_users.c` use only the
  platform layer, which gained `hmi_fs_writable_by_me`.
- Acknowledgement and write audit records carry the logged-in user.
  `hmi_alarm_ack_as`, `hmi_alarm_ack_all_as` and `hmi_writes_submit_as` take the
  name; the older calls record "-". The role checks, audit-before-effect and
  fail-closed behaviour are unchanged. The line formats only gained a trailing
  ` user=<name>`.
- The production executable refuses `--unsigned-config`; the simulation build
  accepts it for tests.

Consequences:
- Changing the configuration is an offline, privileged step (edit, sign,
  restart). The HMI cannot change it.
- A shared-secret MAC does not protect against anyone who holds the key or is
  root, and the serial state file can be deleted by the service user.
- Nothing here was run under real systemd as the service user or on a
  Raspberry Pi.

## Addendum 2026-10-22: persistence and storage

Context: the UI thread waited for the disk when it recorded a write or an
acknowledgement, the alarm journal and the alarm state were kept in memory
until a clean shutdown, the audit file was unbounded and had no wall-clock
time, and the shutdown budget was shorter than the longest legitimate stall.
Roadmap milestone M3 addresses these. Design, formats, limits and evidence are
in docs/STORAGE.md.

Decision:
- One storage worker thread (`hmi_store`) performs every durable write: the
  audit file, the alarm journal and the alarm state. It has a bounded queue, a
  visible fault flag and rotation with a size cap. The platform layer gained
  `hmi_event_*`, `hmi_fs_size`, `hmi_fs_rename`, `hmi_fs_read_tail` and
  `hmi_wall_ms`; no other core file touches the operating system.
- Audit before effect is kept and made asynchronous. `hmi_writes_submit_as`
  and the acknowledgement calls return before the record is durable. A PLC
  write stays RESERVED until its RESERVED record is durable, and the comms
  thread waits until the SENT record is durable before transmitting. An
  acknowledgement returns `HMI_ACK_PENDING` and is applied, with the same
  unseen-alarm check, when its record is durable. A record that fails, or does
  not complete in time, means the action does not happen (fail closed). This
  amends the 2026-10-15 and 2026-10-22 addenda only in when the call returns;
  the order (record, then effect) and the refusal when the record cannot be
  made durable are unchanged. The synchronous hooks remain for tests and for
  use without a storage worker.
- Lock graph: the store lock, like the others, is never held across a file
  operation or a callback, and is never nested with another lock. The alarm
  engine no longer calls its journal sink under its own lock: it queues entries
  and delivers them after unlocking, in order, from one caller at a time.
  Before, the sink was called under the alarm lock, which would have nested the
  alarm lock with any lock the sink takes; no sink was installed in production
  until now.
- A write is checked a second time by the comms thread just before it is sent,
  and a rate limit applies to accepted submissions. Both are part of
  `hmi_writes`; the limits are optional fields of the signed configuration.
- The configuration bounds for `response_timeout_ms` and `connect_timeout_ms`
  were lowered to 2000 and 3000 so that the longest stall of the comms thread
  (8 s) fits the service watchdog and stop timeout. `hmi_runtime_stop` returns
  whether everything stopped and was released.
- Audit lines carry a wall-clock time and whether the clock is synchronised, in
  addition to the monotonic time. The line formats are in
  `hmi_auditfmt`.
- `hmi_alarm.c` no longer reads its state file with `fopen`; it uses
  `hmi_fs_read_all` of the platform layer, which tells an absent, an unreadable
  and an oversized file apart.

Consequences:
- A login still waits on the UI thread for its record, for at most 2 s.
- Records still queued when the process dies are lost (at most 64); an action
  that needed a record never took effect without it.
- Nothing here was run on a Raspberry Pi, on real flash storage, or through a
  real power failure.

## Addendum 2026-10-22: screens, trends and configuration changes from the device

Context: roadmap milestone M4 adds screens (overview, alarms, trends, settings), alarm priority,
account management and configuration changes from the screen. The signed configuration must
stay unwritable by the service (the M2 decision), so changes from the screen need a separate
process.

Decision:
- The UI is split into one file per screen plus `ui_priv.h`; `hmi_ui.c` keeps the shared state
  and the test seams. A screen is a table of `reset`, `build` and `update` functions added to
  one list. Only the visible screen is updated.
- `hmi_trend` keeps min and max per bucket in fixed rings in RAM, fed by the supervisor from
  GOOD readings only. Its lock is a leaf: it is never held across another lock, a file operation
  or a callback. Trend data is lost at restart (owner decision D2).
- Account management lives in `hmi_auth` (add, password, role, remove, with the role check, the
  protection of one's own account and of the last administrator, and the audit record before the
  effect) and the settings screen only calls it. The password hash runs as a job polled by
  `hmi_auth_poll`, like the login hash.
- Configuration changes use a request file and a root helper, not a write by the service:
  - `hmi_config_write` writes a configuration as text that the strict parser reads back to the
    same structure (`hmi_config_serialize`, `hmi_config_equal`). It leaves out members that hold
    their default.
  - `hmi_cfgedit` defines the request, a strict parser, the whitelist and bounds
    (`hmi_cfgedit_apply`), and the service side (`hmi_cfgreq_submit`, `hmi_cfgreq_state`). The
    role is checked in the core (`HMI_ACT_EDIT_SETTINGS`, Engineer and above) and the request is
    recorded in the audit file before it leaves.
  - `p99_cfg` (`src/app/p99_cfg.c`), started as root by a systemd path unit, re-checks everything,
    records `CONFIG_CHANGED` before changing anything, replaces the configuration and signature
    and writes a result. Details and limits are in `docs/CONFIG_INTEGRITY.md`.
  - The shared-secret MAC is unchanged. A compromised service can still ask for any change on
    the whitelist; it cannot change the write switch, who may write, the allow-list or the
    tags' addresses and scaling.
- `--spool DIR` turns the CONFIG view on. Without it the view says it is not set up.

Consequences:
- The service restarts to use a new configuration (the helper restarts it). There is no reload
  without a restart.
- The pool for the UI grew from 128 KB to 192 KB.
- Nothing here ran under real systemd, on a Raspberry Pi, on a display or with a real PLC.

## Addendum 2026-10-23: authentication removed, small static build, one font and a palette

Context: the owner asked for a process under 1 MB and a size of a few hundred KB, and for the Argon2id password hash
(8 MiB per login) to be removed from the scope. Details and measurements are in
`docs/OPTIMIZATION_ROADMAP.md`; the removal is in `docs/AUTHENTICATION.md`.

Decisions:
- **No authentication.** The "Addendum 2026-10-22: authentication and sessions" above no longer describes the product. The
  login, the user store, the password hash, lockout, idle logout and account management are gone. The role is the
  claimed `--role` of the process (default viewer) in both builds, and the core still decides on every call (the
  decision point of the "authorization in the core, claimed role" addendum is now the only source of the role).
  Rejected: replacing Argon2id by another hash (the owner chose removal); keeping `--role` refused in the production
  build, which would have made an operator panel impossible.
- **Core boundary unchanged.** The portable core still has no LVGL and no operating system calls. Two helpers
  (`hmi_crc32`, `hmi_wipe`) moved from the removed modules to `src/hmi/hmi_util.c`; Monocypher is included only by
  `hmi_mac.c` and `hmi_util.c`.
- **The device build is static musl.** `Makefile.linux STATIC=1 CC=musl-gcc`: one file, size optimised with link-time
  optimisation, no unwind tables, libmodbus compiled from its source (LGPL: the object files or the build must be
  deliverable, `docs/THIRD_PARTY.md`). The development build stays dynamic glibc so that sanitizers work.
- **The UI has one font, one theme and eleven colours** (`src/ui/ui_theme.c`, `src/ui/font`). LVGL is configured for
  the widgets the screens use and for RGB565 only; the framebuffer must be 16 bits.
- **Profiles.** `LITE=1` (compile time) removes the trend screen, the CONFIG view and the chart widget, and halves the
  tag table; the full profile is the default.
- **Lazy screens.** TRENDS and the CONFIG view are built on first use and then kept.

Consequences:
- Requirements that assumed accounts are no longer met and are recorded as removed by decision.
- The audit trail names the role but not a person (`user=-`).
- Nothing here ran on a Raspberry Pi, a real display, a real touch panel or a real PLC.
