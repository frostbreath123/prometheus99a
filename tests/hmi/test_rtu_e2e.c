/* End-to-end test of Modbus RTU (HMI_RTU, Linux): the real runtime and libmodbus's RTU backend on one end of a
 * pseudo-terminal pair, a small fake RTU slave on the other. No serial hardware: the pty carries the bytes, so the
 * framing, the CRC, the timeouts and the recovery are those of libmodbus, but not the timing of a real line.
 *
 * Checked: values are read and GOOD; an audited write is sent once, confirmed by the read-back and lands in the slave's
 * register; a silent slave (frames read, no answer) is shown as a lost link within the configured timeouts; the link
 * comes back when the slave answers again; the runtime stops cleanly. */
#define _GNU_SOURCE
#include "test_support.h"
#include "../../src/hmi/plc_libmodbus.h"
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

/* ---- fake RTU slave on the pty master ---- */
static int g_fd = -1;
static uint16_t g_hold[64];
static volatile int g_silent, g_stop;
static volatile int g_requests, g_writes;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
    }
    return c;
}

static void reply(uint8_t *r, size_t n)
{
    uint16_t c = crc16(r, n);
    r[n] = (uint8_t)(c & 0xFF);
    r[n + 1] = (uint8_t)(c >> 8);
    ssize_t w = write(g_fd, r, n + 2);
    (void)w;
}

/* One request: collect bytes until the line is quiet for 5 ms (an RTU frame ends with a silent gap). */
static void *slave_thread(void *arg)
{
    (void)arg;
    uint8_t f[260];
    while (!__atomic_load_n(&g_stop, __ATOMIC_SEQ_CST)) {
        size_t n = 0;
        struct pollfd p = { g_fd, POLLIN, 0 };
        if (poll(&p, 1, 50) <= 0) continue;
        for (;;) {
            ssize_t r = read(g_fd, f + n, sizeof(f) - n);
            if (r > 0) n += (size_t)r;
            if (n >= sizeof(f) || poll(&p, 1, 5) <= 0) break;
        }
        if (n < 4 || f[0] != 1 || crc16(f, n - 2) != (uint16_t)(f[n - 2] | (f[n - 1] << 8))) continue;
        __atomic_fetch_add(&g_requests, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g_silent, __ATOMIC_SEQ_CST)) continue; /* read, never answered */
        uint8_t r[260];
        uint16_t addr = (uint16_t)(f[2] << 8 | f[3]), cnt = (uint16_t)(f[4] << 8 | f[5]);
        r[0] = 1;
        r[1] = f[1];
        pthread_mutex_lock(&g_mx);
        if ((f[1] == 3 || f[1] == 4) && addr + cnt <= 64 && cnt >= 1 && cnt <= 125) {
            r[2] = (uint8_t)(2 * cnt);
            for (uint16_t i = 0; i < cnt; i++) {
                r[3 + 2 * i] = (uint8_t)(g_hold[addr + i] >> 8);
                r[4 + 2 * i] = (uint8_t)(g_hold[addr + i] & 0xFF);
            }
            pthread_mutex_unlock(&g_mx);
            reply(r, 3 + 2u * cnt);
        } else if (f[1] == 6 && addr < 64) {
            g_hold[addr] = cnt; /* for function 6 the second word is the value */
            pthread_mutex_unlock(&g_mx);
            __atomic_fetch_add(&g_writes, 1, __ATOMIC_SEQ_CST);
            memcpy(r, f, 6);
            reply(r, 6);
        } else if (f[1] == 16 && addr + cnt <= 64 && n >= 9u + 2u * cnt) {
            for (uint16_t i = 0; i < cnt; i++) g_hold[addr + i] = (uint16_t)(f[7 + 2 * i] << 8 | f[8 + 2 * i]);
            pthread_mutex_unlock(&g_mx);
            __atomic_fetch_add(&g_writes, 1, __ATOMIC_SEQ_CST);
            memcpy(r, f, 6);
            reply(r, 6);
        } else {
            pthread_mutex_unlock(&g_mx);
            r[1] = (uint8_t)(f[1] | 0x80);
            r[2] = 2; /* illegal data address */
            reply(r, 3);
        }
    }
    return NULL;
}

static uint16_t hold(int i)
{
    pthread_mutex_lock(&g_mx);
    uint16_t v = g_hold[i];
    pthread_mutex_unlock(&g_mx);
    return v;
}

static bool wait_quality(hmi_runtime_t *rt, hmi_quality_t q, int max_ms, int *took)
{
    for (int t = 0; t < max_ms; t += 20) {
        hmi_tag_value_t v;
        hmi_tagdb_get(&rt->db, 0, &v);
        if (v.quality == q) {
            if (took) *took = t;
            return true;
        }
        hmi_sleep_ms(20);
    }
    return false;
}

static void test_rtu_e2e(void)
{
    SECTION("rtu e2e: the runtime over libmodbus RTU on a pty: read, audited write, silent line, recovery");
    int master, slave;
    char name[128];
    CHECK(openpty(&master, &slave, name, NULL, NULL) == 0);
    struct termios t;
    tcgetattr(master, &t);
    cfmakeraw(&t);
    tcsetattr(master, TCSANOW, &t);
    g_fd = master;
    g_hold[0] = 421; /* LVL 42.1 */
    pthread_t th;
    CHECK(pthread_create(&th, NULL, slave_thread, NULL) == 0);

    char json[1024], err[128];
    snprintf(json, sizeof(json),
             "{\"version\":1,\"writes_enabled\":true,\"write_min_interval_ms\":100,"
             "\"plc\":{\"transport\":\"rtu\",\"device\":\"%s\",\"baud\":115200,\"parity\":\"none\",\"unit\":1,"
             "\"response_timeout_ms\":200,\"allowed_devices\":[\"%s\"]},"
             "\"tags\":[{\"name\":\"LVL\",\"address\":0,\"type\":\"u16\",\"scale\":0.1,\"min\":0,\"max\":100,\"poll_ms\":100,"
             "\"write\":{\"allowed\":true,\"role\":\"operator\",\"min\":0,\"max\":100}}]}",
             name, name);
    static hmi_app_cfg_t cfg;
    CHECK(parse(json, &cfg, err));
    CHECK(cfg.transport == HMI_TRANSPORT_RTU && hmi_config_host_allowed(&cfg));
    plc_libmodbus_t *plc = plc_libmodbus_create_rtu(cfg.device, cfg.baud, (char)cfg.parity, cfg.stop_bits, cfg.unit,
                                                    cfg.response_timeout_ms);
    CHECK(plc != NULL);
    hmi_plc_driver_t d;
    plc_libmodbus_driver(plc, &d);
    const char *audit = "p99_rtu_e2e_audit.log";
    remove(audit);
    static hmi_runtime_t rt;
    CHECK(hmi_runtime_start(&rt, &cfg, &d, audit, NULL));
    CHECK(rt.writes_effective);

    /* read */
    int took = 0;
    CHECK(wait_quality(&rt, HMI_Q_GOOD, 3000, &took));
    hmi_tag_value_t v;
    hmi_tagdb_get(&rt.db, 0, &v);
    printf("  first GOOD value after %d ms: %.1f\n", took, v.eng);
    CHECK(fabs(v.eng - 42.1) < 1e-9);

    /* an audited write: sent once, confirmed by the read-back, in the slave's register */
    int w0 = __atomic_load_n(&g_writes, __ATOMIC_SEQ_CST);
    uint32_t id = 0;
    CHECK(hmi_writes_submit_as(&rt.writes, 0, 55.5, HMI_ROLE_OPERATOR, "rtu-test", hmi_comms_link_health(&rt.comms),
                               hmi_now_ms(), &id) == HMI_WRS_OK);
    hmi_write_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    for (int i = 0; i < 150; i++) {
        if (hmi_writes_status(&rt.writes, id, &rec) && rec.state == HMI_WR_CONFIRMED) break;
        hmi_sleep_ms(20);
    }
    CHECK(rec.state == HMI_WR_CONFIRMED);
    CHECK(hold(0) == 555 && __atomic_load_n(&g_writes, __ATOMIC_SEQ_CST) == w0 + 1);

    /* a silent slave: the link is lost (data BAD) within the timeouts; requests stay bounded */
    __atomic_store_n(&g_silent, 1, __ATOMIC_SEQ_CST);
    int r0 = __atomic_load_n(&g_requests, __ATOMIC_SEQ_CST);
    CHECK(wait_quality(&rt, HMI_Q_BAD, 8000, &took));
    printf("  silent slave: data BAD after %d ms, %d requests meanwhile\n", took,
           __atomic_load_n(&g_requests, __ATOMIC_SEQ_CST) - r0);
    hmi_alarm_point_t cp;
    for (int i = 0; i < 100; i++) {
        hmi_alarm_get(&rt.alarms, hmi_tagdb_count(&rt.db), &cp);
        if (cp.kind == HMI_ALK_COMMS) break;
        hmi_sleep_ms(50);
    }
    CHECK(cp.kind == HMI_ALK_COMMS); /* the communication-loss alarm, as for TCP */

    /* the slave answers again: the link comes back */
    pthread_mutex_lock(&g_mx);
    g_hold[0] = 333;
    pthread_mutex_unlock(&g_mx);
    __atomic_store_n(&g_silent, 0, __ATOMIC_SEQ_CST);
    CHECK(wait_quality(&rt, HMI_Q_GOOD, 15000, &took));
    hmi_tagdb_get(&rt.db, 0, &v);
    printf("  slave answers again: GOOD after %d ms, value %.1f\n", took, v.eng);
    CHECK(fabs(v.eng - 33.3) < 1e-9);

    CHECK(hmi_runtime_stop(&rt));
    plc_libmodbus_destroy(plc);
    __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);
    pthread_join(th, NULL);
    close(slave);
    close(master);
    remove(audit);
}

int main(void)
{
    test_rtu_e2e();
    return test_finish();
}
