/**
 * @file hmi_auth.h
 * @brief Login, lockout, idle logout and first-boot administrator creation.
 *
 * Owned by one thread (the UI thread). Only the password hash computation may
 * run on a worker thread, and only through hmi_auth_login_begin /
 * hmi_auth_setup_begin, which keep every other piece of state on the caller's
 * thread. Every function takes the monotonic time as a parameter, so tests use
 * a fake clock exactly as they do for hmi_comms.
 *
 * What it guarantees
 *  - hmi_auth_role() is the only source of the role the UI passes to the core
 *    (hmi_alarm_ack*, hmi_writes_submit). It returns Viewer unless a session
 *    is active and has not idled out.
 *  - Failed logins are delayed: 1 s gap after each failure, then a lock of
 *    30 s after the 5th consecutive failure that doubles on each further
 *    failure up to 15 minutes. The failure counter is stored in a file next to
 *    the user store and survives a restart. A restart cannot shorten a lock
 *    (the monotonic clock is not stored, so the lock timer restarts in full).
 *  - An unknown user name runs the same password hash and counts against a
 *    shared "unknown" counter, so it is neither faster nor free.
 *  - A granted login is written to the audit trail BEFORE the session starts
 *    and is refused if that write fails (same rule as alarm acknowledgements).
 *    Failures, logouts, locks are audited best effort.
 *  - Passwords are never logged and the working copies are wiped.
 *
 * What it does not do: protect the user file from someone who can write it
 * (see hmi_users.h) or work against an attacker who controls the process.
 */
#ifndef HMI_AUTH_H
#define HMI_AUTH_H

#include "hmi_users.h"
#include "hmi_platform.h"

#define HMI_AUTH_IDLE_MS_DEFAULT 300000u   /* 5 minutes without touch */
#define HMI_AUTH_FAILS_BEFORE_LOCK 5u
#define HMI_AUTH_GAP_MS 1000u              /* minimum gap after any failed attempt */
#define HMI_AUTH_LOCK_BASE_MS 30000u       /* lock after the 5th failure */
#define HMI_AUTH_LOCK_MAX_MS 900000u       /* 15 minutes */

/* Pure: how long the user must wait after `failures` consecutive failures
 * (1 s for 1..4, then 30 s doubling per failure, capped). 0 for 0 failures. */
uint32_t hmi_auth_delay_ms(uint32_t failures);

typedef enum {
    HMI_AUTH_EV_LOGIN_OK = 0,
    HMI_AUTH_EV_LOGIN_FAIL,
    HMI_AUTH_EV_LOCKOUT,
    HMI_AUTH_EV_LOGOUT,
    HMI_AUTH_EV_IDLE_LOGOUT,
    HMI_AUTH_EV_USER_CREATE,          /* first administrator, created on screen or with the tool */
    HMI_AUTH_EV_USER_CREATE_FAILED,
    HMI_AUTH_EV_USER_ADD,             /* offline tool: account added */
    HMI_AUTH_EV_USER_REMOVE,
    HMI_AUTH_EV_USER_PASSWORD         /* offline tool: password or role changed */
} hmi_auth_event_t;

typedef struct {
    uint64_t t_ms;
    hmi_auth_event_t ev;
    const char *user;     /* a valid user name, or "?" for an unknown or malformed name */
    hmi_role_t role;      /* meaningful for LOGIN_OK, LOGOUT, IDLE_LOGOUT, USER_CREATE */
    uint32_t failures;    /* meaningful for LOGIN_FAIL and LOCKOUT */
    uint32_t lock_ms;     /* meaningful for LOCKOUT */
} hmi_auth_audit_rec_t;

/* Return true only when the record is durably stored. */
typedef bool (*hmi_auth_audit_fn)(void *user, const hmi_auth_audit_rec_t *rec);

/* One line, no newline inside, ends with '\n'. Returns the length, 0 on overflow. */
size_t hmi_auth_format_audit(const hmi_auth_audit_rec_t *rec, char *line, size_t cap);
/* Ready-made sink: `user` is the audit file path (const char *). */
bool hmi_auth_file_audit(void *user, const hmi_auth_audit_rec_t *rec);

typedef enum {
    HMI_AUTH_STORE_NONE = 0,   /* no user file: first boot */
    HMI_AUTH_STORE_READY,
    HMI_AUTH_STORE_INVALID     /* file present but untrusted: login disabled, no first-boot creation */
} hmi_auth_store_t;

typedef struct {
    const char *users_path;           /* required; lockout counters live in "<users_path>.lock" */
    uint32_t idle_ms;                 /* 0 selects HMI_AUTH_IDLE_MS_DEFAULT */
    const hmi_kdf_params_t *kdf;      /* NULL selects the defaults; tests pass a cheap cost */
} hmi_auth_cfg_t;

typedef enum {
    HMI_LOGIN_OK = 0,
    HMI_LOGIN_PENDING,         /* begin() accepted the attempt; poll for the result */
    HMI_LOGIN_BAD,             /* wrong user or password */
    HMI_LOGIN_LOCKED,          /* locked after repeated failures; wait_ms says how long */
    HMI_LOGIN_WAIT,            /* too soon after the last failure; wait_ms says how long */
    HMI_LOGIN_BUSY,            /* another attempt is being computed */
    HMI_LOGIN_NO_STORE,        /* first boot: create the administrator first */
    HMI_LOGIN_STORE_INVALID,
    HMI_LOGIN_SESSION_ACTIVE,  /* log out first */
    HMI_LOGIN_AUDIT_FAILED,    /* password was right, but the trail could not be written: refused */
    HMI_LOGIN_INPUT,           /* password longer than the policy allows (not counted, not hashed) */
    HMI_LOGIN_ERROR            /* memory, thread or KDF failure */
} hmi_login_result_t;

typedef enum {
    HMI_SETUP_OK = 0,
    HMI_SETUP_PENDING,
    HMI_SETUP_NOT_FIRST_BOOT,  /* a store already exists (valid or not) */
    HMI_SETUP_NAME,
    HMI_SETUP_PASSWORD,        /* see hmi_auth_setup_reason() */
    HMI_SETUP_BUSY,
    HMI_SETUP_AUDIT_FAILED,
    HMI_SETUP_SAVE_FAILED,
    HMI_SETUP_ERROR
} hmi_setup_result_t;

typedef struct {
    hmi_login_result_t result;
    uint32_t wait_ms;          /* for LOCKED and WAIT */
    uint32_t failures;         /* consecutive failures for this user after this attempt */
} hmi_login_info_t;

typedef enum { HMI_AUTH_JOB_NONE = 0, HMI_AUTH_JOB_LOGIN, HMI_AUTH_JOB_SETUP } hmi_auth_job_kind_t;

typedef struct {
    hmi_auth_job_kind_t kind;
    hmi_login_info_t login;      /* valid when kind == HMI_AUTH_JOB_LOGIN */
    hmi_setup_result_t setup;    /* valid when kind == HMI_AUTH_JOB_SETUP */
} hmi_auth_done_t;

typedef struct {
    hmi_auth_job_kind_t kind;
    hmi_kdf_params_t params;
    uint8_t salt[HMI_KDF_SALT_LEN];
    char pw[HMI_PASSWORD_MAX + 1];
    uint8_t out[HMI_KDF_HASH_LEN];
    int user_index;              /* login: index, or -1 for the unknown bucket */
    char name[HMI_USER_NAME_MAX + 1];
    hmi_role_t role;             /* setup */
    uint8_t expected[HMI_KDF_HASH_LEN];
    bool hash_ok;
    bool active;                 /* a job is running or waiting to be polled */
    hmi_thread_t thread;
    bool threaded;
    hmi_atomic_flag_t done;
} hmi_auth_job_t;

typedef struct {
    char users_path[256];
    char lock_path[264];
    hmi_users_t users;
    hmi_auth_store_t store;
    char store_err[120];
    bool store_loose_read;       /* the store can be read by group or others (warning only) */
    hmi_kdf_params_t kdf;
    uint32_t idle_ms;

    uint32_t fail[HMI_USERS_MAX];
    uint64_t lock_until[HMI_USERS_MAX];
    uint32_t fail_unknown;
    uint64_t lock_until_unknown;
    uint64_t gap_until;          /* global minimum gap after any failure */
    bool lock_persist_failed;    /* the counter file could not be written (still enforced in memory) */

    bool session_active;
    int session_user;
    hmi_role_t session_role;
    char session_name[HMI_USER_NAME_MAX + 1];
    uint64_t last_activity_ms;
    const char *setup_reason;    /* literal from the password policy */

    hmi_auth_audit_fn audit;
    void *audit_user;
    hmi_auth_job_t job;
} hmi_auth_t;

/* Loads the user store and the lockout counters. Returns false only when the
 * arguments are unusable; a missing or invalid store is a state, not a failure. */
bool hmi_auth_init(hmi_auth_t *a, const hmi_auth_cfg_t *cfg, uint64_t now_ms);
/* Waits for a running hash job (bounded), then wipes the state. */
void hmi_auth_deinit(hmi_auth_t *a);
void hmi_auth_set_audit(hmi_auth_t *a, hmi_auth_audit_fn fn, void *user);

hmi_auth_store_t hmi_auth_store_state(const hmi_auth_t *a);
const char *hmi_auth_store_error(const hmi_auth_t *a);
/* True when the store file is readable (not writable) by group or others. */
bool hmi_auth_store_readable_by_others(const hmi_auth_t *a);

/* ---- login ---- */
/* Synchronous: blocks for the password hash time. For tests and tools. */
hmi_login_info_t hmi_auth_login(hmi_auth_t *a, const char *name, const char *pw, uint64_t now_ms);
/* Asynchronous: returns HMI_LOGIN_PENDING when accepted, or the refusal. The
 * password is copied before returning; the caller wipes its own buffer. */
hmi_login_info_t hmi_auth_login_begin(hmi_auth_t *a, const char *name, const char *pw, uint64_t now_ms);

/* ---- first boot ---- */
/* Creates the first administrator. Only while the store is absent. */
hmi_setup_result_t hmi_auth_setup(hmi_auth_t *a, const char *name, const char *pw, uint64_t now_ms);
hmi_setup_result_t hmi_auth_setup_begin(hmi_auth_t *a, const char *name, const char *pw, uint64_t now_ms);
const char *hmi_auth_setup_reason(const hmi_auth_t *a); /* password policy text after HMI_SETUP_PASSWORD */

/* Completes a begun job. Returns true once, when it finished, and fills *out. */
bool hmi_auth_poll(hmi_auth_t *a, uint64_t now_ms, hmi_auth_done_t *out);
bool hmi_auth_busy(const hmi_auth_t *a);

/* ---- session ---- */
/* The effective role: Viewer when logged out or idled out. Pure (no state change). */
hmi_role_t hmi_auth_role(const hmi_auth_t *a, uint64_t now_ms);
bool hmi_auth_logged_in(const hmi_auth_t *a, uint64_t now_ms);
const char *hmi_auth_user(const hmi_auth_t *a, uint64_t now_ms); /* "" when not logged in */
/* The user touched the screen `inactive_ms` ago (LVGL's inactivity time). */
void hmi_auth_note_activity(hmi_auth_t *a, uint64_t now_ms, uint32_t inactive_ms);
/* Ends an idled-out session and audits it. Call regularly. True if it ended one. */
bool hmi_auth_tick(hmi_auth_t *a, uint64_t now_ms);
void hmi_auth_logout(hmi_auth_t *a, uint64_t now_ms);

/* How long until `name` may try again (0 = now). Uses the unknown-user
 * counter for a name that does not exist. For the on-screen countdown. */
uint32_t hmi_auth_wait_ms(const hmi_auth_t *a, const char *name, uint64_t now_ms);

#endif /* HMI_AUTH_H */
