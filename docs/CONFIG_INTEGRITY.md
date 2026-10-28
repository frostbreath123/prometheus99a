# Configuration integrity and safe startup
> **Note, 2026-10-23:** the login, the user store and the password hash described in the parts of this file that mention logins or accounts were removed from the product (`docs/AUTHENTICATION.md`). Those passages are kept as the record of M1 to M4 and no longer describe the program: there are no login or account records in the audit file and no user file, and the audit line shows `user=-`.

Status: implemented and tested on the development host (Windows with MSYS2, and
WSL2 Ubuntu 24.04) on 2026-10-22. It has not been run on a Raspberry Pi, under
real systemd as the `prometheus` user, or against a real PLC. Every number
below is a host measurement.

This is the design record for roadmap milestone M2. The code is in
`src/hmi/hmi_mac.*` (HMAC-SHA-512), `src/hmi/hmi_cfgsec.*` (signature, key,
rollback, allow-list), the allow-list in `src/hmi/hmi_config.c`, the start-up
wiring in `src/app/main_linux.c`, and the tool `src/app/p99_sign.c`.

## What it does

The device uses a configuration only if all of this holds, in this order:

1. The key file exists, is exactly 32 bytes, is not accessible to others and is
   not writable by the group (modes 0400, 0440, 0600 and 0640 pass).
2. The configuration file can be read, is not larger than 32 KiB, and is not
   writable by group or others.
3. A detached signature file `<config>.sig` exists, has the right size, magic,
   version and zero reserved bytes, and is not writable by group or others.
4. The length and CRC-32 held in the signature match the file. This step only
   exists to tell an accident (a damaged or hand-edited file) from an attack in
   the message the operator sees. It is not a security control by itself.
5. The HMAC-SHA-512 over the signature header and the exact bytes of the
   configuration matches. **Nothing is parsed before this succeeds**, so
   untrusted bytes never reach the JSON parser.
6. The signature serial is not lower than the highest serial this device has
   accepted before (rollback protection).
7. The signed text passes the strict parser that already existed.
8. `plc.allowed_hosts` is present and `plc.host` is one of its entries.

Any failure is safe mode, the behaviour that already existed for an invalid
configuration: a banner on the screen with the reason, no PLC endpoint, writes
off, and login still works. A rejected configuration can never enable writes,
because no configuration is loaded at all.

Every start-up appends one line to the audit file: `CONFIG_ACCEPTED serial=N`
or `CONFIG_REJECTED reason=<code>`. Codes: `config_unreadable`,
`config_too_large`, `config_mode`, `signature_missing`, `signature_format`,
`config_corrupt`, `key_missing`, `key_format`, `key_mode`,
`signature_mismatch`, `rollback`, `config_invalid`, `host_not_allowed`. The line
is best effort, because nothing depends on it.

## Why a detached signature and HMAC-SHA-512

The mentor's document asks for an HMAC. HMAC-SHA-512 comes from Monocypher's
optional `monocypher-ed25519` files, which are part of the same release already
vendored for the password hash (see `docs/THIRD_PARTY.md`). It adds no new
dependency and matches the wording of the requirement. It passes the RFC 4231
test vectors (cases 1, 2, 3 and 6, also cross-checked with Python's `hmac`).

The signature is a separate file so that the configuration stays the exact text
the operator wrote, and so that the existing parser, the golden corpus of 184
cases, and the tests did not change.

## Where the files live and who owns them

This is a shared-secret MAC. Anyone who can read the key can also sign. The
design therefore does not try to hide the key from the service. It makes sure
the service cannot **write** anything the key protects:

| File | Owner and mode | Written by |
|---|---|---|
| `/etc/prometheus99/` (directory) | root, 0755 | root |
| `/etc/prometheus99/hmi.json` | root, 0644 | root, through `p99_sign` |
| `/etc/prometheus99/hmi.json.sig` | root, 0644 | `p99_sign` |
| `/etc/prometheus99/config.key` | root:prometheus, 0440 | `p99_sign keygen` |
| `/var/lib/prometheus99/config.serial` | prometheus, 0600 | the service |

A compromised service can read the key and the configuration but cannot rewrite
either, and cannot replace them by renaming, because the directory is not
writable by it. Tampering then needs root.

Consequence, stated plainly: **the HMI itself cannot save configuration changes.**
Changing the configuration is an offline step by root: edit, `p99_sign sign`,
restart the service. The roadmap item "configuration save restricted to Admin"
therefore needs a privileged helper and is deferred to the settings screen in
milestone M4. `hmi_config_save_text` still exists in the code but nothing in the
product calls it.

## Rollback protection

The signature carries a serial number, and the device remembers the highest one
it has accepted in `config.serial`. A genuinely signed but older configuration,
for example an old file put back with its old signature, is refused.
`p99_sign sign` increases the serial by one each time (or takes `--serial N`).

Limits: the state file is written by the service, so anyone who can delete it as
the service user resets the protection. A damaged state file is reported on
standard error and treated as absent. If the new serial cannot be stored, the
configuration is still accepted and a warning says that rollback protection is
not active (refusing would stop the device on a read-only or full disk).
`p99_sign verify` never touches the state.

## The PLC allow-list

`plc.allowed_hosts` is a list of 1 to 8 IPv4 literals inside the signed text.
`p99_sign` refuses to sign a configuration whose `plc.host` is not in it, and
the device refuses to run one. It is a second check inside the signed text, so
a mistake such as a mistyped address is caught before the device is shipped.
It does not stop someone who holds the key.

## What this does not protect against

- Someone who holds the key, or who is root.
- A configuration that is signed and wrong. The signature says who approved the
  text, not that the text is a good idea. Ranges and limits in it are
  trusted once signed.
- The Modbus TCP link itself, which has no authentication or encryption.
- Replacement of the whole device software.
- Deleting the serial state file as the service user (see above).
- Time: there is no wall clock on the device, so there is no expiry.
- A compromised service asking `p99_cfg` for a change inside the whitelist (see the section on changing the configuration on the device).

## Test-only escape

The simulation build (`make -f Makefile.linux hmi-sim`, compiled with
`-DHMI_TEST_ROLE=1`) accepts `--unsigned-config`, which loads the file with the
old unsigned loader and prints "configuration NOT verified". The production
build refuses the flag with exit status 2. The simulation build must never be
deployed.

## Audit identity

Acknowledgement and write lines in the audit file now end with `user=<name>`,
the logged-in account that performed the action (`user=-` when no account is
known, for example in a test build). Names are validated, so a caller cannot put
a space, a newline or `=` into a line. The existing prefix of each line is
unchanged. Milestone M3 added the old value and a wall-clock time with the clock
state to the write lines and a wall-clock time to the acknowledgement lines (see
`docs/STORAGE.md`). The start-up `CONFIG_ACCEPTED` and `CONFIG_REJECTED` lines are
written before the storage worker exists and receive the same wall-clock stamp.

## Schema additions in M3

Two optional top-level fields, `write_min_interval_ms` (100 to 3600000, default
1000) and `write_max_per_minute` (1 to 600, default 30), set the write rate
limit. They are part of the signed text like every other field, so they cannot
be changed without signing again. The bounds of `plc.response_timeout_ms` and
`plc.connect_timeout_ms` are now 2000 and 3000 (they were 30000), so that the
longest stall of the comms thread fits the service watchdog and stop timeout.
A configuration signed before M3 with a larger timeout is now rejected as
`config_invalid` and the device starts in safe mode.

## Changing the configuration on the device (M4)

The service cannot write its configuration, so an Engineer who changes a setting on the screen
does not change it. The service writes a small request file into a spool directory
(`--spool`, for example `/var/spool/prometheus99`, owned by the service user, mode 0750, under
a directory that only root can change). A systemd path unit
(`deploy/prometheus99-config.path`) starts `p99_cfg` (`src/app/p99_cfg.c`) as root when
`<spool>/request` exists. The request text and the whitelist are in `src/hmi/hmi_cfgedit.h`.

What a request can change: `plc.host` (only to a host already in `plc.allowed_hosts`),
`plc.port`, `plc.unit`, and per tag the four alarm limits (each can be switched off), the
deadband, the on and off delays and the priority. Nothing else. A change that makes the
configuration invalid for the strict parser, or that changes nothing, refuses the whole request.

What the service does (`hmi_cfgreq_submit`): the core checks the role (Engineer and above),
the audit line `CONFIG_REQUEST by=<user> changes=<n> base_serial=<s>` is made durable first (if
that fails nothing is written), then `<spool>/request` is written atomically. A second request
while one is waiting is refused.

What `p99_cfg apply` does, in this order, stopping at the first failure:
1. Opens the spool directory without following links (it must belong to the service user or
   root and must not be writable by others) and opens the request without following links. The
   request must be a regular file with one link, owned by the service user or root, not writable
   by group or others, and at most 8 KiB.
2. Verifies the current configuration exactly as the device does, without touching the rollback
   state.
3. Parses the request strictly, requires `base_serial` to equal the serial of the signature on
   disk, refuses serial 4294967295 (it cannot be increased), and applies the changes with the
   whitelist and bounds. The changed configuration is written out, read back with the strict
   parser and compared, so the result is exactly what the device will load.
4. Appends `<ms> CONFIG_CHANGED serial=<new> from_serial=<old> changes=<n> by=<user> wall=... sync=...`
   to the audit file BEFORE anything is changed. The audit file must be a regular file with one
   link and owned by the service user or root, opened without following links. If the line
   cannot be written, nothing is changed.
5. Keeps `<config>.prev` and `<config>.sig.prev`, then replaces the configuration and the
   signature (temporary file, flush, rename, mode 0644, owner and group of the old files, serial
   plus one). If the signature cannot be written the old configuration is put back.
6. Writes `<spool>/result` (`applied serial=N changes=K by=U` or `rejected: reason`), removes the
   request (also after every refusal), and with `--restart UNIT` runs `systemctl restart UNIT`.
   The screen shows the first line of the result.

Exit status: 0 applied, 1 refused (the result says why), 2 wrong usage, 3 failed without leaving a
result. The unit accepts status 1 as success.

Stated limits:
- The helper cannot tell who is really logged in on the screen. A compromised service could ask
  for any change on the whitelist, for example to raise an alarm limit. The whitelist, the
  bounds, the audit lines and the result are the protection, not proof of identity. A
  compromised service cannot change the write switch, who may write, write limits, the allow-list
  or add a PLC host that is not already allowed.
- The service group can read the key (it must verify the signature). What stops a compromised
  service from signing is that it cannot write the configuration directory.
- The configuration and the signature are replaced by two renames. If the service restarted
  between the two it would see a mismatch and start in safe mode. The window is a few
  milliseconds and was not measured.
- Audit time stamps of the helper use the same monotonic clock as the service
  (`CLOCK_MONOTONIC`), so the lines order together.

## Evidence (host only)

| Item | Environment | Result |
|---|---|---|
| HMAC-SHA-512 known answers (RFC 4231 cases 1, 2, 3, 6) | Windows and WSL2 | 13 checks, 0 failed |
| Signature and loader: format, every rejection class, tamper sweeps, rollback, allow-list | Windows | 203 checks, 0 failed |
| Same, with the POSIX file-mode rules | WSL2 | 223 checks, 0 failed |
| Flipping every byte of the configuration, and of the signature, in turn | both | every flip rejected, nothing loaded |
| Re-computing length and CRC without the key | both | rejected as `signature_mismatch` |
| Turning `writes_enabled` on by hand, same length, still valid JSON | both | rejected, no configuration loaded |
| Allow-list parsing | both | 33 new checks in the config tests; the golden corpus is unchanged |
| Real executables: signing tool, accepted case, 11 kinds of rejection, no PLC connection in safe mode | WSL2 | `config-exec` passes; a loopback listener saw 0 connections in every safe mode run and 2 in the accepted run |
| Mutation check: MAC check, CRC check, rollback, allow-list, empty allow-list, key mode, config mode, signature mode each disabled in turn | Windows and WSL2 | each made the suite fail, then restored |
| ASan with UBSan, and TSan, every core module | WSL2 | 0 reports |
| Writing a configuration out and reading it back; request format, parser, whitelist, bounds, spool hand-off | Windows and WSL2 | 868 `cfgedit` checks, 0 failed; all 58 accepted corpus configurations read back identically |
| `p99_cfg` against real files, as root | WSL2 | `cfgedit-exec` passes (every refusal listed in `ROADMAP_STATUS.md`), also with the helper built with ASan and UBSan (0 reports) |
| Mutation checks of the new rules | WSL2 | 33 of 34 caught (the survivor is an internal read-back assertion that no input can trigger) |
| Audit identity | both | 28 new write checks, 22 new alarm checks, runtime line formats, UI scenario (84 assertions) names `user=admin` on an acknowledgement and a write |

## Not verified

- The helper and its path unit under real systemd, including the restart of the service (the units pass `systemd-analyze verify`, which only checks syntax).
- A power cut during the replacement of the configuration and the signature.
- Behaviour under real systemd as the `prometheus` user with root-owned files.
- A power cut while the serial state is written.
- Timing on a Raspberry Pi (the check is a few kilobytes of hashing and is not
  expected to matter, but it was not measured).
- Sanitizers on the LVGL harness and the full executable.
