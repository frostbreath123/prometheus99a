#include "hmi_auth.h"
#include "hmi_authz.h"
#include <stdio.h>
#include <string.h>

#define LOCK_ENTRY 36u          /* name[32] + failures u32 */
#define LOCK_MAX_ENTRIES (HMI_USERS_MAX + 1u)
#define LOCK_FILE_MAX (8u + LOCK_ENTRY * LOCK_MAX_ENTRIES + 4u)
#define JOIN_WAIT_MS 30000u
#define FAIL_SATURATE 1000000u

/* The unknown-user bucket is stored under this name. It cannot collide with a
 * user: '*' is not allowed in user names. */
#define UNKNOWN_NAME "*"

uint32_t hmi_auth_delay_ms(uint32_t failures)
{
    if (failures == 0u) return 0u;
    if (failures < HMI_AUTH_FAILS_BEFORE_LOCK) return HMI_AUTH_GAP_MS;
    uint32_t shift = failures - HMI_AUTH_FAILS_BEFORE_LOCK;
    if (shift >= 5u) return HMI_AUTH_LOCK_MAX_MS; /* 30 s << 5 already exceeds the cap */
    uint32_t ms = HMI_AUTH_LOCK_BASE_MS << shift;
    return ms > HMI_AUTH_LOCK_MAX_MS ? HMI_AUTH_LOCK_MAX_MS : ms;
}

/* ---------------------------------------------------------------- audit */
static const char *safe_user(const char *u)
{
    return (u && (strcmp(u, "?") == 0 || hmi_user_name_valid(u))) ? u : "?";
}

size_t hmi_auth_format_audit(const hmi_auth_audit_rec_t *r, char *line, size_t cap)
{
    if (!r || !line || cap == 0) return 0;
    const char *u = safe_user(r->user);
    unsigned long long t = (unsigned long long)r->t_ms;
    int n;
    switch (r->ev) {
        case HMI_AUTH_EV_LOGIN_OK:
            n = snprintf(line, cap, "%llu LOGIN_OK user=%s role=%s\n", t, u, hmi_role_name(r->role));
            break;
        case HMI_AUTH_EV_LOGIN_FAIL:
            n = snprintf(line, cap, "%llu LOGIN_FAIL user=%s failures=%u\n", t, u, (unsigned)r->failures);
            break;
        case HMI_AUTH_EV_LOCKOUT:
            n = snprintf(line, cap, "%llu LOCKOUT user=%s failures=%u lock_ms=%u\n", t, u, (unsigned)r->failures,
                         (unsigned)r->lock_ms);
            break;
        case HMI_AUTH_EV_LOGOUT:
            n = snprintf(line, cap, "%llu LOGOUT user=%s role=%s\n", t, u, hmi_role_name(r->role));
            break;
        case HMI_AUTH_EV_IDLE_LOGOUT:
            n = snprintf(line, cap, "%llu IDLE_LOGOUT user=%s role=%s\n", t, u, hmi_role_name(r->role));
            break;
        case HMI_AUTH_EV_USER_CREATE:
            n = snprintf(line, cap, "%llu USER_CREATE user=%s role=%s\n", t, u, hmi_role_name(r->role));
            break;
        case HMI_AUTH_EV_USER_CREATE_FAILED:
            n = snprintf(line, cap, "%llu USER_CREATE_FAILED user=%s\n", t, u);
            break;
        case HMI_AUTH_EV_USER_ADD:
            n = snprintf(line, cap, "%llu USER_ADD user=%s role=%s\n", t, u, hmi_role_name(r->role));
            break;
        case HMI_AUTH_EV_USER_REMOVE:
            n = snprintf(line, cap, "%llu USER_REMOVE user=%s\n", t, u);
            break;
        case HMI_AUTH_EV_USER_PASSWORD:
            n = snprintf(line, cap, "%llu USER_PASSWORD user=%s role=%s\n", t, u, hmi_role_name(r->role));
            break;
        default:
            return 0;
    }
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

bool hmi_auth_file_audit(void *user, const hmi_auth_audit_rec_t *rec)
{
    char line[160];
    size_t n = hmi_auth_format_audit(rec, line, sizeof(line));
    if (n == 0 || !user) return false;
    return hmi_fs_append_sync((const char *)user, line, n);
}

/* Returns false only when an audit sink exists and did not durably store it. */
static bool emit(hmi_auth_t *a, hmi_auth_event_t ev, uint64_t now, const char *user, hmi_role_t role,
                 uint32_t failures, uint32_t lock_ms)
{
    if (!a->audit) return true;
    hmi_auth_audit_rec_t r;
    r.t_ms = now;
    r.ev = ev;
    r.user = safe_user(user);
    r.role = role;
    r.failures = failures;
    r.lock_ms = lock_ms;
    return a->audit(a->audit_user, &r);
}

/* ------------------------------------------------------- lockout file */
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

/* Format: "P99L", version 1, count, two zero bytes, count x (name[32], u32), CRC-32. */
static void lock_save(hmi_auth_t *a)
{
    uint8_t buf[LOCK_FILE_MAX];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "P99L", 4);
    buf[4] = 1;
    size_t n = 0;
    for (size_t i = 0; i < a->users.n; i++) {
        if (a->fail[i] == 0u) continue;
        uint8_t *e = buf + 8 + LOCK_ENTRY * n++;
        memcpy(e, a->users.u[i].name, strlen(a->users.u[i].name));
        put32(e + 32, a->fail[i]);
    }
    if (a->fail_unknown) {
        uint8_t *e = buf + 8 + LOCK_ENTRY * n++;
        memcpy(e, UNKNOWN_NAME, 1);
        put32(e + 32, a->fail_unknown);
    }
    buf[5] = (uint8_t)n;
    size_t total = 8 + LOCK_ENTRY * n + 4;
    put32(buf + total - 4, hmi_crc32(buf, total - 4));
    a->lock_persist_failed = !hmi_fs_atomic_write(a->lock_path, buf, total);
}

static void lock_set_all_locked(hmi_auth_t *a, uint64_t now)
{
    for (size_t i = 0; i < a->users.n; i++) {
        a->fail[i] = HMI_AUTH_FAILS_BEFORE_LOCK;
        a->lock_until[i] = now + hmi_auth_delay_ms(HMI_AUTH_FAILS_BEFORE_LOCK);
    }
    a->fail_unknown = HMI_AUTH_FAILS_BEFORE_LOCK;
    a->lock_until_unknown = now + hmi_auth_delay_ms(HMI_AUTH_FAILS_BEFORE_LOCK);
}

static void lock_load(hmi_auth_t *a, uint64_t now)
{
    uint8_t buf[LOCK_FILE_MAX + 1];
    size_t len = 0;
    hmi_fs_read_t r = hmi_fs_read_all(a->lock_path, buf, sizeof(buf), &len);
    if (r == HMI_FS_READ_ABSENT) return; /* never failed, or never written */
    bool ok = r == HMI_FS_READ_OK && len >= 12 && memcmp(buf, "P99L", 4) == 0 && buf[4] == 1 && buf[6] == 0 &&
              buf[7] == 0 && buf[5] <= LOCK_MAX_ENTRIES && len == 8u + LOCK_ENTRY * buf[5] + 4u &&
              get32(buf + len - 4) == hmi_crc32(buf, len - 4);
    if (!ok) { /* damaged counters: assume the worst, as if a lock had just started */
        lock_set_all_locked(a, now);
        return;
    }
    for (size_t k = 0; k < buf[5]; k++) {
        const uint8_t *e = buf + 8 + LOCK_ENTRY * k;
        char name[33];
        memcpy(name, e, 32);
        name[32] = '\0';
        uint32_t f = get32(e + 32);
        if (f > FAIL_SATURATE) f = FAIL_SATURATE;
        if (strcmp(name, UNKNOWN_NAME) == 0) {
            a->fail_unknown = f;
            a->lock_until_unknown = f >= HMI_AUTH_FAILS_BEFORE_LOCK ? now + hmi_auth_delay_ms(f) : 0;
            continue;
        }
        int i = hmi_users_find(&a->users, name);
        if (i < 0) continue; /* the account no longer exists */
        a->fail[i] = f;
        a->lock_until[i] = f >= HMI_AUTH_FAILS_BEFORE_LOCK ? now + hmi_auth_delay_ms(f) : 0;
    }
}

/* ----------------------------------------------------------------- init */
bool hmi_auth_init(hmi_auth_t *a, const hmi_auth_cfg_t *cfg, uint64_t now_ms)
{
    if (!a || !cfg || !cfg->users_path || !cfg->users_path[0]) return false;
    memset(a, 0, sizeof(*a));
    if (snprintf(a->users_path, sizeof(a->users_path), "%s", cfg->users_path) >= (int)sizeof(a->users_path))
        return false;
    if (snprintf(a->lock_path, sizeof(a->lock_path), "%s.lock", a->users_path) >= (int)sizeof(a->lock_path))
        return false;
    a->kdf = cfg->kdf ? *cfg->kdf : hmi_kdf_default_params();
    if (!hmi_kdf_params_valid(&a->kdf)) return false;
    a->idle_ms = cfg->idle_ms ? cfg->idle_ms : HMI_AUTH_IDLE_MS_DEFAULT;
    a->session_user = -1;

    hmi_users_load_t l = hmi_users_load(a->users_path, &a->users, a->store_err, sizeof(a->store_err));
    a->store = l == HMI_USERS_LOAD_OK ? HMI_AUTH_STORE_READY
               : l == HMI_USERS_LOAD_ABSENT ? HMI_AUTH_STORE_NONE : HMI_AUTH_STORE_INVALID;
    if (l != HMI_USERS_LOAD_ABSENT) {
        /* A store that others can write is as good as no store: anyone local
         * could add an account. Refuse it (and block first boot too). A store
         * others can only read is allowed but reported. */
        uint32_t mode = 0600u;
        if (hmi_fs_mode(a->users_path, &mode)) {
            if (mode & 022u) {
                a->store = HMI_AUTH_STORE_INVALID;
                memset(&a->users, 0, sizeof(a->users));
                snprintf(a->store_err, sizeof(a->store_err), "user store is writable by group or others");
            } else if (mode & 044u) {
                a->store_loose_read = true;
            }
        }
    }
    if (a->store == HMI_AUTH_STORE_READY) lock_load(a, now_ms);
    return true;
}

static void job_clear(hmi_auth_t *a)
{
    hmi_wipe(&a->job, sizeof(a->job));
}

void hmi_auth_deinit(hmi_auth_t *a)
{
    if (!a) return;
    if (a->job.active && a->job.threaded) hmi_thread_join(&a->job.thread, JOIN_WAIT_MS);
    hmi_wipe(a, sizeof(*a));
}

void hmi_auth_set_audit(hmi_auth_t *a, hmi_auth_audit_fn fn, void *user)
{
    a->audit = fn;
    a->audit_user = user;
}

hmi_auth_store_t hmi_auth_store_state(const hmi_auth_t *a) { return a->store; }
bool hmi_auth_store_readable_by_others(const hmi_auth_t *a) { return a->store_loose_read; }
const char *hmi_auth_store_error(const hmi_auth_t *a) { return a->store_err; }
bool hmi_auth_busy(const hmi_auth_t *a) { return a->job.active; }

/* -------------------------------------------------------------- session */
static bool session_expired(const hmi_auth_t *a, uint64_t now)
{
    return a->session_active && now >= a->last_activity_ms && now - a->last_activity_ms >= a->idle_ms;
}

hmi_role_t hmi_auth_role(const hmi_auth_t *a, uint64_t now_ms)
{
    if (!a->session_active || session_expired(a, now_ms)) return HMI_ROLE_VIEWER;
    return a->session_role;
}

bool hmi_auth_logged_in(const hmi_auth_t *a, uint64_t now_ms)
{
    return a->session_active && !session_expired(a, now_ms);
}

const char *hmi_auth_user(const hmi_auth_t *a, uint64_t now_ms)
{
    return hmi_auth_logged_in(a, now_ms) ? a->session_name : "";
}

static void session_end(hmi_auth_t *a)
{
    a->session_active = false;
    a->session_user = -1;
    a->session_role = HMI_ROLE_VIEWER;
    hmi_wipe(a->session_name, sizeof(a->session_name));
}

void hmi_auth_note_activity(hmi_auth_t *a, uint64_t now_ms, uint32_t inactive_ms)
{
    if (!a->session_active || session_expired(a, now_ms)) return; /* an expired session is never revived */
    uint64_t at = inactive_ms <= now_ms ? now_ms - inactive_ms : 0u;
    if (at > a->last_activity_ms) a->last_activity_ms = at;
}

bool hmi_auth_tick(hmi_auth_t *a, uint64_t now_ms)
{
    if (!session_expired(a, now_ms)) return false;
    emit(a, HMI_AUTH_EV_IDLE_LOGOUT, now_ms, a->session_name, a->session_role, 0, 0);
    session_end(a);
    return true;
}

void hmi_auth_logout(hmi_auth_t *a, uint64_t now_ms)
{
    if (!a->session_active) return;
    if (!session_expired(a, now_ms)) emit(a, HMI_AUTH_EV_LOGOUT, now_ms, a->session_name, a->session_role, 0, 0);
    else emit(a, HMI_AUTH_EV_IDLE_LOGOUT, now_ms, a->session_name, a->session_role, 0, 0);
    session_end(a);
}

/* -------------------------------------------------------------- lockout */
static uint32_t remaining(uint64_t until, uint64_t now)
{
    if (until <= now) return 0u;
    uint64_t d = until - now;
    return d > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d;
}

uint32_t hmi_auth_wait_ms(const hmi_auth_t *a, const char *name, uint64_t now_ms)
{
    int i = name ? hmi_users_find(&a->users, name) : -1;
    uint32_t lock = remaining(i >= 0 ? a->lock_until[i] : a->lock_until_unknown, now_ms);
    uint32_t gap = remaining(a->gap_until, now_ms);
    return lock > gap ? lock : gap;
}

/* ----------------------------------------------------------------- jobs */
static void job_main(void *arg)
{
    hmi_auth_job_t *j = (hmi_auth_job_t *)arg;
    j->hash_ok = hmi_kdf_hash(&j->params, j->pw, strlen(j->pw), j->salt, j->out);
    hmi_wipe(j->pw, sizeof(j->pw)); /* the password is not needed any more */
    hmi_flag_set(&j->done, 1);
}

/* Starts the prepared job; on a worker thread, or inline when !threaded. */
static bool job_start(hmi_auth_t *a, bool threaded)
{
    a->job.active = true;
    a->job.threaded = threaded;
    hmi_flag_set(&a->job.done, 0);
    if (!threaded) {
        job_main(&a->job);
        return true;
    }
    if (!hmi_thread_start(&a->job.thread, job_main, &a->job)) {
        job_clear(a);
        return false;
    }
    return true;
}

static hmi_login_info_t login_info(hmi_login_result_t r, uint32_t wait, uint32_t fails)
{
    hmi_login_info_t i;
    i.result = r;
    i.wait_ms = wait;
    i.failures = fails;
    return i;
}

static hmi_login_info_t login_start(hmi_auth_t *a, const char *name, const char *pw, uint64_t now, bool threaded)
{
    if (!name || !pw) return login_info(HMI_LOGIN_INPUT, 0, 0);
    if (a->job.active) return login_info(HMI_LOGIN_BUSY, 0, 0);
    hmi_auth_tick(a, now); /* an idled-out session ends here, so a new login is allowed */
    if (a->session_active) return login_info(HMI_LOGIN_SESSION_ACTIVE, 0, 0);
    if (a->store == HMI_AUTH_STORE_NONE) return login_info(HMI_LOGIN_NO_STORE, 0, 0);
    if (a->store != HMI_AUTH_STORE_READY) return login_info(HMI_LOGIN_STORE_INVALID, 0, 0);
    if (strlen(pw) > HMI_PASSWORD_MAX) return login_info(HMI_LOGIN_INPUT, 0, 0);

    int idx = hmi_users_find(&a->users, name);
    uint32_t lock = remaining(idx >= 0 ? a->lock_until[idx] : a->lock_until_unknown, now);
    uint32_t gap = remaining(a->gap_until, now);
    if (lock > 0u && lock >= gap) return login_info(HMI_LOGIN_LOCKED, lock, 0);
    if (gap > 0u) return login_info(HMI_LOGIN_WAIT, gap, 0);

    hmi_auth_job_t *j = &a->job;
    memset(j, 0, sizeof(*j));
    j->kind = HMI_AUTH_JOB_LOGIN;
    j->user_index = idx;
    /* An unknown name still pays for a full hash (zero salt, default cost). */
    j->params = idx >= 0 ? a->users.u[idx].kdf : a->kdf;
    if (idx >= 0) {
        memcpy(j->salt, a->users.u[idx].salt, HMI_KDF_SALT_LEN);
        memcpy(j->expected, a->users.u[idx].hash, HMI_KDF_HASH_LEN);
        snprintf(j->name, sizeof(j->name), "%s", a->users.u[idx].name);
    }
    memcpy(j->pw, pw, strlen(pw) + 1u);
    if (!job_start(a, threaded)) return login_info(HMI_LOGIN_ERROR, 0, 0);
    return login_info(HMI_LOGIN_PENDING, 0, 0);
}

hmi_login_info_t hmi_auth_login_begin(hmi_auth_t *a, const char *name, const char *pw, uint64_t now_ms)
{
    return login_start(a, name, pw, now_ms, true);
}

static hmi_login_info_t login_complete(hmi_auth_t *a, uint64_t now)
{
    hmi_auth_job_t *j = &a->job;
    hmi_login_info_t out;
    if (!j->hash_ok) return login_info(HMI_LOGIN_ERROR, 0, 0);

    bool equal = hmi_kdf_equal(j->out, j->expected);
    bool ok = j->user_index >= 0 && equal;
    int idx = j->user_index;

    if (ok) {
        hmi_role_t role = a->users.u[idx].role;
        /* Recorded BEFORE the session exists; no record, no session. */
        if (!emit(a, HMI_AUTH_EV_LOGIN_OK, now, a->users.u[idx].name, role, 0, 0))
            return login_info(HMI_LOGIN_AUDIT_FAILED, 0, a->fail[idx]);
        a->session_active = true;
        a->session_user = idx;
        a->session_role = role;
        snprintf(a->session_name, sizeof(a->session_name), "%s", a->users.u[idx].name);
        a->last_activity_ms = now;
        if (a->fail[idx] != 0u) {
            a->fail[idx] = 0u;
            a->lock_until[idx] = 0u;
            lock_save(a);
        }
        return login_info(HMI_LOGIN_OK, 0, 0);
    }

    uint32_t *fail = idx >= 0 ? &a->fail[idx] : &a->fail_unknown;
    uint64_t *until = idx >= 0 ? &a->lock_until[idx] : &a->lock_until_unknown;
    if (*fail < FAIL_SATURATE) (*fail)++;
    uint32_t delay = hmi_auth_delay_ms(*fail);
    *until = *fail >= HMI_AUTH_FAILS_BEFORE_LOCK ? now + delay : 0u;
    a->gap_until = now + HMI_AUTH_GAP_MS;
    const char *who = idx >= 0 ? a->users.u[idx].name : "?";
    emit(a, HMI_AUTH_EV_LOGIN_FAIL, now, who, HMI_ROLE_VIEWER, *fail, 0);
    if (*fail >= HMI_AUTH_FAILS_BEFORE_LOCK) emit(a, HMI_AUTH_EV_LOCKOUT, now, who, HMI_ROLE_VIEWER, *fail, delay);
    lock_save(a);
    out = login_info(HMI_LOGIN_BAD, delay, *fail);
    return out;
}

/* ---------------------------------------------------------------- setup */
static hmi_setup_result_t setup_start(hmi_auth_t *a, const char *name, const char *pw, uint64_t now, bool threaded)
{
    (void)now;
    if (!name || !pw) return HMI_SETUP_NAME;
    if (a->job.active) return HMI_SETUP_BUSY;
    if (a->store != HMI_AUTH_STORE_NONE) return HMI_SETUP_NOT_FIRST_BOOT;
    if (!hmi_user_name_valid(name)) return HMI_SETUP_NAME;
    a->setup_reason = "";
    if (!hmi_password_policy(name, pw, &a->setup_reason)) return HMI_SETUP_PASSWORD;

    hmi_auth_job_t *j = &a->job;
    memset(j, 0, sizeof(*j));
    j->kind = HMI_AUTH_JOB_SETUP;
    j->params = a->kdf;
    j->role = HMI_ROLE_ADMIN;
    j->user_index = -1;
    snprintf(j->name, sizeof(j->name), "%s", name);
    if (!hmi_random_bytes(j->salt, sizeof(j->salt))) {
        job_clear(a);
        return HMI_SETUP_ERROR;
    }
    memcpy(j->pw, pw, strlen(pw) + 1u);
    if (!job_start(a, threaded)) return HMI_SETUP_ERROR;
    return HMI_SETUP_PENDING;
}

hmi_setup_result_t hmi_auth_setup_begin(hmi_auth_t *a, const char *name, const char *pw, uint64_t now_ms)
{
    return setup_start(a, name, pw, now_ms, true);
}

const char *hmi_auth_setup_reason(const hmi_auth_t *a) { return a->setup_reason ? a->setup_reason : ""; }

static hmi_setup_result_t setup_complete(hmi_auth_t *a, uint64_t now)
{
    hmi_auth_job_t *j = &a->job;
    if (!j->hash_ok) return HMI_SETUP_ERROR;

    hmi_user_t u;
    memset(&u, 0, sizeof(u));
    snprintf(u.name, sizeof(u.name), "%s", j->name);
    u.role = j->role;
    u.kdf = j->params;
    memcpy(u.salt, j->salt, HMI_KDF_SALT_LEN);
    memcpy(u.hash, j->out, HMI_KDF_HASH_LEN);

    /* Never overwrite a store that appeared since the check at begin(). A
     * small window between this read and the rename remains; it needs a
     * second writer on the same file, which is outside this module's reach. */
    uint8_t probe[1];
    size_t plen = 0;
    if (hmi_fs_read_all(a->users_path, probe, sizeof(probe), &plen) != HMI_FS_READ_ABSENT)
        return HMI_SETUP_NOT_FIRST_BOOT;

    if (!emit(a, HMI_AUTH_EV_USER_CREATE, now, u.name, u.role, 0, 0)) return HMI_SETUP_AUDIT_FAILED;

    hmi_users_t s;
    memset(&s, 0, sizeof(s));
    if (hmi_users_add(&s, &u) != HMI_USERS_OK || !hmi_users_save(a->users_path, &s)) {
        emit(a, HMI_AUTH_EV_USER_CREATE_FAILED, now, u.name, u.role, 0, 0);
        return HMI_SETUP_SAVE_FAILED;
    }
    a->users = s;
    a->store = HMI_AUTH_STORE_READY;
    a->store_err[0] = '\0';
    memset(a->fail, 0, sizeof(a->fail));
    memset(a->lock_until, 0, sizeof(a->lock_until));
    return HMI_SETUP_OK;
}

/* ----------------------------------------------------------------- poll */
bool hmi_auth_poll(hmi_auth_t *a, uint64_t now_ms, hmi_auth_done_t *out)
{
    if (!a->job.active) return false;
    if (a->job.threaded) {
        if (!hmi_flag_get(&a->job.done)) return false;
        hmi_thread_join(&a->job.thread, JOIN_WAIT_MS);
    }
    hmi_auth_done_t d;
    memset(&d, 0, sizeof(d));
    d.kind = a->job.kind;
    if (a->job.kind == HMI_AUTH_JOB_LOGIN) d.login = login_complete(a, now_ms);
    else d.setup = setup_complete(a, now_ms);
    job_clear(a);
    if (out) *out = d;
    return true;
}

hmi_login_info_t hmi_auth_login(hmi_auth_t *a, const char *name, const char *pw, uint64_t now_ms)
{
    hmi_login_info_t b = login_start(a, name, pw, now_ms, false);
    if (b.result != HMI_LOGIN_PENDING) return b;
    hmi_auth_done_t d;
    hmi_auth_poll(a, now_ms, &d);
    return d.login;
}

hmi_setup_result_t hmi_auth_setup(hmi_auth_t *a, const char *name, const char *pw, uint64_t now_ms)
{
    hmi_setup_result_t b = setup_start(a, name, pw, now_ms, false);
    if (b != HMI_SETUP_PENDING) return b;
    hmi_auth_done_t d;
    hmi_auth_poll(a, now_ms, &d);
    return d.setup;
}
