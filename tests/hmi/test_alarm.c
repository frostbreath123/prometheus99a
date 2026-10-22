/* Tests for hmi_alarm: state machine, frozen on bad data, persistence.
 * Part of the HMI core host tests; shared fixtures are in test_support.h. */
#include "test_support.h"

/* ---- alarms ---- */
static void al_run(fx_t *f, uint64_t dur, uint16_t raw_lvl, bool link_lost)
{
    f->m.hold[0] = raw_lvl;
    uint64_t end = g_now + dur;
    while (g_now < end) {
        if (!link_lost) hmi_comms_step(&f->c);
        hmi_tagdb_age(&f->db, g_now);
        hmi_alarm_step(&f->al, g_now, link_lost ? HMI_LINK_NO_RESPONSE : HMI_LINK_HEALTHY);
        g_now += 10;
    }
}

static void test_alarm_flow(void)
{
    SECTION("alarms: debounce, hysteresis, ack, escalation, RTN-unack, clear");
    fx_t *f = fx_new(true);
    hmi_alarm_point_t p;
    al_run(f, 400, 500, false); /* 50 % normal */
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_NORMAL);

    al_run(f, 200, 850, false); /* 85 %, shorter than 300 ms debounce */
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_NORMAL);
    al_run(f, 600, 500, false);
    al_run(f, 600, 850, false);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK && p.kind == HMI_ALK_HI);

    /* hysteresis: 79 % is inside the 2 % deadband, stays HI */
    al_run(f, 800, 790, false);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK && p.kind == HMI_ALK_HI);

    CHECK(hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_OK);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_ACK);
    CHECK(hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_NOTHING); /* second ack is a no-op */

    /* escalation after ack re-activates */
    al_run(f, 700, 970, false);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK && p.kind == HMI_ALK_HIHI);
    CHECK(hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_OK);

    /* return to normal after ack: cleared without further action */
    al_run(f, 1000, 500, false);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_NORMAL);

    /* return to normal BEFORE ack: RTN_UNACK until acknowledged */
    al_run(f, 700, 850, false);
    al_run(f, 700, 500, false);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_RTN_UNACK);
    CHECK(hmi_alarm_count_unacked(&f->al) == 1);
    CHECK(hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_OK);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_NORMAL);

    hmi_alarm_log_t j[HMI_JOURNAL_LEN];
    size_t n = hmi_alarm_journal_copy(&f->al, j, HMI_JOURNAL_LEN);
    CHECK(n >= 8);
    for (size_t i = 1; i < n; i++) CHECK(j[i].seq == j[i - 1].seq + 1);
    fx_free(f, true);
}

static void test_alarm_bad_quality_freezes(void)
{
    SECTION("alarms: BAD data freezes state, ack works while disconnected, comms alarm");
    fx_t *f = fx_new(true);
    al_run(f, 400, 500, false);
    al_run(f, 700, 850, false);
    hmi_alarm_point_t p;
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK);

    /* PLC link goes away: tags BAD */
    hmi_tagdb_link_lost(&f->db);
    al_run(f, 1500, 850, true);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK && p.data_suppressed);
    hmi_alarm_point_t cp;
    hmi_alarm_get(&f->al, f->db.n_tags, &cp);
    CHECK(cp.state == HMI_AL_ACTIVE_UNACK && cp.kind == HMI_ALK_COMMS);

    /* acknowledge while disconnected */
    CHECK(hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_OK);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_ACK);

    /* link back, value now normal -> clears; comms alarm goes RTN_UNACK */
    al_run(f, 800, 500, false);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_NORMAL);
    hmi_alarm_get(&f->al, f->db.n_tags, &cp);
    CHECK(cp.state == HMI_AL_RTN_UNACK);
    fx_free(f, true);
}

static void test_alarm_persistence(void)
{
    SECTION("alarms: persistence round trip and corruption rejection");
    fx_t *f = fx_new(true);
    al_run(f, 400, 500, false);
    al_run(f, 700, 960, false);
    hmi_alarm_point_t p;
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK && p.kind == HMI_ALK_HIHI);
    const char *path = "alarm_state_test.bin";
    remove(path);
    CHECK(hmi_alarm_save(&f->al, path));
    fx_free(f, true);

    fx_t *g = fx_new(true);
    CHECK(hmi_alarm_load(&g->al, path));
    hmi_alarm_get(&g->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK && p.kind == HMI_ALK_HIHI);
    fx_free(g, true);

    /* flip one byte: CRC must reject */
    FILE *fp = fopen(path, "r+b");
    fseek(fp, 20, SEEK_SET);
    int c = fgetc(fp);
    fseek(fp, 20, SEEK_SET);
    fputc(c ^ 0xFF, fp);
    fclose(fp);
    g = fx_new(true);
    CHECK(!hmi_alarm_load(&g->al, path));
    hmi_alarm_get(&g->al, 0, &p);
    CHECK(p.state == HMI_AL_NORMAL); /* untouched on failure */
    CHECK(!hmi_alarm_load(&g->al, "no_such_alarm_file.bin"));
    fx_free(g, true);
    remove(path);
}


/* ---- acknowledgement authorization and audit ---- */
typedef struct {
    int calls;
    bool ret;                       /* what the hook reports: durable or not */
    hmi_ack_audit_t last;
    hmi_alarm_t *al;                /* to observe state at the moment of the call */
    hmi_alarm_state_t state0_during;
    bool poke_since;                /* simulate the engine changing the alarm mid-audit */
} ack_rec_t;

static bool ack_hook(void *user, const hmi_ack_audit_t *rec)
{
    ack_rec_t *r = (ack_rec_t *)user;
    r->calls++;
    r->last = *rec;
    if (r->al) {
        hmi_alarm_point_t p;
        hmi_alarm_get(r->al, 0, &p);
        r->state0_during = p.state;
        if (r->poke_since) {
            hmi_mutex_lock(&r->al->lock);
            r->al->pt[0].since_ms += 1;
            hmi_mutex_unlock(&r->al->lock);
        }
    }
    return r->ret;
}

/* Two alarms awaiting acknowledgement: tag 0 HI and the comms alarm. */
static fx_t *fx_two_unacked(void)
{
    fx_t *f = fx_new(true);
    al_run(f, 600, 500, false);
    al_run(f, 600, 850, false);
    al_run(f, 1500, 850, true);
    return f;
}

static bool same_alarm_state(const hmi_alarm_point_t *a, const hmi_alarm_point_t *b)
{
    return a->state == b->state && a->kind == b->kind && a->acked_rank == b->acked_rank &&
           a->since_ms == b->since_ms && a->data_suppressed == b->data_suppressed;
}

static void test_ack_denied_changes_nothing(void)
{
    SECTION("alarms: Viewer acknowledgement is denied and changes nothing");
    fx_t *f = fx_two_unacked();
    size_t nt = hmi_tagdb_count(&f->db);
    CHECK(hmi_alarm_count_unacked(&f->al) == 2);

    hmi_alarm_point_t before[HMI_MAX_TAGS + 1], after;
    for (size_t i = 0; i <= nt; i++) hmi_alarm_get(&f->al, i, &before[i]);
    hmi_alarm_log_t jb[HMI_JOURNAL_LEN], ja[HMI_JOURNAL_LEN];
    size_t njb = hmi_alarm_journal_copy(&f->al, jb, HMI_JOURNAL_LEN);

    CHECK(hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    CHECK(hmi_alarm_ack(&f->al, nt, g_now, HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, (hmi_role_t)77) == HMI_ACK_DENIED); /* corrupt role */
    CHECK(hmi_alarm_ack(&f->al, 0, g_now, (hmi_role_t)-1) == HMI_ACK_DENIED);

    bool same = true;
    for (size_t i = 0; i <= nt; i++) {
        hmi_alarm_get(&f->al, i, &after);
        if (!same_alarm_state(&before[i], &after)) same = false;
    }
    CHECK(same);                                   /* every alarm byte-for-byte unchanged */
    CHECK(hmi_alarm_count_unacked(&f->al) == 2);
    size_t nja = hmi_alarm_journal_copy(&f->al, ja, HMI_JOURNAL_LEN);
    CHECK(nja == njb && (nja == 0 || memcmp(jb, ja, nja * sizeof(ja[0])) == 0)); /* no ACKED event */
    fx_free(f, true);
}

static void test_ack_operator_allowed(void)
{
    SECTION("alarms: Operator and above may acknowledge, single and ACK ALL");
    fx_t *f = fx_two_unacked();
    size_t nt = hmi_tagdb_count(&f->db);
    hmi_alarm_point_t p;

    CHECK(hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_OK);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_ACK);
    hmi_alarm_get(&f->al, nt, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK);         /* the other alarm is untouched */
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_ENGINEER) == HMI_ACK_OK);
    CHECK(hmi_alarm_count_unacked(&f->al) == 0);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_ADMIN) == HMI_ACK_NOTHING);
    CHECK(hmi_alarm_ack(&f->al, nt + 5, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_NOTHING); /* invalid point */
    CHECK(hmi_alarm_ack(&f->al, nt + 5, g_now, HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    fx_free(f, true);
}

static void test_ack_actor(void)
{
    SECTION("alarms: the user who acknowledged is passed to the audit hook");
    fx_t *f = fx_two_unacked();
    ack_rec_t r;
    memset(&r, 0, sizeof(r));
    r.ret = true;
    hmi_alarm_set_ack_audit(&f->al, ack_hook, &r);

    CHECK(hmi_alarm_ack_all_as(&f->al, g_now, HMI_ROLE_VIEWER, "view.er") == HMI_ACK_DENIED);
    CHECK(r.calls == 1 && strcmp(r.last.actor, "view.er") == 0);          /* denied attempts name the user too */
    CHECK(hmi_alarm_ack_as(&f->al, 0, g_now, HMI_ROLE_OPERATOR, "op1") == HMI_ACK_OK);
    CHECK(r.calls == 2 && strcmp(r.last.actor, "op1") == 0 && r.last.result == HMI_ACK_OK);
    CHECK(hmi_alarm_ack(&f->al, hmi_tagdb_count(&f->db), g_now, HMI_ROLE_OPERATOR) == HMI_ACK_OK);
    CHECK(r.calls == 3 && strcmp(r.last.actor, "-") == 0);                /* the unnamed call */
    CHECK(hmi_alarm_ack_all_as(&f->al, g_now, HMI_ROLE_ADMIN, "admin") == HMI_ACK_NOTHING);
    CHECK(r.calls == 3);                                                  /* nothing to do: still no record */
    fx_free(f, true);

    f = fx_two_unacked();
    memset(&r, 0, sizeof(r));
    r.ret = true;
    hmi_alarm_set_ack_audit(&f->al, ack_hook, &r);
    const char *bad[] = { "", "ab", "Admin", "a b", "x=y", "line\nbreak", NULL };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        CHECK(hmi_alarm_ack_all_as(&f->al, g_now, HMI_ROLE_VIEWER, bad[k]) == HMI_ACK_DENIED);
        CHECK(strcmp(r.last.actor, "-") == 0);
    }
    fx_free(f, true);
}

static void test_ack_audit_policy(void)
{
    SECTION("alarms: acknowledgement audit hook, ordering and fail-closed policy");
    fx_t *f = fx_two_unacked();
    size_t nt = hmi_tagdb_count(&f->db);
    ack_rec_t r;
    memset(&r, 0, sizeof(r));
    r.ret = true;
    r.al = &f->al;
    hmi_alarm_set_ack_audit(&f->al, ack_hook, &r);
    hmi_alarm_point_t p;

    /* denied attempt: recorded with role and the alarms it targeted, no effect */
    CHECK(hmi_alarm_ack_all(&f->al, 4242, HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    CHECK(r.calls == 1 && r.last.result == HMI_ACK_DENIED && r.last.role == HMI_ROLE_VIEWER);
    CHECK(r.last.all && r.last.t_ms == 4242 && r.last.n_points == 2);
    CHECK(r.last.points[0] == 0 && r.last.points[1] == nt);
    CHECK(hmi_alarm_count_unacked(&f->al) == 2);

    /* denied single attempt names its alarm; an invalid point names none */
    CHECK(hmi_alarm_ack(&f->al, nt, g_now, HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    CHECK(r.calls == 2 && !r.last.all && r.last.n_points == 1 && r.last.points[0] == nt);
    CHECK(hmi_alarm_ack(&f->al, nt + 9, g_now, HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    CHECK(r.calls == 3 && r.last.n_points == 0);

    /* granted: the record exists BEFORE the state changes */
    CHECK(hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_OK);
    CHECK(r.calls == 4 && r.last.result == HMI_ACK_OK && r.last.role == HMI_ROLE_OPERATOR);
    CHECK(r.last.n_points == 1 && r.last.points[0] == 0 && !r.last.all);
    CHECK(r.state0_during == HMI_AL_ACTIVE_UNACK); /* still unacknowledged inside the hook */
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_ACK);

    /* nothing to acknowledge: no privileged effect, no record */
    int calls = r.calls;
    CHECK(hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_NOTHING);
    CHECK(r.calls == calls);

    /* audit cannot be recorded: acknowledgement refused, nothing changes */
    r.ret = false;
    CHECK(f->al.ack_audit_failures == 0);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_AUDIT_FAILED);
    CHECK(f->al.ack_audit_failures == 1);
    hmi_alarm_get(&f->al, nt, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK);
    CHECK(hmi_alarm_count_unacked(&f->al) == 1);
    /* a denied attempt stays denied even if its record also fails */
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    CHECK(f->al.ack_audit_failures == 2);

    /* recovery: once the trail works again the same request succeeds */
    r.ret = true;
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_OK);
    CHECK(hmi_alarm_count_unacked(&f->al) == 0);
    fx_free(f, true);

    /* an alarm that changes while the record is being written is not acknowledged unseen */
    f = fx_two_unacked();
    memset(&r, 0, sizeof(r));
    r.ret = true;
    r.al = &f->al;
    r.poke_since = true;
    hmi_alarm_set_ack_audit(&f->al, ack_hook, &r);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK);
    CHECK(hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_NOTHING);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK);
    fx_free(f, true);
}

/* ---- journal sink: outside the lock, in order ---- */
typedef struct {
    hmi_alarm_t *al;
    int n;
    uint32_t seq[256];
    uint8_t event[256];
    bool ret;
    int reentered;          /* calls that could read the alarm state, which needs the lock */
    int in_sink;            /* two threads in the sink at once would make this 2 */
    int max_in_sink;
} sink_log_t;

static bool log_sink(void *u, const hmi_alarm_log_t *e)
{
    sink_log_t *s = (sink_log_t *)u;
    s->in_sink++;
    if (s->in_sink > s->max_in_sink) s->max_in_sink = s->in_sink;
    if (s->n < 256) {
        s->seq[s->n] = e->seq;
        s->event[s->n] = e->event;
    }
    s->n++;
    hmi_alarm_point_t p;
    hmi_alarm_get(s->al, e->point, &p); /* takes the alarm lock: a sink called under it would deadlock here */
    s->reentered++;
    s->in_sink--;
    return s->ret;
}

static void test_alarm_sink(void)
{
    SECTION("alarms: journal sink runs outside the alarm lock, in order, one caller at a time");
    fx_t *f = fx_new(true);
    sink_log_t s;
    memset(&s, 0, sizeof(s));
    s.al = &f->al;
    s.ret = true;
    hmi_alarm_set_sink(&f->al, log_sink, &s);
    uint32_t rev0 = hmi_alarm_state_rev(&f->al);

    al_run(f, 400, 500, false);
    CHECK(s.n == 0);
    CHECK(hmi_alarm_state_rev(&f->al) == rev0); /* nothing changed */
    al_run(f, 700, 850, false);                 /* HI raised */
    CHECK(s.n == 1 && s.event[0] == HMI_ALE_RAISED && s.reentered == 1);
    uint32_t rev1 = hmi_alarm_state_rev(&f->al);
    CHECK(rev1 != rev0);
    CHECK(hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_OK); /* acknowledgement is delivered too */
    CHECK(s.n == 2 && s.event[1] == HMI_ALE_ACKED);
    CHECK(hmi_alarm_state_rev(&f->al) != rev1);
    for (int i = 1; i < s.n; i++) CHECK(s.seq[i] == s.seq[i - 1] + 1);

    /* a sink that refuses is counted, and does not stop the engine */
    s.ret = false;
    al_run(f, 1500, 500, false);                /* clears */
    CHECK(s.n == 3 && f->al.sink_failures == 1);
    hmi_alarm_point_t p;
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_NORMAL);
    s.ret = true;

    /* the in-memory journal is unchanged by having a sink */
    hmi_alarm_log_t j[HMI_JOURNAL_LEN];
    CHECK(hmi_alarm_journal_copy(&f->al, j, HMI_JOURNAL_LEN) == 3 && j[2].event == HMI_ALE_CLEARED);

    /* removing the sink: nothing is queued for it any more */
    hmi_alarm_set_sink(&f->al, NULL, NULL);
    al_run(f, 700, 850, false);
    CHECK(s.n == 3 && f->al.sq_count == 0);
    fx_free(f, true);

    /* entries are not lost while no thread drains the queue, and a full queue counts the loss */
    f = fx_new(true);
    memset(&s, 0, sizeof(s));
    s.al = &f->al;
    s.ret = true;
    hmi_alarm_set_sink(&f->al, log_sink, &s);
    hmi_mutex_lock(&f->al.lock);
    f->al.sink_flushing = true; /* pretend another thread is delivering */
    hmi_mutex_unlock(&f->al.lock);
    al_run(f, 400, 500, false);
    al_run(f, 700, 850, false); /* raises: queued, not delivered, because "someone else" is flushing */
    CHECK(s.n == 0 && f->al.sq_count == 1);
    hmi_mutex_lock(&f->al.lock);
    f->al.sink_flushing = false;
    hmi_mutex_unlock(&f->al.lock);
    hmi_alarm_step(&f->al, g_now, HMI_LINK_HEALTHY); /* the next pass delivers what is waiting */
    CHECK(s.n == 1 && f->al.sq_count == 0 && f->al.sink_dropped == 0);

    /* more events than the queue holds while nobody delivers: the loss is counted, the journal in memory is whole */
    hmi_mutex_lock(&f->al.lock);
    f->al.sink_flushing = true;
    hmi_mutex_unlock(&f->al.lock);
    for (int cycle = 0; cycle < 50; cycle++) { /* raised, acknowledged, cleared: three entries each */
        al_run(f, 700, 850, false);
        hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_OPERATOR);
        al_run(f, 1000, 500, false);
    }
    CHECK(f->al.sq_count == HMI_SINK_PENDING);
    CHECK(f->al.sink_dropped >= 50 * 3 - HMI_SINK_PENDING - 1);
    hmi_mutex_lock(&f->al.lock);
    f->al.sink_flushing = false;
    hmi_mutex_unlock(&f->al.lock);
    int before = s.n;
    hmi_alarm_step(&f->al, g_now, HMI_LINK_HEALTHY);
    CHECK(s.n - before == HMI_SINK_PENDING && f->al.sq_count == 0 && s.max_in_sink == 1);
    fx_free(f, true);
}

static void test_alarm_state_bytes(void)
{
    SECTION("alarms: persisted state as bytes, journal restore");
    fx_t *f = fx_new(true);
    al_run(f, 400, 500, false);
    al_run(f, 700, 960, false);
    uint8_t buf[HMI_ALARM_STATE_MAX];
    uint32_t rev = 0;
    CHECK(hmi_alarm_state_serialize(&f->al, buf, 8, &rev) == 0); /* too small */
    size_t n = hmi_alarm_state_serialize(&f->al, buf, sizeof(buf), &rev);
    CHECK(n > 0 && n <= HMI_ALARM_STATE_MAX && rev == hmi_alarm_state_rev(&f->al));
    size_t n2 = hmi_alarm_state_serialize(&f->al, buf, sizeof(buf), NULL); /* rev is optional */
    CHECK(n2 == n);

    fx_t *g = fx_new(true);
    uint32_t grev = hmi_alarm_state_rev(&g->al);
    CHECK(hmi_alarm_state_apply(&g->al, buf, n));
    hmi_alarm_point_t p;
    hmi_alarm_get(&g->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK && p.kind == HMI_ALK_HIHI);
    CHECK(hmi_alarm_state_rev(&g->al) != grev); /* applying counts as a change */

    /* every wrong size, and every single flipped bit, is rejected without touching the state */
    fx_t *h = fx_new(true);
    bool sizes_rejected = true;
    for (size_t k = 0; k < n; k++) if (hmi_alarm_state_apply(&h->al, buf, k)) sizes_rejected = false;
    CHECK(sizes_rejected && !hmi_alarm_state_apply(&h->al, buf, n + 1));
    bool flips_rejected = true;
    for (size_t i = 0; i < n; i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t c[HMI_ALARM_STATE_MAX];
            memcpy(c, buf, n);
            c[i] = (uint8_t)(c[i] ^ (1 << bit));
            if (hmi_alarm_state_apply(&h->al, c, n)) flips_rejected = false;
        }
    }
    CHECK(flips_rejected);
    hmi_alarm_get(&h->al, 0, &p);
    CHECK(p.state == HMI_AL_NORMAL);
    fx_free(h, true);

    /* the file calls and the byte calls agree */
    const char *path = "alarm_state_bytes_test.bin";
    remove(path);
    CHECK(hmi_alarm_save(&f->al, path));
    uint8_t fb[HMI_ALARM_STATE_MAX];
    size_t fl = 0;
    CHECK(hmi_fs_read_all(path, fb, sizeof(fb), &fl) == HMI_FS_READ_OK && fl == n && memcmp(fb, buf, n) == 0);
    remove(path);

    /* journal restore: oldest first, the newest are kept, the sequence continues, no sink call */
    sink_log_t s;
    memset(&s, 0, sizeof(s));
    s.al = &g->al;
    s.ret = true;
    hmi_alarm_set_sink(&g->al, log_sink, &s);
    hmi_alarm_log_t ev[HMI_JOURNAL_LEN + 10];
    memset(ev, 0, sizeof(ev));
    for (size_t i = 0; i < HMI_JOURNAL_LEN + 10; i++) {
        ev[i].seq = (uint32_t)(100 + i);
        ev[i].ts_ms = 1000 + i;
        ev[i].event = HMI_ALE_RAISED;
    }
    hmi_alarm_journal_restore(&g->al, ev, 3);
    hmi_alarm_log_t out[HMI_JOURNAL_LEN];
    CHECK(hmi_alarm_journal_copy(&g->al, out, HMI_JOURNAL_LEN) == 3);
    CHECK(out[0].seq == 100 && out[2].seq == 102);
    hmi_alarm_journal_restore(&g->al, ev, HMI_JOURNAL_LEN + 10);
    CHECK(hmi_alarm_journal_copy(&g->al, out, HMI_JOURNAL_LEN) == HMI_JOURNAL_LEN);
    CHECK(out[0].seq == 110 && out[HMI_JOURNAL_LEN - 1].seq == 100 + HMI_JOURNAL_LEN + 9); /* the newest 64 */
    CHECK(s.n == 0);
    al_run(g, 400, 500, false);
    al_run(g, 800, 850, false); /* a new event continues the numbering */
    CHECK(s.n >= 1 && s.seq[0] == 100 + HMI_JOURNAL_LEN + 10);
    hmi_alarm_journal_restore(&g->al, ev, 0); /* an empty restore empties the journal */
    CHECK(hmi_alarm_journal_copy(&g->al, out, HMI_JOURNAL_LEN) == 0);
    fx_free(g, true);
    fx_free(f, true);
}

/* ---- two-phase acknowledgement with an asynchronous audit hook ---- */
typedef struct {
    int calls;
    uint32_t token[16];
    hmi_ack_audit_t rec[16];
    bool ret;
    bool complete_inside;   /* report the record durable inside the hook */
    hmi_alarm_t *al;
} async_ack_t;

static bool async_ack_hook(void *u, const hmi_ack_audit_t *rec, uint32_t token)
{
    async_ack_t *a = (async_ack_t *)u;
    if (!a->ret) return false;
    if (a->calls < 16) {
        a->token[a->calls] = token;
        a->rec[a->calls] = *rec;
    }
    a->calls++;
    if (a->complete_inside && token) hmi_alarm_ack_audit_done(a->al, token, true, g_now);
    return true;
}

static bool sync_ack_never(void *u, const hmi_ack_audit_t *rec)
{
    (void)rec;
    ++*(int *)u;
    return true;
}

static void test_ack_async(void)
{
    SECTION("alarms: asynchronous audit, the acknowledgement takes effect when the record is durable");
    fx_t *f = fx_two_unacked();
    size_t nt = hmi_tagdb_count(&f->db);
    async_ack_t a;
    memset(&a, 0, sizeof(a));
    a.ret = true;
    a.al = &f->al;
    int sync_calls = 0;
    hmi_alarm_set_ack_audit(&f->al, sync_ack_never, &sync_calls);
    hmi_alarm_set_ack_audit_async(&f->al, async_ack_hook, &a);
    hmi_alarm_point_t p;
    hmi_ack_result_t res = HMI_ACK_OK;
    uint32_t seq0 = hmi_alarm_last_ack(&f->al, &res);

    /* granted: pending, nothing changes until the record is durable */
    CHECK(hmi_alarm_ack_as(&f->al, 0, g_now, HMI_ROLE_OPERATOR, "op1") == HMI_ACK_PENDING);
    CHECK(a.calls == 1 && a.token[0] != 0 && a.rec[0].result == HMI_ACK_OK && strcmp(a.rec[0].actor, "op1") == 0);
    CHECK(a.rec[0].n_points == 1 && a.rec[0].points[0] == 0);
    CHECK(sync_calls == 0);                                    /* the asynchronous hook takes precedence */
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK);
    CHECK(hmi_alarm_count_unacked(&f->al) == 2);
    CHECK(hmi_alarm_last_ack(&f->al, &res) == seq0);           /* no outcome yet */
    hmi_alarm_ack_audit_done(&f->al, a.token[0], true, g_now);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_ACK);
    CHECK(hmi_alarm_last_ack(&f->al, &res) == seq0 + 1 && res == HMI_ACK_OK);
    hmi_alarm_ack_audit_done(&f->al, a.token[0], true, g_now); /* a repeated report changes nothing */
    CHECK(hmi_alarm_last_ack(&f->al, &res) == seq0 + 1);

    /* the record fails: nothing changes, the failure is counted and reported */
    CHECK(f->al.ack_audit_failures == 0);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_PENDING);
    hmi_alarm_ack_audit_done(&f->al, a.token[1], false, g_now);
    hmi_alarm_get(&f->al, nt, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK && hmi_alarm_count_unacked(&f->al) == 1);
    CHECK(f->al.ack_audit_failures == 1);
    CHECK(hmi_alarm_last_ack(&f->al, &res) == seq0 + 2 && res == HMI_ACK_AUDIT_FAILED);

    /* a denied attempt needs no completion: token 0, answered at once */
    int calls = a.calls;
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    CHECK(a.calls == calls + 1 && a.token[calls] == 0 && a.rec[calls].result == HMI_ACK_DENIED);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_ADMIN) == HMI_ACK_PENDING); /* the remaining alarm */
    uint32_t t_ok = a.token[a.calls - 1];
    CHECK(t_ok != 0);
    hmi_alarm_ack_audit_done(&f->al, t_ok, true, g_now);
    CHECK(hmi_alarm_count_unacked(&f->al) == 0);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_ADMIN) == HMI_ACK_NOTHING); /* nothing to record: no hook call */
    CHECK(a.calls == calls + 2);

    /* unknown tokens are ignored */
    uint32_t s1 = hmi_alarm_last_ack(&f->al, &res);
    hmi_alarm_ack_audit_done(&f->al, 123456, true, g_now);
    hmi_alarm_ack_audit_done(&f->al, 0, true, g_now);
    CHECK(hmi_alarm_last_ack(&f->al, &res) == s1);
    fx_free(f, true);

    /* only a bounded number of requests may wait at once */
    f = fx_two_unacked();
    memset(&a, 0, sizeof(a));
    a.ret = true;
    a.al = &f->al;
    hmi_alarm_set_ack_audit_async(&f->al, async_ack_hook, &a);
    for (int i = 0; i < HMI_ACK_PENDING_MAX; i++) CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_PENDING);
    uint32_t fails = f->al.ack_audit_failures;
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_AUDIT_FAILED); /* refused, not silently queued */
    CHECK(f->al.ack_audit_failures == fails + 1 && a.calls == HMI_ACK_PENDING_MAX);
    hmi_alarm_ack_audit_done(&f->al, a.token[0], true, g_now); /* one completes: a slot is free again */
    CHECK(hmi_alarm_count_unacked(&f->al) == 0);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_NOTHING);

    /* a request whose record never completes is dropped, never applied */
    for (int i = 1; i < HMI_ACK_PENDING_MAX; i++) hmi_alarm_ack_audit_done(&f->al, a.token[i], false, g_now); /* tidy */
    fx_free(f, true);
    f = fx_two_unacked();
    memset(&a, 0, sizeof(a));
    a.ret = true;
    a.al = &f->al;
    hmi_alarm_set_ack_audit_async(&f->al, async_ack_hook, &a);
    s1 = hmi_alarm_last_ack(&f->al, &res);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_PENDING);
    hmi_alarm_step(&f->al, g_now + HMI_ACK_PENDING_TTL_MS + 1, HMI_LINK_HEALTHY);
    CHECK(hmi_alarm_last_ack(&f->al, &res) == s1 + 1 && res == HMI_ACK_AUDIT_FAILED);
    hmi_alarm_ack_audit_done(&f->al, a.token[0], true, g_now + HMI_ACK_PENDING_TTL_MS + 2); /* too late */
    CHECK(hmi_alarm_count_unacked(&f->al) == 2);
    fx_free(f, true);

    /* the hook cannot queue: refused, the slot is released */
    f = fx_two_unacked();
    memset(&a, 0, sizeof(a));
    a.al = &f->al;
    a.ret = false;
    hmi_alarm_set_ack_audit_async(&f->al, async_ack_hook, &a);
    for (int i = 0; i < 2 * HMI_ACK_PENDING_MAX; i++)
        CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_AUDIT_FAILED);
    CHECK(hmi_alarm_count_unacked(&f->al) == 2);
    a.ret = true;
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_PENDING);
    fx_free(f, true);

    /* the record completes inside the hook, before the call returns */
    f = fx_two_unacked();
    memset(&a, 0, sizeof(a));
    a.ret = true;
    a.complete_inside = true;
    a.al = &f->al;
    hmi_alarm_set_ack_audit_async(&f->al, async_ack_hook, &a);
    s1 = hmi_alarm_last_ack(&f->al, &res);
    CHECK(hmi_alarm_ack_all(&f->al, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_PENDING);
    CHECK(hmi_alarm_count_unacked(&f->al) == 0);
    CHECK(hmi_alarm_last_ack(&f->al, &res) == s1 + 1 && res == HMI_ACK_OK);
    fx_free(f, true);

    /* an alarm that changes while the record is in flight is not acknowledged unseen */
    f = fx_two_unacked();
    memset(&a, 0, sizeof(a));
    a.ret = true;
    a.al = &f->al;
    hmi_alarm_set_ack_audit_async(&f->al, async_ack_hook, &a);
    s1 = hmi_alarm_last_ack(&f->al, &res);
    CHECK(hmi_alarm_ack(&f->al, 0, g_now, HMI_ROLE_OPERATOR) == HMI_ACK_PENDING);
    hmi_mutex_lock(&f->al.lock);
    f->al.pt[0].since_ms += 5; /* the alarm was raised again meanwhile */
    hmi_mutex_unlock(&f->al.lock);
    hmi_alarm_ack_audit_done(&f->al, a.token[0], true, g_now);
    hmi_alarm_get(&f->al, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK);
    CHECK(hmi_alarm_last_ack(&f->al, &res) == s1 + 1 && res == HMI_ACK_NOTHING);
    fx_free(f, true);
}

int main(void)
{
    test_alarm_sink();
    test_alarm_state_bytes();
    test_ack_async();
    test_alarm_flow();
    test_alarm_bad_quality_freezes();
    test_alarm_persistence();
    test_ack_denied_changes_nothing();
    test_ack_operator_allowed();
    test_ack_actor();
    test_ack_audit_policy();
    return test_finish();
}
