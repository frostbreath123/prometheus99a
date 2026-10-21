/**
 * @file hmi_users.h
 * @brief The user store: accounts with salted Argon2id password hashes.
 *
 * File format (version 1, little endian, exactly 8 + 96 * count + 4 bytes):
 *   header   magic "P99U", version (1), count (1..16), two zero bytes
 *   record   name[32] zero padded, role (1..3), three zero bytes,
 *            t (u32), m_kib (u32), p (u32), salt[16], hash[32]
 *   trailer  CRC-32 over everything before it
 *
 * Parsing is strict: wrong magic or version, nonzero reserved bytes, a bad or
 * duplicate name, a role outside Operator..Admin, KDF parameters outside the
 * bounds of hmi_kdf.h, no administrator, a wrong length or a wrong CRC all make
 * the whole file invalid.
 *
 * The CRC detects corruption and careless edits. It is NOT a defence against
 * an attacker who can write the file: such an attacker can recompute the CRC
 * or replace a hash. Protecting the file from that is a matter of file
 * ownership and mode (0600, owned by the service user), not of this format.
 *
 * Viewer is the logged-out state and has no account.
 */
#ifndef HMI_USERS_H
#define HMI_USERS_H

#include "hmi_types.h"
#include "hmi_kdf.h"

#define HMI_USERS_MAX 16
#define HMI_USER_NAME_MIN 3
#define HMI_USER_NAME_MAX 24
#define HMI_PASSWORD_MIN 8
#define HMI_PASSWORD_MAX 64
#define HMI_USERS_FILE_MAX (8u + 96u * HMI_USERS_MAX + 4u)

typedef struct {
    char name[HMI_USER_NAME_MAX + 1];
    hmi_role_t role;               /* Operator, Engineer or Admin */
    hmi_kdf_params_t kdf;
    uint8_t salt[HMI_KDF_SALT_LEN];
    uint8_t hash[HMI_KDF_HASH_LEN];
} hmi_user_t;

typedef struct {
    hmi_user_t u[HMI_USERS_MAX];
    size_t n;
} hmi_users_t;

/* CRC-32 (IEEE, as zlib). Also used for the lockout file. */
uint32_t hmi_crc32(const uint8_t *p, size_t n);

/* 3..24 characters from [a-z0-9_.-]. Keeps names safe to write into the audit
 * file (no spaces, no newlines) and free of look-alike case variants. */
bool hmi_user_name_valid(const char *name);

/* Password policy: HMI_PASSWORD_MIN..HMI_PASSWORD_MAX bytes, printable ASCII
 * (0x20..0x7E) so it can be typed on the on-screen keyboard, and not equal to
 * the user name. Returns false and a one-line reason (a string literal). */
bool hmi_password_policy(const char *name, const char *pw, const char **reason);

/* ---- serialisation ---- */
size_t hmi_users_serialize(const hmi_users_t *s, uint8_t *buf, size_t cap); /* 0 on failure */
bool hmi_users_parse(const uint8_t *buf, size_t len, hmi_users_t *out, char *err, size_t err_cap);

/* ---- file ---- */
typedef enum {
    HMI_USERS_LOAD_OK = 0,
    HMI_USERS_LOAD_ABSENT,    /* no file: first boot */
    HMI_USERS_LOAD_INVALID    /* a file exists but cannot be trusted or read */
} hmi_users_load_t;

/* ABSENT and INVALID are different on purpose: only an absent file allows
 * first-boot creation of an administrator. *out is cleared on every failure. */
hmi_users_load_t hmi_users_load(const char *path, hmi_users_t *out, char *err, size_t err_cap);
/* Atomic replace (temp file, flush, rename). The platform layer creates it
 * with mode 0600 on POSIX. */
bool hmi_users_save(const char *path, const hmi_users_t *s);

/* ---- editing. hmi_user_make hashes the password, which takes the full KDF time. ---- */
typedef enum {
    HMI_USERS_OK = 0,
    HMI_USERS_ERR_NAME,       /* name invalid */
    HMI_USERS_ERR_PASSWORD,   /* password policy */
    HMI_USERS_ERR_ROLE,       /* role not Operator..Admin */
    HMI_USERS_ERR_EXISTS,
    HMI_USERS_ERR_NOT_FOUND,
    HMI_USERS_ERR_FULL,
    HMI_USERS_ERR_LAST_ADMIN, /* would leave no administrator */
    HMI_USERS_ERR_INTERNAL    /* random source, memory or KDF parameters failed */
} hmi_users_err_t;

const char *hmi_users_err_text(hmi_users_err_t e);

/* Fills one record: new random salt and a hash of pw. Does not touch a store.
 * kdf may be NULL for the defaults. reason (optional) receives the password
 * policy text on HMI_USERS_ERR_PASSWORD. */
hmi_users_err_t hmi_user_make(hmi_user_t *out, const char *name, hmi_role_t role, const char *pw,
                              const hmi_kdf_params_t *kdf, const char **reason);

int hmi_users_find(const hmi_users_t *s, const char *name); /* index or -1 */
hmi_users_err_t hmi_users_add(hmi_users_t *s, const hmi_user_t *u);
hmi_users_err_t hmi_users_remove(hmi_users_t *s, const char *name);
hmi_users_err_t hmi_users_replace(hmi_users_t *s, const hmi_user_t *u); /* same name, new password or role */

#endif /* HMI_USERS_H */
