# Prometheus99

A lightweight C99 HMI runtime for industrial controllers: real LVGL v9.2.2
UI, Modbus TCP via libmodbus, typed tags with per-tag quality, ISA-18.2 style
alarms, audited operator writes. Built by Team Doomsday.

> **Status: working prototype, not production-ready.** Everything below marked
> "host-tested" was exercised on a Windows PC or in WSL2 Linux against a fake
> PLC. Nothing has run on a Raspberry Pi, a real display or a real PLC yet.

The repository holds two things; do not confuse them:

| | Windows prototype (legacy) | Portable HMI runtime (new) |
|---|---|---|
| Where | `src/layer*.c`, `src/main.c`, `include/` | `src/hmi/`, `src/ui/`, `src/app/` |
| Data source | this PC's CPU / RAM / SSD / ASUS EC | Modbus TCP PLC (configurable tags) |
| Renderer | custom 4-bit software renderer behind an LVGL-shaped shim | **real LVGL v9.2.2** |
| Platform | Windows only | portable core + Win32 and POSIX platform files |
| Role | demo of the original hackathon idea | the product path |

See [docs/ADR-0001](docs/ADR-0001-runtime-boundaries.md) for the boundary
rules and [bench/CLAIMS.md](bench/CLAIMS.md) for every measurement.

## What is implemented

Portable runtime (`src/hmi`, host-tested with a mock PLC and a fake clock,
plus end-to-end against real libmodbus):

- **Typed tags**: exact raw 32-bit pattern stored; u16/i16/u32/i32/f32;
  word order and byte order per tag; scale/offset; plausibility range.
- **Quality**: `GOOD / UNCERTAIN / BAD / UNAVAILABLE` plus a reason, per tag,
  with monotonic timestamps and a change version. A successful poll restores
  only the tags of the group it read. Staleness is aged on a supervisor
  thread, independent of blocked I/O.
- **Comms**: connection FSM, merged poll groups, absolute-phase scheduling
  (no drift), bounded timeouts, exponential reconnect backoff with jitter.
  Valid Modbus exceptions are data failures, not link failures. Dead-link
  detection takes `miss_limit x response_timeout`, independent of group count.
- **Link health** (`hmi_comms`, single owner): `HEALTHY / DEGRADED /
  NO_RESPONSE / DOWN` computed from the age of the last valid Modbus reply
  plus the connection state. Runtime, alarm engine, UI and write gate all
  consume this one result; a connected socket with a silent PLC is
  `NO_RESPONSE`.
- **Writes** (`hmi_writes`, separate module from the transport): validated (enabled, tag allowed, role, range, representable),
  queued, sent at most once, read back and verified. States: `PENDING, SENT,
  CONFIRMED, REJECTED, EXPIRED, UNKNOWN, VERIFY_FAILED`. A lost reply gives
  `UNKNOWN`, never an automatic retry. Audit slot is reserved before queueing
  and the intent is persisted before transmitting; if either fails the write
  is refused (fail closed). Without an audit file, writes are forced off.
- **Alarms**: HI/HIHI/LO/LOLO per tag with deadband hysteresis and on/off
  debounce, ack, escalation re-activation, return-to-normal-unacknowledged,
  comms-loss system alarm (judged by valid replies, not TCP state), 64-event
  journal, CRC-protected persistence. Evaluated on its own thread, so alarms
  stay responsive while the comms thread blocks in a Modbus call.
- **Config**: strict JSON (jsmn): unknown/duplicate keys, bad types, bad
  ranges, DNS names, writable tags without role/limits, alarm limit disorder
  all reject the whole file. A rejected config yields no PLC endpoint and
  writes disabled. Saves are atomic (temp + fsync + rename) and validated
  first.
- **UI view-model** (`src/ui/hmi_view.c`, no LVGL): all display wording and
  colour decisions as pure, unit-tested functions (`make test-view`).
- **UI** (`src/ui/hmi_ui.c`, LVGL binder over the view-model): tag table with quality colouring and
  `---` for unusable data, link indicator, alarm summary, ACK ALL, setpoint
  +/- / SEND with visible outcome, alarm journal. Labels are only touched
  when text or colour changed.

## Host-tested vs. needs real validation

| Feature | State |
|---|---|
| Decode/scaling/order, quality, versions, scheduling, backoff | host-tested (the core test modules total 1148 checks on the Windows host, 1168 on WSL2, which also runs the POSIX file-mode tests) |
| Writes: confirm, exception, lost reply, verify, expiry, audit failure, queue/audit capacity | host-tested (mock) and 2 scenarios against libmodbus |
| Alarms: debounce, hysteresis, ack, escalation, RTN, persistence, frozen on BAD | host-tested |
| Config validation + atomic save | host-tested (30+ rejection cases) |
| Real Modbus TCP framing/timeouts/exceptions via libmodbus 3.1.10 | host-tested against in-process fake server (75 checks) |
| LVGL rendering, binder, buttons | rendered headless on host; screenshots in `bench/results/` |
| POSIX platform file, POSIX fake PLC, libmodbus 3.1.10, `Makefile.linux` | built and tested on WSL2 (Ubuntu 24.04, gcc 13.3): 1168 core + 163 view-model + 75 end-to-end checks, 77 UI scenario assertions pass (WSL2 host, not Raspberry Pi) |
| Linux executable `prometheus99` | links on WSL2; with a missing framebuffer path it prints an error and exits 2. A path that opens but is not a framebuffer (e.g. `/dev/null`) is not detected |
| Linux fbdev + evdev touch on a real display | **never executed**; no framebuffer exists on the development host |
| Real PLC interoperability, word/byte order of real devices | **not tested** |
| Raspberry Pi: CPU, RAM, startup, frame time, touch latency | **not measured** |
| Sanitizers (WSL2 gcc) | ASan+UBSan and TSan clean on core and e2e (TSan first found 3 races in the core test code itself, fixed; none in `src/hmi`) |
| Authentication, password hashing, lockout, idle logout, first-boot administrator, `p99_user` | implemented and host-tested (Windows and WSL2, see [docs/AUTHENTICATION.md](docs/AUTHENTICATION.md)); **not run on a Raspberry Pi or a real touchscreen**, hash time on the target not measured |
| Trend chart, multiple screens, settings UI | **not implemented** |
| Liveness supervision (sd_notify READY and WATCHDOG, `HB nn` header mark, `deploy/prometheus99.service`) | tested on WSL2 with a fake notify socket (SIGSTOP, SIGCONT, SIGKILL, hung PLC) and with a transient real-systemd unit; **not run on a Raspberry Pi**, unit not installed under its real user and paths |
| Config reload at runtime, config HMAC/integrity key | **not implemented** |

## Build and test (Windows host, MSYS2 UCRT64 GCC + GNU make)

```sh
mingw32-make test-all      # legacy tests + portable core tests + view-model tests + libmodbus end-to-end
mingw32-make ui-host       # builds LVGL v9.2.2, runs headless UI scenario, writes build/shot_*.ppm
mingw32-make check-linux   # compile-only check of src/app/main_linux.c
mingw32-make all           # legacy Windows prototype
```

Results of the last full run are in [bench/CLAIMS.md](bench/CLAIMS.md) and the
final section of [walkthrough.md](walkthrough.md).

Linux (validated on WSL2 only; see [docs/LINUX_VALIDATION.md](docs/LINUX_VALIDATION.md)):

```sh
sudo apt install build-essential pkg-config libmodbus-dev
make -f Makefile.linux test
make -f Makefile.linux e2e
make -f Makefile.linux ui-host
make -f Makefile.linux hmi          # prometheus99 and the offline user tool p99_user
make -f Makefile.linux hmi-sim      # test build that accepts --role; never deploy it
make -f Makefile.linux cli-exec auth-exec
```

Do not run `./build-linux/prometheus99` with `config/hmi.example.json` as
shipped: its PLC host is a LAN address. Copy the file, set `host` to a
loopback or lab fake PLC, and keep `writes_enabled: false`.
Never point it at a real PLC without explicit authorisation.

## Configuration

See `config/hmi.example.json`. Addresses are **0-based wire addresses**
(Modbus "40001" is address 0). `host` must be an IPv4 literal. Limits:
64 tags, 32 poll groups, 32 KB file.

## Third-party

LVGL v9.2.2 (MIT), libmodbus v3.1.10 (LGPL-2.1+), jsmn v1.1.0 (MIT):
see [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md).

## Legacy Windows prototype notes

Unchanged from the earlier audit: custom renderer, host sensors, simulated
failover. Its previous "0.10 MB RAM" claim was a working-set artefact
(private bytes were 5.7-6.7 MB). See `bench/CLAIMS.md` section 1.

## Known gaps / next work

1. Detect a framebuffer device that opens but is not a framebuffer
   (`FBIOGET_VSCREENINFO` probe) in `main_linux.c`.
2. Raspberry Pi measurements (RSS/PSS, CPU, startup, frame time, touch latency).
3. Config integrity. Periodic alarm-state save and audit-log rotation (both currently only on clean shutdown / unbounded). Hardware watchdog and installing the unit on a real device.
4. Trim `lv_conf.h` (default config builds a 1.5 MB test binary).
5. Soak test, fault injection with `tc netem`, fuzzing the config parser.

## Team and licence

Team Doomsday: Abhilash L, Adarsh P Abhilash, Nikhil Nuguri. MIT licence
for this repository's own code ([LICENSE](LICENSE)).

## Roles and authorization

- **Roles come from logging in.** The device starts as Viewer. A person logs in
  on the screen, and the role of that account is the role the UI hands to the
  core. Passwords are hashed with Argon2id, failed logins are delayed and then
  locked, and an idle session ends after five minutes. While no user file
  exists the login screen offers to create the administrator. Accounts are
  managed offline with `p99_user`. The design, parameters and limits are in
  [docs/AUTHENTICATION.md](docs/AUTHENTICATION.md).
- **`--role` is a test option only.** The production executable refuses it with
  exit status 2. The simulation build (`make -f Makefile.linux hmi-sim`)
  honours it, says so at start-up, and shows a permanent "TEST ROLE ... NOT
  AUTHENTICATED" banner. It must not be deployed. Unknown roles, unknown
  options, missing values and repeated options exit with status 2 in both
  builds.
- Authorization is enforced in the core, not only in the UI. Acknowledging
  alarms needs Operator or above; writing a tag needs the tag's configured
  minimum role. The UI dims the ACK ALL button for a Viewer and shows the
  refusal, but the core refuses regardless.
- Alarm acknowledgements, logins, logouts and lockouts are written to the audit
  file next to PLC writes. A granted acknowledgement and a successful login are
  refused if a configured audit file cannot be written; without `--audit` they
  work and are not recorded. The acknowledgement and write lines carry the role
  but not yet the user name.
- Timestamps in the audit file are milliseconds since boot (monotonic), not
  wall-clock time.
- Limits of the user file: a CRC detects corruption, not an attacker who can
  write the file as the service user. The file's owner and mode (0600, refused
  if group or others can write it) are what protect it.
- Still **not** implemented: config integrity (HMAC), audit-file integrity or
  rotation, an on-screen user management page (the offline tool does this), and
  any protection of the Modbus TCP link itself (it has no authentication or
  encryption).
