/* End-to-end tests: real libmodbus 3.1.10 TCP driver <-> in-process fake
 * Modbus server with fault injection, loopback only. No real PLC. */
#include "../../src/hmi/hmi_runtime.h"
#include "../../src/hmi/plc_libmodbus.h"
#include "fake_modbus_server.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define SECTION(n) printf("%s\n", n)

typedef struct {
    fake_modbus_t *srv;
    uint16_t port;
    hmi_tag_cfg_t cfg[2];
    hmi_tagdb_t db;
    hmi_comms_t c;
    hmi_writes_t w;
    plc_libmodbus_t *plc;
} env_t;

#define RESP_MS 200

static env_t *env_new(void)
{
    env_t *e = (env_t *)calloc(1, sizeof(*e));
    e->srv = fms_start(&e->port);
    fms_set_holding(e->srv, 0, 500);
    fms_set_holding(e->srv, 1, 7);
    memset(e->cfg, 0, sizeof(e->cfg));
    snprintf(e->cfg[0].name, HMI_NAME_LEN, "LVL");
    e->cfg[0].dtype = HMI_DT_U16; e->cfg[0].scale = 0.1; e->cfg[0].eng_max = 100; e->cfg[0].poll_ms = 100;
    e->cfg[0].write_allowed = true; e->cfg[0].write_role = HMI_ROLE_OPERATOR; e->cfg[0].write_max = 100;
    snprintf(e->cfg[1].name, HMI_NAME_LEN, "PRESS");
    e->cfg[1].dtype = HMI_DT_U16; e->cfg[1].address = 1; e->cfg[1].scale = 1; e->cfg[1].eng_max = 1000; e->cfg[1].poll_ms = 100;
    hmi_tagdb_init(&e->db, e->cfg, 2, hmi_now_ms());
    e->plc = plc_libmodbus_create("127.0.0.1", e->port, 1, RESP_MS);
    hmi_plc_driver_t d;
    plc_libmodbus_driver(e->plc, &d);
    hmi_comms_cfg_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.response_timeout_ms = RESP_MS; cc.connect_timeout_ms = 500; cc.miss_limit = 3;
    cc.backoff_min_ms = 100; cc.backoff_max_ms = 400; cc.jitter_seed = 3;
    hmi_comms_init(&e->c, &e->db, &d, &cc);
    hmi_writes_init(&e->w, &e->db, true, 3000);
    hmi_comms_set_writes(&e->c, &e->w);
    return e;
}

static void env_free(env_t *e)
{
    hmi_writes_destroy(&e->w);
    hmi_comms_destroy(&e->c);
    plc_libmodbus_destroy(e->plc);
    hmi_tagdb_destroy(&e->db);
    fms_stop(e->srv);
    free(e);
}

static void run_ms(env_t *e, uint32_t ms)
{
    uint64_t end = hmi_now_ms() + ms;
    while (hmi_now_ms() < end) {
        uint32_t h = hmi_comms_step(&e->c);
        if (h > 0) hmi_sleep_ms(h > 5 ? 5 : h);
    }
}

static bool run_until_state(env_t *e, hmi_conn_state_t s, uint32_t max_ms, uint32_t *took)
{
    uint64_t t0 = hmi_now_ms();
    while (hmi_now_ms() - t0 < max_ms) {
        uint32_t h = hmi_comms_step(&e->c);
        if (hmi_comms_state(&e->c) == s) {
            if (took) *took = (uint32_t)(hmi_now_ms() - t0);
            return true;
        }
        if (h > 0) hmi_sleep_ms(h > 5 ? 5 : h);
    }
    return false;
}

static hmi_tag_value_t val(env_t *e, size_t i)
{
    hmi_tag_value_t v;
    hmi_tagdb_get(&e->db, i, &v);
    return v;
}

static void test_poll_real_modbus(void)
{
    SECTION("e2e: poll over real Modbus TCP, decode");
    env_t *e = env_new();
    CHECK(e->srv && e->plc);
    run_ms(e, 400);
    CHECK(hmi_comms_state(&e->c) == HMI_CONN_CONNECTED);
    CHECK(val(e, 0).quality == HMI_Q_GOOD && fabs(val(e, 0).eng - 50.0) < 1e-9);
    CHECK(val(e, 1).quality == HMI_Q_GOOD && val(e, 1).raw == 7);
    hmi_comms_stats_t st;
    hmi_comms_stats(&e->c, &st);
    CHECK(st.polls_ok >= 3);
    printf("  poll latency last=%u ms max=%u ms (loopback)\n", st.last_poll_latency_ms, st.max_poll_latency_ms);
    env_free(e);
}

static void test_exception_reply(void)
{
    SECTION("e2e: real Modbus exception reply -> BAD(EXCEPTION), link up");
    env_t *e = env_new();
    run_ms(e, 300);
    fms_set_mode(e->srv, FMS_EXCEPTION, 2);
    run_ms(e, 400);
    CHECK(hmi_comms_state(&e->c) == HMI_CONN_CONNECTED);
    CHECK(val(e, 0).quality == HMI_Q_BAD && val(e, 0).reason == HMI_REASON_EXCEPTION);
    hmi_comms_stats_t st;
    hmi_comms_stats(&e->c, &st);
    CHECK(st.exceptions > 0 && st.links_lost == 0);
    fms_set_mode(e->srv, FMS_NORMAL, 0);
    run_ms(e, 300);
    CHECK(val(e, 0).quality == HMI_Q_GOOD);
    env_free(e);
}

static void test_hang_detection_and_recovery(void)
{
    SECTION("e2e: unanswered requests -> bounded detection, reconnect, recovery");
    env_t *e = env_new();
    run_ms(e, 300);
    CHECK(hmi_comms_state(&e->c) == HMI_CONN_CONNECTED);
    fms_set_mode(e->srv, FMS_HANG, 0);
    uint32_t took = 0;
    CHECK(run_until_state(e, HMI_CONN_BACKOFF, 4000, &took));
    printf("  link declared lost after %u ms (limit 3 x %d ms timeout)\n", took, RESP_MS);
    CHECK(took >= 3 * RESP_MS - 50 && took <= 3 * RESP_MS + 500);
    CHECK(val(e, 0).quality == HMI_Q_BAD && val(e, 0).reason == HMI_REASON_COMM_LOSS);
    fms_set_mode(e->srv, FMS_NORMAL, 0);
    CHECK(run_until_state(e, HMI_CONN_CONNECTED, 5000, &took));
    run_ms(e, 300);
    CHECK(val(e, 0).quality == HMI_Q_GOOD);
    env_free(e);
}

static void test_dropped_connection(void)
{
    SECTION("e2e: server closes connection -> reconnect");
    env_t *e = env_new();
    run_ms(e, 300);
    fms_kick_client(e->srv);
    run_ms(e, 2500);
    CHECK(hmi_comms_state(&e->c) == HMI_CONN_CONNECTED);
    CHECK(val(e, 0).quality == HMI_Q_GOOD);
    hmi_comms_stats_t st;
    hmi_comms_stats(&e->c, &st);
    CHECK(st.connects_ok >= 2);
    env_free(e);
}

static void test_truncated_and_bad_txid(void)
{
    SECTION("e2e: truncated / mismatching replies are never data");
    env_t *e = env_new();
    run_ms(e, 300);
    fms_set_mode(e->srv, FMS_TRUNCATE, 0);
    run_ms(e, 1200);
    hmi_tag_value_t v = val(e, 0);
    CHECK(v.quality != HMI_Q_GOOD);
    hmi_comms_stats_t st;
    hmi_comms_stats(&e->c, &st);
    CHECK(st.polls_failed > 0);
    fms_set_mode(e->srv, FMS_BAD_TXID, 0);
    run_ms(e, 1200);
    CHECK(val(e, 0).quality != HMI_Q_GOOD);
    fms_set_mode(e->srv, FMS_NORMAL, 0);
    run_ms(e, 2500);
    CHECK(val(e, 0).quality == HMI_Q_GOOD);
    env_free(e);
}

static void test_connect_refused(void)
{
    SECTION("e2e: nothing listening -> BACKOFF, values stay UNAVAILABLE");
    env_t *e = env_new();
    plc_libmodbus_destroy(e->plc);
    /* port 1 on loopback: refused. libmodbus connect is bounded by timeout. */
    e->plc = plc_libmodbus_create("127.0.0.1", 1, 1, RESP_MS);
    plc_libmodbus_driver(e->plc, &e->c.drv);
    uint32_t took;
    CHECK(run_until_state(e, HMI_CONN_BACKOFF, 3000, &took));
    CHECK(val(e, 0).quality == HMI_Q_UNAVAILABLE);
    hmi_comms_stats_t st;
    hmi_comms_stats(&e->c, &st);
    CHECK(st.connects_failed >= 1 && st.connects_ok == 0);
    env_free(e);
}

static void test_write_real(void)
{
    SECTION("e2e: write + read-back confirm over Modbus");
    env_t *e = env_new();
    run_ms(e, 300);
    uint32_t id;
    CHECK(hmi_writes_submit(&e->w, 0, 75.5, HMI_ROLE_OPERATOR, hmi_comms_link_health(&e->c), hmi_now_ms(), &id) == HMI_WRS_OK);
    run_ms(e, 150);
    hmi_write_rec_t r;
    CHECK(hmi_writes_status(&e->w, id, &r) && r.state == HMI_WR_CONFIRMED);
    CHECK(fms_get_holding(e->srv, 0) == 755);
    CHECK(fms_write_count(e->srv) == 1);
    run_ms(e, 300);
    CHECK(fabs(val(e, 0).eng - 75.5) < 1e-9);
    env_free(e);
}

static void test_write_lost_reply_real(void)
{
    SECTION("e2e: write applied, reply lost -> UNKNOWN, exactly one transmission");
    env_t *e = env_new();
    run_ms(e, 300);
    fms_set_mode(e->srv, FMS_APPLY_NO_REPLY, 0);
    uint32_t id;
    CHECK(hmi_writes_submit(&e->w, 0, 33.0, HMI_ROLE_OPERATOR, hmi_comms_link_health(&e->c), hmi_now_ms(), &id) == HMI_WRS_OK);
    run_ms(e, 300);
    hmi_write_rec_t r;
    CHECK(hmi_writes_status(&e->w, id, &r) && r.state == HMI_WR_UNKNOWN);
    fms_set_mode(e->srv, FMS_NORMAL, 0);
    run_ms(e, 2500); /* reconnect and keep polling */
    CHECK(hmi_comms_state(&e->c) == HMI_CONN_CONNECTED);
    CHECK(fms_write_count(e->srv) == 1); /* never replayed */
    CHECK(fms_get_holding(e->srv, 0) == 330); /* PLC did apply it: UNKNOWN was the honest answer */
    env_free(e);
}

static void test_runtime_with_libmodbus(void)
{
    SECTION("e2e: full runtime (threads) over libmodbus, clean stop x5");
    for (int round = 0; round < 5; round++) {
        uint16_t port;
        fake_modbus_t *srv = fms_start(&port);
        fms_set_holding(srv, 0, 900); /* 90 % -> HIGH alarm */
        static hmi_app_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.host, sizeof(cfg.host), "127.0.0.1");
        cfg.port = port; cfg.unit = 1; cfg.response_timeout_ms = 200; cfg.connect_timeout_ms = 500;
        cfg.n_tags = 1;
        snprintf(cfg.tags[0].name, HMI_NAME_LEN, "LVL");
        cfg.tags[0].dtype = HMI_DT_U16; cfg.tags[0].scale = 0.1; cfg.tags[0].eng_max = 100; cfg.tags[0].poll_ms = 100;
        cfg.tags[0].alarm.en_hi = true; cfg.tags[0].alarm.hi = 80; cfg.tags[0].alarm.on_ms = 100;
        plc_libmodbus_t *plc = plc_libmodbus_create(cfg.host, cfg.port, cfg.unit, cfg.response_timeout_ms);
        hmi_plc_driver_t d;
        plc_libmodbus_driver(plc, &d);
        static hmi_runtime_t rt;
        CHECK(hmi_runtime_start(&rt, &cfg, &d, NULL, NULL));
        hmi_sleep_ms(700);
        hmi_tag_value_t v;
        hmi_tagdb_get(&rt.db, 0, &v);
        CHECK(v.quality == HMI_Q_GOOD && fabs(v.eng - 90.0) < 1e-9);
        hmi_alarm_point_t p;
        hmi_alarm_get(&rt.alarms, 0, &p);
        CHECK(p.state == HMI_AL_ACTIVE_UNACK && p.kind == HMI_ALK_HI);
        /* alarm responsiveness while the comms thread blocks in a request:
         * the PLC hangs; supervisor must still age data and keep evaluating */
        fms_set_mode(srv, FMS_HANG, 0);
        hmi_sleep_ms(3300);
        hmi_tagdb_get(&rt.db, 0, &v);
        CHECK(v.quality == HMI_Q_BAD);
        /* comms-loss system alarm must be raised even though TCP keeps
         * (re)connecting: link health is judged by valid replies */
        hmi_alarm_point_t cp;
        hmi_alarm_get(&rt.alarms, hmi_tagdb_count(&rt.db), &cp);
        CHECK(cp.state == HMI_AL_ACTIVE_UNACK && cp.kind == HMI_ALK_COMMS);
        hmi_alarm_get(&rt.alarms, 0, &p);
        CHECK(p.state == HMI_AL_ACTIVE_UNACK && p.data_suppressed);
        CHECK(hmi_alarm_ack(&rt.alarms, 0, hmi_now_ms(), HMI_ROLE_OPERATOR) == HMI_ACK_OK); /* ack while disconnected */
        fms_set_mode(srv, FMS_NORMAL, 0);
        hmi_runtime_stop(&rt);
        CHECK(!rt.started);
        plc_libmodbus_destroy(plc);
        fms_stop(srv);
    }
}

int main(void)
{
    test_poll_real_modbus();
    test_exception_reply();
    test_hang_detection_and_recovery();
    test_dropped_connection();
    test_truncated_and_bad_txid();
    test_connect_refused();
    test_write_real();
    test_write_lost_reply_real();
    test_runtime_with_libmodbus();
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
