/* Shared fixtures for the per-module HMI core tests (see test_support.h). */
#include "test_support.h"

int g_fail = 0, g_checks = 0;

int test_finish(void)
{
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

/* ------------------------------------------------------------ fake clock */
uint64_t g_now = 1000;
static uint64_t fake_clock(void *u) { (void)u; return g_now; }

/* ------------------------------------------------------------ mock PLC */
static hmi_io_status_t m_connect(void *c, uint32_t to)
{
    mock_t *m = (mock_t *)c;
    m->n_connect++;
    g_now += m->connect_st == HMI_IO_OK ? m->connect_cost : to;
    return m->connect_st;
}

static hmi_io_status_t m_read(void *c, hmi_area_t a, uint16_t addr, uint16_t n, uint16_t *out, uint8_t *exc)
{
    mock_t *m = (mock_t *)c;
    m->n_read++;
    m->hit_addr[addr & 255]++;
    if (m->fail_next_read_after_write) {
        m->fail_next_read_after_write = false;
        g_now += m->timeout_cost;
        return m->readback_st;
    }
    if (m->read_st != HMI_IO_OK && (m->fail_addr < 0 || m->fail_addr == (int)addr)) {
        if (m->read_st == HMI_IO_EXCEPTION) {
            *exc = m->read_exc;
            g_now += 2;
        } else {
            g_now += m->timeout_cost;
        }
        return m->read_st;
    }
    const uint16_t *src = (a == HMI_AREA_INPUT) ? m->inp : m->hold;
    /* Relaxed atomic loads: test_concurrent_consumers changes hold[0] from the
     * main thread while this runs on the comms thread. */
    for (uint16_t i = 0; i < n; i++) out[i] = __atomic_load_n(&src[addr + i], __ATOMIC_RELAXED);
    g_now += 1;
    return HMI_IO_OK;
}

static hmi_io_status_t m_write(void *c, uint16_t addr, uint16_t n, const uint16_t *v, uint8_t *exc)
{
    mock_t *m = (mock_t *)c;
    m->n_write++;
    if (m->write_st == HMI_IO_OK) {
        for (uint16_t i = 0; i < n; i++) m->hold[addr + i] = (uint16_t)(v[i] + (m->corrupt_write ? 1 : 0));
        g_now += 1;
        return HMI_IO_OK;
    }
    if (m->write_st == HMI_IO_EXCEPTION) {
        *exc = m->write_exc;
        g_now += 2;
        return HMI_IO_EXCEPTION;
    }
    if (m->apply_on_fail) {
        for (uint16_t i = 0; i < n; i++) m->hold[addr + i] = v[i];
    }
    g_now += m->timeout_cost;
    return m->write_st;
}

static void m_close(void *c) { ((mock_t *)c)->n_close++; }

void mock_init(mock_t *m)
{
    memset(m, 0, sizeof(*m));
    m->fail_addr = -1;
    m->timeout_cost = 100;
    m->connect_cost = 5;
}

hmi_plc_driver_t mock_drv(mock_t *m)
{
    hmi_plc_driver_t d = { m, m_connect, m_read, m_write, m_close };
    return d;
}

/* ------------------------------------------------------------ fixtures */
void make_tags(hmi_tag_cfg_t *t, size_t *n)
{
    memset(t, 0, 4 * sizeof(*t));
    snprintf(t[0].name, HMI_NAME_LEN, "LVL");
    t[0].area = HMI_AREA_HOLDING; t[0].address = 0; t[0].dtype = HMI_DT_U16;
    t[0].scale = 0.1; t[0].eng_min = 0; t[0].eng_max = 100; t[0].poll_ms = 100;
    t[0].alarm.en_hi = true; t[0].alarm.hi = 80; t[0].alarm.en_hihi = true; t[0].alarm.hihi = 95;
    t[0].alarm.en_lo = true; t[0].alarm.lo = 10; t[0].alarm.deadband = 2;
    t[0].alarm.on_ms = 300; t[0].alarm.off_ms = 300;
    t[0].write_allowed = true; t[0].write_role = HMI_ROLE_OPERATOR; t[0].write_min = 0; t[0].write_max = 100;

    snprintf(t[1].name, HMI_NAME_LEN, "PRESS");
    t[1].area = HMI_AREA_HOLDING; t[1].address = 1; t[1].dtype = HMI_DT_I16;
    t[1].scale = 1.0; t[1].eng_min = -1000; t[1].eng_max = 1000; t[1].poll_ms = 100;

    snprintf(t[2].name, HMI_NAME_LEN, "TEMP32");
    t[2].area = HMI_AREA_HOLDING; t[2].address = 10; t[2].dtype = HMI_DT_F32;
    t[2].scale = 1.0; t[2].eng_min = -50; t[2].eng_max = 500; t[2].poll_ms = 100;

    snprintf(t[3].name, HMI_NAME_LEN, "CNT");
    t[3].area = HMI_AREA_INPUT; t[3].address = 20; t[3].dtype = HMI_DT_U32; t[3].word_order = HMI_WORD_LOW_FIRST;
    t[3].scale = 1.0; t[3].eng_min = 0; t[3].eng_max = 4294967295.0; t[3].poll_ms = 500;
    *n = 4;
}

hmi_comms_cfg_t default_cc(void)
{
    hmi_comms_cfg_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.response_timeout_ms = 100;
    cc.connect_timeout_ms = 200;
    cc.miss_limit = 3;
    cc.backoff_min_ms = 500;
    cc.backoff_max_ms = 4000;
    cc.jitter_seed = 7;
    return cc;
}

fx_t *fx_new(bool with_alarm)
{
    fx_t *f = (fx_t *)calloc(1, sizeof(*f));
    g_now = 1000;
    mock_init(&f->m);
    make_tags(f->cfg, &f->n);
    f->m.hold[0] = 500;            /* 50.0 % */
    f->m.hold[1] = (uint16_t)-12;  /* -12 */
    f->m.inp[20] = 0x0001;         /* low word first: low=0x0001 */
    f->m.inp[21] = 0x0002;         /* high=0x0002 -> 0x00020001 */
    float t = 21.5f;
    uint32_t tb;
    memcpy(&tb, &t, 4);
    f->m.hold[10] = (uint16_t)(tb >> 16);
    f->m.hold[11] = (uint16_t)(tb & 0xFFFF);
    hmi_tagdb_init(&f->db, f->cfg, f->n, g_now);
    hmi_plc_driver_t d = mock_drv(&f->m);
    hmi_comms_cfg_t cc = default_cc();
    hmi_comms_init(&f->c, &f->db, &d, &cc);
    hmi_comms_set_clock(&f->c, fake_clock, NULL);
    hmi_writes_init(&f->w, &f->db, true, 2000);
    hmi_comms_set_writes(&f->c, &f->w);
    if (with_alarm) hmi_alarm_init(&f->al, &f->db);
    return f;
}

void fx_free(fx_t *f, bool with_alarm)
{
    if (with_alarm) hmi_alarm_destroy(&f->al);
    hmi_writes_destroy(&f->w);
    hmi_comms_destroy(&f->c);
    hmi_tagdb_destroy(&f->db);
    free(f);
}

hmi_wr_submit_t submit(fx_t *f, size_t tag, double eng, hmi_role_t role, uint32_t *id)
{
    return hmi_writes_submit(&f->w, tag, eng, role, hmi_comms_link_health(&f->c), g_now, id);
}

/* Run comms until fake time reaches t_end. Returns steps taken. */
int run_until(fx_t *f, uint64_t t_end)
{
    int steps = 0;
    while (g_now < t_end && steps < 100000) {
        uint32_t hint = hmi_comms_step(&f->c);
        steps++;
        if (hint > 0) {
            uint64_t adv = hint;
            if (g_now + adv > t_end) adv = t_end - g_now;
            g_now += adv;
        }
    }
    return steps;
}

hmi_tag_value_t val(fx_t *f, size_t i)
{
    hmi_tag_value_t v;
    hmi_tagdb_get(&f->db, i, &v);
    return v;
}

/* ---- config text ---- */
const char *GOOD_CFG =
    "{\"version\":1,"
    "\"plc\":{\"host\":\"127.0.0.1\",\"port\":5020,\"unit\":1,\"response_timeout_ms\":300},"
    "\"writes_enabled\":true,"
    "\"tags\":["
    "{\"name\":\"LVL\",\"area\":\"holding\",\"address\":0,\"type\":\"u16\",\"scale\":0.1,\"units\":\"%\","
    "\"min\":0,\"max\":100,\"poll_ms\":200,"
    "\"alarm\":{\"high\":80,\"high_high\":95,\"deadband\":2,\"on_ms\":500,\"off_ms\":500},"
    "\"write\":{\"allowed\":true,\"role\":\"operator\",\"min\":0,\"max\":100}},"
    "{\"name\":\"TEMP\",\"area\":\"input\",\"address\":10,\"type\":\"f32\",\"word_order\":\"low_first\"}"
    "]}";

bool parse(const char *s, hmi_app_cfg_t *c, char *err)
{
    return hmi_config_parse(s, strlen(s), c, err, 128);
}
