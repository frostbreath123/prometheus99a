# Authentication and sessions

Status: implemented and tested on the development host (Windows with MSYS2, and
WSL2 Ubuntu 24.04) on 2026-10-22. It has not been run on a Raspberry Pi, on a
real touchscreen, or in a deployed service. Every measurement below is a host
measurement and must not be quoted as target-hardware evidence.

This document is the design record for roadmap milestone M1. The code is in
`src/hmi/hmi_kdf.*`, `hmi_users.*`, `hmi_auth.*`, the login screen in
`src/ui/hmi_ui.c`, the wording in `src/ui/hmi_view.c`, and the offline tool in
`src/app/p99_user.c`.

## What it does

- The device starts with no logged-in user, and the role handed to the core is
  Viewer. Viewer can read everything and cannot acknowledge alarms or write tags.
- A person logs in on the screen with a user name and a password. The role of
  that account (Operator, Engineer or Admin) is then the role the UI passes to
  `hmi_alarm_ack_all` and `hmi_writes_submit`. The UI reads the role through
  `hmi_auth_role()` at every privileged action and keeps no copy of it, so an
  idle timeout or a logout takes effect on the next action even if the
  periodic check has not yet run.
- An idle session ends after five minutes without touch input, and the header
  returns to Viewer.
- The authorization rules in `hmi_authz` are unchanged. The existing write
  pipeline, audit-before-send rule and fail-closed behaviour are unchanged.

## Passwords

- Algorithm: Argon2id from Monocypher 4.0.2 (vendored in `third_party/monocypher`,
  see `docs/THIRD_PARTY.md` for source, licence and checksums).
- Default cost: 3 passes, 8 MiB of memory, 1 lane, a random 16-byte salt per
  account, a 32-byte hash. The cost is stored in every account record, so it can
  be raised later without invalidating existing accounts. A record that asks for
  more than 16 passes, 64 MiB or 4 lanes is rejected as corrupt.
- The scratch memory is allocated for one hash and wiped and freed afterwards.
  It is a short, transient allocation made only while a password is being
  checked. It is not a steady-state allocation, and the idle heap-call count in
  the headless UI test stays at zero.
- Comparison of hashes is constant time. The hash runs on a worker thread when
  a person logs in on the screen, so the UI and the watchdog keep running.
- Password policy: 8 to 64 printable ASCII characters, and not equal to the user
  name. The limit of 8 is a deliberate compromise with typing on a touchscreen.
- Passwords are never written to the audit file or to standard error. The
  working copies are wiped after use.
- The default cost is below what is usually recommended for a server. It was
  chosen, with the project owner, for a small device. The main protections
  against guessing are the lockout and the fact that the file is only readable
  by the service user.

Measured on the host (WSL2, x86-64, `p99_user bench`, three runs): 41 ms, 17 ms
and 13 ms for one hash. This host is much faster than a Raspberry Pi. The real
time on the target is not known. Run `p99_user bench` on the device and raise
`t` or `m` if the hash is shorter than about a third of a second. Per-record
parameters allow this without a data migration, but there is no tool command
yet that re-hashes an existing account at a new cost (a changed password is
hashed with the current default).

## The user file

- Path: `--users FILE`, default `/var/lib/prometheus99/users.bin`.
- Binary format, version 1, exactly `8 + 96 * count + 4` bytes: a header (magic
  `P99U`, version, count, two zero bytes), one record per account (name, role,
  cost parameters, salt, hash) and a CRC-32 trailer. At most 16 accounts. The
  exact layout is in `src/hmi/hmi_users.h`.
- Parsing is strict. A wrong magic, version, length, checksum, reserved byte,
  role, name, duplicate name, out-of-range cost, or a store with no
  administrator makes the whole file invalid.
- Names are 3 to 24 characters from `a-z 0-9 _ . -`. This keeps them safe to
  write into the audit file.
- The file is written atomically (temporary file, flush, rename, directory
  flush) with mode 0600.
- Three states are kept apart on purpose:
  - **Absent:** first boot. The login screen offers to create the administrator.
  - **Valid:** normal operation.
  - **Present but not trustworthy** (corrupt, truncated, empty, too large, or
    writable by group or others): login is disabled, the screen says so, and
    first-boot creation is **not** offered, so damaging the file cannot be used
    to reach the first-boot screen.
- A store that group or others can read but not write is accepted, and a warning
  is printed at start-up.

### What the file format does not protect against

The CRC-32 detects corruption and careless edits. It does not stop someone who
can write the file as the service user: that person can recompute the CRC, or
replace a hash with one for a password they know, or delete the file to reach
the first-boot screen again. Defence against that rests on the file's owner and
mode, and on keeping other software off the device. A keyed integrity check for
the configuration is planned for milestone M2, but a key that the service user
can read does not change this particular limit, and the roadmap records that
design question as open. The roadmap's acceptance wording "a tampered user file
is rejected" is therefore met only for corruption and unsophisticated edits.

## First boot

1. No user file exists. The login button opens "CREATE ADMINISTRATOR" with a user
   name, a password and a confirmation.
2. The password policy is checked, the two passwords must match, and the account
   is created as Admin. There is no default or shared account.
3. The creation is written to the audit file first. If an audit file is
   configured and cannot be written, nothing is created.
4. The screen then switches to the normal login. Creating the account does not
   log in.

Known limits: anyone with physical access to the screen during first boot can
create the administrator, and anyone who can delete the user file as the service
user can return the device to first boot. The offline tool can create the first
administrator over SSH before the screen is exposed.

## Lockout

- After each failed attempt the next attempt must wait one second.
- The fifth consecutive failure for an account locks it for 30 seconds. Each
  further failure doubles the lock, up to 15 minutes. A correct login resets the
  count.
- Names that do not exist share one counter and one lock. An unknown name also
  runs a full password hash, so a wrong name is neither faster nor free.
- The failure counter is stored next to the user file (`<users>.lock`, CRC
  protected) and survives a restart. The monotonic clock is not stored, so after
  a restart a running lock starts again from its full length. Restarting the
  device never shortens a lock.
- A damaged counter file is treated as if a 30 second lock had just started for
  every account. A missing counter file means no recorded failures.
- Known limits: someone who can delete the counter file (service-user write
  access) resets the counters. The messages for a wrong user name and a wrong
  password are identical, but a locked name and an unlocked name behave
  differently, so the existence of an account can be inferred by an attacker who
  is willing to cause a lock.

## Audit records

Written to the same audit file as writes and acknowledgements, one line per
event, starting with the monotonic milliseconds since boot like the other
lines. The user is the account name, or `?` for any name that is not an
account (so a password typed into the wrong field never reaches the file).

| Line | When | If the record cannot be written |
|---|---|---|
| `LOGIN_OK user=... role=...` | before the session starts | the login is refused |
| `LOGIN_FAIL user=... failures=n` | wrong password | the failure still counts |
| `LOCKOUT user=... failures=n lock_ms=m` | a lock starts | best effort |
| `LOGOUT`, `IDLE_LOGOUT` | session ends | best effort |
| `USER_CREATE user=... role=admin` | first administrator | nothing is created |
| `USER_CREATE_FAILED` | the account file could not be saved | best effort |
| `USER_ADD`, `USER_REMOVE`, `USER_PASSWORD` | offline tool | the change is refused |

Without `--audit` logins work and are not recorded, as for acknowledgements. Since
milestone M2 the acknowledgement and write lines also end with `user=<name>`
(see `docs/CONFIG_INTEGRITY.md`), so they no longer have to be matched to the
login lines by time.

## The login screen

A full-screen layer with a user name box, a password box (characters are never
shown, not even the last one), the LVGL keyboard, OK and Cancel. The user name
box only accepts `a-z 0-9 _ . -`. While the hash is being computed the screen
says "Checking" and cannot be left. After a failure the wording is the same for
a wrong user and a wrong password. After a lock the screen shows a countdown.
The keyboard's hide-keyboard key cancels the screen.

The password text box overwrites its contents before clearing. LVGL may keep
copies in its memory pool, so the time a password stays in memory is shortened
but not proven to be zero. On the host the LVGL pool use grows from 21 KB to 29
KB of 121 KB with the login layer built.

## `--role`

The production executable refuses `--role` with exit status 2. A separate
simulation build, compiled with `-DHMI_TEST_ROLE=1` (`make -f Makefile.linux
hmi-sim`), still accepts it for tests, prints that it is a test build, and shows
"TEST ROLE ... NOT AUTHENTICATED" permanently on the screen. The simulation
build must never be deployed. The command line parser still understands
`--role` in both builds, so a mistyped value is still an error.

## Offline user tool

`p99_user` (Linux, `make -f Makefile.linux hmi`) works on the user file while the
HMI is stopped or from a console: `init`, `add`, `passwd`, `role`, `remove`,
`list`, `bench`. It reads passwords without echo, asks twice, never prints them,
refuses to remove or demote the last administrator, and with `--audit` refuses
to change anything it cannot record. Run it as the service user so the file keeps
its owner. The running HMI reads the file only when it starts, so a change made
with the tool takes effect after a restart. The counter file is also read only at
start, so the tool cannot unlock a locked account in a running device.

## Evidence (host only)

| Item | Environment | Result |
|---|---|---|
| Argon2id known answer (RFC 9106 section 5.3) | Windows and WSL2 | passes through `hmi_kdf` |
| Core tests including `kdf`, `platform`, `users`, `auth`, `cli` | Windows host | 1148 checks, 0 failed |
| Same, with the POSIX file-mode tests | WSL2 | 1168 checks, 0 failed |
| ASan with UBSan, and TSan, every core module | WSL2 | 0 reports |
| View-model | both | 163 checks, 0 failed |
| Headless LVGL scenario with first boot, wrong password, lock wait, login, idle logout, logout, audit | both | 77 assertions, 0 failed; 0 flushes in 1.5 s idle with the login layer present |
| Real executables: refusal of `--role`, store states, file modes, tool commands, audit lines | WSL2 | `cli-exec` and `auth-exec` pass |
| Mutation check of the auth tests (idle ignored, audit ignored, lock ignored, counters not loaded) | Windows | each change made the suite fail, then restored |

## Not verified

- Hash time and memory on a Raspberry Pi.
- Typing credentials on a real touchscreen and the keyboard's usability.
- A deployed service under its own user and the real file ownership.
- Behaviour with a power cut during a counter or store write (the atomic write
  is the same one used elsewhere and has no power-cut test yet).
- The sanitizers were run on the core modules, not on the LVGL harness or the
  full executable.
