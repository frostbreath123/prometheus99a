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

int main(void)
{
    test_alarm_flow();
    test_alarm_bad_quality_freezes();
    test_alarm_persistence();
    test_ack_denied_changes_nothing();
    test_ack_operator_allowed();
    test_ack_audit_policy();
    return test_finish();
}
