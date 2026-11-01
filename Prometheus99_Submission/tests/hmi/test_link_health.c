/* Tests for the single link-health evaluation in hmi_comms.
 * Part of the HMI core host tests; shared fixtures are in test_support.h. */
#include "test_support.h"

/* ---- link health (single authoritative evaluation in hmi_comms) ---- */
static void test_link_health_thresholds(void)
{
    SECTION("link health: HEALTHY / DEGRADED / NO_RESPONSE boundaries on the age of the last reply");
    fx_t *f = fx_new(false);
    CHECK(hmi_comms_link_health(&f->c) == HMI_LINK_DOWN); /* never connected */
    run_until(f, g_now + 300);
    CHECK(hmi_comms_state(&f->c) == HMI_CONN_CONNECTED);
    CHECK(hmi_comms_link_health(&f->c) == HMI_LINK_HEALTHY);
    uint64_t t = hmi_comms_last_ok_ms(&f->c);
    /* defaults for response_timeout 100 ms: degraded > 200, no-response > 1000 */
    CHECK(hmi_comms_link_health_at(&f->c, t) == HMI_LINK_HEALTHY);
    CHECK(hmi_comms_link_health_at(&f->c, t + 200) == HMI_LINK_HEALTHY);   /* == threshold: still better */
    CHECK(hmi_comms_link_health_at(&f->c, t + 201) == HMI_LINK_DEGRADED);
    CHECK(hmi_comms_link_health_at(&f->c, t + 1000) == HMI_LINK_DEGRADED);
    CHECK(hmi_comms_link_health_at(&f->c, t + 1001) == HMI_LINK_NO_RESPONSE);
    /* TCP state says CONNECTED the whole time: health is NOT the TCP state */
    CHECK(hmi_comms_state(&f->c) == HMI_CONN_CONNECTED);
    CHECK(strlen(hmi_link_health_text(HMI_LINK_NO_RESPONSE)) > 0);
    fx_free(f, false);

    /* explicit thresholds; inconsistent thresholds are rejected */
    fx_t *g = fx_new(false);
    hmi_comms_t c2;
    hmi_plc_driver_t d = mock_drv(&g->m);
    hmi_comms_cfg_t cc = default_cc();
    cc.link_degraded_ms = 500;
    cc.link_no_response_ms = 2000;
    CHECK(hmi_comms_init(&c2, &g->db, &d, &cc));
    hmi_comms_destroy(&c2);
    cc.link_degraded_ms = 3000;
    CHECK(!hmi_comms_init(&c2, &g->db, &d, &cc));
    fx_free(g, false);
}

static void test_link_health_delayed_and_recovery(void)
{
    SECTION("link health: one missed reply -> DEGRADED, valid reply -> HEALTHY");
    fx_t *f = fx_new(false);
    run_until(f, g_now + 300);
    CHECK(hmi_comms_link_health(&f->c) == HMI_LINK_HEALTHY);
    f->m.read_st = HMI_IO_TIMEOUT;
    int guard = 0;
    while (hmi_comms_state(&f->c) == HMI_CONN_CONNECTED && guard++ < 1000) {
        uint32_t h = hmi_comms_step(&f->c);
        if (h) g_now += h;
    }
    CHECK(hmi_comms_state(&f->c) == HMI_CONN_DEGRADED);
    CHECK(hmi_comms_link_health(&f->c) == HMI_LINK_DEGRADED);
    f->m.read_st = HMI_IO_OK;
    run_until(f, g_now + 200);
    CHECK(hmi_comms_link_health(&f->c) == HMI_LINK_HEALTHY);
    fx_free(f, false);
}

static void test_link_health_connected_but_silent(void)
{
    SECTION("link health: socket connects, PLC never answers -> never HEALTHY, comms alarm raised");
    fx_t *f = fx_new(true);
    f->m.read_st = HMI_IO_TIMEOUT; /* connect() succeeds, every request times out */
    int healthy_after_first_failure = 0;
    uint64_t end = g_now + 4000;
    while (g_now < end) {
        hmi_comms_step(&f->c);
        hmi_link_health_t h = hmi_comms_link_health(&f->c);
        hmi_comms_stats_t st;
        hmi_comms_stats(&f->c, &st);
        if (st.polls_failed > 0 && h == HMI_LINK_HEALTHY) healthy_after_first_failure++;
        hmi_tagdb_age(&f->db, g_now);
        hmi_alarm_step(&f->al, g_now, h);
        g_now += 10;
    }
    CHECK(healthy_after_first_failure == 0);
    hmi_alarm_point_t cp;
    hmi_alarm_get(&f->al, f->db.n_tags, &cp);
    CHECK(cp.state == HMI_AL_ACTIVE_UNACK && cp.kind == HMI_ALK_COMMS);
    hmi_comms_stats_t st;
    hmi_comms_stats(&f->c, &st);
    CHECK(st.connects_ok >= 1 && st.polls_ok == 0);
    fx_free(f, true);
}

static void test_link_health_disconnect_reconnect(void)
{
    SECTION("link health: link loss -> DOWN, reconnect + valid reply -> HEALTHY");
    fx_t *f = fx_new(false);
    run_until(f, g_now + 300);
    f->m.read_st = HMI_IO_TRANSPORT;
    int guard = 0;
    while (hmi_comms_state(&f->c) != HMI_CONN_BACKOFF && guard++ < 1000) {
        uint32_t h = hmi_comms_step(&f->c);
        if (h) g_now += h;
    }
    CHECK(hmi_comms_link_health(&f->c) == HMI_LINK_DOWN);
    f->m.connect_st = HMI_IO_TRANSPORT; /* reconnect attempts fail too */
    run_until(f, g_now + 1500);
    CHECK(hmi_comms_link_health(&f->c) == HMI_LINK_DOWN);
    f->m.read_st = HMI_IO_OK;
    f->m.connect_st = HMI_IO_OK;
    run_until(f, g_now + 6000);
    CHECK(hmi_comms_state(&f->c) == HMI_CONN_CONNECTED);
    CHECK(hmi_comms_link_health(&f->c) == HMI_LINK_HEALTHY);
    fx_free(f, false);
}

static void test_link_health_alarm_consistency(void)
{
    SECTION("link health: alarm engine and predicate agree for every health value");
    const hmi_link_health_t all[] = { HMI_LINK_DOWN, HMI_LINK_NO_RESPONSE, HMI_LINK_DEGRADED, HMI_LINK_HEALTHY };
    for (size_t i = 0; i < 4; i++) {
        fx_t *f = fx_new(true);
        run_until(f, g_now + 300);
        uint64_t end = g_now + 1500;
        while (g_now < end) {
            hmi_alarm_step(&f->al, g_now, all[i]);
            g_now += 10;
        }
        hmi_alarm_point_t cp;
        hmi_alarm_get(&f->al, f->db.n_tags, &cp);
        bool active = (cp.state == HMI_AL_ACTIVE_UNACK);
        if (active != hmi_link_health_is_lost(all[i])) printf("  mismatch for %s\n", hmi_link_health_text(all[i]));
        CHECK(active == hmi_link_health_is_lost(all[i]));
        fx_free(f, true);
    }
    /* every health value has distinct operator text (the UI shows exactly this text) */
    for (size_t i = 0; i < 4; i++)
        for (size_t j = i + 1; j < 4; j++)
            CHECK(strcmp(hmi_link_health_text(all[i]), hmi_link_health_text(all[j])) != 0);
}

static void test_link_health_clock_edges(void)
{
    SECTION("link health: unsigned wrap and backwards clock step");
    fx_t *f = fx_new(false);
    run_until(f, g_now + 300);
    /* last reply just before the 64-bit counter wraps */
    hmi_mutex_lock(&f->c.lock);
    f->c.last_ok_ms = UINT64_MAX - 50;
    hmi_mutex_unlock(&f->c.lock);
    CHECK(hmi_comms_link_health_at(&f->c, UINT64_MAX) == HMI_LINK_HEALTHY);
    CHECK(hmi_comms_link_health_at(&f->c, 10) == HMI_LINK_HEALTHY);          /* age 61 across the wrap */
    CHECK(hmi_comms_link_health_at(&f->c, 300) == HMI_LINK_DEGRADED);        /* age 351 */
    CHECK(hmi_comms_link_health_at(&f->c, 5000) == HMI_LINK_NO_RESPONSE);
    /* clock stepped backwards: unknown age must fail safe */
    hmi_mutex_lock(&f->c.lock);
    f->c.last_ok_ms = 100000;
    hmi_mutex_unlock(&f->c.lock);
    CHECK(hmi_comms_link_health_at(&f->c, 99000) == HMI_LINK_NO_RESPONSE);
    fx_free(f, false);
}

int main(void)
{
    test_link_health_thresholds();
    test_link_health_delayed_and_recovery();
    test_link_health_connected_but_silent();
    test_link_health_disconnect_reconnect();
    test_link_health_alarm_consistency();
    test_link_health_clock_edges();
    return test_finish();
}
