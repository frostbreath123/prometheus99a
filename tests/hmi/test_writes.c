/* Tests for the write pipeline: through comms and hmi_writes directly.
 * Part of the HMI core host tests; shared fixtures are in test_support.h. */
#include "test_support.h"

/* ---- writes ---- */
static void settle_connected(fx_t *f)
{
    run_until(f, g_now + 300);
}

static void test_write_confirmed(void)
{
    SECTION("writes: validate, send, read-back confirm");
    fx_t *f = fx_new(false);
    uint32_t id = 0;
    CHECK(submit(f, 0, 42.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_ERR_LINK_DOWN);
    settle_connected(f);
    CHECK(submit(f, 0, 42.0, HMI_ROLE_VIEWER, &id) == HMI_WRS_ERR_ROLE);
    CHECK(submit(f, 0, 101.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_ERR_RANGE);
    CHECK(submit(f, 0, NAN, HMI_ROLE_OPERATOR, &id) == HMI_WRS_ERR_RANGE);
    CHECK(submit(f, 1, 5.0, HMI_ROLE_ADMIN, &id) == HMI_WRS_ERR_NOT_WRITABLE);
    CHECK(submit(f, 9, 5.0, HMI_ROLE_ADMIN, &id) == HMI_WRS_ERR_NO_TAG);
    CHECK(f->m.n_write == 0);
    CHECK(submit(f, 0, 42.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    hmi_write_rec_t r;
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_PENDING);
    run_until(f, g_now + 100);
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_CONFIRMED);
    CHECK(f->m.hold[0] == 420 && f->m.n_write == 1);
    fx_free(f, false);
}

static void test_write_disabled(void)
{
    SECTION("writes: master switch off rejects everything");
    fx_t *f = fx_new(false);
    hmi_writes_set_enabled(&f->w, false);
    settle_connected(f);
    uint32_t id;
    CHECK(submit(f, 0, 42.0, HMI_ROLE_ADMIN, &id) == HMI_WRS_ERR_DISABLED);
    CHECK(f->m.n_write == 0);
    fx_free(f, false);
}

static void test_write_exception(void)
{
    SECTION("writes: PLC exception -> REJECTED, link stays up");
    fx_t *f = fx_new(false);
    settle_connected(f);
    f->m.write_st = HMI_IO_EXCEPTION;
    f->m.write_exc = 3;
    uint32_t id;
    CHECK(submit(f, 0, 42.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    run_until(f, g_now + 100);
    hmi_write_rec_t r;
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_REJECTED && r.exc == 3);
    CHECK(hmi_comms_state(&f->c) != HMI_CONN_BACKOFF);
    fx_free(f, false);
}

static void test_write_lost_reply_never_replayed(void)
{
    SECTION("writes: lost reply -> UNKNOWN, never replayed after reconnect");
    fx_t *f = fx_new(false);
    settle_connected(f);
    f->m.write_st = HMI_IO_TIMEOUT;
    f->m.apply_on_fail = true; /* PLC applied it but the reply was lost */
    uint32_t id;
    CHECK(submit(f, 0, 33.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    run_until(f, g_now + 100);
    hmi_write_rec_t r;
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_UNKNOWN);
    CHECK(f->m.n_write == 1);
    /* even after the link drops and recovers, nothing is re-sent */
    f->m.read_st = HMI_IO_TIMEOUT;
    run_until(f, g_now + 1500);
    f->m.read_st = HMI_IO_OK;
    f->m.write_st = HMI_IO_OK;
    run_until(f, g_now + 8000);
    CHECK(hmi_comms_state(&f->c) == HMI_CONN_CONNECTED);
    CHECK(f->m.n_write == 1);
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_UNKNOWN);
    fx_free(f, false);
}

static void test_write_verify_failed(void)
{
    SECTION("writes: read-back mismatch -> VERIFY_FAILED (not CONFIRMED)");
    fx_t *f = fx_new(false);
    settle_connected(f);
    f->m.corrupt_write = true;
    uint32_t id;
    CHECK(submit(f, 0, 20.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    run_until(f, g_now + 100);
    hmi_write_rec_t r;
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_VERIFY_FAILED);
    CHECK(r.raw == 200 && r.readback_raw == 201);
    fx_free(f, false);
}

static void test_write_readback_lost(void)
{
    SECTION("writes: accepted but read-back lost -> UNKNOWN");
    fx_t *f = fx_new(false);
    settle_connected(f);
    f->m.fail_next_read_after_write = true;
    f->m.readback_st = HMI_IO_TIMEOUT;
    uint32_t id;
    CHECK(submit(f, 0, 20.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    run_until(f, g_now + 100);
    hmi_write_rec_t r;
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_UNKNOWN);
    fx_free(f, false);
}

static void test_write_expiry_on_link_loss(void)
{
    SECTION("writes: queued writes expire, not replayed, when link lost");
    fx_t *f = fx_new(false);
    settle_connected(f);
    uint32_t id;
    CHECK(submit(f, 0, 10.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    /* kill the link before the write is serviced: lose connection state */
    f->m.read_st = HMI_IO_TIMEOUT;
    f->m.write_st = HMI_IO_OK;
    /* force the pending write to be older than ttl without sending it:
     * advance time, then step once (expiry runs before dispatch) */
    g_now += 2500;
    hmi_comms_step(&f->c);
    hmi_write_rec_t r;
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_EXPIRED);
    CHECK(f->m.n_write == 0);
    fx_free(f, false);
}

static void test_write_dropped_on_link_loss(void)
{
    SECTION("writes: link loss expires still-queued writes immediately");
    fx_t *f = fx_new(false);
    settle_connected(f);
    f->m.write_st = HMI_IO_TIMEOUT; /* every transmitted write times out */
    uint32_t id[4];
    for (int i = 0; i < 4; i++)
        CHECK(submit(f, 0, 10.0 + i, HMI_ROLE_OPERATOR, &id[i]) == HMI_WRS_OK);
    for (int i = 0; i < 4; i++) hmi_comms_step(&f->c); /* 3 timeouts reach miss_limit */
    CHECK(hmi_comms_state(&f->c) == HMI_CONN_BACKOFF);
    hmi_write_rec_t r;
    CHECK(hmi_writes_status(&f->w, id[0], &r) && r.state == HMI_WR_UNKNOWN);
    CHECK(hmi_writes_status(&f->w, id[3], &r) && r.state == HMI_WR_EXPIRED);
    CHECK(f->m.n_write == 3); /* the fourth was never transmitted */
    f->m.write_st = HMI_IO_OK;
    run_until(f, g_now + 8000);
    CHECK(f->m.n_write == 3); /* and is not replayed after reconnect */
    fx_free(f, false);
}

typedef struct { int fail_on; int calls; } audit_t;
static bool audit_fn(void *u, const hmi_write_rec_t *r, const char *ev)
{
    (void)r;
    audit_t *a = (audit_t *)u;
    a->calls++;
    return !(a->fail_on && strcmp(ev, a->fail_on == 1 ? "RESERVED" : "SENT") == 0);
}

static void test_audit_fail_closed(void)
{
    SECTION("writes: audit failure fails closed (reserve and pre-send)");
    fx_t *f = fx_new(false);
    settle_connected(f);
    audit_t a = { 1, 0 };
    hmi_writes_set_audit(&f->w, audit_fn, &a);
    uint32_t id;
    CHECK(submit(f, 0, 10.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_ERR_AUDIT);
    CHECK(f->m.n_write == 0);
    a.fail_on = 2; /* reserve ok, send-intent record fails */
    CHECK(submit(f, 0, 10.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    run_until(f, g_now + 100);
    hmi_write_rec_t r;
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_REJECTED);
    CHECK(f->m.n_write == 0); /* nothing was transmitted */
    fx_free(f, false);
}

static void test_write_queue_and_audit_capacity(void)
{
    SECTION("writes: queue overflow and exhausted audit capacity");
    fx_t *f = fx_new(false);
    settle_connected(f);
    uint32_t id;
    for (int i = 0; i < HMI_WRITE_QUEUE_LEN; i++)
        CHECK(submit(f, 0, (double)i, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    CHECK(submit(f, 0, 1.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_ERR_QUEUE_FULL);
    run_until(f, g_now + 500); /* drain */
    CHECK(f->m.n_write == HMI_WRITE_QUEUE_LEN);
    /* history wraps: many more writes keep working (finals are recycled) */
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < HMI_WRITE_QUEUE_LEN; i++)
            CHECK(submit(f, 0, 5.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
        run_until(f, g_now + 500);
    }
    /* make every audit slot unresolved: fill ring with RESERVED states */
    hmi_mutex_lock(&f->w.lock);
    for (int i = 0; i < HMI_AUDIT_LEN; i++) f->w.ring[i].state = HMI_WR_PENDING;
    f->w.q_len = 0;
    hmi_mutex_unlock(&f->w.lock);
    CHECK(submit(f, 0, 5.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_ERR_AUDIT);
    fx_free(f, false);
}


/* ---- hmi_writes module directly (no comms, no transport) ---- */
static int g_audit_fail_event; /* 0 none, 1 RESERVED, 2 SENT */
static int g_audit_sent_calls;
static bool wr_audit(void *u, const hmi_write_rec_t *r, const char *ev)
{
    (void)u; (void)r;
    if (strcmp(ev, "SENT") == 0) g_audit_sent_calls++;
    if (g_audit_fail_event == 1 && strcmp(ev, "RESERVED") == 0) return false;
    if (g_audit_fail_event == 2 && strcmp(ev, "SENT") == 0 && g_audit_sent_calls == 1) return false;
    return true;
}

static void test_writes_module(void)
{
    SECTION("writes module: interface-level lifecycle, FIFO, single transmission, 32-bit verify");
    hmi_tag_cfg_t t[2];
    memset(t, 0, sizeof(t));
    snprintf(t[0].name, HMI_NAME_LEN, "SP16");
    t[0].dtype = HMI_DT_U16; t[0].scale = 1; t[0].eng_max = 1000; t[0].poll_ms = 100;
    t[0].write_allowed = true; t[0].write_role = HMI_ROLE_OPERATOR; t[0].write_max = 1000;
    snprintf(t[1].name, HMI_NAME_LEN, "SP32");
    t[1].area = HMI_AREA_HOLDING; t[1].address = 10; t[1].dtype = HMI_DT_U32; t[1].word_order = HMI_WORD_LOW_FIRST;
    t[1].scale = 1; t[1].eng_max = 4e9; t[1].poll_ms = 100;
    t[1].write_allowed = true; t[1].write_role = HMI_ROLE_ENGINEER; t[1].write_max = 4e9;
    hmi_tagdb_t db;
    CHECK(hmi_tagdb_init(&db, t, 2, 0));
    hmi_writes_t w;
    CHECK(hmi_writes_init(&w, &db, true, 1000));
    uint32_t id1, id2, id3;
    hmi_write_job_t job;
    hmi_write_rec_t r;

    /* link states: lost refused, degraded allowed */
    CHECK(hmi_writes_submit(&w, 0, 5, HMI_ROLE_OPERATOR, HMI_LINK_DOWN, 100, &id1) == HMI_WRS_ERR_LINK_DOWN);
    CHECK(hmi_writes_submit(&w, 0, 5, HMI_ROLE_OPERATOR, HMI_LINK_NO_RESPONSE, 100, &id1) == HMI_WRS_ERR_LINK_DOWN);
    CHECK(hmi_writes_submit(&w, 0, 5, HMI_ROLE_OPERATOR, HMI_LINK_DEGRADED, 100, &id1) == HMI_WRS_OK);
    CHECK(hmi_writes_submit(&w, 0, 6, HMI_ROLE_OPERATOR, HMI_LINK_HEALTHY, 100, &id2) == HMI_WRS_OK);
    CHECK(hmi_writes_submit(&w, 1, 70000, HMI_ROLE_OPERATOR, HMI_LINK_HEALTHY, 100, &id3) == HMI_WRS_ERR_ROLE);
    CHECK(hmi_writes_submit(&w, 1, 70000, HMI_ROLE_ENGINEER, HMI_LINK_HEALTHY, 100, &id3) == HMI_WRS_OK);

    /* FIFO order, one transmission each */
    CHECK(hmi_writes_take(&w, 100, &job) && job.tag == 0 && job.regs[0] == 5);
    hmi_writes_finish_exception(&w, &job, 3, 101);
    CHECK(hmi_writes_status(&w, id1, &r) && r.state == HMI_WR_REJECTED && r.exc == 3);
    CHECK(hmi_writes_take(&w, 100, &job) && job.tag == 0 && job.regs[0] == 6);
    hmi_writes_finish_unknown(&w, &job, 0, 102);
    CHECK(hmi_writes_status(&w, id2, &r) && r.state == HMI_WR_UNKNOWN);
    CHECK(hmi_writes_take(&w, 100, &job) && job.tag == 1 && job.n_regs == 2 && job.address == 10);
    /* 70000 = 0x00011170 low word first: regs[0]=0x1170, regs[1]=0x0001 */
    CHECK(job.regs[0] == 0x1170 && job.regs[1] == 0x0001);
    uint16_t good[2] = { 0x1170, 0x0001 }, bad[2] = { 0x1170, 0x0002 };
    hmi_writes_finish_verified(&w, &job, bad, 103);
    CHECK(hmi_writes_status(&w, id3, &r) && r.state == HMI_WR_VERIFY_FAILED && r.readback_raw == 0x00021170u);
    CHECK(!hmi_writes_take(&w, 100, &job)); /* nothing is ever re-offered */

    CHECK(hmi_writes_submit(&w, 1, 70000, HMI_ROLE_ENGINEER, HMI_LINK_HEALTHY, 200, &id3) == HMI_WRS_OK);
    CHECK(hmi_writes_take(&w, 200, &job));
    hmi_writes_finish_verified(&w, &job, good, 201);
    CHECK(hmi_writes_status(&w, id3, &r) && r.state == HMI_WR_CONFIRMED);

    /* expiry boundary: strictly older than ttl */
    CHECK(hmi_writes_submit(&w, 0, 7, HMI_ROLE_OPERATOR, HMI_LINK_HEALTHY, 1000, &id1) == HMI_WRS_OK);
    hmi_writes_expire(&w, 2000, false);                 /* age == ttl */
    CHECK(hmi_writes_status(&w, id1, &r) && r.state == HMI_WR_PENDING);
    hmi_writes_expire(&w, 2001, false);
    CHECK(hmi_writes_status(&w, id1, &r) && r.state == HMI_WR_EXPIRED);
    CHECK(!hmi_writes_take(&w, 2001, &job));
    /* all=true expires fresh ones too */
    CHECK(hmi_writes_submit(&w, 0, 8, HMI_ROLE_OPERATOR, HMI_LINK_HEALTHY, 3000, &id1) == HMI_WRS_OK);
    hmi_writes_expire(&w, 3000, true);
    CHECK(hmi_writes_status(&w, id1, &r) && r.state == HMI_WR_EXPIRED);

    /* audit: failure to record SENT skips that request and serves the next */
    hmi_writes_set_audit(&w, wr_audit, NULL);
    g_audit_fail_event = 2;
    g_audit_sent_calls = 0;
    CHECK(hmi_writes_submit(&w, 0, 1, HMI_ROLE_OPERATOR, HMI_LINK_HEALTHY, 4000, &id1) == HMI_WRS_OK);
    CHECK(hmi_writes_submit(&w, 0, 2, HMI_ROLE_OPERATOR, HMI_LINK_HEALTHY, 4000, &id2) == HMI_WRS_OK);
    CHECK(hmi_writes_take(&w, 4000, &job) && job.regs[0] == 2); /* first one was fail-closed */
    CHECK(hmi_writes_status(&w, id1, &r) && r.state == HMI_WR_REJECTED);
    hmi_writes_finish_verified(&w, &job, (uint16_t[]){ 2 }, 4001);
    /* audit failure at reserve: refused, no ring slot leaked */
    g_audit_fail_event = 1;
    for (int i = 0; i < 100; i++)
        CHECK(hmi_writes_submit(&w, 0, 1, HMI_ROLE_OPERATOR, HMI_LINK_HEALTHY, 5000, &id1) == HMI_WRS_ERR_AUDIT);
    g_audit_fail_event = 0;
    CHECK(hmi_writes_submit(&w, 0, 1, HMI_ROLE_OPERATOR, HMI_LINK_HEALTHY, 5000, &id1) == HMI_WRS_OK);

    hmi_writes_set_enabled(&w, false);
    CHECK(hmi_writes_submit(&w, 0, 1, HMI_ROLE_ADMIN, HMI_LINK_HEALTHY, 5000, &id1) == HMI_WRS_ERR_DISABLED);

    hmi_writes_stats_t st;
    hmi_writes_stats(&w, &st);
    CHECK(st.confirmed == 2 && st.rejected == 2 && st.unknown == 1 && st.verify_failed == 1 && st.expired == 2);
    CHECK(st.audit_failures >= 101);
    hmi_writes_destroy(&w);
    hmi_tagdb_destroy(&db);
}

/* ---- the user who asked is part of every audit record of the write ---- */
typedef struct {
    int n;
    char actor[16][HMI_ACTOR_LEN];
    char event[16][16];
} actor_log_t;

static bool actor_audit(void *u, const hmi_write_rec_t *r, const char *ev)
{
    actor_log_t *l = (actor_log_t *)u;
    if (l->n < 16) {
        snprintf(l->actor[l->n], HMI_ACTOR_LEN, "%s", r->actor);
        snprintf(l->event[l->n], sizeof(l->event[0]), "%s", ev);
        l->n++;
    }
    return true;
}

static void test_write_actor(void)
{
    SECTION("writes: the logged-in user is kept in the audit record");
    fx_t *f = fx_new(false);
    settle_connected(f);
    actor_log_t log;
    memset(&log, 0, sizeof(log));
    hmi_writes_set_audit(&f->w, actor_audit, &log);

    uint32_t id;
    hmi_write_rec_t r;
    CHECK(hmi_writes_submit_as(&f->w, 0, 42.0, HMI_ROLE_OPERATOR, "op1", hmi_comms_link_health(&f->c), g_now, &id) == HMI_WRS_OK);
    CHECK(hmi_writes_status(&f->w, id, &r) && strcmp(r.actor, "op1") == 0);
    run_until(f, g_now + 100);
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_CONFIRMED && strcmp(r.actor, "op1") == 0);
    CHECK(log.n >= 2);                                       /* RESERVED, SENT and the final record */
    int all_op1 = 1;
    for (int i = 0; i < log.n; i++) all_op1 &= strcmp(log.actor[i], "op1") == 0;
    CHECK(all_op1);                                          /* every record of this write names the user */

    /* the unnamed call and invalid names record "-" */
    int before = log.n;
    CHECK(submit(f, 0, 43.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    CHECK(hmi_writes_status(&f->w, id, &r) && strcmp(r.actor, "-") == 0);
    run_until(f, g_now + 100);
    CHECK(log.n > before && strcmp(log.actor[before], "-") == 0);
    const char *bad[] = { "", "ab", "Admin", "a b c", "evil\nuser", "user=admin", "abcdefghijklmnopqrstuvwxy", "Ã©tienne" };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        CHECK(hmi_writes_submit_as(&f->w, 0, 44.0, HMI_ROLE_OPERATOR, bad[k], hmi_comms_link_health(&f->c), g_now, &id) == HMI_WRS_OK);
        CHECK(hmi_writes_status(&f->w, id, &r) && strcmp(r.actor, "-") == 0);
        run_until(f, g_now + 100);
    }
    CHECK(hmi_writes_submit_as(&f->w, 0, 45.0, HMI_ROLE_OPERATOR, NULL, hmi_comms_link_health(&f->c), g_now, &id) == HMI_WRS_OK);
    CHECK(hmi_writes_status(&f->w, id, &r) && strcmp(r.actor, "-") == 0);
    run_until(f, g_now + 100);
    /* a name of exactly 24 characters is kept whole */
    CHECK(hmi_writes_submit_as(&f->w, 0, 46.0, HMI_ROLE_OPERATOR, "abcdefghijklmnopqrstuvwx", hmi_comms_link_health(&f->c), g_now, &id) == HMI_WRS_OK);
    CHECK(hmi_writes_status(&f->w, id, &r) && strcmp(r.actor, "abcdefghijklmnopqrstuvwx") == 0);
    run_until(f, g_now + 100);
    fx_free(f, false);
}

/* ---- rate limit and the second check before sending ---- */
static void two_tag_db(hmi_tag_cfg_t *t, hmi_tagdb_t *db)
{
    memset(t, 0, 2 * sizeof(*t));
    snprintf(t[0].name, HMI_NAME_LEN, "SP_A");
    t[0].dtype = HMI_DT_U16; t[0].scale = 1; t[0].eng_max = 1000; t[0].poll_ms = 100;
    t[0].write_allowed = true; t[0].write_role = HMI_ROLE_OPERATOR; t[0].write_max = 1000;
    snprintf(t[1].name, HMI_NAME_LEN, "SP_B");
    t[1].address = 10; t[1].dtype = HMI_DT_U16; t[1].scale = 1; t[1].eng_max = 1000; t[1].poll_ms = 100;
    t[1].write_allowed = true; t[1].write_role = HMI_ROLE_OPERATOR; t[1].write_max = 1000;
    CHECK(hmi_tagdb_init(db, t, 2, 0));
}

static hmi_wr_submit_t sub(hmi_writes_t *w, size_t tag, uint64_t now, uint32_t *id)
{
    return hmi_writes_submit(w, tag, 10, HMI_ROLE_OPERATOR, HMI_LINK_HEALTHY, now, id);
}

/* take and finish everything queued, so the queue never fills */
static void drain(hmi_writes_t *w, uint64_t now)
{
    hmi_write_job_t job;
    while (hmi_writes_take(w, now, &job)) hmi_writes_finish_exception(w, &job, 3, now);
}

static void test_write_rate_limit(void)
{
    SECTION("writes: rate limit per tag and per minute");
    hmi_tag_cfg_t t[2];
    hmi_tagdb_t db;
    two_tag_db(t, &db);
    hmi_writes_t w;
    CHECK(hmi_writes_init(&w, &db, true, 100000));
    uint32_t id, id2;
    hmi_write_rec_t r;

    /* without limits (the default of the bare module) nothing is refused */
    for (int i = 0; i < 5; i++) CHECK(sub(&w, 0, 100, &id) == HMI_WRS_OK);
    drain(&w, 100);

    /* at least 1000 ms between two writes to the same tag; another tag is independent */
    hmi_writes_set_limits(&w, 1000, 0);
    CHECK(sub(&w, 0, 5000, &id) == HMI_WRS_OK);
    CHECK(sub(&w, 0, 5500, &id2) == HMI_WRS_ERR_RATE_LIMITED);
    CHECK(sub(&w, 1, 5500, &id2) == HMI_WRS_OK);          /* a different tag has its own interval */
    CHECK(sub(&w, 0, 5999, &id2) == HMI_WRS_ERR_RATE_LIMITED);
    CHECK(sub(&w, 0, 6000, &id2) == HMI_WRS_OK);          /* exactly the interval: allowed */
    CHECK(id2 == id + 2);                                  /* refused requests took no id and no audit slot */
    CHECK(sub(&w, 0, 6500, &id) == HMI_WRS_ERR_RATE_LIMITED); /* the interval runs from the last ACCEPTED write */
    hmi_writes_stats_t st;
    hmi_writes_stats(&w, &st);
    CHECK(st.rate_limited == 3);
    drain(&w, 6600);

    /* at most 3 in any 60 s window */
    hmi_writes_set_limits(&w, 0, 3);
    CHECK(sub(&w, 0, 1000, &id) == HMI_WRS_OK);
    CHECK(sub(&w, 1, 2000, &id) == HMI_WRS_OK);
    CHECK(sub(&w, 0, 3000, &id) == HMI_WRS_OK);
    CHECK(sub(&w, 1, 4000, &id) == HMI_WRS_ERR_RATE_LIMITED);
    CHECK(sub(&w, 0, 60999, &id) == HMI_WRS_ERR_RATE_LIMITED); /* the write at 1000 is 59999 ms old */
    CHECK(sub(&w, 0, 61000, &id) == HMI_WRS_OK);               /* it is exactly a minute old: forgotten */
    CHECK(sub(&w, 0, 61500, &id) == HMI_WRS_ERR_RATE_LIMITED); /* 2000, 3000, 61000 count */
    CHECK(sub(&w, 1, 62000, &id) == HMI_WRS_OK);               /* 2000 is forgotten */
    drain(&w, 62500);
    CHECK(sub(&w, 0, 500000, &id) == HMI_WRS_OK);              /* a long quiet time empties the window */
    CHECK(sub(&w, 0, 500001, &id) == HMI_WRS_OK);
    CHECK(sub(&w, 0, 500002, &id) == HMI_WRS_OK);
    CHECK(sub(&w, 0, 500003, &id) == HMI_WRS_ERR_RATE_LIMITED);
    drain(&w, 500004);

    /* both limits together; a refused request uses up neither */
    hmi_writes_set_limits(&w, 1000, 2);
    CHECK(sub(&w, 0, 10000, &id) == HMI_WRS_OK);
    CHECK(sub(&w, 0, 10500, &id) == HMI_WRS_ERR_RATE_LIMITED);          /* too soon for the tag */
    CHECK(sub(&w, 1, 10500, &id) == HMI_WRS_OK);
    CHECK(sub(&w, 0, 11000, &id) == HMI_WRS_ERR_RATE_LIMITED);          /* window is full (2) although the tag is due */
    CHECK(sub(&w, 0, 70000, &id) == HMI_WRS_OK);                        /* a minute later both are free */

    /* the largest value is clamped, and set_limits starts from a clean state */
    hmi_writes_set_limits(&w, 1, 1000000);
    CHECK(w.max_per_minute == HMI_WRITE_RATE_MAX);
    drain(&w, 70001);
    hmi_writes_set_limits(&w, 1000, 1);
    CHECK(sub(&w, 0, 70002, &id) == HMI_WRS_OK);                        /* earlier history does not count */

    /* an unrelated refusal (role, range, queue) never counts as an accepted write */
    drain(&w, 70003);
    hmi_writes_set_limits(&w, 1000, 1);
    CHECK(hmi_writes_submit(&w, 0, 10, HMI_ROLE_VIEWER, HMI_LINK_HEALTHY, 80000, &id) == HMI_WRS_ERR_ROLE);
    CHECK(hmi_writes_submit(&w, 0, 5000, HMI_ROLE_OPERATOR, HMI_LINK_HEALTHY, 80000, &id) == HMI_WRS_ERR_RANGE);
    CHECK(sub(&w, 0, 80001, &id) == HMI_WRS_OK);
    CHECK(hmi_writes_status(&w, id, &r) && r.state == HMI_WR_PENDING);
    hmi_writes_destroy(&w);
    hmi_tagdb_destroy(&db);
}

static char g_rv_events[8][32];
static int g_rv_n;
static bool rv_audit(void *u, const hmi_write_rec_t *r, const char *ev)
{
    (void)u; (void)r;
    if (g_rv_n < 8) snprintf(g_rv_events[g_rv_n], sizeof(g_rv_events[0]), "%s", ev);
    g_rv_n++;
    return true;
}

static void test_write_revalidation(void)
{
    SECTION("writes: a queued request is checked again just before it is sent");
    hmi_tag_cfg_t t[2];
    hmi_tagdb_t db;
    two_tag_db(t, &db);
    hmi_writes_t w;
    CHECK(hmi_writes_init(&w, &db, true, 100000));
    hmi_writes_set_audit(&w, rv_audit, NULL);
    uint32_t id;
    hmi_write_job_t job;
    hmi_write_rec_t r;
    hmi_writes_stats_t st;

    /* unchanged: it is sent */
    CHECK(sub(&w, 0, 100, &id) == HMI_WRS_OK);
    CHECK(hmi_writes_take(&w, 100, &job) && job.tag == 0);
    hmi_writes_finish_exception(&w, &job, 3, 101);

    /* master switch turned off while queued: not sent, said so in the audit trail */
    g_rv_n = 0;
    CHECK(sub(&w, 0, 200, &id) == HMI_WRS_OK);
    hmi_writes_set_enabled(&w, false);
    CHECK(!hmi_writes_take(&w, 200, &job));
    CHECK(hmi_writes_status(&w, id, &r) && r.state == HMI_WR_REJECTED);
    CHECK(g_rv_n == 2 && strcmp(g_rv_events[0], "RESERVED") == 0 && strcmp(g_rv_events[1], "REJECTED_REVALIDATION") == 0);
    hmi_writes_stats(&w, &st);
    CHECK(st.revalidation_failed == 1);
    hmi_writes_set_enabled(&w, true);

    /* one rejected request does not hide the next valid one */
    CHECK(sub(&w, 0, 300, &id) == HMI_WRS_OK);
    uint32_t id2;
    CHECK(sub(&w, 1, 300, &id2) == HMI_WRS_OK);
    hmi_mutex_lock(&w.lock);
    for (int i = 0; i < HMI_AUDIT_LEN; i++) if (w.ring[i].state == HMI_WR_PENDING && w.ring[i].id == id) w.ring[i].raw ^= 1u; /* damaged entry */
    hmi_mutex_unlock(&w.lock);
    CHECK(hmi_writes_take(&w, 300, &job) && job.tag == 1);
    CHECK(hmi_writes_status(&w, id, &r) && r.state == HMI_WR_REJECTED);
    hmi_writes_finish_exception(&w, &job, 3, 301);

    /* each fact that is checked again, changed one at a time */
    struct { const char *what; int kind; } cases[] = {
        { "role lowered below the tag's minimum", 0 }, { "value above the write limit", 1 }, { "value below the raised lower limit", 2 },
        { "value not a number", 3 }, { "tag no longer writable", 4 }, { "tag now an input register", 5 },
        { "tag index out of range", 6 }, { "encoding differs from the audited one", 7 },
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        uint64_t now = 1000 + k * 10;
        hmi_tagdb_destroy(&db);
        two_tag_db(t, &db);
        CHECK(sub(&w, 0, now, &id) == HMI_WRS_OK);
        hmi_mutex_lock(&w.lock);
        hmi_write_rec_t *e = NULL;
        for (int i = 0; i < HMI_AUDIT_LEN; i++) if (w.ring[i].state == HMI_WR_PENDING && w.ring[i].id == id) e = &w.ring[i];
        switch (cases[k].kind) {
            case 0: e->role = HMI_ROLE_VIEWER; break;
            case 1: e->eng = 1001; e->raw = 1001; break;       /* consistently encoded, so only the range fails */
            case 2: db.cfg[0].write_min = 50; e->eng = 20; e->raw = 20; break;
            case 3: e->eng = NAN; break;
            case 4: db.cfg[0].write_allowed = false; break;
            case 5: db.cfg[0].area = HMI_AREA_INPUT; break;
            case 6: e->tag = 200; break;
            case 7: e->raw += 1; break;
        }
        hmi_mutex_unlock(&w.lock);
        CHECK(!hmi_writes_take(&w, now, &job));
        CHECK(hmi_writes_status(&w, id, &r) && r.state == HMI_WR_REJECTED);
        if (!(r.state == HMI_WR_REJECTED)) printf("  case: %s\n", cases[k].what);
        hmi_tagdb_destroy(&db);
        two_tag_db(t, &db);
    }
    hmi_writes_stats(&w, &st);
    CHECK(st.revalidation_failed == 1 + 1 + sizeof(cases) / sizeof(cases[0]));
    hmi_writes_destroy(&w);
    hmi_tagdb_destroy(&db);
}

/* ---- asynchronous audit of the RESERVED record (storage worker) ---- */
typedef struct {
    int n;
    uint32_t id[16];
    hmi_write_rec_t rec[16];
    bool ret;                 /* the hook's answer */
    bool complete_inside;     /* report the record durable before the hook returns */
    hmi_writes_t *w;
    int sync_n;               /* calls of the synchronous hook */
    char sync_ev[16][16];
} async_log_t;

static bool async_hook(void *u, const hmi_write_rec_t *r, const char *ev)
{
    async_log_t *l = (async_log_t *)u;
    CHECK(strcmp(ev, "RESERVED") == 0);
    if (!l->ret) return false;
    if (l->n < 16) {
        l->id[l->n] = r->id;
        l->rec[l->n] = *r;
        l->n++;
    }
    if (l->complete_inside) hmi_writes_audit_done(l->w, r->id, true, 0);
    return true;
}

static bool sync_after(void *u, const hmi_write_rec_t *r, const char *ev)
{
    (void)r;
    async_log_t *l = (async_log_t *)u;
    if (l->sync_n < 16) snprintf(l->sync_ev[l->sync_n], sizeof(l->sync_ev[0]), "%s", ev);
    l->sync_n++;
    return true;
}

static void test_write_async_audit(void)
{
    SECTION("writes: asynchronous RESERVED record, request waits until it is durable");
    fx_t *f = fx_new(false);
    settle_connected(f);
    async_log_t l;
    memset(&l, 0, sizeof(l));
    l.ret = true;
    l.w = &f->w;
    hmi_writes_set_audit(&f->w, sync_after, &l);
    hmi_writes_set_audit_async(&f->w, async_hook, &l);
    hmi_write_rec_t r;
    uint32_t id = 0;

    /* the call returns at once; the request is RESERVED and is not transmitted */
    hmi_tag_value_t before = val(f, 0);
    CHECK(submit(f, 0, 42.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    CHECK(l.n == 1 && l.id[0] == id);
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_RESERVED);
    CHECK(l.rec[0].old_quality == HMI_Q_GOOD && l.rec[0].old_eng == before.eng); /* the value being overwritten */
    CHECK(strcmp(l.rec[0].actor, "-") == 0 && l.rec[0].eng == 42.0);
    uint64_t t0 = g_now;
    run_until(f, g_now + 300);
    CHECK(f->m.n_write == 0);                                      /* never sent while not durable */
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_RESERVED);
    CHECK(l.sync_n == 0);                                          /* RESERVED does not use the synchronous hook */

    /* durable: PENDING, then sent through the normal path */
    hmi_writes_audit_done(&f->w, id, true, g_now);
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_PENDING);
    hmi_writes_audit_done(&f->w, id, true, g_now);                 /* a repeated report changes nothing */
    run_until(f, g_now + 100);
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_CONFIRMED);
    CHECK(f->m.hold[0] == 420 && f->m.n_write == 1);
    CHECK(l.sync_n >= 2 && strcmp(l.sync_ev[0], "SENT") == 0 && strcmp(l.sync_ev[l.sync_n - 1], "CONFIRMED") == 0);
    (void)t0;

    /* the record fails: REJECTED, never transmitted, counted */
    uint32_t writes = f->m.n_write;
    CHECK(submit(f, 0, 43.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    hmi_writes_audit_done(&f->w, id, false, g_now);
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_REJECTED);
    run_until(f, g_now + 100);
    CHECK(f->m.n_write == writes);
    hmi_writes_stats_t st;
    hmi_writes_stats(&f->w, &st);
    CHECK(st.rejected >= 1 && st.audit_failures >= 1);

    /* the hook cannot queue the record: refused at once, no slot is leaked */
    l.ret = false;
    for (int i = 0; i < 100; i++) CHECK(submit(f, 0, 44.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_ERR_AUDIT);
    l.ret = true;
    CHECK(submit(f, 0, 44.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    hmi_writes_audit_done(&f->w, id, true, g_now);
    run_until(f, g_now + 100);
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_CONFIRMED);

    /* a record that completes inside the hook (before the call returns) */
    l.complete_inside = true;
    CHECK(submit(f, 0, 45.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_PENDING);
    run_until(f, g_now + 100);
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_CONFIRMED);
    l.complete_inside = false;

    /* unknown ids are ignored */
    hmi_writes_audit_done(&f->w, 999999, true, g_now);
    hmi_writes_audit_done(&f->w, 0, true, g_now);
    fx_free(f, false);
}

static void test_write_async_capacity_and_expiry(void)
{
    SECTION("writes: reserved requests count against the queue and expire if never made durable");
    fx_t *f = fx_new(false);
    settle_connected(f);
    async_log_t l;
    memset(&l, 0, sizeof(l));
    l.ret = true;
    l.w = &f->w;
    hmi_writes_set_audit_async(&f->w, async_hook, &l);
    uint32_t ids[HMI_WRITE_QUEUE_LEN], id;
    hmi_write_rec_t r;

    for (int i = 0; i < HMI_WRITE_QUEUE_LEN; i++) CHECK(submit(f, 0, (double)i, HMI_ROLE_OPERATOR, &ids[i]) == HMI_WRS_OK);
    CHECK(submit(f, 0, 1.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_ERR_QUEUE_FULL); /* none is queued yet, all are reserved */
    for (int i = 0; i < HMI_WRITE_QUEUE_LEN; i++) hmi_writes_audit_done(&f->w, ids[i], true, g_now);
    for (int i = 0; i < HMI_WRITE_QUEUE_LEN; i++) CHECK(hmi_writes_status(&f->w, ids[i], &r) && r.state == HMI_WR_PENDING);
    CHECK(f->w.q_len == HMI_WRITE_QUEUE_LEN); /* never more than the queue holds */
    run_until(f, g_now + 500);
    CHECK(f->m.n_write == HMI_WRITE_QUEUE_LEN); /* and they go out in order, once each */

    /* a record that never completes: the request expires, untransmitted, and a late report is ignored */
    CHECK(submit(f, 0, 77.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    uint32_t writes = f->m.n_write;
    hmi_writes_expire(&f->w, g_now + 6000, false); /* time to live is 5000 ms */
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_EXPIRED);
    hmi_writes_audit_done(&f->w, id, true, g_now + 6001);
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_EXPIRED); /* not resurrected */
    run_until(f, g_now + 500);
    CHECK(f->m.n_write == writes);

    /* link declared lost expires reserved requests too */
    CHECK(submit(f, 0, 78.0, HMI_ROLE_OPERATOR, &id) == HMI_WRS_OK);
    hmi_writes_expire(&f->w, g_now, true);
    CHECK(hmi_writes_status(&f->w, id, &r) && r.state == HMI_WR_EXPIRED);
    fx_free(f, false);
}

int main(void)
{
    test_write_rate_limit();
    test_write_revalidation();
    test_write_async_audit();
    test_write_async_capacity_and_expiry();
    test_write_actor();
    test_write_confirmed();
    test_write_disabled();
    test_write_exception();
    test_write_lost_reply_never_replayed();
    test_write_verify_failed();
    test_write_readback_lost();
    test_write_expiry_on_link_loss();
    test_write_dropped_on_link_loss();
    test_audit_fail_closed();
    test_write_queue_and_audit_capacity();
    test_writes_module();
    return test_finish();
}
