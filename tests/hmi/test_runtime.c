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
    /* GOOD_CFG: response 300 ms, connect default 1000 ms: the write step is the longest,
     * 2*300 + 2000 (audit wait), and 2000 more as margin */
    CHECK(hmi_runtime_worst_stall_ms(&rt) == 4600u);
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

/* An acknowledgement is two-phase when an audit trail exists: the call returns
 * HMI_ACK_PENDING and the outcome arrives from the storage worker. */
static hmi_ack_result_t settle(hmi_runtime_t *rt, uint32_t seq0, hmi_ack_result_t r)
{
    if (r != HMI_ACK_PENDING) return r;
    for (int t = 0; t < 4000; t += 5) {
        hmi_ack_result_t res = HMI_ACK_PENDING;
        if (hmi_alarm_last_ack(&rt->alarms, &res) != seq0) return res;
        hmi_sleep_ms(5);
    }
    return HMI_ACK_PENDING;
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
    CHECK(hmi_alarm_ack_all_as(&rt.alarms, hmi_now_ms(), HMI_ROLE_VIEWER, "view.er") == HMI_ACK_DENIED);
    CHECK(hmi_alarm_ack_all_as(&rt.alarms, hmi_now_ms(), HMI_ROLE_VIEWER, "bad name=x") == HMI_ACK_DENIED); /* hostile name */
    CHECK(hmi_alarm_count_unacked(&rt.alarms) == unacked);
    uint32_t s0 = hmi_alarm_last_ack(&rt.alarms, NULL);
    hmi_ack_result_t r1 = hmi_alarm_ack_as(&rt.alarms, 0, hmi_now_ms(), HMI_ROLE_OPERATOR, "op1");
    CHECK(r1 == HMI_ACK_PENDING); /* the call did not wait for the disk */
    CHECK(settle(&rt, s0, r1) == HMI_ACK_OK);
    CHECK(hmi_alarm_count_unacked(&rt.alarms) == unacked - 1);
    CHECK(!hmi_runtime_storage_fault(&rt));
    hmi_runtime_stop(&rt);

    size_t len = slurp(path, text, sizeof(text));
    CHECK(len > 0);
    CHECK(strstr(text, " ALARM_ACK role=viewer action=ACK_ALL result=DENIED n=1 points=LVL user=- wall=") != NULL);        /* the unnamed call records "-" */
    CHECK(strstr(text, " ALARM_ACK role=viewer action=ACK_ALL result=DENIED n=1 points=LVL user=view.er wall=") != NULL);
    CHECK(strstr(text, " ALARM_ACK role=operator action=ACK result=GRANTED n=1 points=LVL user=op1 wall=") != NULL);
    CHECK(strstr(text, " BOOT wall=") != NULL);       /* the start marker comes first */
    CHECK(strstr(text, " BOOT wall=") < strstr(text, " ALARM_ACK "));
    CHECK(strstr(text, " sync=ok\n") != NULL || strstr(text, " sync=no\n") != NULL); /* every line ends with the stamp */
    CHECK(strstr(text, "bad name") == NULL && strstr(text, "x user=") == NULL);          /* a hostile name never reaches the file */
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
    s0 = hmi_alarm_last_ack(&rt.alarms, NULL);
    CHECK(settle(&rt, s0, hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_OPERATOR)) == HMI_ACK_AUDIT_FAILED);
    CHECK(hmi_alarm_count_unacked(&rt.alarms) == unacked);          /* still annunciated */
    CHECK(hmi_runtime_storage_fault(&rt));                          /* and the fault is visible */
    CHECK(hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    hmi_runtime_stop(&rt);

    /* no audit path configured: acknowledgements work and are not recorded */
    mock_init(&m);
    m.hold[0] = 900;
    m.timeout_cost = 0;
    d = mock_drv(&m);
    CHECK(hmi_runtime_start(&rt, &cfg, &d, NULL, NULL));
    CHECK(wait_unacked(&rt, 1, 4000));
    CHECK(hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_OPERATOR) == HMI_ACK_OK); /* synchronous: no trail, no worker */
    CHECK(hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_VIEWER) == HMI_ACK_DENIED);
    CHECK(!hmi_runtime_storage_fault(&rt));
    hmi_runtime_stop(&rt);
}

/* ---- alarm journal and alarm state on disk ---- */
#include "../../src/hmi/hmi_auditfmt.h"

#define RT_STATE "p99_rt_state.bin"
#define RT_JRN   "p99_rt_state.bin.journal"

static void clean_state_files(void)
{
    remove(RT_STATE);
    remove(RT_STATE ".tmp");
    remove(RT_JRN);
    for (int i = 1; i <= 9; i++) {
        char n[64];
        snprintf(n, sizeof(n), RT_JRN ".%d", i);
        remove(n);
    }
}

static bool wait_for(bool (*cond)(hmi_runtime_t *), hmi_runtime_t *rt, int max_ms)
{
    for (int t = 0; t < max_ms; t += 10) {
        if (cond(rt)) return true;
        hmi_sleep_ms(10);
    }
    return cond(rt);
}

static bool cond_state_saved(hmi_runtime_t *rt)
{
    hmi_runtime_storage_info_t i;
    hmi_runtime_storage_info(rt, &i);
    return i.state_saves >= 1 && i.store.written >= 1 && i.store.written >= i.store.enqueued - 1;
}

static bool same_state_on_disk(hmi_runtime_t *rt)
{
    uint8_t now_[HMI_ALARM_STATE_MAX], disk[HMI_ALARM_STATE_MAX];
    size_t n = hmi_alarm_state_serialize(&rt->alarms, now_, sizeof(now_), NULL), len = 0;
    return n > 0 && hmi_fs_read_all(RT_STATE, disk, sizeof(disk), &len) == HMI_FS_READ_OK && len == n &&
           memcmp(now_, disk, n) == 0;
}

static void test_runtime_journal_and_state(void)
{
    SECTION("runtime: alarm journal and state are written while running and reloaded at start");
    static hmi_app_cfg_t cfg;
    char err[128], text[8192];
    CHECK(parse(GOOD_CFG, &cfg, err));
    static mock_t m;
    static hmi_runtime_t rt;
    clean_state_files();

    mock_init(&m);
    m.hold[0] = 900;
    m.timeout_cost = 0;
    hmi_plc_driver_t d = mock_drv(&m);
    hmi_runtime_storage_t st;
    memset(&st, 0, sizeof(st));
    st.alarm_state_path = RT_STATE;
    st.state_min_interval_ms = 50;
    CHECK(hmi_runtime_start_ex(&rt, &cfg, &d, &st));
    CHECK(wait_unacked(&rt, 1, 4000));
    CHECK(wait_for(cond_state_saved, &rt, 4000));
    CHECK(same_state_on_disk(&rt));     /* on disk without a clean shutdown */
    CHECK(slurp(RT_JRN, text, sizeof(text)) > 0 && strstr(text, " ALARM_EVENT seq=1 point=") != NULL);
    CHECK(strstr(text, " event=RAISED ") != NULL && strstr(text, " crc=") != NULL);
    hmi_alarm_log_t j[HMI_JOURNAL_LEN];
    size_t nj = hmi_alarm_journal_copy(&rt.alarms, j, HMI_JOURNAL_LEN);
    CHECK(nj >= 1);
    uint32_t last_seq = j[nj - 1].seq;
    hmi_runtime_storage_info_t info;
    hmi_runtime_storage_info(&rt, &info);
    CHECK(info.journal_restored == 0 && info.sink_failures == 0 && !info.store.fault);
    hmi_runtime_stop(&rt);

    /* second start from the same files: the recent events are back on the screen model, numbering continues */
    mock_init(&m);
    m.hold[0] = 900;
    m.timeout_cost = 0;
    d = mock_drv(&m);
    CHECK(hmi_runtime_start_ex(&rt, &cfg, &d, &st));
    hmi_runtime_storage_info(&rt, &info);
    CHECK(info.journal_restored >= 1);
    hmi_alarm_log_t k[HMI_JOURNAL_LEN];
    size_t nk = hmi_alarm_journal_copy(&rt.alarms, k, HMI_JOURNAL_LEN);
    CHECK(nk >= 1 && k[0].seq == 1 && k[0].event == HMI_ALE_RAISED);
    CHECK(wait_unacked(&rt, 1, 4000));
    hmi_alarm_point_t p;
    hmi_alarm_get(&rt.alarms, 0, &p);
    CHECK(p.state == HMI_AL_ACTIVE_UNACK); /* the alarm came back as it was (still in alarm) */
    CHECK(hmi_alarm_ack_as(&rt.alarms, 0, hmi_now_ms(), HMI_ROLE_OPERATOR, "op1") == HMI_ACK_OK); /* no audit trail: synchronous */
    hmi_runtime_stop(&rt);
    CHECK(slurp(RT_JRN, text, sizeof(text)) > 0 && strstr(text, " event=ACKED ") != NULL);
    /* the entry written by this run continues the numbering of the first run */
    const char *acked = strstr(text, " event=ACKED ");
    const char *seq_at = acked ? strstr(acked - 40 > text ? acked - 40 : text, "seq=") : NULL;
    CHECK(seq_at != NULL && (uint32_t)strtoul(seq_at + 4, NULL, 10) > last_seq);
    clean_state_files();
}

/* Builds a journal file of valid lines, one per sequence number, with the module's own formatter. */
static void write_journal_file(const char *const *names, size_t nn, const uint32_t *seqs, size_t n)
{
    static char big[400 * 1024];
    size_t off = 0;
    hmi_wall_t w;
    w.unix_ms = 1792664103120ull;
    w.valid = true;
    w.synced = true;
    for (size_t i = 0; i < n; i++) {
        hmi_alarm_log_t e;
        memset(&e, 0, sizeof(e));
        e.seq = seqs[i];
        e.ts_ms = 1000u + seqs[i];
        e.point = 0;
        e.kind = HMI_ALK_HI;
        e.event = HMI_ALE_RAISED;
        e.state_after = HMI_AL_ACTIVE_UNACK;
        e.value = 91.0;
        char line[400];
        size_t ln = hmi_audit_format_journal(line, sizeof(line), &e, names, nn, &w);
        memcpy(big + off, line, ln);
        off += ln;
    }
    hmi_fs_atomic_write(RT_JRN, big, off);
}

static void test_runtime_journal_reload_damage(void)
{
    SECTION("runtime: journal reload skips damaged, unknown and torn lines and keeps the newest");
    static hmi_app_cfg_t cfg;
    char err[128];
    CHECK(parse(GOOD_CFG, &cfg, err));
    const char *names[HMI_MAX_TAGS];
    for (size_t i = 0; i < cfg.n_tags; i++) names[i] = cfg.tags[i].name;
    static mock_t m;
    static hmi_runtime_t rt;
    hmi_runtime_storage_t st;
    memset(&st, 0, sizeof(st));
    st.alarm_state_path = RT_STATE;
    st.state_min_interval_ms = 50;

    /* three good lines around one with a damaged byte, one for a tag that no longer exists, and a torn last line */
    clean_state_files();
    {
        static char file[4096];
        size_t off = 0;
        hmi_wall_t w;
        w.unix_ms = 1792664103120ull;
        w.valid = true;
        w.synced = false;
        const char *other[HMI_MAX_TAGS];
        for (size_t i = 0; i < cfg.n_tags; i++) other[i] = cfg.tags[i].name;
        const char *gone[1] = { "GONE_TAG" };
        for (int i = 0; i < 7; i++) {
            hmi_alarm_log_t e;
            memset(&e, 0, sizeof(e));
            e.seq = (uint32_t)(10 + i);
            e.ts_ms = 5000u + (uint32_t)i;
            e.point = 0;
            e.kind = HMI_ALK_HI;
            e.event = HMI_ALE_RAISED;
            e.state_after = HMI_AL_ACTIVE_UNACK;
            e.value = 91.0;
            char line[400];
            size_t ln = hmi_audit_format_journal(line, sizeof(line), &e, i == 4 ? gone : other, i == 4 ? 1 : cfg.n_tags, &w);
            if (i == 3) line[20] = (char)(line[20] ^ 0x01); /* one damaged byte */
            if (i == 6) ln -= 9;                              /* torn: no CRC end, no newline */
            memcpy(file + off, line, ln);
            off += ln;
        }
        hmi_fs_atomic_write(RT_JRN, file, off);
    }
    mock_init(&m);
    m.timeout_cost = 0;
    hmi_plc_driver_t d = mock_drv(&m);
    CHECK(hmi_runtime_start_ex(&rt, &cfg, &d, &st));
    hmi_runtime_storage_info_t info;
    hmi_runtime_storage_info(&rt, &info);
    CHECK(info.journal_restored == 4); /* seq 10, 11, 12 and 15; not 13 (damaged), 14 (unknown tag), 16 (torn) */
    hmi_alarm_log_t j[HMI_JOURNAL_LEN];
    size_t nj = hmi_alarm_journal_copy(&rt.alarms, j, HMI_JOURNAL_LEN);
    CHECK(nj == 4 && j[0].seq == 10 && j[1].seq == 11 && j[2].seq == 12 && j[3].seq == 15);
    hmi_runtime_stop(&rt);

    /* a journal much larger than the reload buffer: the partial first line is dropped, the newest entries are kept */
    clean_state_files();
    {
        static uint32_t seqs[3000];
        for (size_t i = 0; i < 3000; i++) seqs[i] = (uint32_t)(i + 1);
        write_journal_file(names, cfg.n_tags, seqs, 3000);
    }
    mock_init(&m);
    m.timeout_cost = 0;
    d = mock_drv(&m);
    CHECK(hmi_runtime_start_ex(&rt, &cfg, &d, &st));
    hmi_runtime_storage_info(&rt, &info);
    CHECK(info.journal_restored == HMI_JOURNAL_LEN);
    nj = hmi_alarm_journal_copy(&rt.alarms, j, HMI_JOURNAL_LEN);
    CHECK(nj == HMI_JOURNAL_LEN && j[nj - 1].seq == 3000 && j[0].seq == 3000 - HMI_JOURNAL_LEN + 1);
    bool consecutive = true;
    for (size_t i = 1; i < nj; i++) if (j[i].seq != j[i - 1].seq + 1) consecutive = false;
    CHECK(consecutive);
    hmi_runtime_stop(&rt);

    /* nothing readable: an empty journal, no error */
    clean_state_files();
    hmi_fs_atomic_write(RT_JRN, "this is not a journal\nat all\n", 29);
    mock_init(&m);
    m.timeout_cost = 0;
    d = mock_drv(&m);
    CHECK(hmi_runtime_start_ex(&rt, &cfg, &d, &st));
    hmi_runtime_storage_info(&rt, &info);
    CHECK(info.journal_restored == 0 && hmi_alarm_journal_copy(&rt.alarms, j, HMI_JOURNAL_LEN) == 0);
    hmi_runtime_stop(&rt);
    clean_state_files();
}

/* ---- alarm state saving: limits, failure and retry, final save ---- */
static int g_fail_replace; /* fail the next N replaces (changed by the test, read by the worker) */
static int g_replace_calls;
static bool io_replace_failing(const char *p, const void *d, size_t n)
{
    __atomic_fetch_add(&g_replace_calls, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_fail_replace, __ATOMIC_SEQ_CST) > 0) {
        __atomic_fetch_sub(&g_fail_replace, 1, __ATOMIC_SEQ_CST);
        return false;
    }
    return hmi_fs_atomic_write(p, d, n);
}
static const hmi_store_io_t k_failing_io = { hmi_fs_append_sync, io_replace_failing, hmi_fs_size, hmi_fs_rename };

static bool cond_second_save(hmi_runtime_t *rt)
{
    hmi_runtime_storage_info_t i;
    hmi_runtime_storage_info(rt, &i);
    return i.state_saves >= 2;
}
static bool cond_deferred(hmi_runtime_t *rt)
{
    hmi_runtime_storage_info_t i;
    hmi_runtime_storage_info(rt, &i);
    return i.state_deferred >= 1;
}
static bool cond_replace_ok_and_clear(hmi_runtime_t *rt)
{
    return same_state_on_disk(rt) && !hmi_runtime_storage_fault(rt);
}

static void test_runtime_state_limits(void)
{
    SECTION("runtime: alarm state file is rewritten on change, within limits, retried after a failure");
    static hmi_app_cfg_t cfg;
    char err[128];
    CHECK(parse(GOOD_CFG, &cfg, err));
    static mock_t m;
    static hmi_runtime_t rt;
    hmi_runtime_storage_info_t info;

    /* hourly limit: the second change is held back, written at the clean shutdown */
    clean_state_files();
    mock_init(&m);
    m.hold[0] = 900;
    m.timeout_cost = 0;
    hmi_plc_driver_t d = mock_drv(&m);
    hmi_runtime_storage_t st;
    memset(&st, 0, sizeof(st));
    st.alarm_state_path = RT_STATE;
    st.state_min_interval_ms = 20;
    st.state_saves_per_hour = 1;
    CHECK(hmi_runtime_start_ex(&rt, &cfg, &d, &st));
    CHECK(wait_unacked(&rt, 1, 4000));
    CHECK(wait_for(cond_state_saved, &rt, 4000));
    CHECK(hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_OPERATOR) == HMI_ACK_OK); /* a second change */
    CHECK(wait_for(cond_deferred, &rt, 4000));
    hmi_sleep_ms(200);
    hmi_runtime_storage_info(&rt, &info);
    CHECK(info.state_saves == 1 && info.state_deferred >= 1);  /* held back, and counted once per hour, not per pass */
    CHECK(info.state_deferred <= 2);
    CHECK(!same_state_on_disk(&rt));                           /* the file is one change behind ... */
    hmi_runtime_stop(&rt);
    uint8_t disk[HMI_ALARM_STATE_MAX];
    size_t len = 0;
    CHECK(hmi_fs_read_all(RT_STATE, disk, sizeof(disk), &len) == HMI_FS_READ_OK && len > 0);
    {
        /* ... until the shutdown, which ignores the limit: a new runtime loads the acknowledged state */
        mock_init(&m);
        m.timeout_cost = 0;
        m.read_st = HMI_IO_TIMEOUT; /* no data: the alarm cannot clear itself while we look */
        d = mock_drv(&m);
        memset(&st, 0, sizeof(st));
        st.alarm_state_path = RT_STATE;
        CHECK(hmi_runtime_start_ex(&rt, &cfg, &d, &st));
        hmi_alarm_point_t p;
        hmi_alarm_get(&rt.alarms, 0, &p);
        CHECK(p.state == HMI_AL_ACTIVE_ACK);
        hmi_runtime_stop(&rt);
    }

    /* the interval limit: a second change shortly after the first waits for the interval, then is written */
    clean_state_files();
    mock_init(&m);
    m.hold[0] = 900;
    m.timeout_cost = 0;
    d = mock_drv(&m);
    memset(&st, 0, sizeof(st));
    st.alarm_state_path = RT_STATE;
    st.state_min_interval_ms = 1500;
    CHECK(hmi_runtime_start_ex(&rt, &cfg, &d, &st));
    CHECK(wait_unacked(&rt, 1, 4000));
    CHECK(wait_for(cond_state_saved, &rt, 4000));
    uint64_t t_first = hmi_now_ms();
    CHECK(hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_OPERATOR) == HMI_ACK_OK);
    hmi_sleep_ms(500);
    hmi_runtime_storage_info(&rt, &info);
    CHECK(info.state_saves == 1 && !same_state_on_disk(&rt)); /* held back by the interval, not by the hourly limit */
    CHECK(info.state_deferred == 0);
    CHECK(wait_for(cond_second_save, &rt, 5000));
    CHECK(hmi_now_ms() - t_first >= 1400u); /* and written once the interval had passed */
    CHECK(wait_for(cond_replace_ok_and_clear, &rt, 3000));
    hmi_runtime_stop(&rt);

    /* a failed write is retried and the fault flag follows */
    clean_state_files();
    mock_init(&m);
    m.hold[0] = 900;
    m.timeout_cost = 0;
    d = mock_drv(&m);
    memset(&st, 0, sizeof(st));
    st.alarm_state_path = RT_STATE;
    st.state_min_interval_ms = 30;
    st.io = &k_failing_io;
    g_fail_replace = 2;
    g_replace_calls = 0;
    CHECK(hmi_runtime_start_ex(&rt, &cfg, &d, &st));
    CHECK(wait_unacked(&rt, 1, 4000));
    CHECK(wait_for(cond_replace_ok_and_clear, &rt, 6000));
    hmi_runtime_storage_info(&rt, &info);
    CHECK(info.store.failed == 2 && info.state_saves >= 3 && g_replace_calls >= 3);
    CHECK(!hmi_runtime_storage_fault(&rt));
    hmi_runtime_stop(&rt);
    clean_state_files();
}

/* ---- stop while the comms thread is inside a slow PLC call ---- */
typedef struct {
    hmi_plc_driver_t inner;
    uint32_t delay_ms;
    volatile int32_t in_write;     /* set when the write call starts */
} slow_ctx_t;

static hmi_io_status_t slow_connect(void *c, uint32_t t) { slow_ctx_t *s = (slow_ctx_t *)c; return s->inner.connect(s->inner.ctx, t); }
static hmi_io_status_t slow_read(void *c, hmi_area_t a, uint16_t addr, uint16_t n, uint16_t *out, uint8_t *exc)
{
    slow_ctx_t *s = (slow_ctx_t *)c;
    if (__atomic_load_n(&s->in_write, __ATOMIC_SEQ_CST)) hmi_sleep_ms(s->delay_ms); /* the read-back is slow too */
    return s->inner.read_regs(s->inner.ctx, a, addr, n, out, exc);
}
static hmi_io_status_t slow_write(void *c, uint16_t addr, uint16_t n, const uint16_t *v, uint8_t *exc)
{
    slow_ctx_t *s = (slow_ctx_t *)c;
    __atomic_store_n(&s->in_write, 1, __ATOMIC_SEQ_CST);
    hmi_sleep_ms(s->delay_ms);
    return s->inner.write_regs(s->inner.ctx, addr, n, v, exc);
}
static void slow_close(void *c) { slow_ctx_t *s = (slow_ctx_t *)c; s->inner.close(s->inner.ctx); }

static void test_runtime_stop_during_write(void)
{
    SECTION("runtime: stop while the PLC write is in flight completes inside the budget and keeps the audit trail");
    static hmi_app_cfg_t cfg;
    char err[128], text[8192];
    CHECK(parse(GOOD_CFG, &cfg, err));
    static mock_t m;
    static hmi_runtime_t rt;
    static slow_ctx_t sc;
    const char *path = "p99_stop_audit_test.log";
    remove(path);
    mock_init(&m);
    m.timeout_cost = 0;
    sc.inner = mock_drv(&m);
    sc.delay_ms = 700;
    sc.in_write = 0;
    hmi_plc_driver_t d = { &sc, slow_connect, slow_read, slow_write, slow_close };
    CHECK(hmi_runtime_start(&rt, &cfg, &d, path, NULL));
    CHECK(rt.writes_effective);
    hmi_sleep_ms(400); /* connected and polling */
    uint32_t id = 0;
    CHECK(hmi_writes_submit_as(&rt.writes, 0, 40.0, HMI_ROLE_OPERATOR, "op1", hmi_comms_link_health(&rt.comms),
                               hmi_now_ms(), &id) == HMI_WRS_OK);
    for (int t = 0; t < 4000 && !__atomic_load_n(&sc.in_write, __ATOMIC_SEQ_CST); t += 5) hmi_sleep_ms(5);
    CHECK(__atomic_load_n(&sc.in_write, __ATOMIC_SEQ_CST) == 1); /* the comms thread is now inside the write */
    uint64_t t0 = hmi_now_ms();
    CHECK(hmi_runtime_stop(&rt));        /* waits for the write and its read-back, then drains the queue */
    uint64_t took = hmi_now_ms() - t0;
    CHECK(took < hmi_runtime_worst_stall_ms(&rt) + HMI_STORE_STOP_MS);
    CHECK(!rt.started);
    CHECK(hmi_runtime_stop(&rt));        /* a second call is harmless */

    CHECK(slurp(path, text, sizeof(text)) > 0);
    CHECK(strstr(text, " RESERVED id=") != NULL && strstr(text, " SENT id=") != NULL);
    CHECK(strstr(text, " CONFIRMED id=") != NULL);   /* the final record was not lost at shutdown */
    CHECK(strstr(text, " RESERVED id=") < strstr(text, " SENT id=") && strstr(text, " SENT id=") < strstr(text, " CONFIRMED id="));
    CHECK(m.hold[0] == 400);                          /* 40.0 at scale 0.1 */
    CHECK(text[strlen(text) - 1] == '\n');            /* only complete lines */
    remove(path);
}

/* ---- the last alarm state is written at shutdown even when the storage queue is full ---- */
static bool slow_append_short(const char *path, const void *d, size_t n)
{
    hmi_sleep_ms(40);
    return hmi_fs_append_sync(path, d, n);
}
static const hmi_store_io_t k_slow_short_io = { slow_append_short, hmi_fs_atomic_write, hmi_fs_size, hmi_fs_rename };

static void test_runtime_final_state_with_full_queue(void)
{
    SECTION("runtime: the final alarm state is written at shutdown even if the storage queue is full");
    static hmi_app_cfg_t cfg;
    char err[128];
    CHECK(parse(GOOD_CFG, &cfg, err));
    static mock_t m;
    static hmi_runtime_t rt;
    clean_state_files();
    mock_init(&m);
    m.hold[0] = 900;
    m.timeout_cost = 0;
    hmi_plc_driver_t d = mock_drv(&m);
    hmi_runtime_storage_t st;
    memset(&st, 0, sizeof(st));
    st.alarm_state_path = RT_STATE;
    st.state_min_interval_ms = 3600000; /* after the first save, changes wait for the shutdown */
    st.io = &k_slow_short_io;
    CHECK(hmi_runtime_start_ex(&rt, &cfg, &d, &st));
    CHECK(wait_unacked(&rt, 1, 4000));
    CHECK(wait_for(cond_state_saved, &rt, 4000));
    CHECK(hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_OPERATOR) == HMI_ACK_OK);
    hmi_sleep_ms(200);
    CHECK(!same_state_on_disk(&rt)); /* the acknowledged state is not on disk yet */
    int queued = 0;
    while (hmi_store_enqueue(&rt.store, 1, "filler line\n", 12, NULL, NULL, 0, NULL)) queued++; /* fill the queue; the disk is slow */
    CHECK(queued >= HMI_STORE_QUEUE_LEN - 2);
    CHECK(hmi_runtime_stop(&rt));
    uint8_t disk[HMI_ALARM_STATE_MAX];
    size_t len = 0;
    CHECK(hmi_fs_read_all(RT_STATE, disk, sizeof(disk), &len) == HMI_FS_READ_OK && len > 0);
    hmi_alarm_point_t p;
    {
        /* load it into a fresh runtime: the alarm must come back acknowledged */
        mock_init(&m);
        m.timeout_cost = 0;
        m.read_st = HMI_IO_TIMEOUT;
        d = mock_drv(&m);
        memset(&st, 0, sizeof(st));
        st.alarm_state_path = RT_STATE;
        CHECK(hmi_runtime_start_ex(&rt, &cfg, &d, &st));
        hmi_alarm_get(&rt.alarms, 0, &p);
        CHECK(p.state == HMI_AL_ACTIVE_ACK);
        hmi_runtime_stop(&rt);
    }
    clean_state_files();
}

/* ---- the SENT record is durable before the request leaves, even with a slow disk ---- */
typedef struct {
    hmi_plc_driver_t inner;
    const char *audit_path;
    volatile int32_t writes;
    volatile int32_t sent_visible;   /* writes that found their SENT record already in the file */
} probe_ctx_t;

static hmi_io_status_t pr_connect(void *c, uint32_t t) { probe_ctx_t *p = (probe_ctx_t *)c; return p->inner.connect(p->inner.ctx, t); }
static hmi_io_status_t pr_read(void *c, hmi_area_t a, uint16_t addr, uint16_t n, uint16_t *out, uint8_t *exc)
{
    probe_ctx_t *p = (probe_ctx_t *)c;
    return p->inner.read_regs(p->inner.ctx, a, addr, n, out, exc);
}
static hmi_io_status_t pr_write(void *c, uint16_t addr, uint16_t n, const uint16_t *v, uint8_t *exc)
{
    probe_ctx_t *p = (probe_ctx_t *)c;
    char text[8192];
    size_t len = slurp(p->audit_path, text, sizeof(text)); /* what is on disk at the moment the request leaves */
    __atomic_fetch_add(&p->writes, 1, __ATOMIC_SEQ_CST);
    if (len > 0 && strstr(text, " SENT id=") != NULL) __atomic_fetch_add(&p->sent_visible, 1, __ATOMIC_SEQ_CST);
    return p->inner.write_regs(p->inner.ctx, addr, n, v, exc);
}
static void pr_close(void *c) { probe_ctx_t *p = (probe_ctx_t *)c; p->inner.close(p->inner.ctx); }

static bool slow_append(const char *path, const void *d, size_t n)
{
    hmi_sleep_ms(120); /* a slow disk */
    return hmi_fs_append_sync(path, d, n);
}
static const hmi_store_io_t k_slow_io = { slow_append, hmi_fs_atomic_write, hmi_fs_size, hmi_fs_rename };

static void test_runtime_sent_before_send(void)
{
    SECTION("runtime: the SENT record is on disk before the PLC receives the write, with a slow disk");
    static hmi_app_cfg_t cfg;
    char err[128];
    CHECK(parse(GOOD_CFG, &cfg, err));
    cfg.write_min_interval_ms = 0;
    cfg.write_max_per_minute = 0;
    static mock_t m;
    static hmi_runtime_t rt;
    static probe_ctx_t pc;
    const char *path = "p99_sent_first_test.log";
    remove(path);
    mock_init(&m);
    m.timeout_cost = 0;
    pc.inner = mock_drv(&m);
    pc.audit_path = path;
    pc.writes = 0;
    pc.sent_visible = 0;
    hmi_plc_driver_t d = { &pc, pr_connect, pr_read, pr_write, pr_close };
    hmi_runtime_storage_t st;
    memset(&st, 0, sizeof(st));
    st.audit_path = path;
    st.io = &k_slow_io;
    CHECK(hmi_runtime_start_ex(&rt, &cfg, &d, &st));
    hmi_sleep_ms(500); /* the boot marker is written, the link is up */
    uint32_t ids[3];
    for (int i = 0; i < 3; i++) {
        CHECK(hmi_writes_submit_as(&rt.writes, 0, 20.0 + i, HMI_ROLE_OPERATOR, "op1", hmi_comms_link_health(&rt.comms),
                                   hmi_now_ms(), &ids[i]) == HMI_WRS_OK);
    }
    for (int t = 0; t < 8000 && __atomic_load_n(&pc.writes, __ATOMIC_SEQ_CST) < 3; t += 10) hmi_sleep_ms(10);
    CHECK(__atomic_load_n(&pc.writes, __ATOMIC_SEQ_CST) == 3);
    CHECK(__atomic_load_n(&pc.sent_visible, __ATOMIC_SEQ_CST) == 3); /* every request found its record already durable */
    CHECK(hmi_runtime_stop(&rt));
    remove(path);
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
    test_runtime_journal_and_state();
    test_runtime_journal_reload_damage();
    test_runtime_state_limits();
    test_runtime_stop_during_write();
    test_runtime_final_state_with_full_queue();
    test_runtime_sent_before_send();
    test_concurrent_consumers();
    return test_finish();
}
