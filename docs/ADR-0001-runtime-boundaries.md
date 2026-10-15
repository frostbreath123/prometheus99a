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
  hmi_platform.h  the ONLY OS surface: clock, mutex, thread, atomic flag, atomic file write
src/hmi/hmi_platform_win32.c   host tests / Windows demo
src/hmi/hmi_platform_posix.c   Linux target (not yet compiled+run, see README)
src/hmi/plc_libmodbus.*        driver backed by libmodbus 3.1.10 (the only file that includes <modbus.h>)
src/ui/hmi_ui.*               LVGL v9.2.2 presentation; UI thread only
src/app/main_linux.c          Linux entry point (fbdev + evdev)

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
  clock (521 checks) and with real libmodbus against an in-process fake PLC
  (75 checks), with no hardware.
- Linux behaviour is only as verified as the POSIX platform file; until it is
  built and run on Linux the claim is "compiles in principle", nothing more.
- Roles are a launch parameter, not authentication. A real login/lockout
  module is future work (see README "Not implemented").

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
