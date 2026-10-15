/* Tests for hmi_runtime: lifecycle with real threads and concurrent readers.
 * Part of the HMI core host tests; shared fixtures are in test_support.h. */
#include "test_support.h"

/* ---- runtime lifecycle with real threads ---- */
static void test_runtime_lifecycle(void)
{
    SECTION("runtime: start/stop repeatedly, writes forced off without audit trail");
    static hmi_app_cfg_t cfg;
    char err[128];
    CHECK(parse(GOOD_CFG, &cfg, err));
    for (int round = 0; round < 10; round++) {
        mock_t m;
        mock_init(&m);
        m.hold[0] = 500;
        m.timeout_cost = 0; /* real clock: no fake time advance needed */
        hmi_plc_driver_t d = mock_drv(&m);
        static hmi_runtime_t rt;
        g_now = 0;
        /* m_* functions advance g_now, harmless here */
        CHECK(hmi_runtime_start(&rt, &cfg, &d, NULL, NULL));
        CHECK(!rt.writes_effective); /* no audit path => writes forced off */
        hmi_sleep_ms(120);
        hmi_runtime_stop(&rt);
        CHECK(!rt.started);
    }
}

/* ---- worker liveness counters ---- */
static void test_runtime_beats(void)
{
    SECTION("runtime: worker heartbeat counters advance while running");
    static hmi_app_cfg_t cfg;
    char err[128];
    CHECK(parse(GOOD_CFG, &cfg, err));
    static mock_t m;
    mock_init(&m);
    m.hold[0] = 500;
    m.timeout_cost = 0;
    hmi_plc_driver_t d = mock_drv(&m);
    static hmi_runtime_t rt;
    CHECK(hmi_runtime_start(&rt, &cfg, &d, NULL, NULL));
    hmi_runtime_beats_t a = hmi_runtime_beats(&rt);
    hmi_sleep_ms(300);
    hmi_runtime_beats_t b = hmi_runtime_beats(&rt);
    CHECK(b.comms != a.comms);       /* comms loop made progress */
    CHECK(b.supervisor != a.supervisor); /* 50 ms supervisor loop made progress */
    /* GOOD_CFG: response 300 ms, connect default 1000 ms => 2*300 + 1000 + 2000 */
    CHECK(hmi_runtime_worst_stall_ms(&rt) == 3600u);
    hmi_runtime_stop(&rt);
    hmi_runtime_beats_t c = hmi_runtime_beats(&rt);
    hmi_sleep_ms(200);
    hmi_runtime_beats_t e = hmi_runtime_beats(&rt);
    CHECK(c.comms == e.comms && c.supervisor == e.supervisor); /* stopped threads do not beat */
}

/* ---- acknowledgement authorization and audit through the real runtime ---- */
static bool wait_unacked(hmi_runtime_t *rt, size_t want, int max_ms)
{
    for (int t = 0; t < max_ms; t += 20) {
        if (hmi_alarm_count_unacked(&rt->alarms) >= want) return true;
        hmi_sleep_ms(20);
    }
    return false;
}

static size_t slurp(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

static void test_runtime_ack_audit(void)
{
    SECTION("runtime: acknowledgement is authorized in the core and audited");
    static hmi_app_cfg_t cfg;
    char err[128], text[4096];
    CHECK(parse(GOOD_CFG, &cfg, err));
    static mock_t m;
    static hmi_runtime_t rt;
    const char *path = "p99_ack_audit_test.log";

    /* working audit trail */
    remove(path);
    mock_init(&m);
    m.hold[0] = 900; /* 90.0 %: above the 80 % HI limit of the example tag */
    m.timeout_cost = 0;
    hmi_plc_driver_t d = mock_drv(&m);
    CHECK(hmi_runtime_start(&rt, &cfg, &d, path, NULL));
    CHECK(wait_unacked(&rt, 1, 4000));
    size_t unacked = hmi_alarm_count_unacked(&rt.alarms);

    CHECK(hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    CHECK(hmi_alarm_count_unacked(&rt.alarms) == unacked);
    CHECK(hmi_alarm_ack(&rt.alarms, 0, hmi_now_ms(), HMI_ROLE_OPERATOR) == HMI_ACK_OK);
    CHECK(hmi_alarm_count_unacked(&rt.alarms) == unacked - 1);
    hmi_runtime_stop(&rt);

    size_t len = slurp(path, text, sizeof(text));
    CHECK(len > 0);
    CHECK(strstr(text, " ALARM_ACK role=viewer action=ACK_ALL result=DENIED n=1 points=LVL\n") != NULL);
    CHECK(strstr(text, " ALARM_ACK role=operator action=ACK result=GRANTED n=1 points=LVL\n") != NULL);
    CHECK(text[len - 1] == '\n');                  /* only complete lines */
    remove(path);

    /* audit trail that cannot be written: grants are refused, nothing changes */
    mock_init(&m);
    m.hold[0] = 900;
    m.timeout_cost = 0;
    d = mock_drv(&m);
    CHECK(hmi_runtime_start(&rt, &cfg, &d, "no_such_dir_p99/audit.log", NULL));
    CHECK(wait_unacked(&rt, 1, 4000));
    unacked = hmi_alarm_count_unacked(&rt.alarms);
    CHECK(hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_OPERATOR) == HMI_ACK_AUDIT_FAILED);
    CHECK(hmi_alarm_count_unacked(&rt.alarms) == unacked);          /* still annunciated */
    CHECK(hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    hmi_runtime_stop(&rt);

    /* no audit path configured: acknowledgements work and are not recorded */
    mock_init(&m);
    m.hold[0] = 900;
    m.timeout_cost = 0;
    d = mock_drv(&m);
    CHECK(hmi_runtime_start(&rt, &cfg, &d, NULL, NULL));
    CHECK(wait_unacked(&rt, 1, 4000));
    CHECK(hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_OPERATOR) == HMI_ACK_OK);
    CHECK(hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    hmi_runtime_stop(&rt);
}

/* ---- concurrent consumers while runtime is live ---- */
static hmi_runtime_t g_rt;
static hmi_atomic_flag_t g_rd_stop;
static volatile int32_t g_bad_reads;
static volatile int32_t g_reads;

static void reader_thread(void *arg)
{
    (void)arg;
    uint32_t seen = 0;
    while (!hmi_flag_get(&g_rd_stop)) {
        hmi_tag_value_t v[HMI_MAX_TAGS];
        uint32_t gv = hmi_tagdb_snapshot(&g_rt.db, v, HMI_MAX_TAGS);
        (void)seen;
        seen = gv;
        for (size_t i = 0; i < g_rt.db.n_tags; i++) {
            /* invariant: GOOD value must equal its raw decoded with scale */
            if (v[i].quality == HMI_Q_GOOD && i == 0) {
                double e = (double)(uint16_t)v[i].raw * 0.1;
                if (fabs(e - v[i].eng) > 1e-9) __atomic_fetch_add(&g_bad_reads, 1, __ATOMIC_RELAXED);
            }
        }
        __atomic_fetch_add(&g_reads, 1, __ATOMIC_RELAXED);
    }
}

static void test_concurrent_consumers(void)
{
    SECTION("runtime: concurrent readers vs live comms/supervisor threads");
    static hmi_app_cfg_t cfg;
    char err[128];
    CHECK(parse(GOOD_CFG, &cfg, err));
    static mock_t m;
    mock_init(&m);
    m.timeout_cost = 0;
    hmi_plc_driver_t d = mock_drv(&m);
    g_bad_reads = 0;
    g_reads = 0;
    hmi_flag_set(&g_rd_stop, 0);
    CHECK(hmi_runtime_start(&g_rt, &cfg, &d, NULL, NULL));
    hmi_thread_t r1, r2;
    CHECK(hmi_thread_start(&r1, reader_thread, NULL));
    CHECK(hmi_thread_start(&r2, reader_thread, NULL));
    for (int i = 0; i < 40; i++) {
        /* PLC value changes underneath */
        __atomic_store_n(&m.hold[0], (uint16_t)(100 + i * 7), __ATOMIC_RELAXED);
        hmi_sleep_ms(10);
    }
    hmi_flag_set(&g_rd_stop, 1);
    CHECK(hmi_thread_join(&r1, 3000));
    CHECK(hmi_thread_join(&r2, 3000));
    hmi_runtime_stop(&g_rt);
    printf("  reads=%d bad=%d\n", (int)g_reads, (int)g_bad_reads);
    CHECK(g_bad_reads == 0 && g_reads > 100);
}

int main(void)
{
    test_runtime_lifecycle();
    test_runtime_beats();
    test_runtime_ack_audit();
    test_concurrent_consumers();
    return test_finish();
}
