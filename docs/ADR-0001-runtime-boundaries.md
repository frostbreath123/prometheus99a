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
  hmi_platform.h  the ONLY OS surface: clock, mutex, thread, atomic flag, atomic file write
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

## Addendum 2026-10-22: authentication and sessions

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
  not the user name. Adding it is planned for milestones M2 and M3.
- Nothing here was run on a Raspberry Pi. The hash time on the target is
  unmeasured.
