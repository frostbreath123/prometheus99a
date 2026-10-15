/* Tests for hmi_comms: polling, link loss and recovery, backoff, staleness, scheduling.
 * Part of the HMI core host tests; shared fixtures are in test_support.h. */
#include "test_support.h"

static void test_poll_and_decode(void)
{
    SECTION("comms: connect, poll, typed decode");
    fx_t *f = fx_new(false);
    CHECK(val(f, 0).quality == HMI_Q_UNAVAILABLE);
    run_until(f, g_now + 600);
    CHECK(hmi_comms_state(&f->c) == HMI_CONN_CONNECTED);
    hmi_tag_value_t v = val(f, 0);
    CHECK(v.quality == HMI_Q_GOOD && v.raw == 500 && fabs(v.eng - 50.0) < 1e-9);
    v = val(f, 1);
    CHECK(v.quality == HMI_Q_GOOD && fabs(v.eng - (-12.0)) < 1e-9);
    v = val(f, 2);
    CHECK(v.quality == HMI_Q_GOOD && fabs(v.eng - 21.5) < 1e-6);
    v = val(f, 3);
    CHECK(v.quality == HMI_Q_GOOD && v.raw == 0x00020001u && v.eng == 131073.0);
    fx_free(f, false);
}

static void test_out_of_range_uncertain(void)
{
    SECTION("tags: implausible value is UNCERTAIN not GOOD");
    fx_t *f = fx_new(false);
    f->m.hold[0] = 1500; /* 150 % > eng_max 100 */
    run_until(f, g_now + 300);
    CHECK(val(f, 0).quality == HMI_Q_UNCERTAIN && val(f, 0).reason == HMI_REASON_OUT_OF_RANGE);
    CHECK(val(f, 1).quality == HMI_Q_GOOD);
    fx_free(f, false);
}

static void test_link_loss_and_recovery(void)
{
    SECTION("comms: timeouts -> DEGRADED -> BAD -> backoff -> recovery");
    fx_t *f = fx_new(false);
    run_until(f, g_now + 300);
    CHECK(hmi_comms_state(&f->c) == HMI_CONN_CONNECTED);

    f->m.read_st = HMI_IO_TIMEOUT;
    uint64_t t_fail = g_now;
    /* step until first miss */
    int guard = 0;
    while (hmi_comms_state(&f->c) == HMI_CONN_CONNECTED && guard++ < 1000) {
        uint32_t h = hmi_comms_step(&f->c);
        if (h) g_now += h;
    }
    CHECK(hmi_comms_state(&f->c) == HMI_CONN_DEGRADED);
    CHECK(val(f, 0).quality == HMI_Q_UNCERTAIN && val(f, 0).reason == HMI_REASON_MISSED_POLL);

    guard = 0;
    while (hmi_comms_state(&f->c) == HMI_CONN_DEGRADED && guard++ < 1000) {
        uint32_t h = hmi_comms_step(&f->c);
        if (h) g_now += h;
    }
    CHECK(hmi_comms_state(&f->c) == HMI_CONN_BACKOFF);
    CHECK(val(f, 0).quality == HMI_Q_BAD && val(f, 0).reason == HMI_REASON_COMM_LOSS);
    CHECK(f->m.n_close >= 1);
    /* dead-link detection: miss_limit * timeout (+ a little), NOT groups * ... */
    CHECK(g_now - t_fail <= 3 * 100 + 150);

    /* PLC comes back */
    f->m.read_st = HMI_IO_OK;
    run_until(f, g_now + 3000);
    CHECK(hmi_comms_state(&f->c) == HMI_CONN_CONNECTED);
    CHECK(val(f, 0).quality == HMI_Q_GOOD);
    hmi_comms_stats_t st;
    hmi_comms_stats(&f->c, &st);
    CHECK(st.links_lost == 1 && st.connects_ok == 2);
    fx_free(f, false);
}

static void test_backoff_growth(void)
{
    SECTION("comms: reconnect backoff doubles and is capped");
    fx_t *f = fx_new(false);
    f->m.connect_st = HMI_IO_TRANSPORT;
    uint64_t attempts_t[8];
    uint32_t seen = 0;
    uint32_t last = 0;
    uint64_t end = g_now + 60000;
    while (g_now < end && seen < 8) {
        uint32_t h = hmi_comms_step(&f->c);
        if (f->m.n_connect != last) {
            last = f->m.n_connect;
            attempts_t[seen++] = g_now;
        }
        if (h) g_now += h;
    }
    CHECK(seen >= 7);
    uint64_t gap1 = attempts_t[2] - attempts_t[1];
    uint64_t gap_last = attempts_t[seen - 1] - attempts_t[seen - 2];
    CHECK(gap1 >= 500);
    CHECK(gap_last <= 4000 + 4000 / 4 + 300); /* cap + jitter + connect time */
    CHECK(gap_last >= 3000);
    CHECK(val(f, 0).quality == HMI_Q_UNAVAILABLE); /* never connected: never fabricated */
    fx_free(f, false);
}

static void test_detection_independent_of_groups(void)
{
    SECTION("comms: dead-link detection time independent of group count");
    fx_t *f = fx_new(false);
    run_until(f, g_now + 300);
    f->m.read_st = HMI_IO_TIMEOUT;
    uint64_t t0 = g_now;
    int guard = 0;
    while (hmi_comms_state(&f->c) != HMI_CONN_BACKOFF && guard++ < 1000) {
        uint32_t h = hmi_comms_step(&f->c);
        if (h) g_now += h;
    }
    uint64_t t3 = g_now - t0;
    CHECK(f->db.n_groups == 3);
    CHECK(t3 <= 3u * 100u + 150u);
    fx_free(f, false);
}

static void test_exception_keeps_link(void)
{
    SECTION("comms: Modbus exception is data failure, not link failure");
    fx_t *f = fx_new(false);
    run_until(f, g_now + 300);
    f->m.read_st = HMI_IO_EXCEPTION;
    f->m.read_exc = 2;
    f->m.fail_addr = 10; /* only the TEMP32 group */
    run_until(f, g_now + 1000);
    CHECK(hmi_comms_state(&f->c) == HMI_CONN_CONNECTED);
    CHECK(val(f, 2).quality == HMI_Q_BAD && val(f, 2).reason == HMI_REASON_EXCEPTION);
    CHECK(val(f, 0).quality == HMI_Q_GOOD);
    hmi_comms_stats_t st;
    hmi_comms_stats(&f->c, &st);
    CHECK(st.exceptions > 0 && st.links_lost == 0);
    /* group recovers on its own */
    f->m.read_st = HMI_IO_OK;
    run_until(f, g_now + 300);
    CHECK(val(f, 2).quality == HMI_Q_GOOD);
    fx_free(f, false);
}

static void test_recovery_restores_only_polled_tags(void)
{
    SECTION("tags: successful poll restores only its own tags");
    fx_t *f = fx_new(false);
    run_until(f, g_now + 300);
    /* take everything BAD directly, then let only group 0 succeed */
    hmi_tagdb_link_lost(&f->db);
    CHECK(val(f, 2).quality == HMI_Q_BAD);
    uint16_t regs[2] = { 600, 5 };
    hmi_tagdb_group_ok(&f->db, 0, regs, g_now);
    CHECK(val(f, 0).quality == HMI_Q_GOOD && val(f, 1).quality == HMI_Q_GOOD);
    CHECK(val(f, 2).quality == HMI_Q_BAD && val(f, 3).quality == HMI_Q_BAD);
    fx_free(f, false);
}

static void test_stale_and_versions(void)
{
    SECTION("tags: staleness (monotonic), per-consumer versions");
    fx_t *f = fx_new(false);
    run_until(f, g_now + 300);
    hmi_tag_value_t a = val(f, 0);
    uint32_t seen_a = a.version, seen_b = a.version; /* two independent consumers */
    f->m.hold[0] = 700;
    run_until(f, g_now + 300);
    hmi_tag_value_t b = val(f, 0);
    CHECK(b.version != seen_a && b.eng == 70.0);
    seen_a = b.version; /* consumer A catches up; B must still see the change */
    CHECK(b.version != seen_b);
    /* no data change -> no version bump even though polls continue */
    uint32_t gv = hmi_tagdb_global_version(&f->db);
    run_until(f, g_now + 300);
    CHECK(hmi_tagdb_global_version(&f->db) == gv);
    /* stale: stop polling entirely, only age */
    uint64_t t = g_now;
    hmi_tagdb_age(&f->db, t + 500);
    CHECK(val(f, 0).quality == HMI_Q_GOOD);
    hmi_tagdb_age(&f->db, t + 1500);
    CHECK(val(f, 0).quality == HMI_Q_BAD && val(f, 0).reason == HMI_REASON_STALE);
    fx_free(f, false);
}

static void test_no_drift(void)
{
    SECTION("comms: phase-aligned scheduling without cumulative drift");
    fx_t *f = fx_new(false);
    run_until(f, g_now + 1000);
    hmi_comms_stats_t s0, s1;
    hmi_comms_stats(&f->c, &s0);
    uint64_t t0 = g_now;
    run_until(f, t0 + 10000);
    hmi_comms_stats(&f->c, &s1);
    /* groups 0 and 1 at 100 ms, group 2 at 500 ms: 100+100+20 per 10 s */
    uint32_t polls = s1.polls_ok - s0.polls_ok;
    CHECK(polls >= 215 && polls <= 225);
    fx_free(f, false);
}


int main(void)
{
    test_poll_and_decode();
    test_out_of_range_uncertain();
    test_link_loss_and_recovery();
    test_backoff_growth();
    test_detection_independent_of_groups();
    test_exception_keeps_link();
    test_recovery_restores_only_polled_tags();
    test_stale_and_versions();
    test_no_drift();
    return test_finish();
}
