/* Tests for hmi_auth: first boot, login, lockout, persistence of counters,
 * idle logout, audit rules and the asynchronous hash job. A fake clock is a
 * plain variable passed to every call. The KDF cost is cheap on purpose. */
#include "test_support.h"
#include "../../src/hmi/hmi_auth.h"
#include "../../src/hmi/hmi_authz.h"
#include <stdio.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

static const hmi_kdf_params_t CHEAP = { 1, 64, 1 };
#define USERS "auth_test_users.bin"
#define LOCKF "auth_test_users.bin.lock"
#define IDLE 60000u
#define LOCK_FILE_PROBE_MAX 700 /* larger than any valid counter file */

static hmi_auth_t A; /* large; keep off the stack */

/* ---- audit sink that remembers every line and can be told to fail ---- */
typedef struct {
    char line[48][160];
    int n;
    bool fail;
    bool fail_only_login_ok;
    int ok_calls_with_session; /* LOGIN_OK seen while the session was already active */
} sink_t;
static sink_t S;

static bool sink_fn(void *u, const hmi_auth_audit_rec_t *r)
{
    (void)u;
    if (r->ev == HMI_AUTH_EV_LOGIN_OK && hmi_auth_role(&A, r->t_ms) != HMI_ROLE_VIEWER) S.ok_calls_with_session++;
    if (S.fail || (S.fail_only_login_ok && r->ev == HMI_AUTH_EV_LOGIN_OK)) return false;
    if (S.n < 48) hmi_auth_format_audit(r, S.line[S.n++], sizeof(S.line[0]));
    return true;
}

static int sink_count(const char *what)
{
    int c = 0;
    for (int i = 0; i < S.n; i++)
        if (strstr(S.line[i], what)) c++;
    return c;
}

static void sink_reset(void)
{
    memset(&S, 0, sizeof(S));
}

static void clean(void)
{
    remove(USERS);
    remove(LOCKF);
    remove(USERS ".tmp");
    remove(LOCKF ".tmp");
}

static hmi_auth_cfg_t cfg(void)
{
    hmi_auth_cfg_t c;
    c.users_path = USERS;
    c.idle_ms = IDLE;
    c.kdf = &CHEAP;
    return c;
}

static void init_at(uint64_t t)
{
    hmi_auth_cfg_t c = cfg();
    CHECK(hmi_auth_init(&A, &c, t));
    hmi_auth_set_audit(&A, sink_fn, NULL);
}

/* admin / operator / engineer accounts already on disk, then init. */
static void seeded(uint64_t t)
{
    clean();
    hmi_users_t s;
    memset(&s, 0, sizeof(s));
    const struct { const char *n; hmi_role_t r; const char *pw; } u[] = {
        { "admin", HMI_ROLE_ADMIN, "admin-pass-1" },
        { "op1", HMI_ROLE_OPERATOR, "operator-pass-1" },
        { "eng1", HMI_ROLE_ENGINEER, "engineer-pass-1" },
    };
    for (int i = 0; i < 3; i++) {
        hmi_user_t x;
        if (hmi_user_make(&x, u[i].n, u[i].r, u[i].pw, &CHEAP, NULL) != HMI_USERS_OK) exit(3);
        hmi_users_add(&s, &x);
    }
    if (!hmi_users_save(USERS, &s)) exit(3);
    sink_reset();
    init_at(t);
}

static bool contains(const void *hay, size_t hn, const char *needle)
{
    size_t nn = strlen(needle);
    const char *h = (const char *)hay;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(h + i, needle, nn) == 0) return true;
    return false;
}

static void test_delay(void)
{
    SECTION("auth: delay schedule");
    CHECK(hmi_auth_delay_ms(0) == 0);
    for (uint32_t f = 1; f <= 4; f++) CHECK(hmi_auth_delay_ms(f) == 1000);
    CHECK(hmi_auth_delay_ms(5) == 30000);
    CHECK(hmi_auth_delay_ms(6) == 60000);
    CHECK(hmi_auth_delay_ms(7) == 120000);
    CHECK(hmi_auth_delay_ms(8) == 240000);
    CHECK(hmi_auth_delay_ms(9) == 480000);
    CHECK(hmi_auth_delay_ms(10) == 900000);   /* capped at 15 minutes */
    CHECK(hmi_auth_delay_ms(11) == 900000);
    CHECK(hmi_auth_delay_ms(0xFFFFFFFFu) == 900000);
}

static void test_first_boot(void)
{
    SECTION("auth: first boot creates one administrator, never overwrites");
    clean();
    sink_reset();
    init_at(100);
    CHECK(hmi_auth_store_state(&A) == HMI_AUTH_STORE_NONE);
    CHECK(hmi_auth_login(&A, "admin", "whatever-1", 100).result == HMI_LOGIN_NO_STORE);
    CHECK(hmi_auth_role(&A, 100) == HMI_ROLE_VIEWER);

    CHECK(hmi_auth_setup(&A, "Admin", "admin-pass-1", 100) == HMI_SETUP_NAME);
    CHECK(hmi_auth_setup(&A, "admin", "short", 100) == HMI_SETUP_PASSWORD);
    CHECK(strstr(hmi_auth_setup_reason(&A), "short") != NULL);
    CHECK(hmi_auth_setup(&A, "adminadmin", "adminadmin", 100) == HMI_SETUP_PASSWORD);
    CHECK(hmi_auth_setup(&A, NULL, "admin-pass-1", 100) == HMI_SETUP_NAME);
    uint8_t b[4];
    size_t l;
    CHECK(hmi_fs_read_all(USERS, b, sizeof(b), &l) == HMI_FS_READ_ABSENT); /* nothing written by failures */

    S.fail = true; /* the creation must be audited first; no record, no account */
    CHECK(hmi_auth_setup(&A, "admin", "admin-pass-1", 100) == HMI_SETUP_AUDIT_FAILED);
    CHECK(hmi_fs_read_all(USERS, b, sizeof(b), &l) == HMI_FS_READ_ABSENT);
    CHECK(hmi_auth_store_state(&A) == HMI_AUTH_STORE_NONE);
    S.fail = false;

    CHECK(hmi_auth_setup(&A, "admin", "admin-pass-1", 200) == HMI_SETUP_OK);
    CHECK(hmi_auth_store_state(&A) == HMI_AUTH_STORE_READY);
    CHECK(sink_count("USER_CREATE user=admin role=admin") == 1);
    CHECK(hmi_auth_role(&A, 200) == HMI_ROLE_VIEWER); /* creating the account does not log in */
    CHECK(hmi_auth_setup(&A, "second", "second-pass-1", 200) == HMI_SETUP_NOT_FIRST_BOOT);

    /* The file is the account, in the strict format, with an Admin. */
    hmi_users_t u;
    CHECK(hmi_users_load(USERS, &u, NULL, 0) == HMI_USERS_LOAD_OK && u.n == 1 && u.u[0].role == HMI_ROLE_ADMIN);
    CHECK(hmi_auth_login(&A, "admin", "admin-pass-1", 300).result == HMI_LOGIN_OK);

    /* A store that appeared between begin() and completion is not overwritten. */
    hmi_auth_deinit(&A);
    clean();
    init_at(1000);
    CHECK(hmi_auth_setup_begin(&A, "late-admin", "late-admin-pass", 1000) == HMI_SETUP_PENDING);
    hmi_users_t other;
    memset(&other, 0, sizeof(other));
    hmi_user_t x;
    CHECK(hmi_user_make(&x, "other", HMI_ROLE_ADMIN, "other-pass-12", &CHEAP, NULL) == HMI_USERS_OK);
    hmi_users_add(&other, &x);
    CHECK(hmi_users_save(USERS, &other));
    hmi_auth_done_t d;
    int spins = 0;
    while (!hmi_auth_poll(&A, 1000, &d) && spins++ < 5000) hmi_sleep_ms(1);
    CHECK(d.kind == HMI_AUTH_JOB_SETUP && d.setup == HMI_SETUP_NOT_FIRST_BOOT);
    CHECK(hmi_users_load(USERS, &u, NULL, 0) == HMI_USERS_LOAD_OK && strcmp(u.u[0].name, "other") == 0);
    hmi_auth_deinit(&A);
    clean();
}

static void test_invalid_store(void)
{
    SECTION("auth: an invalid store disables login and never allows first boot");
    clean();
    uint8_t junk[300];
    memset(junk, 0x41, sizeof(junk));
    CHECK(hmi_fs_atomic_write(USERS, junk, sizeof(junk)));
    sink_reset();
    init_at(100);
    CHECK(hmi_auth_store_state(&A) == HMI_AUTH_STORE_INVALID);
    CHECK(strlen(hmi_auth_store_error(&A)) > 0);
    CHECK(hmi_auth_login(&A, "admin", "admin-pass-1", 100).result == HMI_LOGIN_STORE_INVALID);
    CHECK(hmi_auth_setup(&A, "admin", "admin-pass-1", 100) == HMI_SETUP_NOT_FIRST_BOOT);
    uint8_t back[300];
    size_t l = 0;
    CHECK(hmi_fs_read_all(USERS, back, sizeof(back), &l) == HMI_FS_READ_OK && l == 300 && memcmp(back, junk, 300) == 0);
    CHECK(hmi_auth_role(&A, 100) == HMI_ROLE_VIEWER);
    hmi_auth_deinit(&A);
    clean();
}

static void test_permissions(void)
{
    SECTION("auth: a user store that others can write is refused");
#ifndef _WIN32
    seeded(100);
    hmi_auth_deinit(&A);
    CHECK(chmod(USERS, 0644) == 0);                    /* readable by others: allowed, reported */
    init_at(100);
    CHECK(hmi_auth_store_state(&A) == HMI_AUTH_STORE_READY && hmi_auth_store_readable_by_others(&A));
    hmi_auth_deinit(&A);
    CHECK(chmod(USERS, 0660) == 0);                    /* writable by the group: refused */
    init_at(100);
    CHECK(hmi_auth_store_state(&A) == HMI_AUTH_STORE_INVALID);
    CHECK(strstr(hmi_auth_store_error(&A), "writable") != NULL);
    CHECK(hmi_auth_login(&A, "op1", "operator-pass-1", 100).result == HMI_LOGIN_STORE_INVALID);
    CHECK(hmi_auth_setup(&A, "admin2", "admin2-pass-12", 100) == HMI_SETUP_NOT_FIRST_BOOT);
    hmi_auth_deinit(&A);
    CHECK(chmod(USERS, 0602) == 0);                    /* writable by others: refused */
    init_at(100);
    CHECK(hmi_auth_store_state(&A) == HMI_AUTH_STORE_INVALID);
    hmi_auth_deinit(&A);
    CHECK(chmod(USERS, 0600) == 0);
    init_at(100);
    CHECK(hmi_auth_store_state(&A) == HMI_AUTH_STORE_READY && !hmi_auth_store_readable_by_others(&A));
    hmi_auth_deinit(&A);
    clean();
#endif
}

static void test_login_basic(void)
{
    SECTION("auth: login, role, session, wrong password");
    uint64_t t = 1000;
    seeded(t);
    CHECK(hmi_auth_store_state(&A) == HMI_AUTH_STORE_READY);
    CHECK(hmi_auth_role(&A, t) == HMI_ROLE_VIEWER && !hmi_auth_logged_in(&A, t) && hmi_auth_user(&A, t)[0] == '\0');

    hmi_login_info_t r = hmi_auth_login(&A, "op1", "wrong-password", t);
    CHECK(r.result == HMI_LOGIN_BAD && r.failures == 1 && r.wait_ms == 1000);
    CHECK(hmi_auth_role(&A, t) == HMI_ROLE_VIEWER);
    CHECK(S.ok_calls_with_session == 0);

    t += 1000;
    r = hmi_auth_login(&A, "op1", "operator-pass-1", t);
    CHECK(r.result == HMI_LOGIN_OK);
    CHECK(hmi_auth_role(&A, t) == HMI_ROLE_OPERATOR && hmi_auth_logged_in(&A, t));
    CHECK(strcmp(hmi_auth_user(&A, t), "op1") == 0);
    CHECK(S.ok_calls_with_session == 0); /* LOGIN_OK was recorded BEFORE the session existed */
    CHECK(hmi_auth_login(&A, "eng1", "engineer-pass-1", t + 5000).result == HMI_LOGIN_SESSION_ACTIVE);
    CHECK(hmi_auth_role(&A, t) == HMI_ROLE_OPERATOR); /* unchanged by the refused attempt */

    hmi_auth_logout(&A, t + 10);
    CHECK(hmi_auth_role(&A, t + 10) == HMI_ROLE_VIEWER && !hmi_auth_logged_in(&A, t + 10));
    CHECK(sink_count("LOGOUT user=op1 role=operator") == 1);
    hmi_auth_logout(&A, t + 20); /* second logout is a no-op */
    CHECK(sink_count("LOGOUT") == 1);

    t += 20000;
    CHECK(hmi_auth_login(&A, "eng1", "engineer-pass-1", t).result == HMI_LOGIN_OK);
    CHECK(hmi_auth_role(&A, t) == HMI_ROLE_ENGINEER);
    hmi_auth_logout(&A, t);
    t += 20000;
    CHECK(hmi_auth_login(&A, "admin", "admin-pass-1", t).result == HMI_LOGIN_OK);
    CHECK(hmi_auth_role(&A, t) == HMI_ROLE_ADMIN);
    hmi_auth_logout(&A, t);

    /* Case matters for names and passwords. */
    t += 20000;
    CHECK(hmi_auth_login(&A, "ADMIN", "admin-pass-1", t).result == HMI_LOGIN_BAD);
    t += 20000;
    CHECK(hmi_auth_login(&A, "admin", "Admin-pass-1", t).result == HMI_LOGIN_BAD);
    t += 20000;
    CHECK(hmi_auth_login(&A, "admin", "admin-pass-1 ", t).result == HMI_LOGIN_BAD);

    /* Inputs the policy can never accept are refused without hashing or counting. */
    t += 20000;
    char longpw[100];
    memset(longpw, 'a', 99);
    longpw[99] = '\0';
    r = hmi_auth_login(&A, "admin", longpw, t);
    CHECK(r.result == HMI_LOGIN_INPUT);
    CHECK(hmi_auth_login(&A, NULL, "x", t).result == HMI_LOGIN_INPUT);
    CHECK(hmi_auth_login(&A, "admin", NULL, t).result == HMI_LOGIN_INPUT);
    hmi_auth_deinit(&A);
    clean();
}

static void test_lockout(void)
{
    SECTION("auth: lockout timing with the fake clock");
    uint64_t t = 10000;
    seeded(t);
    hmi_login_info_t r;
    /* failures 1..4: a one second gap after each */
    for (uint32_t f = 1; f <= 4; f++) {
        r = hmi_auth_login(&A, "op1", "bad-password-x", t);
        CHECK(r.result == HMI_LOGIN_BAD && r.failures == f && r.wait_ms == 1000);
        r = hmi_auth_login(&A, "op1", "operator-pass-1", t + 999); /* right password, too soon */
        CHECK(r.result == HMI_LOGIN_WAIT && r.wait_ms == 1);
        CHECK(hmi_auth_role(&A, t + 999) == HMI_ROLE_VIEWER);
        t += 1000;
    }
    /* the 5th failure starts a 30 s lock */
    r = hmi_auth_login(&A, "op1", "bad-password-x", t);
    CHECK(r.result == HMI_LOGIN_BAD && r.failures == 5 && r.wait_ms == 30000);
    CHECK(hmi_auth_wait_ms(&A, "op1", t) == 30000);
    CHECK(sink_count("LOCKOUT user=op1 failures=5 lock_ms=30000") == 1);
    uint64_t t5 = t;
    r = hmi_auth_login(&A, "op1", "operator-pass-1", t5 + 29999); /* correct password, still locked */
    CHECK(r.result == HMI_LOGIN_LOCKED && r.wait_ms == 1);
    CHECK(hmi_auth_role(&A, t5 + 29999) == HMI_ROLE_VIEWER);
    /* another user is not locked by op1's failures (only the 1 s global gap, long past) */
    r = hmi_auth_login(&A, "eng1", "engineer-pass-1", t5 + 29999);
    CHECK(r.result == HMI_LOGIN_OK);
    hmi_auth_logout(&A, t5 + 29999);
    t = t5 + 40000;

    /* the lock ends exactly at 30 s; one more failure doubles it */
    r = hmi_auth_login(&A, "op1", "bad-password-x", t5 + 30000);
    CHECK(r.result == HMI_LOGIN_BAD && r.failures == 6 && r.wait_ms == 60000);
    uint64_t t6 = t5 + 30000;
    uint32_t want[] = { 120000, 240000, 480000, 900000, 900000, 900000 };
    uint64_t tt = t6;
    for (int i = 0; i < 6; i++) {
        tt += hmi_auth_delay_ms(6 + (uint32_t)i);
        r = hmi_auth_login(&A, "op1", "bad-password-x", tt);
        CHECK(r.result == HMI_LOGIN_BAD && r.wait_ms == want[i]);
    }
    /* a correct password after the full wait resets the counter */
    tt += 900000;
    CHECK(hmi_auth_login(&A, "op1", "operator-pass-1", tt).result == HMI_LOGIN_OK);
    hmi_auth_logout(&A, tt);
    r = hmi_auth_login(&A, "op1", "bad-password-x", tt + 5000);
    CHECK(r.result == HMI_LOGIN_BAD && r.failures == 1 && r.wait_ms == 1000);
    hmi_auth_deinit(&A);
    clean();
}

static void test_unknown_user(void)
{
    SECTION("auth: unknown names pay for a hash and share one counter");
    uint64_t t = 5000;
    seeded(t);
    hmi_login_info_t r;
    const char *names[] = { "nobody", "ghost", "Admin", "root", "x y", "nobody" };
    for (int i = 0; i < 5; i++) {
        r = hmi_auth_login(&A, names[i], "whatever-pass", t);
        CHECK(r.result == HMI_LOGIN_BAD && r.failures == (uint32_t)(i + 1));
        t += 1000;
    }
    r = hmi_auth_login(&A, "brand-new", "whatever-pass", t - 1000 + 1);
    CHECK(r.result == HMI_LOGIN_LOCKED || r.result == HMI_LOGIN_WAIT); /* shared bucket is locked */
    CHECK(hmi_auth_wait_ms(&A, "anything", t - 1000) == 30000);
    CHECK(hmi_auth_wait_ms(&A, "op1", t - 1000 + 1000) == 0); /* real users are not locked by it */
    CHECK(hmi_auth_login(&A, "op1", "operator-pass-1", t + 1).result == HMI_LOGIN_OK);
    /* no user name ever reaches the audit file for an unknown account */
    CHECK(sink_count("LOGIN_FAIL user=? failures=1") == 1);
    CHECK(sink_count("nobody") == 0 && sink_count("ghost") == 0 && sink_count("x y") == 0);
    hmi_auth_deinit(&A);
    clean();
}

static void test_persistence(void)
{
    SECTION("auth: failure counters survive a restart, a restart never shortens a lock");
    uint64_t t = 1000;
    seeded(t);
    for (int i = 0; i < 3; i++) {
        CHECK(hmi_auth_login(&A, "op1", "bad-password-x", t).result == HMI_LOGIN_BAD);
        t += 1000;
    }
    CHECK(!A.lock_persist_failed);
    hmi_auth_deinit(&A);
    init_at(50);                                   /* restart: the monotonic clock starts over */
    hmi_login_info_t r = hmi_auth_login(&A, "op1", "bad-password-x", 50);
    CHECK(r.result == HMI_LOGIN_BAD && r.failures == 4); /* 3 remembered, this is the 4th */
    r = hmi_auth_login(&A, "op1", "bad-password-x", 1050);
    CHECK(r.failures == 5 && r.wait_ms == 30000);
    hmi_auth_deinit(&A);

    init_at(7);                                    /* restart in the middle of a lock */
    CHECK(hmi_auth_wait_ms(&A, "op1", 7) == 30000); /* full delay again, never less */
    r = hmi_auth_login(&A, "op1", "operator-pass-1", 7 + 29999);
    CHECK(r.result == HMI_LOGIN_LOCKED && r.wait_ms == 1);
    CHECK(hmi_auth_login(&A, "op1", "operator-pass-1", 7 + 30000).result == HMI_LOGIN_OK);
    hmi_auth_logout(&A, 7 + 30000);
    hmi_auth_deinit(&A);

    init_at(9);                                    /* success cleared the counter on disk */
    r = hmi_auth_login(&A, "op1", "bad-password-x", 9);
    CHECK(r.failures == 1);
    hmi_auth_deinit(&A);

    /* A damaged counter file is treated as a lock that has just started. */
    uint8_t garbage[40];
    memset(garbage, 0x55, sizeof(garbage));
    CHECK(hmi_fs_atomic_write(LOCKF, garbage, sizeof(garbage)));
    init_at(100);
    CHECK(hmi_auth_wait_ms(&A, "op1", 100) == 30000 && hmi_auth_wait_ms(&A, "eng1", 100) == 30000);
    CHECK(hmi_auth_login(&A, "op1", "operator-pass-1", 100 + 29999).result == HMI_LOGIN_LOCKED);
    CHECK(hmi_auth_login(&A, "op1", "operator-pass-1", 100 + 30000).result == HMI_LOGIN_OK);
    hmi_auth_logout(&A, 100 + 30000);
    hmi_auth_deinit(&A);

    /* One flipped bit in an otherwise valid counter file is also damage. */
    init_at(0);
    hmi_auth_login(&A, "op1", "bad-password-x", 0);
    hmi_auth_deinit(&A);
    uint8_t lf[LOCK_FILE_PROBE_MAX];
    size_t ll = 0;
    CHECK(hmi_fs_read_all(LOCKF, lf, sizeof(lf), &ll) == HMI_FS_READ_OK && ll > 12);
    lf[9] ^= 0x01;
    CHECK(hmi_fs_atomic_write(LOCKF, lf, ll));
    init_at(0);
    CHECK(hmi_auth_wait_ms(&A, "op1", 0) == 30000);
    hmi_auth_deinit(&A);

    /* Entries for accounts that no longer exist are ignored. */
    seeded(0);
    hmi_auth_login(&A, "op1", "bad-password-x", 0);
    hmi_auth_deinit(&A);
    hmi_users_t s;
    CHECK(hmi_users_load(USERS, &s, NULL, 0) == HMI_USERS_LOAD_OK);
    CHECK(hmi_users_remove(&s, "op1") == HMI_USERS_OK && hmi_users_save(USERS, &s));
    init_at(0);
    CHECK(hmi_auth_wait_ms(&A, "eng1", 0) == 0);
    hmi_auth_deinit(&A);
    clean();
}

static void test_idle(void)
{
    SECTION("auth: idle logout");
    uint64_t t = 100000;
    seeded(t);
    CHECK(hmi_auth_login(&A, "op1", "operator-pass-1", t).result == HMI_LOGIN_OK);
    CHECK(hmi_auth_role(&A, t + IDLE - 1) == HMI_ROLE_OPERATOR);
    CHECK(hmi_auth_role(&A, t + IDLE) == HMI_ROLE_VIEWER);          /* pure check: no state change needed */
    CHECK(!hmi_auth_logged_in(&A, t + IDLE) && hmi_auth_user(&A, t + IDLE)[0] == '\0');
    CHECK(!hmi_auth_tick(&A, t + IDLE - 1));
    CHECK(sink_count("IDLE_LOGOUT") == 0);

    /* touch activity postpones the logout */
    hmi_auth_note_activity(&A, t + 30000, 0);                       /* touched just now */
    CHECK(hmi_auth_role(&A, t + 30000 + IDLE - 1) == HMI_ROLE_OPERATOR);
    CHECK(hmi_auth_role(&A, t + 30000 + IDLE) == HMI_ROLE_VIEWER);
    hmi_auth_note_activity(&A, t + 40000, 10000);                   /* touched 10 s ago = at +30000: not newer */
    CHECK(hmi_auth_role(&A, t + 30000 + IDLE) == HMI_ROLE_VIEWER);
    hmi_auth_note_activity(&A, t + 50000, 1000);                    /* touched at +49000 */
    CHECK(hmi_auth_role(&A, t + 49000 + IDLE - 1) == HMI_ROLE_OPERATOR);
    hmi_auth_note_activity(&A, 500, 900000);                        /* inactivity longer than uptime: no wrap */
    CHECK(hmi_auth_role(&A, t + 49000 + IDLE - 1) == HMI_ROLE_OPERATOR);

    uint64_t end = t + 49000 + IDLE;
    CHECK(hmi_auth_tick(&A, end));
    CHECK(sink_count("IDLE_LOGOUT user=op1 role=operator") == 1);
    CHECK(!hmi_auth_tick(&A, end + 1));                             /* once */
    hmi_auth_note_activity(&A, end + 10, 0);                        /* an ended session is never revived */
    CHECK(hmi_auth_role(&A, end + 10) == HMI_ROLE_VIEWER);

    /* idled-out but not yet ticked: a new login is allowed and ends the old session */
    t = end + 5000;
    CHECK(hmi_auth_login(&A, "eng1", "engineer-pass-1", t).result == HMI_LOGIN_OK);
    CHECK(hmi_auth_role(&A, t) == HMI_ROLE_ENGINEER);
    uint64_t late = t + IDLE + 1;
    CHECK(hmi_auth_login(&A, "op1", "operator-pass-1", late).result == HMI_LOGIN_OK);
    CHECK(hmi_auth_role(&A, late) == HMI_ROLE_OPERATOR);
    CHECK(sink_count("IDLE_LOGOUT user=eng1") == 1);
    hmi_auth_deinit(&A);
    clean();
}

static void test_audit(void)
{
    SECTION("auth: audit rules");
    uint64_t t = 1000;
    seeded(t);

    /* no record, no session */
    S.fail_only_login_ok = true;
    hmi_login_info_t r = hmi_auth_login(&A, "op1", "operator-pass-1", t);
    CHECK(r.result == HMI_LOGIN_AUDIT_FAILED);
    CHECK(hmi_auth_role(&A, t) == HMI_ROLE_VIEWER && !hmi_auth_logged_in(&A, t));
    S.fail_only_login_ok = false;
    CHECK(hmi_auth_login(&A, "op1", "operator-pass-1", t).result == HMI_LOGIN_OK); /* a refused grant is not a failed login */
    hmi_auth_logout(&A, t);

    /* failures, locks and logouts are best effort: a dead trail does not change the decision */
    S.fail = true;
    t += 10000;
    r = hmi_auth_login(&A, "op1", "wrong-password", t);
    CHECK(r.result == HMI_LOGIN_BAD && r.failures == 1);
    S.fail = false;

    /* line format and content */
    sink_reset();
    t += 10000;
    hmi_auth_login(&A, "eng1", "wrong-password", t);
    CHECK(sink_count("LOGIN_FAIL user=eng1 failures=1") == 1);
    t += 2000;
    hmi_auth_login(&A, "eng1", "engineer-pass-1", t);
    CHECK(sink_count("LOGIN_OK user=eng1 role=engineer") == 1);
    char want[64];
    snprintf(want, sizeof(want), "%llu LOGIN_OK", (unsigned long long)t);
    CHECK(sink_count(want) == 1); /* starts with the monotonic ms like the other audit lines */
    for (int i = 0; i < S.n; i++) {
        CHECK(strchr(S.line[i], '\n') == S.line[i] + strlen(S.line[i]) - 1);
        CHECK(!strstr(S.line[i], "pass"));
    }

    /* hostile user names never reach the file */
    hmi_auth_audit_rec_t rec;
    char line[160];
    memset(&rec, 0, sizeof(rec));
    rec.ev = HMI_AUTH_EV_LOGIN_FAIL;
    rec.user = "evil\nname role=admin";
    CHECK(hmi_auth_format_audit(&rec, line, sizeof(line)) > 0 && strstr(line, "user=? ") && !strstr(line, "evil"));
    rec.user = NULL;
    CHECK(hmi_auth_format_audit(&rec, line, sizeof(line)) > 0 && strstr(line, "user=? "));
    CHECK(hmi_auth_format_audit(&rec, line, 8) == 0);   /* too small: nothing partial */
    CHECK(hmi_auth_format_audit(&rec, NULL, 8) == 0);
    rec.ev = (hmi_auth_event_t)99;
    CHECK(hmi_auth_format_audit(&rec, line, sizeof(line)) == 0);

    /* lines written by the offline user tool */
    rec.t_ms = 7;
    rec.user = "op1";
    rec.role = HMI_ROLE_ENGINEER;
    rec.ev = HMI_AUTH_EV_USER_ADD;
    CHECK(hmi_auth_format_audit(&rec, line, sizeof(line)) > 0 && strcmp(line, "7 USER_ADD user=op1 role=engineer\n") == 0);
    rec.ev = HMI_AUTH_EV_USER_REMOVE;
    CHECK(hmi_auth_format_audit(&rec, line, sizeof(line)) > 0 && strcmp(line, "7 USER_REMOVE user=op1\n") == 0);
    rec.ev = HMI_AUTH_EV_USER_PASSWORD;
    CHECK(hmi_auth_format_audit(&rec, line, sizeof(line)) > 0 &&
          strcmp(line, "7 USER_PASSWORD user=op1 role=engineer\n") == 0);
    rec.ev = HMI_AUTH_EV_USER_CREATE_FAILED;
    CHECK(hmi_auth_format_audit(&rec, line, sizeof(line)) > 0 && strcmp(line, "7 USER_CREATE_FAILED user=op1\n") == 0);

    /* the ready-made file sink appends durable lines */
    const char *path = "auth_test_audit.log";
    remove(path);
    rec.ev = HMI_AUTH_EV_LOGOUT;
    rec.user = "op1";
    rec.role = HMI_ROLE_OPERATOR;
    rec.t_ms = 42;
    CHECK(hmi_auth_file_audit((void *)path, &rec));
    CHECK(hmi_auth_file_audit((void *)path, &rec));
    char back[200];
    size_t n = 0;
    CHECK(hmi_fs_read_all(path, back, sizeof(back) - 1, &n) == HMI_FS_READ_OK);
    back[n] = '\0';
    CHECK(strcmp(back, "42 LOGOUT user=op1 role=operator\n42 LOGOUT user=op1 role=operator\n") == 0);
    CHECK(!hmi_auth_file_audit(NULL, &rec));
    CHECK(!hmi_auth_file_audit((void *)"no_such_dir_p99/audit.log", &rec)); /* cannot append: reported */
    remove(path);
    hmi_auth_deinit(&A);
    clean();
}

static void test_secrets(void)
{
    SECTION("auth: passwords do not stay in memory the module owns");
    uint64_t t = 1000;
    seeded(t);
    const char *pw = "Zq7!unique-secret-passphrase";
    hmi_users_t s;
    CHECK(hmi_users_load(USERS, &s, NULL, 0) == HMI_USERS_LOAD_OK);
    hmi_user_t u;
    CHECK(hmi_user_make(&u, "secret1", HMI_ROLE_OPERATOR, pw, &CHEAP, NULL) == HMI_USERS_OK);
    hmi_users_add(&s, &u);
    CHECK(hmi_users_save(USERS, &s));
    hmi_auth_deinit(&A);
    init_at(t);
    CHECK(hmi_auth_login(&A, "secret1", pw, t).result == HMI_LOGIN_OK);
    CHECK(!contains(&A, sizeof(A), pw));                       /* job copy was wiped */
    hmi_auth_logout(&A, t);
    t += 5000;
    CHECK(hmi_auth_login(&A, "secret1", "wrong-secret-pass", t).result == HMI_LOGIN_BAD);
    CHECK(!contains(&A, sizeof(A), "wrong-secret-pass"));
    uint8_t raw[HMI_USERS_FILE_MAX];
    size_t n = 0;
    CHECK(hmi_fs_read_all(USERS, raw, sizeof(raw), &n) == HMI_FS_READ_OK);
    CHECK(!contains(raw, n, pw));                              /* only a hash is stored */
    hmi_auth_deinit(&A);
    CHECK(A.users_path[0] == '\0' && A.users.n == 0);         /* deinit wipes the state */
    clean();
}

static void test_async(void)
{
    SECTION("auth: the hash can run on a worker thread");
    uint64_t t = 1000;
    seeded(t);
    hmi_login_info_t r = hmi_auth_login_begin(&A, "op1", "operator-pass-1", t);
    CHECK(r.result == HMI_LOGIN_PENDING);
    CHECK(hmi_auth_busy(&A));
    CHECK(hmi_auth_login_begin(&A, "eng1", "engineer-pass-1", t).result == HMI_LOGIN_BUSY);
    CHECK(hmi_auth_setup_begin(&A, "zzz-admin", "zzz-admin-pass", t) == HMI_SETUP_BUSY);
    hmi_auth_done_t d;
    memset(&d, 0, sizeof(d));
    int spins = 0;
    while (!hmi_auth_poll(&A, t, &d) && spins++ < 5000) hmi_sleep_ms(1);
    CHECK(d.kind == HMI_AUTH_JOB_LOGIN && d.login.result == HMI_LOGIN_OK);
    CHECK(!hmi_auth_busy(&A));
    CHECK(!hmi_auth_poll(&A, t, &d)); /* nothing left to collect */
    CHECK(hmi_auth_role(&A, t) == HMI_ROLE_OPERATOR);
    hmi_auth_logout(&A, t);

    t += 5000;
    CHECK(hmi_auth_login_begin(&A, "op1", "bad-password-x", t).result == HMI_LOGIN_PENDING);
    spins = 0;
    while (!hmi_auth_poll(&A, t, &d) && spins++ < 5000) hmi_sleep_ms(1);
    CHECK(d.login.result == HMI_LOGIN_BAD && d.login.failures == 1);

    /* deinit with a job still running must not leak or crash */
    t += 5000;
    CHECK(hmi_auth_login_begin(&A, "op1", "operator-pass-1", t).result == HMI_LOGIN_PENDING);
    hmi_auth_deinit(&A);
    clean();
}

static void test_core_integration(void)
{
    SECTION("auth: only the session role reaches the core");
    uint64_t t = 1000;
    seeded(t);
    fx_t *f = fx_new(true);
    hmi_role_t role = hmi_auth_role(&A, t);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, role) == HMI_ACK_DENIED);
    uint32_t id = 0;
    CHECK(submit(f, 0, 50, role, &id) == HMI_WRS_ERR_ROLE);

    CHECK(hmi_auth_login(&A, "op1", "operator-pass-1", t).result == HMI_LOGIN_OK);
    role = hmi_auth_role(&A, t);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, role) != HMI_ACK_DENIED);
    CHECK(submit(f, 0, 50, role, &id) != HMI_WRS_ERR_ROLE);

    /* after the idle timeout the same call sites are refused again, without any tick */
    role = hmi_auth_role(&A, t + IDLE);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, role) == HMI_ACK_DENIED);
    CHECK(submit(f, 0, 50, role, &id) == HMI_WRS_ERR_ROLE);
    fx_free(f, true);
    hmi_auth_deinit(&A);
    clean();
}

int main(void)
{
    test_delay();
    test_first_boot();
    test_invalid_store();
    test_permissions();
    test_login_basic();
    test_lockout();
    test_unknown_user();
    test_persistence();
    test_idle();
    test_audit();
    test_secrets();
    test_async();
    test_core_integration();
    clean();
    return test_finish();
}
