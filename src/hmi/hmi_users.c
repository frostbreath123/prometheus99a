#include "hmi_users.h"
#include "hmi_platform.h"
#include "hmi_authz.h"
#include <stdio.h>
#include <string.h>

#define NAME_FIELD 32
#define REC_SIZE 96u
#define HDR_SIZE 8u

uint32_t hmi_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool hmi_user_name_valid(const char *name)
{
    if (!name) return false;
    size_t n = 0;
    for (; name[n]; n++) {
        char c = name[n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok || n >= HMI_USER_NAME_MAX) return false;
    }
    return n >= HMI_USER_NAME_MIN;
}

bool hmi_password_policy(const char *name, const char *pw, const char **reason)
{
    const char *dummy;
    if (!reason) reason = &dummy;
    if (!pw) {
        *reason = "password missing";
        return false;
    }
    size_t n = 0;
    for (; pw[n]; n++) {
        unsigned char c = (unsigned char)pw[n];
        if (c < 0x20 || c > 0x7E) {
            *reason = "password may only use printable ASCII";
            return false;
        }
        if (n >= HMI_PASSWORD_MAX) {
            *reason = "password is too long (64 characters at most)";
            return false;
        }
    }
    if (n < HMI_PASSWORD_MIN) {
        *reason = "password is too short (8 characters at least)";
        return false;
    }
    if (name && strcmp(name, pw) == 0) {
        *reason = "password must differ from the user name";
        return false;
    }
    *reason = "";
    return true;
}

static bool role_ok(hmi_role_t r) { return r >= HMI_ROLE_OPERATOR && r <= HMI_ROLE_ADMIN; }

static bool has_admin(const hmi_users_t *s)
{
    for (size_t i = 0; i < s->n; i++)
        if (s->u[i].role == HMI_ROLE_ADMIN) return true;
    return false;
}

size_t hmi_users_serialize(const hmi_users_t *s, uint8_t *buf, size_t cap)
{
    if (!s || !buf || s->n < 1 || s->n > HMI_USERS_MAX) return 0;
    size_t total = HDR_SIZE + REC_SIZE * s->n + 4u;
    if (cap < total) return 0;
    memset(buf, 0, total);
    memcpy(buf, "P99U", 4);
    buf[4] = 1;
    buf[5] = (uint8_t)s->n;
    for (size_t i = 0; i < s->n; i++) {
        const hmi_user_t *u = &s->u[i];
        if (!hmi_user_name_valid(u->name) || !role_ok(u->role) || !hmi_kdf_params_valid(&u->kdf)) return 0;
        uint8_t *r = buf + HDR_SIZE + REC_SIZE * i;
        memcpy(r, u->name, strlen(u->name));
        r[NAME_FIELD] = (uint8_t)u->role;
        put32(r + 36, u->kdf.t);
        put32(r + 40, u->kdf.m_kib);
        put32(r + 44, u->kdf.p);
        memcpy(r + 48, u->salt, HMI_KDF_SALT_LEN);
        memcpy(r + 64, u->hash, HMI_KDF_HASH_LEN);
    }
    put32(buf + total - 4u, hmi_crc32(buf, total - 4u));
    return total;
}

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err && cap) snprintf(err, cap, "%s", msg);
}

bool hmi_users_parse(const uint8_t *buf, size_t len, hmi_users_t *out, char *err, size_t err_cap)
{
    hmi_users_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (out) memset(out, 0, sizeof(*out));
    if (!buf || !out || len < HDR_SIZE + REC_SIZE + 4u) {
        set_err(err, err_cap, "user store too short");
        return false;
    }
    if (memcmp(buf, "P99U", 4) != 0) {
        set_err(err, err_cap, "user store has the wrong magic");
        return false;
    }
    if (buf[4] != 1) {
        set_err(err, err_cap, "user store has an unsupported version");
        return false;
    }
    if (buf[6] != 0 || buf[7] != 0) {
        set_err(err, err_cap, "user store header has nonzero reserved bytes");
        return false;
    }
    size_t count = buf[5];
    if (count < 1 || count > HMI_USERS_MAX) {
        set_err(err, err_cap, "user store has an invalid account count");
        return false;
    }
    size_t total = HDR_SIZE + REC_SIZE * count + 4u;
    if (len != total) {
        set_err(err, err_cap, "user store has the wrong length");
        return false;
    }
    if (get32(buf + total - 4u) != hmi_crc32(buf, total - 4u)) {
        set_err(err, err_cap, "user store checksum mismatch");
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        const uint8_t *r = buf + HDR_SIZE + REC_SIZE * i;
        hmi_user_t *u = &tmp.u[i];
        size_t nl = 0;
        while (nl < NAME_FIELD && r[nl]) nl++;
        for (size_t k = nl; k < NAME_FIELD; k++) {
            if (r[k]) {
                set_err(err, err_cap, "user store has a malformed name field");
                return false;
            }
        }
        if (nl > HMI_USER_NAME_MAX) {
            set_err(err, err_cap, "user store has a name that is too long");
            return false;
        }
        memcpy(u->name, r, nl);
        u->name[nl] = '\0';
        if (!hmi_user_name_valid(u->name)) {
            set_err(err, err_cap, "user store has an invalid user name");
            return false;
        }
        if (r[33] || r[34] || r[35]) {
            set_err(err, err_cap, "user store record has nonzero reserved bytes");
            return false;
        }
        if (r[NAME_FIELD] < HMI_ROLE_OPERATOR || r[NAME_FIELD] > HMI_ROLE_ADMIN) {
            set_err(err, err_cap, "user store has an invalid role");
            return false;
        }
        u->role = (hmi_role_t)r[NAME_FIELD];
        u->kdf.t = get32(r + 36);
        u->kdf.m_kib = get32(r + 40);
        u->kdf.p = get32(r + 44);
        if (!hmi_kdf_params_valid(&u->kdf)) {
            set_err(err, err_cap, "user store has KDF parameters outside the allowed range");
            return false;
        }
        memcpy(u->salt, r + 48, HMI_KDF_SALT_LEN);
        memcpy(u->hash, r + 64, HMI_KDF_HASH_LEN);
        for (size_t j = 0; j < i; j++) {
            if (strcmp(tmp.u[j].name, u->name) == 0) {
                set_err(err, err_cap, "user store has a duplicate user name");
                return false;
            }
        }
    }
    tmp.n = count;
    if (!has_admin(&tmp)) {
        set_err(err, err_cap, "user store has no administrator");
        return false;
    }
    *out = tmp;
    hmi_wipe(&tmp, sizeof(tmp));
    return true;
}

hmi_users_load_t hmi_users_load(const char *path, hmi_users_t *out, char *err, size_t err_cap)
{
    uint8_t buf[HMI_USERS_FILE_MAX];
    size_t len = 0;
    if (out) memset(out, 0, sizeof(*out));
    hmi_fs_read_t r = hmi_fs_read_all(path, buf, sizeof(buf), &len);
    if (r == HMI_FS_READ_ABSENT) {
        set_err(err, err_cap, "no user store");
        return HMI_USERS_LOAD_ABSENT;
    }
    if (r == HMI_FS_READ_TOO_BIG) {
        set_err(err, err_cap, "user store is larger than any valid store");
        return HMI_USERS_LOAD_INVALID;
    }
    if (r != HMI_FS_READ_OK) {
        set_err(err, err_cap, "user store cannot be read");
        return HMI_USERS_LOAD_INVALID;
    }
    return hmi_users_parse(buf, len, out, err, err_cap) ? HMI_USERS_LOAD_OK : HMI_USERS_LOAD_INVALID;
}

bool hmi_users_save(const char *path, const hmi_users_t *s)
{
    uint8_t buf[HMI_USERS_FILE_MAX];
    if (!s || !has_admin(s)) return false;
    size_t n = hmi_users_serialize(s, buf, sizeof(buf));
    if (n == 0) return false;
    bool ok = hmi_fs_atomic_write(path, buf, n);
    hmi_wipe(buf, sizeof(buf));
    return ok;
}

const char *hmi_users_err_text(hmi_users_err_t e)
{
    switch (e) {
        case HMI_USERS_OK: return "ok";
        case HMI_USERS_ERR_NAME: return "user name must be 3 to 24 characters of a-z 0-9 _ . -";
        case HMI_USERS_ERR_PASSWORD: return "password does not meet the policy";
        case HMI_USERS_ERR_ROLE: return "role must be operator, engineer or admin";
        case HMI_USERS_ERR_EXISTS: return "user already exists";
        case HMI_USERS_ERR_NOT_FOUND: return "no such user";
        case HMI_USERS_ERR_FULL: return "the user store is full";
        case HMI_USERS_ERR_LAST_ADMIN: return "that would leave no administrator";
        case HMI_USERS_ERR_INTERNAL: return "internal error (random source, memory or KDF)";
    }
    return "unknown error";
}

hmi_users_err_t hmi_user_make(hmi_user_t *out, const char *name, hmi_role_t role, const char *pw,
                              const hmi_kdf_params_t *kdf, const char **reason)
{
    const char *dummy;
    if (!reason) reason = &dummy;
    *reason = "";
    if (!out) return HMI_USERS_ERR_INTERNAL;
    if (!hmi_user_name_valid(name)) return HMI_USERS_ERR_NAME;
    if (!role_ok(role)) return HMI_USERS_ERR_ROLE;
    if (!hmi_password_policy(name, pw, reason)) return HMI_USERS_ERR_PASSWORD;
    hmi_kdf_params_t p = kdf ? *kdf : hmi_kdf_default_params();
    if (!hmi_kdf_params_valid(&p)) return HMI_USERS_ERR_INTERNAL;
    hmi_user_t u;
    memset(&u, 0, sizeof(u));
    snprintf(u.name, sizeof(u.name), "%s", name);
    u.role = role;
    u.kdf = p;
    if (!hmi_random_bytes(u.salt, sizeof(u.salt))) return HMI_USERS_ERR_INTERNAL;
    if (!hmi_kdf_hash(&p, pw, strlen(pw), u.salt, u.hash)) return HMI_USERS_ERR_INTERNAL;
    *out = u;
    return HMI_USERS_OK;
}

int hmi_users_find(const hmi_users_t *s, const char *name)
{
    if (!s || !name) return -1;
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->u[i].name, name) == 0) return (int)i;
    return -1;
}

hmi_users_err_t hmi_users_add(hmi_users_t *s, const hmi_user_t *u)
{
    if (!s || !u || !hmi_user_name_valid(u->name)) return HMI_USERS_ERR_NAME;
    if (!role_ok(u->role)) return HMI_USERS_ERR_ROLE;
    if (hmi_users_find(s, u->name) >= 0) return HMI_USERS_ERR_EXISTS;
    if (s->n >= HMI_USERS_MAX) return HMI_USERS_ERR_FULL;
    s->u[s->n++] = *u;
    return HMI_USERS_OK;
}

hmi_users_err_t hmi_users_remove(hmi_users_t *s, const char *name)
{
    int i = hmi_users_find(s, name);
    if (i < 0) return HMI_USERS_ERR_NOT_FOUND;
    hmi_users_t after = *s;
    for (size_t k = (size_t)i; k + 1 < after.n; k++) after.u[k] = after.u[k + 1];
    hmi_wipe(&after.u[after.n - 1], sizeof(after.u[0]));
    after.n--;
    if (after.n == 0 || !has_admin(&after)) return HMI_USERS_ERR_LAST_ADMIN;
    *s = after;
    return HMI_USERS_OK;
}

hmi_users_err_t hmi_users_replace(hmi_users_t *s, const hmi_user_t *u)
{
    if (!s || !u) return HMI_USERS_ERR_INTERNAL;
    int i = hmi_users_find(s, u->name);
    if (i < 0) return HMI_USERS_ERR_NOT_FOUND;
    if (!role_ok(u->role)) return HMI_USERS_ERR_ROLE;
    hmi_users_t after = *s;
    after.u[i] = *u;
    if (!has_admin(&after)) return HMI_USERS_ERR_LAST_ADMIN; /* demoting the only admin */
    *s = after;
    return HMI_USERS_OK;
}
