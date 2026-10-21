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

int main(void)
{
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
