/* Child process of the kill-loop test (tests/hmi/killloop_test.sh, Linux).
 *
 * Runs the real runtime against the scripted mock PLC with the audit file, the
 * alarm journal and the alarm state in one directory, and keeps generating
 * work: the process value moves in and out of alarm, setpoints are written,
 * alarms are acknowledged. The script kills it with SIGKILL at random times.
 * SIGTERM stops it cleanly.
 *
 * Before the mock "transmits" a write it records the write in plc.log and
 * flushes that file, which stands for the moment the request leaves the
 * device. The verifier then checks that every write in plc.log has a SENT
 * record in the audit file: the record of intent was durable before the
 * request could leave, even when the process died at any point.
 *
 *   usage: killloop_child DIR
 */
#include "test_support.h"
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static void on_term(int s)
{
    (void)s;
    g_stop = 1;
}

typedef struct {
    hmi_plc_driver_t inner;
    int fd;
} logdrv_t;

static hmi_io_status_t l_connect(void *c, uint32_t t) { logdrv_t *d = (logdrv_t *)c; return d->inner.connect(d->inner.ctx, t); }
static hmi_io_status_t l_read(void *c, hmi_area_t a, uint16_t addr, uint16_t n, uint16_t *out, uint8_t *exc)
{
    logdrv_t *d = (logdrv_t *)c;
    return d->inner.read_regs(d->inner.ctx, a, addr, n, out, exc);
}
static hmi_io_status_t l_write(void *c, uint16_t addr, uint16_t n, const uint16_t *v, uint8_t *exc)
{
    logdrv_t *d = (logdrv_t *)c;
    char line[80];
    int len = snprintf(line, sizeof(line), "W addr=%u n=%u v0=%u v1=%u\n", (unsigned)addr, (unsigned)n, (unsigned)v[0],
                       n > 1 ? (unsigned)v[1] : 0u);
    if (len > 0 && write(d->fd, line, (size_t)len) == len) fsync(d->fd); /* the request leaves here */
    return d->inner.write_regs(d->inner.ctx, addr, n, v, exc);
}
static void l_close(void *c) { logdrv_t *d = (logdrv_t *)c; d->inner.close(d->inner.ctx); }

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: killloop_child DIR\n");
        return 2;
    }
    char audit[300], alarms[300], plclog[300];
    snprintf(audit, sizeof(audit), "%s/audit.log", argv[1]);
    snprintf(alarms, sizeof(alarms), "%s/alarms.bin", argv[1]);
    snprintf(plclog, sizeof(plclog), "%s/plc.log", argv[1]);

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    static hmi_app_cfg_t cfg;
    char err[128];
    if (!parse(GOOD_CFG, &cfg, err)) return 2;
    cfg.write_min_interval_ms = 0; /* generate load: no rate limit in this test */
    cfg.write_max_per_minute = 0;

    static mock_t m;
    mock_init(&m);
    m.hold[0] = 500;
    m.timeout_cost = 0;
    static logdrv_t ld;
    ld.inner = mock_drv(&m);
    ld.fd = open(plclog, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (ld.fd < 0) return 2;
    hmi_plc_driver_t drv = { &ld, l_connect, l_read, l_write, l_close };

    hmi_runtime_storage_t st;
    memset(&st, 0, sizeof(st));
    st.audit_path = audit;
    st.alarm_state_path = alarms;
    st.log_max_bytes = 32768; /* small, so that rotation happens while we run; the 8 kept files hold more than a run writes */
    st.log_generations = 8;
    st.state_min_interval_ms = 100;
    static hmi_runtime_t rt;
    if (!hmi_runtime_start_ex(&rt, &cfg, &drv, &st)) {
        fprintf(stderr, "killloop_child: runtime did not start\n");
        return 3;
    }
    printf("STARTED\n");
    fflush(stdout);

    uint64_t t_toggle = hmi_now_ms(), t_write = t_toggle, t_ack = t_toggle;
    unsigned k = 0;
    while (!g_stop) {
        uint64_t now = hmi_now_ms();
        if (now - t_toggle >= 1100) {
            t_toggle = now;
            k++;
        }
        if (now - t_write >= 15) {
            /* The setpoint is the process value of this mock, so the written value moves the alarm in and
             * out: 90 % raises the high alarm (limit 80), 50 % clears it. */
            uint32_t id;
            double base = (k & 1u) ? 50.0 : 90.0;
            hmi_writes_submit_as(&rt.writes, 0, base + (double)((now / 15) % 10) / 10.0, HMI_ROLE_OPERATOR, "killer",
                                 hmi_comms_link_health(&rt.comms), now, &id);
            t_write = now;
        }
        if (now - t_ack >= 400) {
            hmi_alarm_ack_all_as(&rt.alarms, now, HMI_ROLE_OPERATOR, "killer");
            t_ack = now;
        }
        hmi_sleep_ms(3);
    }
    bool clean = hmi_runtime_stop(&rt);
    printf("STOPPED clean=%d\n", clean ? 1 : 0);
    close(ld.fd);
    return clean ? 0 : 3;
}
