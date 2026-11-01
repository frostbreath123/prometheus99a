# Authentication: removed from the product (2026-10-23)

Status: there is no login, no password, no user store and no password hash in the product. This file replaces
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
  `docs/ROADMAP_STATUS.md` about login, roles from accounts, lockout and idle logout) are no longer met. They are
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
