# Authentication

## Named-user PIN login (added 2026-11-02, phase 3 of `docs/MASTER_PROMPT_FINAL.md`)

The product has a login again, of a smaller design than the one removed on 2026-10-23 (below). It is additive: a configuration without users behaves exactly as before (the role claimed with `--role`).

- **Users** live in the signed configuration (`"users"`, at most 8): a name (letters, digits, `.`, `_`, `-`), a role (viewer, operator, engineer, admin), a random 16-byte salt and a 32-byte PIN hash. A user list that is not signed is never used: the configuration as a whole is refused (safe mode) or, in the test build with `--unsigned-config`, login stays off and a warning says so.
- **PIN hash.** `pin_hash = PBKDF2-HMAC-SHA-512(password = HMAC-SHA-512(pin key, PIN), salt, 10,000 iterations)`, first 32 bytes. PBKDF2 is RFC 8018, implemented on Monocypher's HMAC-SHA-512 and checked against published test vectors (`tests/hmi/test_auth.c`, confirmed with Python's `hashlib`). The pin key is derived from the configuration key (`HMAC-SHA-512(configuration key, "prometheus99 pin hash v1")`, first 32 bytes), so a copy of the configuration alone does not let anyone test PINs offline. One check takes 12 ms on the development PC (x86-64); on a Raspberry Pi it will be slower (not measured).
- **PIN rules.** 4 to 8 digits. Five wrong PINs in a row lock the login for 60 s; each further lock without a success in between doubles the time, up to 16 minutes; a correct PIN ends it. A logged-in user is logged out after 5 minutes without a touch (LVGL's input inactivity counter). The lock is kept in memory only.
- **Screen.** With users, the header shows the user and role (for example `op1 OPERATOR`) or `LOGGED OUT`, and a LOGIN / LOGOUT button. LOGIN opens a panel with one button per user and a digit keypad (keys of 92 x 56 design pixels); the PIN is shown as stars. The Settings screen says "Login: named users with a PIN".
- **Roles.** Nobody logged in means the viewer role. A claimed `--role` is ignored when the configuration has users (the program says so on standard error). The core checks the role on every privileged call, unchanged; the login decides which role and name the screen passes.
- **Audit.** `LOGIN user=<name> role=<role>`, `LOGIN_FAILED user=<name> reason=wrong_pin`, `LOGIN_LOCKED user=<name> lock_s=<s>`, `LOGOUT user=<name> reason=operator|idle`, all sealed in the audit chain (`docs/STORAGE.md`). Every write and acknowledgement record carries the logged-in user's name in `user=`. A login whose record cannot be made durable is refused ("LOGIN REFUSED: audit unavailable"), as writes are; without an audit file at all there is nothing to record and login works.
- **Managing users.** `echo 2468 | p99_sign add-user --config FILE --key KEY --name op1 --role operator` (the PIN comes from standard input, never from the command line) and `p99_sign del-user --config FILE --key KEY --name op1`. Both rewrite the configuration in the program's own format and sign it with the next serial. The running program reads users only at start.
- **Recovery** (a forgotten PIN, everybody locked out): restart the service (the lock is in memory), or as the engineer delete and re-add the user with `p99_sign` and restart. Both need access to the device or its configuration key, which is the intended boundary.

### Limits, stated plainly

- A 4 to 8 digit PIN has at most 10^8 values. Online guessing is slowed by the lock; offline guessing needs the configuration key, and with it the whole PIN space can be searched (10,000 PBKDF2 iterations make that slower, not impossible). The PIN protects against casual use of an unattended panel, not against someone who has the device's files.
- The lock is lost on a restart, so someone who can restart the program can keep guessing (about 5 PINs per restart).
- Shared accounts are possible if users share PINs; the program cannot prevent that.
- There is no encryption on Modbus TCP; the login does not change what can be read on the network.
- Not run on target hardware: timing of the PIN check, readability and touch size on a real panel.

The rest of this file is the record of the earlier design and its removal.

# Authentication: removed from the product (2026-10-23)

Status at that time: there is no login, no password, no user store and no password hash in the product. This file replaces
the earlier description of milestone M1 (login, Argon2id, lockout, idle logout, accounts on the device). That
design is in the Git history (commit `fe165c2` and the M3 and M4 commits that extended it) and was removed on the
owner's instruction when the size and memory work of `docs/OPTIMIZATION_ROADMAP.md` was carried out.

## What was removed

| Removed | What it was |
|---|---|
| `src/hmi/hmi_kdf.c`, `hmi_kdf.h` | The Argon2id password hash (Monocypher's `crypto_argon2`), 8 MiB of memory per hash |
| `src/hmi/hmi_users.c`, `hmi_users.h` | The user file (salted hashes, CRC), the password policy, account editing |
| `src/hmi/hmi_auth.c`, `hmi_auth.h` | Login, lockout counters, idle logout, first-boot administrator, account changes on the device |
| `src/ui/ui_login.c`, `ui_users.c` | The login and first-boot screen, the accounts view of the settings screen |
| `src/app/p99_user.c` | The offline tool for accounts |
| `tests/hmi/test_kdf.c`, `test_users.c`, `test_auth.c`, `auth_exec_test.sh` | Their tests (about 620 checks) |
| `--users` option, `HMI_ACT_MANAGE_USERS` | The user file path and the action only an administrator had |
| Login records in the audit file | `LOGIN_OK`, `LOGIN_FAIL`, `LOCKOUT`, `LOGOUT`, `IDLE_LOGOUT`, `USER_*` lines |

Two small helpers that lived in `hmi_kdf.c` and `hmi_users.c` and are still needed (`hmi_wipe`, `hmi_crc32`) are now in
`src/hmi/hmi_util.c`. Monocypher stays as a source file because the configuration signature uses its SHA-512 and HMAC
(`src/hmi/hmi_mac.c`), its constant-time comparison and its wipe; the linker drops the Argon2id code, and
`bench/gate.sh` fails the build if any Argon2id or BLAKE2b symbol is found in the executable.

Nothing was put in its place. No home-made hash and no weaker hash was added, and no password or credential of any kind
is stored or read by the product any more.

## What the product does instead

The role is a **claimed** role. It is given with `--role viewer|operator|engineer|admin` by whoever starts the process
(in practice the service unit, `deploy/prometheus99.service`). The default is `viewer`, which cannot acknowledge alarms,
write a tag or ask for a configuration change. The rule "who may do what" is unchanged and still enforced in the core,
not only in the screen (`src/hmi/hmi_authz.c`):

| Action | Role needed |
|---|---|
| Acknowledge an alarm | operator or above |
| Write a tag | at least the `write.role` of that tag in the configuration |
| Ask for a configuration change (CONFIG view) | engineer or above |

Every acknowledgement, write and configuration request still goes to the audit file before it takes effect, with the
role (`role=operator`) and with the user shown as `-`, because nobody is identified. The header of the screen and the
SYSTEM view say which role is in force and that it is not authenticated. The program writes the same to standard error
at start, with a warning for any role above `viewer`.

Both builds honour `--role`. Before this change the production build refused it and could only be raised by a login.
The test build (`-DHMI_TEST_ROLE=1`, `make hmi-sim`) differs only in accepting `--unsigned-config` and printing a test
banner. A signed configuration is still required in the production build, and the PLC allow-list, the serial
(rollback) protection and the write rate limits are untouched.

## What this means for security

- **There is no authentication.** Anyone who can start the process, or edit its service unit, or run it as another
  user, chooses the role, including `admin`. The role is only as trustworthy as the control of the device itself:
  root access, physical access and the integrity of `/etc/systemd/system`. On a device where a person at the screen
  must be told apart from another, this product does not do that.
- A viewer-only screen, an operator panel and an engineer panel are different **installations** (different `--role`
  in the unit), not different logins. Changing the role needs a restart of the service and root.
- The audit trail no longer says who did something, only which role was in force.
- Nothing is stored that could be stolen offline: there is no password file, so the attack that Argon2id's memory cost
  was chosen to slow down no longer exists. The online lockout and the first-boot administrator went with the login.
- The configuration is still protected by an HMAC-SHA-512 signature and the rollback serial; those are unrelated to
  accounts (`docs/CONFIG_INTEGRITY.md`).
- Requirements of the mentor's document that assumed accounts (rows of the requirements matrix in
  `docs/history/ROADMAP_STATUS.md` about login, roles from accounts, lockout and idle logout) are no longer met. They are
  recorded there as removed by decision, not as defects.

## If authentication is wanted again

Do not restore the removed design as it was without the owner's decision on its memory cost (8 MiB per login was eight
times the whole size goal). Options considered, none of them implemented:

1. Argon2id with a small memory cost (for example 128 KiB and more passes). Keeps one modern design; weaker per guess
   against an attacker who copied the user file.
2. PBKDF2-HMAC-SHA-512 (RFC 8018). Needs almost no memory and the HMAC code already exists; weaker against GPUs.
3. A hardware-backed approach on the target (a secure element or the platform's own login), keeping the HMI free of
   password handling.

Whatever is chosen: the hash must be a standard algorithm from a reviewed library, never a home-made one; credentials
are never stored in plain text; and the audit line gets its `user=` field back (the field is still in the format).
