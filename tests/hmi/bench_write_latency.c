/* Write latency of the real runtime (docs/MASTER_PROMPT_FINAL.md, phase 2): how long a set-point takes from the
 * submit call to its arrival at the PLC, and to the CONFIRMED state after the read-back.
 *
 *   bench_write_latency [writes]      (default 60)
 *
 * The runtime runs with its real threads, the storage worker and an audit file (so a write is audited before it is
 * sent, as on the device), libmodbus and the in-process fake PLC on loopback. The tag is polled once a second, so the
 * comms thread is idle between polls and the time a queued write waits for it is visible. Writes are submitted at
 * irregular intervals (250 to 450 ms) so they fall at every phase of the comms loop. Times are taken with
 * CLOCK_MONOTONIC and polled every 0.2 ms. Linux only; a measurement tool, not a test (it has no pass or fail). */
#include "../../src/hmi/hmi_runtime.h"
#include "../../src/hmi/plc_libmodbus.h"
#include "fake_modbus_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static void nap_us(long us)
{
    struct timespec ts = { 0, us * 1000L };
    nanosleep(&ts, NULL);
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void report(const char *what, double *v, size_t n)
{
    if (n == 0) {
        printf("%s: no samples\n", what);
        return;
    }
    qsort(v, n, sizeof(double), cmp_d);
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += v[i];
    printf("%s: n=%u min=%.1f median=%.1f mean=%.1f p95=%.1f max=%.1f ms\n", what, (unsigned)n, v[0] / 1000.0,
           v[n / 2] / 1000.0, sum / (double)n / 1000.0, v[(size_t)((double)n * 0.95)] / 1000.0, v[n - 1] / 1000.0);
}

int main(int argc, char **argv)
{
    int writes = argc > 1 ? atoi(argv[1]) : 60;
    if (writes < 1 || writes > 1000) writes = 60;

    uint16_t port;
    fake_modbus_t *srv = fms_start(&port);
    fms_set_holding(srv, 0, 500);
    char json[1024];
    snprintf(json, sizeof(json),
             "{\"version\":1,\"writes_enabled\":true,\"write_min_interval_ms\":100,\"write_max_per_minute\":600,"
             "\"plc\":{\"host\":\"127.0.0.1\",\"port\":%u,\"unit\":1,\"response_timeout_ms\":200,\"connect_timeout_ms\":500},"
             "\"tags\":[{\"name\":\"SP\",\"address\":0,\"type\":\"u16\",\"scale\":0.1,\"min\":0,\"max\":100,\"poll_ms\":1000,"
             "\"write\":{\"allowed\":true,\"role\":\"operator\",\"min\":0,\"max\":100}}]}",
             (unsigned)port);
    static hmi_app_cfg_t cfg;
    char err[128];
    if (!hmi_config_parse(json, strlen(json), &cfg, err, sizeof(err))) {
        printf("config rejected: %s\n", err);
        return 2;
    }
    char audit[64];
    snprintf(audit, sizeof(audit), "/tmp/p99_wlat_%d.log", (int)getpid());
    remove(audit);
    plc_libmodbus_t *plc = plc_libmodbus_create(cfg.host, cfg.port, cfg.unit, cfg.response_timeout_ms);
    hmi_plc_driver_t drv;
    plc_libmodbus_driver(plc, &drv);
    static hmi_runtime_t rt;
    if (!hmi_runtime_start(&rt, &cfg, &drv, audit, NULL) || !rt.writes_effective) {
        printf("runtime start failed or writes not effective\n");
        return 3;
    }
    hmi_sleep_ms(1500); /* connected, first poll done */

    double *t_sent = (double *)calloc((size_t)writes, sizeof(double));
    double *t_conf = (double *)calloc((size_t)writes, sizeof(double));
    size_t n_sent = 0, n_conf = 0, refused = 0, other = 0;
    srand(12345);
    for (int i = 0; i < writes; i++) {
        hmi_sleep_ms((uint32_t)(250 + rand() % 200));
        uint32_t before = fms_write_count(srv);
        uint32_t id = 0;
        double v = 10.0 + (double)(i % 50);
        double t0 = now_us();
        hmi_wr_submit_t r = hmi_writes_submit_as(&rt.writes, 0, v, HMI_ROLE_OPERATOR, "bench",
                                                 hmi_comms_link_health(&rt.comms), hmi_now_ms(), &id);
        if (r != HMI_WRS_OK) {
            refused++;
            continue;
        }
        bool seen = false;
        for (;;) {
            double t = now_us();
            if (t - t0 > 3e6) break; /* 3 s: give up on this one */
            if (!seen && fms_write_count(srv) != before) {
                t_sent[n_sent++] = t - t0;
                seen = true;
            }
            hmi_write_rec_t rec;
            if (hmi_writes_status(&rt.writes, id, &rec) && rec.state != HMI_WR_RESERVED && rec.state != HMI_WR_PENDING &&
                rec.state != HMI_WR_SENT) {
                if (rec.state == HMI_WR_CONFIRMED) t_conf[n_conf++] = t - t0;
                else other++;
                break;
            }
            nap_us(200);
        }
    }
    hmi_runtime_stop(&rt);
    plc_libmodbus_destroy(plc);
    fms_stop(srv);
    remove(audit);

    printf("writes submitted=%d refused=%u ended other than CONFIRMED=%u\n", writes, (unsigned)refused, (unsigned)other);
    report("submit to arrival at the PLC", t_sent, n_sent);
    report("submit to CONFIRMED (after read-back)", t_conf, n_conf);
    free(t_sent);
    free(t_conf);
    return 0;
}
