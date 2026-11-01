/* Headless host harness: real LVGL v9.2.2 + real UI binder + real libmodbus
 * runtime against the fake Modbus server. Renders into memory (no window),
 * drives a scripted scenario, writes PPM screenshots and prints raw
 * measurements. HOST measurements only: not representative of a Raspberry Pi. */
#include "lvgl.h"
#include "../../src/ui/hmi_ui.h"
#include "../../src/hmi/plc_libmodbus.h"
#include "../../src/hmi/hmi_auditfmt.h"
#include "fake_modbus_server.h"
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <psapi.h>
#define UI_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <time.h>
#define UI_MKDIR(p) mkdir((p), 0700)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 800
#define H 480

/* The lite profile (Makefile.linux, LITE=1) has no trend screen and no CONFIG view: three screens, one settings view. */
#ifdef P99_LITE
#define SCREEN_SETTINGS 2
#define SCREENS_EXPECTED 3
#define SETTINGS_VIEWS 1
#define TRENDED_EXPECTED "0"
#else
#define SCREEN_SETTINGS 3
#define SCREENS_EXPECTED 4
#define SETTINGS_VIEWS 2
#define TRENDED_EXPECTED "2"
#endif

uint32_t alloc_count_total(void);
uint32_t alloc_count_free(void);
static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static uint16_t s_fb[W * H];
/* The partial draw buffer: as many lines as the display backends use (P99_DRAW_LINES in src/app/display.h), so that the
 * render times printed below are those of the product. */
#ifndef UI_BUF_LINES
#define UI_BUF_LINES 20
#endif
static uint8_t s_buf[W * UI_BUF_LINES * 2] __attribute__((aligned(64)));
static uint32_t s_flushes, s_flush_px;
static double s_flush_us_total;
static double s_last_flush_us;   /* when the newest flush started (responsiveness measurements) */

#ifdef _WIN32
static LARGE_INTEGER s_qf;
static double now_us(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)s_qf.QuadPart;
}
#else
static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}
#endif

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    double t0 = now_us();
    s_last_flush_us = t0;
    int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    const uint16_t *src = (const uint16_t *)px;
    for (int y = 0; y < h; y++) memcpy(&s_fb[(a->y1 + y) * W + a->x1], &src[y * w], (size_t)w * 2);
    s_flushes++;
    s_flush_px += (uint32_t)(w * h);
    s_flush_us_total += now_us() - t0;
    lv_display_flush_ready(d);
}

static uint32_t tick_cb(void) { return (uint32_t)hmi_now_ms(); }

static void save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t p = s_fb[i];
        uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 31) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63),
                           (uint8_t)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}


static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void pump(double seconds, double **samples, size_t *n, size_t cap)
{
    uint64_t end = hmi_now_ms() + (uint64_t)(seconds * 1000);
    while (hmi_now_ms() < end) {
        double t0 = now_us();
        lv_timer_handler();
        double dt = now_us() - t0;
        if (*n < cap) (*samples)[(*n)++] = dt;
        hmi_sleep_ms(33); /* 30 Hz UI loop */
    }
}

#ifndef _WIN32
/* Linux: RssAnon / VmRSS / VmHWM from /proc/self/status (kB). */
static void report_proc(const char *tag)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return;
    char line[160];
    long rss = -1, hwm = -1, anon = -1;
    while (fgets(line, sizeof(line), f)) {
        sscanf(line, "VmRSS: %ld", &rss);
        sscanf(line, "VmHWM: %ld", &hwm);
        sscanf(line, "RssAnon: %ld", &anon);
    }
    fclose(f);
    printf("[%s] RssAnon=%ld KB  VmRSS=%ld KB  VmHWM=%ld KB\n", tag, anon, rss, hwm);
}
#else
static void report_proc(const char *tag)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof(pmc));
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
        printf("[%s] PrivateUsage=%.0f KB  WorkingSet=%.0f KB  PeakWorkingSet=%.0f KB\n", tag,
               pmc.PrivateUsage / 1024.0, pmc.WorkingSetSize / 1024.0, pmc.PeakWorkingSetSize / 1024.0);
    }
}
#endif


/* ---- responsiveness: application level, into a memory framebuffer ----
 * What is measured: from an input event (or a tab press) to the moment LVGL hands the pixels of the answer to the
 * flush callback, with the UI loop paced as main_linux.c paces it (lv_timer_handler()'s own wait, between 5 and 33 ms).
 * What is not: the touch controller, the driver, the display's own latency. Host figures, not a Raspberry Pi. */
static int g_tx, g_ty;
static bool g_tdown;

static void touch_read_cb(lv_indev_t *i, lv_indev_data_t *d)
{
    (void)i;
    d->point.x = g_tx;
    d->point.y = g_ty;
    d->state = g_tdown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void loop_once(void)
{
    uint32_t w = lv_timer_handler();
    if (w > 33u) w = 33u;
    if (w < 5u) w = 5u;
    hmi_sleep_ms(w);
}

/* Delivers one input event at a random moment of the UI loop's sleep, as a real touch would arrive, and returns the
 * time until the newest flush that follows it started (the first pixels of the response). `r` picks the moment. */
static double inject_and_wait(bool down, uint32_t r)
{
    uint32_t w = lv_timer_handler();
    if (w > 33u) w = 33u;
    if (w < 5u) w = 5u;
    uint32_t before = r % w;
    hmi_sleep_ms(before);
    uint32_t f0 = s_flushes;
    double t_in = now_us();
    g_tdown = down;
    hmi_sleep_ms(w - before);      /* the rest of the sleep: the event waits for the loop, as it would on the device */
    while (s_flushes == f0 && now_us() - t_in < 500000.0) loop_once();
    return s_flushes == f0 ? 500000.0 : s_last_flush_us - t_in;
}

/* Runs the loop until the screen has been quiet for 80 ms (or 1 s). Returns the time of the newest flush. */
static double settle_us(void)
{
    double t_end = now_us() + 1e6;
    while (now_us() < t_end) {
        loop_once();
        if (now_us() - s_last_flush_us > 80000.0) break;
    }
    return s_last_flush_us;
}

static void print_dist(const char *what, double *v, size_t n)
{
    if (n == 0) return;
    qsort(v, n, sizeof(double), cmp_d);
    printf("[perf] %s: n=%u min=%.1f median=%.1f p95=%.1f max=%.1f ms\n", what, (unsigned)n, v[0] / 1000.0,
           v[n / 2] / 1000.0, v[(size_t)((double)n * 0.95)] / 1000.0, v[n - 1] / 1000.0);
}
int main(int argc, char **argv)
{
    const char *prefix = argc > 1 ? argv[1] : "build/shot";
    const char *audit_path = argc > 2 ? argv[2] : "build/ui_audit.log";
#ifdef _WIN32
    QueryPerformanceFrequency(&s_qf);
#endif
    report_proc("start");

    uint16_t port;
    fake_modbus_t *srv = fms_start(&port);
    fms_set_holding(srv, 0, 500);  /* LVL 50.0 % */
    fms_set_holding(srv, 1, 213);  /* TEMP 21.3 C */
    fms_set_holding(srv, 2, 40);   /* PRESS 40 */
    fms_set_holding(srv, 3, 1500); /* FLOW */

    char json[2048];
    snprintf(json, sizeof(json),
        "{\"version\":1,\"writes_enabled\":true,\"trend_bucket_ms\":1000,"
        "\"plc\":{\"host\":\"127.0.0.1\",\"port\":%u,\"unit\":1,\"response_timeout_ms\":200,\"connect_timeout_ms\":500,"
        "\"allowed_hosts\":[\"127.0.0.1\",\"127.0.0.2\"]},"
        "\"tags\":["
        "{\"name\":\"TANK_LEVEL\",\"trend\":true,\"address\":0,\"type\":\"u16\",\"scale\":0.1,\"units\":\"%%\",\"min\":0,\"max\":100,\"poll_ms\":100,"
        "\"alarm\":{\"high\":80,\"high_high\":95,\"low\":10,\"deadband\":2,\"on_ms\":300,\"off_ms\":300,\"priority\":\"critical\"},"
        "\"write\":{\"allowed\":true,\"role\":\"operator\",\"min\":0,\"max\":100}},"
        "{\"name\":\"TEMP\",\"trend\":true,\"address\":1,\"type\":\"i16\",\"scale\":0.1,\"units\":\"C\",\"min\":-40,\"max\":150,\"poll_ms\":100},"
        "{\"name\":\"PRESSURE\",\"address\":2,\"type\":\"u16\",\"units\":\"kPa\",\"min\":0,\"max\":1000,\"poll_ms\":100},"
        "{\"name\":\"FLOW\",\"address\":3,\"type\":\"u16\",\"units\":\"l/min\",\"min\":0,\"max\":5000,\"poll_ms\":250}"
        "]}", (unsigned)port);

    static hmi_app_cfg_t cfg;
    char err[128];
    if (!hmi_config_parse(json, strlen(json), &cfg, err, sizeof(err))) {
        printf("config rejected: %s\n", err);
        return 2;
    }
    plc_libmodbus_t *plc = plc_libmodbus_create(cfg.host, cfg.port, cfg.unit, cfg.response_timeout_ms);
    hmi_plc_driver_t drv;
    plc_libmodbus_driver(plc, &drv);

    remove(audit_path);
    static hmi_runtime_t rt;
    if (!hmi_runtime_start(&rt, &cfg, &drv, audit_path, NULL)) {
        printf("runtime start failed\n");
        return 3;
    }
    printf("writes_effective=%d\n", (int)rt.writes_effective);

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_buf, NULL, sizeof(s_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    hmi_ui_init(&rt, HMI_ROLE_OPERATOR, NULL);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("LVGL heap after UI build: total=%u free=%u used=%u KB\n", (unsigned)mon.total_size,
           (unsigned)mon.free_size, (unsigned)((mon.total_size - mon.free_size) / 1024));
    /* RAM gate (docs/OPTIMIZATION_ROADMAP.md, phase 6): the pool in use after the UI is built, and its peak at the end of
     * the scenario, may not grow past these. Measured on the host: 48 and 74 KB (full), 46 and 47 KB (lite). */
#define POOL_AFTER_BUILD_MAX_KB 56
#ifdef P99_LITE
#define POOL_PEAK_MAX_KB 56
#else
#define POOL_PEAK_MAX_KB 88
#endif
    CHECK((mon.total_size - mon.free_size) / 1024 <= POOL_AFTER_BUILD_MAX_KB);
    report_proc("after_ui_build");

    uint32_t alloc_after_init = alloc_count_total();
    size_t cap = 4000, n = 0;
    double *samples = (double *)malloc(cap * sizeof(double));

    /* scenario, with assertions on what the UI actually shows */
    pump(1.5, &samples, &n, cap);
    save_ppm((snprintf(err, sizeof(err), "%s_1_normal.ppm", prefix), err));
    printf("[ui] link='%s' TANK_LEVEL value='%s' quality='%s'\n", hmi_ui_link_text(), hmi_ui_row_text(0, 0),
           hmi_ui_row_text(0, 1));
    CHECK(strcmp(hmi_ui_link_text(), "PLC: HEALTHY") == 0);
    CHECK(strncmp(hmi_ui_row_text(0, 0), "50.0", 4) == 0 && strcmp(hmi_ui_row_text(0, 1), "GOOD") == 0);
    CHECK(strncmp(hmi_ui_row_text(1, 0), "21.3", 4) == 0);
    uint32_t f0 = s_flushes, px0 = s_flush_px;
    uint32_t a0 = alloc_count_total();
    pump(2.0, &samples, &n, cap); /* steady state, nothing changes */
    printf("heap allocation calls (app+LVGL+libmodbus objects) in 2.0 s steady state: %u\n",
           alloc_count_total() - a0);
    printf("steady-state (nothing changing): flushes=%u px=%u in 2.0 s\n", s_flushes - f0, s_flush_px - px0);
    CHECK(s_flushes - f0 == 0);                  /* heartbeat is off by default: truly idle */
    CHECK(hmi_ui_heartbeat_text()[0] == '\0');

    /* Heartbeat on: one small header label changes once a second. */
    hmi_ui_enable_heartbeat(true);
    f0 = s_flushes;
    px0 = s_flush_px;
    pump(3.0, &samples, &n, cap);
    printf("heartbeat on: text='%s' flushes=%u px=%u in 3.0 s\n", hmi_ui_heartbeat_text(), s_flushes - f0,
           s_flush_px - px0);
    CHECK(strncmp(hmi_ui_heartbeat_text(), "HB ", 3) == 0);
    CHECK(s_flushes - f0 >= 2 && s_flushes - f0 <= 5);   /* about one per second, never per frame */
    CHECK(s_flush_px - px0 < 8000);                     /* a few characters, not the screen */
    hmi_ui_enable_heartbeat(false);
    pump(0.3, &samples, &n, cap);
    f0 = s_flushes;
    pump(1.5, &samples, &n, cap);
    CHECK(s_flushes - f0 == 0);                  /* off again: idle again */

    fms_set_holding(srv, 0, 700); /* register changes while running: 70.0 % */
    pump(0.8, &samples, &n, cap);
    printf("[ui] after register 500->700: value='%s'\n", hmi_ui_row_text(0, 0));
    CHECK(strncmp(hmi_ui_row_text(0, 0), "70.0", 4) == 0);
    CHECK(hmi_ui_row_text(0, 2)[0] == '\0'); /* no alarm at 70 % */

    fms_set_holding(srv, 0, 910); /* 91 % -> HI alarm */
    pump(1.5, &samples, &n, cap);
    printf("[ui] after register ->910: value='%s' alarm='%s'\n", hmi_ui_row_text(0, 0), hmi_ui_row_text(0, 2));
    CHECK(strncmp(hmi_ui_row_text(0, 0), "91.0", 4) == 0);
    CHECK(strcmp(hmi_ui_row_text(0, 2), "HI UNACK") == 0);
    save_ppm((snprintf(err, sizeof(err), "%s_2_alarm.ppm", prefix), err));

    hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_OPERATOR);
    uint32_t id = 0;
    hmi_wr_submit_t sr = hmi_writes_submit(&rt.writes, 0, 60.0, HMI_ROLE_OPERATOR, hmi_comms_link_health(&rt.comms), hmi_now_ms(), &id);
    printf("write submit -> %d id=%u\n", (int)sr, id);
    CHECK(sr == HMI_WRS_OK);
    pump(1.0, &samples, &n, cap);
    {
        hmi_write_rec_t wrec;
        CHECK(hmi_writes_status(&rt.writes, id, &wrec) && wrec.state == HMI_WR_CONFIRMED);
        CHECK(fms_get_holding(srv, 0) == 600);
        CHECK(fms_write_count(srv) == 1);
        printf("[ui] write state=%d (CONFIRMED=%d) fake-PLC register=%u (expected 600)\n", (int)wrec.state,
               (int)HMI_WR_CONFIRMED, (unsigned)fms_get_holding(srv, 0));
    }
    CHECK(strncmp(hmi_ui_row_text(0, 0), "60.0", 4) == 0);
    save_ppm((snprintf(err, sizeof(err), "%s_3_acked_write.ppm", prefix), err));

    fms_set_mode(srv, FMS_HANG, 0);
    pump(3.5, &samples, &n, cap);
    save_ppm((snprintf(err, sizeof(err), "%s_4_commloss.ppm", prefix), err));
    printf("[ui] PLC hung: link='%s' value='%s' quality='%s'\n", hmi_ui_link_text(), hmi_ui_row_text(0, 0),
           hmi_ui_row_text(0, 1));
    CHECK(strcmp(hmi_ui_row_text(0, 0), "---") == 0); /* no stale number shown */
    CHECK(strncmp(hmi_ui_row_text(0, 1), "BAD", 3) == 0);
    CHECK(strcmp(hmi_ui_link_text(), "PLC: HEALTHY") != 0 && strcmp(hmi_ui_link_text(), "PLC: DEGRADED") != 0);
    {
        hmi_alarm_point_t cp;
        hmi_alarm_get(&rt.alarms, hmi_tagdb_count(&rt.db), &cp);
        CHECK(cp.state == HMI_AL_ACTIVE_UNACK && cp.kind == HMI_ALK_COMMS); /* comms alarm raised */
    }
    fms_set_mode(srv, FMS_NORMAL, 0);
    pump(3.5, &samples, &n, cap);
    save_ppm((snprintf(err, sizeof(err), "%s_5_recovered.ppm", prefix), err));
    printf("[ui] recovered: link='%s' value='%s' quality='%s'\n", hmi_ui_link_text(), hmi_ui_row_text(0, 0),
           hmi_ui_row_text(0, 1));
    CHECK(strcmp(hmi_ui_link_text(), "PLC: HEALTHY") == 0);
    CHECK(strcmp(hmi_ui_row_text(0, 1), "GOOD") == 0);
    CHECK(strncmp(hmi_ui_row_text(0, 0), "60.0", 4) == 0);
    {
        hmi_alarm_point_t cp;
        hmi_alarm_get(&rt.alarms, hmi_tagdb_count(&rt.db), &cp);
        CHECK(cp.state == HMI_AL_RTN_UNACK); /* link back; alarm waits for the operator */
        CHECK(hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_OPERATOR) == HMI_ACK_PENDING); /* recorded first, by the worker */
        pump(0.5, &samples, &n, cap);
        CHECK(hmi_alarm_count_unacked(&rt.alarms) == 0);
        hmi_alarm_get(&rt.alarms, hmi_tagdb_count(&rt.db), &cp);
        CHECK(cp.state == HMI_AL_NORMAL);
    }

    /* Acknowledgement authorization through the UI path: Viewer is refused by
     * the core, Operator is accepted, and both attempts are in the audit file. */
    {
        fms_set_holding(srv, 0, 910); /* 91 %: HI alarm awaiting acknowledgement */
        pump(1.5, &samples, &n, cap);
        size_t unacked = hmi_alarm_count_unacked(&rt.alarms);
        CHECK(unacked >= 1);

        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_VIEWER, NULL);
        pump(0.3, &samples, &n, cap);
        CHECK(strncmp(hmi_ui_ack_status_text(), "ACK DISABLED", 12) == 0);
        CHECK(!hmi_ui_send_enabled());                              /* a viewer: the setpoint buttons are dimmed */
        hmi_ui_press_ack_all(); /* what the button would do, though it is disabled */
        printf("[ui] viewer ACK ALL: '%s'\n", hmi_ui_ack_status_text());
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACK DENIED: role not permitted") == 0);
        CHECK(hmi_alarm_count_unacked(&rt.alarms) == unacked); /* unchanged */
        pump(0.3, &samples, &n, cap);
        save_ppm((snprintf(err, sizeof(err), "%s_6_viewer_denied.ppm", prefix), err));

        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_OPERATOR, NULL);
        pump(0.3, &samples, &n, cap);
        hmi_ui_press_ack_all();
        printf("[ui] operator ACK ALL: '%s'\n", hmi_ui_ack_status_text());
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACK PENDING: recording") == 0); /* the button did not wait for the disk */
        CHECK(hmi_ui_send_enabled());                                /* an operator may write TANK_LEVEL */
        pump(0.5, &samples, &n, cap);
        printf("[ui] operator ACK ALL settled: '%s'\n", hmi_ui_ack_status_text());
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACKNOWLEDGED") == 0);
        CHECK(hmi_alarm_count_unacked(&rt.alarms) == 0);

        FILE *af = fopen(audit_path, "rb");
        static char atext[65536];
        size_t alen = af ? fread(atext, 1, sizeof(atext) - 1, af) : 0;
        if (af) fclose(af);
        atext[alen] = '\0';
        CHECK(strstr(atext, "ALARM_ACK role=viewer action=ACK_ALL result=DENIED") != NULL);
        CHECK(strstr(atext, "ALARM_ACK role=operator action=ACK_ALL result=GRANTED") != NULL);
    }

    /* Screens: tabs, the alarm list sorted by priority, acknowledging one alarm,
     * the guard against an alarm that changed after it was drawn, and Viewer. */
    {
        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_OPERATOR, NULL);
        pump(0.3, &samples, &n, cap);
        CHECK(hmi_ui_screen_count() == SCREENS_EXPECTED);
        CHECK(hmi_ui_screen() == HMI_UI_SCREEN_OVERVIEW && hmi_ui_screen_visible(HMI_UI_SCREEN_OVERVIEW));
        CHECK(!hmi_ui_screen_visible(HMI_UI_SCREEN_ALARMS));
        hmi_ui_press_tab(HMI_UI_SCREEN_ALARMS);
        pump(0.3, &samples, &n, cap);
        CHECK(hmi_ui_screen() == HMI_UI_SCREEN_ALARMS && hmi_ui_screen_visible(HMI_UI_SCREEN_ALARMS));
        CHECK(!hmi_ui_screen_visible(HMI_UI_SCREEN_OVERVIEW));
        hmi_ui_press_tab(7);                                       /* no such screen: nothing changes */
        CHECK(hmi_ui_screen() == HMI_UI_SCREEN_ALARMS);

        /* TANK_LEVEL is still above its limit and was acknowledged earlier */
        CHECK(hmi_ui_alarm_rows() == 1);
        CHECK(strcmp(hmi_ui_alarm_row_text(0, 0), "CRIT") == 0);
        CHECK(strcmp(hmi_ui_alarm_row_text(0, 1), "TANK_LEVEL") == 0);
        CHECK(strcmp(hmi_ui_alarm_row_text(0, 2), "HI ACK") == 0);
        CHECK(!hmi_ui_alarm_ack_enabled(0));                       /* nothing to acknowledge */
        CHECK(hmi_ui_alarm_row_text(5, 1)[0] == '\0');             /* unused rows are empty */

        /* back to normal, then a new alarm that waits for the operator */
        fms_set_holding(srv, 0, 700);
        pump(1.0, &samples, &n, cap);
        CHECK(hmi_ui_alarm_rows() == 0);
        fms_set_holding(srv, 0, 910);
        pump(1.5, &samples, &n, cap);
        CHECK(hmi_ui_alarm_rows() == 1 && strcmp(hmi_ui_alarm_row_text(0, 2), "HI UNACK") == 0);
        CHECK(hmi_ui_alarm_ack_enabled(0));
        save_ppm((snprintf(err, sizeof(err), "%s_10_alarm_list.ppm", prefix), err));

        hmi_ui_press_alarm_ack(0);
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACK PENDING: recording") == 0);
        pump(0.5, &samples, &n, cap);
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACKNOWLEDGED") == 0);
        CHECK(strcmp(hmi_ui_alarm_row_text(0, 2), "HI ACK") == 0 && !hmi_ui_alarm_ack_enabled(0));

        /* an alarm that changed after the list was drawn is not acknowledged */
        fms_set_holding(srv, 0, 700);
        pump(1.0, &samples, &n, cap);
        fms_set_holding(srv, 0, 910);
        pump(1.5, &samples, &n, cap);
        CHECK(strcmp(hmi_ui_alarm_row_text(0, 2), "HI UNACK") == 0);
        {
            hmi_mutex_lock(&rt.alarms.lock);
            rt.alarms.pt[0].since_ms += 1;                         /* the engine changed it after the draw */
            hmi_mutex_unlock(&rt.alarms.lock);
            hmi_ui_press_alarm_ack(0);
            CHECK(strcmp(hmi_ui_ack_status_text(), "ALARM CHANGED: check the list") == 0);
            CHECK(hmi_alarm_count_unacked(&rt.alarms) == 1);       /* still waiting for the operator */
        }
        pump(0.3, &samples, &n, cap);

        /* Viewer sees the list but cannot acknowledge, and the core refuses even a forced press */
        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_VIEWER, NULL);
        hmi_ui_press_tab(HMI_UI_SCREEN_ALARMS);
        pump(0.4, &samples, &n, cap);
        CHECK(hmi_ui_alarm_rows() == 1 && !hmi_ui_alarm_ack_enabled(0));
        hmi_ui_press_alarm_ack(0);
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACK DENIED: role not permitted") == 0);
        CHECK(hmi_alarm_count_unacked(&rt.alarms) == 1);
        CHECK(hmi_ui_alarm_pages() == 1 && hmi_ui_alarm_page() == 0);
        hmi_ui_press_alarm_page(1);                                /* one page: stays */
        CHECK(hmi_ui_alarm_page() == 0);

        /* an idle overview stays idle after a visit to the other screen */
        hmi_ui_press_tab(HMI_UI_SCREEN_OVERVIEW);
        pump(0.5, &samples, &n, cap);
        f0 = s_flushes;
        pump(1.5, &samples, &n, cap);
        CHECK(s_flushes - f0 == 0);
        CHECK(hmi_ui_alarm_row_text(0, 1)[0] != '\0');             /* the alarm screen kept its rows while hidden */

        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_OPERATOR, NULL);
        pump(0.3, &samples, &n, cap);
        hmi_ui_press_ack_all();
        pump(0.5, &samples, &n, cap);
        CHECK(hmi_alarm_count_unacked(&rt.alarms) == 0);
    }

#ifndef P99_LITE
    /* Trends: the history of the two trended tags, with the gap the hung PLC left,
     * switching the tag and the window, and no redraw between two buckets. */
    {
        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_VIEWER, NULL);
        hmi_ui_press_tab(HMI_UI_SCREEN_TRENDS);
        pump(0.5, &samples, &n, cap);
        CHECK(hmi_ui_screen() == HMI_UI_SCREEN_TRENDS && hmi_ui_screen_visible(HMI_UI_SCREEN_TRENDS));
        CHECK(hmi_trend_series_count(&rt.trend) == 2);
        CHECK(strcmp(hmi_ui_trend_name(), "TANK_LEVEL") == 0);
        printf("[ui] trend: %s | axis %s .. %s | %u points\n", hmi_ui_trend_status_text(), hmi_ui_trend_axis_text(1),
               hmi_ui_trend_axis_text(0), (unsigned)hmi_ui_trend_points());
        CHECK(hmi_ui_trend_points() >= 8 && hmi_ui_trend_points() <= 120);
        CHECK(atof(hmi_ui_trend_axis_text(0)) > atof(hmi_ui_trend_axis_text(1)));
        CHECK(strstr(hmi_ui_trend_status_text(), "TANK_LEVEL  min ") != NULL && strstr(hmi_ui_trend_status_text(), " max ") != NULL);

        /* the lines: the upper one is never below the lower one, the newest bucket is on the right,
         * and the hung PLC left at least one gap between values */
        int voids_between = 0;
        bool order_ok = true, seen_value = false, in_gap = false;
        for (size_t i = 0; i < 120; i++) {
            int hi = hmi_ui_trend_value(0, i), lo = hmi_ui_trend_value(1, i);
            bool has = hi != (int)LV_CHART_POINT_NONE;
            order_ok = order_ok && (has == (lo != (int)LV_CHART_POINT_NONE)) && (!has || (hi >= lo && lo >= 0 && hi <= 1000));
            if (has && in_gap) voids_between++;
            if (has) { seen_value = true; in_gap = false; }
            else if (seen_value) in_gap = true;
        }
        CHECK(order_ok);
        CHECK(voids_between >= 1);
        CHECK(hmi_ui_trend_value(0, 0) == (int)LV_CHART_POINT_NONE);       /* the history is short: the left is empty */
        CHECK(hmi_ui_trend_value(0, 120) == (int)LV_CHART_POINT_NONE);     /* out of range is not a read past the chart */
        save_ppm((snprintf(err, sizeof(err), "%s_12_trend.ppm", prefix), err));

        hmi_ui_press_trend_series(1);
        pump(0.3, &samples, &n, cap);
        CHECK(hmi_ui_trend_series() == 1 && strcmp(hmi_ui_trend_name(), "TEMP") == 0);
        CHECK(hmi_ui_trend_points() >= 8);
        CHECK(strstr(hmi_ui_trend_status_text(), "TEMP  min 21.3  max 21.3 C") != NULL);   /* a flat line: padded, not divided by zero */
        hmi_ui_press_trend_series(1);                                      /* two series: wraps to the first */
        pump(0.3, &samples, &n, cap);
        CHECK(hmi_ui_trend_series() == 0 && strcmp(hmi_ui_trend_name(), "TANK_LEVEL") == 0);
        hmi_ui_press_trend_series(-1);
        pump(0.3, &samples, &n, cap);
        CHECK(hmi_ui_trend_series() == 1);
        hmi_ui_press_trend_series(-1);
        pump(0.3, &samples, &n, cap);

        hmi_ui_press_trend_window(2);                                      /* four times as long: squeezed, extremes kept */
        pump(0.3, &samples, &n, cap);
        CHECK(hmi_ui_trend_window() == 2 && hmi_ui_trend_points() >= 4);
        hmi_ui_press_trend_window(9);                                      /* no such window: nothing changes */
        CHECK(hmi_ui_trend_window() == 2);
        hmi_ui_press_trend_window(0);
        pump(0.3, &samples, &n, cap);
        CHECK(hmi_ui_trend_window() == 0);

        /* between two buckets the chart is not redrawn: at most one redraw per second of bucket time */
        pump(1.2, &samples, &n, cap);
        f0 = s_flushes;
        uint32_t rd0 = hmi_ui_trend_redraws();
        pump(3.5, &samples, &n, cap);
        printf("[ui] trend screen idle: %u chart redraws, %u flush calls in 3.5 s (1 s buckets)\n",
               hmi_ui_trend_redraws() - rd0, s_flushes - f0);
        CHECK(hmi_ui_trend_redraws() - rd0 >= 2 && hmi_ui_trend_redraws() - rd0 <= 5);   /* one per bucket, not one per tick */

        hmi_ui_press_tab(HMI_UI_SCREEN_OVERVIEW);
        pump(0.5, &samples, &n, cap);
        f0 = s_flushes;
        pump(1.5, &samples, &n, cap);
        CHECK(s_flushes - f0 == 0);                                        /* a hidden chart costs nothing */
        lv_mem_monitor(&mon);
        printf("LVGL heap with four screens built: used=%u KB, max_used=%u KB\n",
               (unsigned)((mon.total_size - mon.free_size) / 1024), (unsigned)(mon.max_used / 1024));
    }
#endif

    /* Paging: 23 tags need three pages of ten. The PLC of this runtime is not
     * reachable, which does not matter here: the names, the pages and the
     * hidden rows are what is checked. */
    {
        static char big[8192];
        size_t off = (size_t)snprintf(big, sizeof(big),
            "{\"version\":1,\"plc\":{\"host\":\"127.0.0.1\",\"port\":1,\"unit\":1,"
            "\"response_timeout_ms\":200,\"connect_timeout_ms\":300},\"tags\":[");
        for (int i = 0; i < 23; i++)
            off += (size_t)snprintf(big + off, sizeof(big) - off, "%s{\"name\":\"T%02d\",\"address\":%d,\"type\":\"u16\"}",
                                    i ? "," : "", i, i);
        snprintf(big + off, sizeof(big) - off, "]}");
        static hmi_app_cfg_t cfg2;
        CHECK(hmi_config_parse(big, strlen(big), &cfg2, err, sizeof(err)));
        plc_libmodbus_t *plc2 = plc_libmodbus_create(cfg2.host, cfg2.port, cfg2.unit, cfg2.response_timeout_ms);
        hmi_plc_driver_t drv2;
        plc_libmodbus_driver(plc2, &drv2);
        static hmi_runtime_t rt2;
        CHECK(hmi_runtime_start(&rt2, &cfg2, &drv2, NULL, NULL));

        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt2, HMI_ROLE_VIEWER, NULL);
        pump(0.4, &samples, &n, cap);
        CHECK(hmi_ui_overview_pages() == 3 && hmi_ui_overview_page() == 0);
        CHECK(strcmp(hmi_ui_row_name(0), "T00") == 0 && strcmp(hmi_ui_row_name(9), "T09") == 0);
        CHECK(strcmp(hmi_ui_row_text(0, 0), "---") == 0);               /* never a number for a tag without data */
        hmi_ui_press_overview_page(1);
        pump(0.2, &samples, &n, cap);
        CHECK(hmi_ui_overview_page() == 1 && strcmp(hmi_ui_row_name(0), "T10") == 0 && strcmp(hmi_ui_row_name(9), "T19") == 0);
        hmi_ui_press_overview_page(1);
        pump(0.2, &samples, &n, cap);
        CHECK(hmi_ui_overview_page() == 2 && strcmp(hmi_ui_row_name(2), "T22") == 0);
        CHECK(hmi_ui_row_name(3)[0] == '\0' && hmi_ui_row_text(3, 1)[0] == '\0');   /* the last page has three rows */
        save_ppm((snprintf(err, sizeof(err), "%s_11_last_page.ppm", prefix), err));
        hmi_ui_press_overview_page(1);                                    /* wraps to the first page */
        CHECK(hmi_ui_overview_page() == 0 && strcmp(hmi_ui_row_name(0), "T00") == 0);
        hmi_ui_press_overview_page(-1);                                   /* and back to the last */
        CHECK(hmi_ui_overview_page() == 2);

#ifndef P99_LITE
        /* no trended tag: the trend screen says so and does nothing else */
        CHECK(hmi_trend_series_count(&rt2.trend) == 0);
        hmi_ui_press_tab(HMI_UI_SCREEN_TRENDS);
        pump(0.4, &samples, &n, cap);
        CHECK(hmi_ui_screen() == HMI_UI_SCREEN_TRENDS);
        CHECK(hmi_ui_trend_name()[0] == '\0' && hmi_ui_trend_points() == 0);
        hmi_ui_press_trend_series(1);
        hmi_ui_press_trend_window(1);
        pump(0.2, &samples, &n, cap);
        CHECK(hmi_ui_trend_series() == 0);
        save_ppm((snprintf(err, sizeof(err), "%s_13_no_trend.ppm", prefix), err));
#endif

        hmi_runtime_stop(&rt2);
        plc_libmodbus_destroy(plc2);
        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_OPERATOR, NULL);
        pump(0.2, &samples, &n, cap);
    }

    /* Settings: the system lines for everyone, then the CONFIG view for each claimed role. There is no login:
     * the role is the one the UI was built with, and the core checks it on every call. */
    {
        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_VIEWER, NULL);
        hmi_ui_set_config_info("Configuration: serial 7, signed");
        hmi_ui_press_tab(SCREEN_SETTINGS);
        pump(1.4, &samples, &n, cap);
        CHECK(hmi_ui_screen() == SCREEN_SETTINGS && hmi_ui_settings_view_count() == SETTINGS_VIEWS);
        CHECK(hmi_ui_settings_view() == HMI_UI_VIEW_SYSTEM);
        for (size_t i = 0; i < 7; i++) printf("[ui] system %zu: %s\n", i, hmi_ui_system_text(i));
        CHECK(strncmp(hmi_ui_system_text(0), "PLC 127.0.0.1:", 14) == 0 && strstr(hmi_ui_system_text(0), "unit 1") != NULL);
        CHECK(strcmp(hmi_ui_system_text(1), "Writes: enabled") == 0);
        CHECK(strcmp(hmi_ui_system_text(2), "Configuration: serial 7, signed") == 0);
        CHECK(strncmp(hmi_ui_system_text(3), "Storage: ok, ", 13) == 0);
        CHECK(strncmp(hmi_ui_system_text(4), "Clock: ", 7) == 0);
        CHECK(strcmp(hmi_ui_system_text(5), "Tags: 4   with alarms: 1   trended: " TRENDED_EXPECTED) == 0);
        CHECK(strcmp(hmi_ui_system_text(6), "Role: VIEWER (read only)") == 0);
        CHECK(hmi_ui_system_text(9)[0] == '\0');
        save_ppm((snprintf(err, sizeof(err), "%s_14_settings_system.ppm", prefix), err));
        hmi_ui_press_settings_view(5);                                    /* no such view: nothing changes */
        CHECK(hmi_ui_settings_view() == HMI_UI_VIEW_SYSTEM);

        /* the header and the system line name a claimed role, and say it is not authenticated */
        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_ENGINEER, NULL);
        hmi_ui_set_config_info("Configuration: serial 7, signed");
        hmi_ui_press_tab(SCREEN_SETTINGS);
        pump(1.3, &samples, &n, cap);
        CHECK(strcmp(hmi_ui_role_text(), "ENGINEER") == 0);
        CHECK(strstr(hmi_ui_system_text(6), "engineer") != NULL && strstr(hmi_ui_system_text(6), "NOT authenticated") != NULL);
        save_ppm((snprintf(err, sizeof(err), "%s_14b_settings_system_engineer.ppm", prefix), err));
        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_VIEWER, NULL);
        hmi_ui_set_config_info("Configuration: serial 7, signed");
        hmi_ui_press_tab(SCREEN_SETTINGS);
        pump(0.3, &samples, &n, cap);
        CHECK(strcmp(hmi_ui_role_text(), "VIEWER") == 0);

#ifndef P99_LITE
        /* The CONFIG view: an engineer edits settings, APPLY twice sends one request to the spool directory,
         * and nothing changes in the configuration here. A viewer sees why the view is empty, and the core
         * refuses a forced request. */
        {
            static char spool[256], req_path[300], res_path[300];
            snprintf(spool, sizeof(spool), "%s_spool", prefix);
            snprintf(req_path, sizeof(req_path), "%s/request", spool);
            snprintf(res_path, sizeof(res_path), "%s/result", spool);
            remove(req_path);
            remove(res_path);
            UI_MKDIR(spool);

            hmi_ui_press_settings_view(HMI_UI_VIEW_CONFIG);
            pump(0.4, &samples, &n, cap);
            CHECK(hmi_ui_settings_view() == HMI_UI_VIEW_CONFIG && hmi_ui_config_rows() == 0);
            CHECK(strstr(hmi_ui_config_message_text(), "not set up") != NULL);       /* no spool directory given */
            hmi_ui_set_config_edit(spool, 7);
            pump(0.3, &samples, &n, cap);
            CHECK(strstr(hmi_ui_config_message_text(), "engineer") != NULL && hmi_ui_config_rows() == 0);   /* a viewer */
            save_ppm((snprintf(err, sizeof(err), "%s_17_config_viewer.ppm", prefix), err));

            /* an operator may look but not edit; an engineer edits */
            lv_obj_clean(lv_screen_active());
            hmi_ui_init(&rt, HMI_ROLE_OPERATOR, NULL);
            hmi_ui_set_config_info("Configuration: serial 7, signed");
            hmi_ui_set_config_edit(spool, 7);
            hmi_ui_press_tab(SCREEN_SETTINGS);
            hmi_ui_press_settings_view(HMI_UI_VIEW_CONFIG);
            pump(0.4, &samples, &n, cap);
            CHECK(strstr(hmi_ui_config_message_text(), "engineer") != NULL && hmi_ui_config_rows() == 0);   /* an operator */

            lv_obj_clean(lv_screen_active());
            hmi_ui_init(&rt, HMI_ROLE_ENGINEER, NULL);
            hmi_ui_set_config_info("Configuration: serial 7, signed");
            hmi_ui_set_config_edit(spool, 7);
            hmi_ui_press_tab(SCREEN_SETTINGS);
            hmi_ui_press_settings_view(HMI_UI_VIEW_CONFIG);
            pump(1.3, &samples, &n, cap);
            pump(1.3, &samples, &n, cap);
            CHECK(hmi_ui_config_message_text()[0] == '\0' && hmi_ui_config_targets() == 5);   /* the PLC and four tags */
            CHECK(strcmp(hmi_ui_config_target_text(), "PLC connection") == 0 && hmi_ui_config_rows() == 3);
            CHECK(strcmp(hmi_ui_config_row_text(0, 0), "PLC HOST") == 0 && strcmp(hmi_ui_config_row_text(0, 1), "127.0.0.1") == 0);
            CHECK(strcmp(hmi_ui_config_row_text(2, 0), "MODBUS UNIT") == 0 && strcmp(hmi_ui_config_row_text(2, 1), "1") == 0);
            CHECK(hmi_ui_config_row_text(3, 0)[0] == '\0');
            hmi_ui_press_config_row(0);                                           /* the host steps through the allow-list only */
            pump(0.3, &samples, &n, cap);
            CHECK(hmi_ui_config_pending() == 1 && strcmp(hmi_ui_config_row_text(0, 2), "-> 127.0.0.2") == 0);
            hmi_ui_press_config_row(0);
            pump(0.3, &samples, &n, cap);
            CHECK(hmi_ui_config_pending() == 0 && hmi_ui_config_row_text(0, 2)[0] == '\0');
            hmi_ui_press_config_row(1);                                           /* the port needs a number */
            hmi_ui_press_config_key("1"); hmi_ui_press_config_key("2"); hmi_ui_press_config_key("OK");
            pump(0.3, &samples, &n, cap);
            CHECK(hmi_ui_config_pending() == 1 && strcmp(hmi_ui_config_row_text(1, 2), "-> 12") == 0);
            hmi_ui_press_config(1);                                               /* DISCARD */
            CHECK(hmi_ui_config_pending() == 0);

            hmi_ui_press_config_target(1);
            pump(0.3, &samples, &n, cap);
            CHECK(strncmp(hmi_ui_config_target_text(), "TAG TANK_LEVEL", 14) == 0 && hmi_ui_config_rows() == 8);
            CHECK(strcmp(hmi_ui_config_row_text(0, 0), "HIGH-HIGH") == 0 && strcmp(hmi_ui_config_row_text(0, 1), "95") == 0);
            CHECK(strcmp(hmi_ui_config_row_text(1, 1), "80") == 0 && strcmp(hmi_ui_config_row_text(2, 1), "10") == 0);
            CHECK(strcmp(hmi_ui_config_row_text(3, 1), "off") == 0 && strcmp(hmi_ui_config_row_text(4, 1), "2") == 0);
            CHECK(strcmp(hmi_ui_config_row_text(5, 1), "300") == 0 && strcmp(hmi_ui_config_row_text(7, 1), "CRITICAL") == 0);
            CHECK(!hmi_ui_config_button_enabled(0) && hmi_ui_config_pending() == 0);       /* nothing to send */
            hmi_ui_press_config(0);
            CHECK(strstr(hmi_ui_config_status_text(), "Nothing to send") != NULL);

            /* the number pad: digits, one point, a minus only first, delete, and a value equal to the current one is no edit */
            hmi_ui_press_config_row(1);
            CHECK(hmi_ui_config_keypad_open() && hmi_ui_config_keypad_text()[0] == '\0');
            pump(0.3, &samples, &n, cap);
            save_ppm((snprintf(err, sizeof(err), "%s_18_config_keypad.ppm", prefix), err));
            hmi_ui_press_config_key("OK");
            CHECK(hmi_ui_config_keypad_open() && strstr(hmi_ui_config_status_text(), "Type a number") != NULL);
            hmi_ui_press_config_key("-"); hmi_ui_press_config_key("8"); hmi_ui_press_config_key("-"); hmi_ui_press_config_key(".");
            hmi_ui_press_config_key("."); hmi_ui_press_config_key("5");
            CHECK(strcmp(hmi_ui_config_keypad_text(), "-8.5") == 0);
            hmi_ui_press_config_key("DEL"); hmi_ui_press_config_key("DEL"); hmi_ui_press_config_key("DEL"); hmi_ui_press_config_key("DEL");
            hmi_ui_press_config_key("DEL");
            CHECK(hmi_ui_config_keypad_text()[0] == '\0');
            hmi_ui_press_config_key("8"); hmi_ui_press_config_key("0");
            hmi_ui_press_config_key("OK");
            CHECK(!hmi_ui_config_keypad_open() && hmi_ui_config_pending() == 0);           /* 80 is what it already is */
            hmi_ui_press_config_row(1);
            hmi_ui_press_config_key("8"); hmi_ui_press_config_key("2");
            hmi_ui_press_config_key("CANCEL");
            CHECK(!hmi_ui_config_keypad_open() && hmi_ui_config_pending() == 0);
            hmi_ui_press_config_key("5");                                                 /* a key with no pad open does nothing */
            hmi_ui_press_config_row(1);
            hmi_ui_press_config_key("8"); hmi_ui_press_config_key("2");
            hmi_ui_press_config_key("OK");
            pump(0.3, &samples, &n, cap);
            CHECK(hmi_ui_config_pending() == 1 && strcmp(hmi_ui_config_row_text(1, 2), "-> 82") == 0);
            CHECK(hmi_ui_config_button_enabled(0));

            /* switching a limit off, the priority cycle, and a whole number where one is needed */
            hmi_ui_press_config_row(0);
            hmi_ui_press_config_key("OFF");
            hmi_ui_press_config_row(7);
            pump(0.3, &samples, &n, cap);
            CHECK(strcmp(hmi_ui_config_row_text(0, 2), "-> off") == 0 && strcmp(hmi_ui_config_row_text(7, 2), "-> LOW") == 0);
            CHECK(hmi_ui_config_pending() == 3);
            hmi_ui_press_config_row(7); hmi_ui_press_config_row(7); hmi_ui_press_config_row(7);
            pump(0.3, &samples, &n, cap);
            CHECK(hmi_ui_config_row_text(7, 2)[0] == '\0' && hmi_ui_config_pending() == 2);   /* round again to the first value: no edit */
            hmi_ui_press_config_row(5);
            hmi_ui_press_config_key("1"); hmi_ui_press_config_key("."); hmi_ui_press_config_key("5");
            hmi_ui_press_config_key("OK");
            CHECK(hmi_ui_config_keypad_open() && strstr(hmi_ui_config_status_text(), "whole number") != NULL);
            hmi_ui_press_config_key("CANCEL");
            hmi_ui_press_config_row(4);                                                    /* OFF is not offered for a plain number */
            hmi_ui_press_config_key("OFF");
            CHECK(hmi_ui_config_keypad_open());
            hmi_ui_press_config_key("CANCEL");

            /* the rules run before anything is sent: a limit outside the tag range is shown as refused */
            hmi_ui_press_config_row(2);
            hmi_ui_press_config_key("5"); hmi_ui_press_config_key("0"); hmi_ui_press_config_key("0");
            hmi_ui_press_config_key("OK");
            hmi_ui_press_config(0);
            CHECK(strncmp(hmi_ui_config_status_text(), "Refused: ", 9) == 0 && strstr(hmi_ui_config_status_text(), "inside") != NULL);
            CHECK(hmi_ui_config_pending() == 3);
            hmi_ui_press_config_row(2);
            hmi_ui_press_config_key("1"); hmi_ui_press_config_key("2");
            hmi_ui_press_config_key("OK");
            pump(0.3, &samples, &n, cap);
            CHECK(hmi_ui_config_pending() == 3 && strcmp(hmi_ui_config_row_text(2, 2), "-> 12") == 0);

            /* the first press asks, the second sends, one request for all three edits */
            hmi_ui_press_config(0);
            printf("[ui] config status: %s\n", hmi_ui_config_status_text());
            CHECK(strcmp(hmi_ui_config_status_text(), "Press APPLY again to send 3 changes") == 0);
            CHECK(fopen(req_path, "rb") == NULL);                                         /* nothing has left yet */
            pump(0.3, &samples, &n, cap);
            save_ppm((snprintf(err, sizeof(err), "%s_19_config_apply.ppm", prefix), err));
            hmi_ui_press_config_row(1);                                                    /* another edit: the question lapses */
            hmi_ui_press_config_key("8"); hmi_ui_press_config_key("3");
            hmi_ui_press_config_key("OK");
            hmi_ui_press_config(0);
            CHECK(strstr(hmi_ui_config_status_text(), "Press APPLY again to send 3") != NULL);
            hmi_ui_press_config(0);
            CHECK(strncmp(hmi_ui_config_status_text(), "Request sent", 12) == 0 && hmi_ui_config_pending() == 0);
            {
                FILE *rf = fopen(req_path, "rb");
                static char rtext[2048];
                size_t rl = rf ? fread(rtext, 1, sizeof(rtext) - 1, rf) : 0;
                if (rf) fclose(rf);
                rtext[rl] = '\0';
                printf("[ui] request:\n%s", rtext);
                CHECK(strncmp(rtext, "P99REQ 1\nbase_serial=7\nuser=-\n", 30) == 0);
                CHECK(strstr(rtext, "tag.TANK_LEVEL.alarm.high=83\n") != NULL && strstr(rtext, "tag.TANK_LEVEL.alarm.high_high=off\n") != NULL);
                CHECK(strstr(rtext, "tag.TANK_LEVEL.alarm.low=12\n") != NULL && strstr(rtext, "priority") == NULL);
                CHECK(strlen(rtext) > 3 && strcmp(rtext + rl - 4, "END\n") == 0);
            }
            CHECK(strcmp(hmi_ui_config_row_text(1, 1), "80") == 0);                       /* the running configuration did not change */
            pump(1.3, &samples, &n, cap);
            CHECK(strcmp(hmi_ui_config_result_text(), "Last request: Waiting for the configuration helper") == 0);

            /* a second request waits for the first */
            hmi_ui_press_config_row(1);
            hmi_ui_press_config_key("8"); hmi_ui_press_config_key("4");
            hmi_ui_press_config_key("OK");
            hmi_ui_press_config(0);
            hmi_ui_press_config(0);
            CHECK(strstr(hmi_ui_config_status_text(), "earlier request") != NULL);
            hmi_ui_press_config(1);
            CHECK(hmi_ui_config_pending() == 0 && strcmp(hmi_ui_config_status_text(), "Changes discarded") == 0);

            /* the helper answers: shown as it is, and a refusal is shown as a failure */
            {
                remove(req_path);
                FILE *rf = fopen(res_path, "wb");
                if (rf) { fputs("applied serial=8 changes=3 by=eng1\n", rf); fclose(rf); }
                pump(1.3, &samples, &n, cap);
                CHECK(strcmp(hmi_ui_config_result_text(), "Last request: applied serial=8 changes=3 by=eng1. The service restarts to use it") == 0);
                rf = fopen(res_path, "wb");
                if (rf) { fputs("rejected: stale request: made for serial 7, the configuration is serial 8\n", rf); fclose(rf); }
                pump(1.3, &samples, &n, cap);
                CHECK(strncmp(hmi_ui_config_result_text(), "Last request: rejected: stale request", 37) == 0);
                save_ppm((snprintf(err, sizeof(err), "%s_20_config_result.ppm", prefix), err));
            }


            /* a viewer: the edits of the engineer are gone with the old screen, and nothing can be sent */
            lv_obj_clean(lv_screen_active());
            hmi_ui_init(&rt, HMI_ROLE_VIEWER, NULL);
            hmi_ui_set_config_info("Configuration: serial 7, signed");
            hmi_ui_set_config_edit(spool, 7);
            hmi_ui_press_tab(SCREEN_SETTINGS);
            hmi_ui_press_settings_view(HMI_UI_VIEW_CONFIG);
            pump(0.4, &samples, &n, cap);
            hmi_ui_press_config(0);
            hmi_ui_press_config(0);
            CHECK(strncmp(hmi_ui_config_status_text(), "Request sent", 12) != 0);
            CHECK(fopen(req_path, "rb") == NULL);
            CHECK(hmi_ui_config_pending() == 0 && strstr(hmi_ui_config_message_text(), "engineer") != NULL && hmi_ui_config_rows() == 0);
            remove(res_path);

            FILE *cf = fopen(audit_path, "rb");
            static char ctext[131072];
            size_t clen = cf ? fread(ctext, 1, sizeof(ctext) - 1, cf) : 0;
            if (cf) fclose(cf);
            ctext[clen] = '\0';
            CHECK(strstr(ctext, " CONFIG_REQUEST by=- changes=3 base_serial=7 wall=") != NULL);
        }
#endif
    }

    /* The claimed role through the UI path: the role handed to the core is the role the UI was built with, nothing
     * else. Viewer cannot acknowledge, operator and above can, an idle screen stays idle, and the audit file names
     * the role but no user. */
    {
        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_VIEWER, NULL);
        lv_mem_monitor(&mon);
        printf("LVGL heap with every screen built: used=%u KB of %u KB\n",
               (unsigned)((mon.total_size - mon.free_size) / 1024), (unsigned)(mon.total_size / 1024));
        pump(0.3, &samples, &n, cap);
        CHECK(strcmp(hmi_ui_role_text(), "VIEWER") == 0);
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACK DISABLED: viewer role") == 0);
        hmi_ui_press_ack_all();                                        /* the core refuses a forced press */
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACK DENIED: role not permitted") == 0);

        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_ADMIN, NULL);
        pump(0.3, &samples, &n, cap);
        printf("[ui] header='%s'\n", hmi_ui_role_text());
        CHECK(strcmp(hmi_ui_role_text(), "ADMIN") == 0);
        fms_set_holding(srv, 0, 700);          /* back to normal ... */
        pump(1.0, &samples, &n, cap);
        fms_set_holding(srv, 0, 910);          /* ... and a new high alarm that waits for acknowledgement */
        pump(1.5, &samples, &n, cap);
        CHECK(hmi_alarm_count_unacked(&rt.alarms) >= 1);
        hmi_ui_press_ack_all();
        pump(0.5, &samples, &n, cap);
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACKNOWLEDGED") == 0);
        uint32_t writes_before = fms_write_count(srv);
        hmi_ui_press_setpoint_send();
        pump(1.0, &samples, &n, cap);
        CHECK(fms_write_count(srv) == writes_before + 1);
        save_ppm((snprintf(err, sizeof(err), "%s_9_admin_role.ppm", prefix), err));

        /* settled again: an idle screen stays idle */
        pump(0.5, &samples, &n, cap);
        f0 = s_flushes;
        pump(1.5, &samples, &n, cap);
        CHECK(s_flushes - f0 == 0);

        FILE *af = fopen(audit_path, "rb");
        static char atext[131072];
        size_t alen = af ? fread(atext, 1, sizeof(atext) - 1, af) : 0;
        if (af) fclose(af);
        atext[alen] = '\0';
        CHECK(strstr(atext, "ALARM_ACK role=viewer action=ACK_ALL result=DENIED") != NULL);
        CHECK(strstr(atext, "ALARM_ACK role=admin action=ACK_ALL result=GRANTED") != NULL);
        CHECK(strstr(atext, " user=admin") == NULL && strstr(atext, "LOGIN") == NULL);   /* no accounts: no names, no logins */
        {
            const char *wl = strstr(atext, " RESERVED id=");
            const char *last_res = NULL;
            while (wl) { last_res = wl; wl = strstr(wl + 1, " RESERVED id="); }
            CHECK(last_res != NULL && strstr(last_res, "role=3") != NULL);          /* role 3 = admin */
            const char *eol = last_res ? strchr(last_res, 10) : NULL;
            char one[400];
            size_t ln = (eol && (size_t)(eol - last_res) < sizeof(one)) ? (size_t)(eol - last_res) : 0;
            if (ln) memcpy(one, last_res, ln);
            one[ln] = '\0';
            CHECK(ln > 10 && strstr(one, " user=- old=") != NULL);                   /* no name, then the value it replaced */
            CHECK(strstr(one, " oldq=GOOD wall=") != NULL && strstr(one, " sync=") != NULL);
            CHECK(strstr(atext, " BOOT wall=") != NULL);                              /* the start marker */
        }
        CHECK(!hmi_runtime_storage_fault(&rt));                                       /* every record was written */
    }


    /* States of a write and of the data, each drawn and written to a screenshot: a write the PLC does not answer
     * (pending), a write the PLC refuses with a Modbus exception (rejected), a write the PLC applied but did not
     * confirm (unknown), a value outside its range, the largest numbers, the longest name and unit, and a long run of
     * screen transitions after which the table must look exactly as before. */
    {
        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_OPERATOR, NULL);
        fms_set_mode(srv, FMS_NORMAL, 0);
        fms_set_holding(srv, 0, 500);
        pump(1.2, &samples, &n, cap);
        CHECK(hmi_ui_write_status_text()[0] == '\0' || strncmp(hmi_ui_write_status_text(), "WRITE #", 7) == 0);

        /* pending: the PLC reads the request and never answers */
        uint32_t req0 = fms_request_count(srv);
        fms_set_mode(srv, FMS_HANG, 0);
        hmi_ui_press_setpoint_send();
        pump(0.12, &samples, &n, cap);   /* before the 200 ms reply timeout of this configuration */
        printf("[ui] write while the PLC does not answer: '%s'\n", hmi_ui_write_status_text());
        CHECK(strstr(hmi_ui_write_status_text(), "PENDING") != NULL || strstr(hmi_ui_write_status_text(), "RECORDING") != NULL ||
              strstr(hmi_ui_write_status_text(), "SENT") != NULL || strstr(hmi_ui_write_status_text(), "UNKNOWN") != NULL);   /* the state depends on timing */
        save_ppm((snprintf(err, sizeof(err), "%s_21_write_pending.ppm", prefix), err));
        pump(3.0, &samples, &n, cap);
        printf("[ui] the same write a little later: '%s'\n", hmi_ui_write_status_text());
        CHECK(strstr(hmi_ui_write_status_text(), "UNKNOWN") != NULL || strstr(hmi_ui_write_status_text(), "EXPIRED") != NULL ||
              strstr(hmi_ui_write_status_text(), "PENDING") != NULL);
        save_ppm((snprintf(err, sizeof(err), "%s_22_write_no_answer.ppm", prefix), err));
        (void)req0;

        /* rejected: a Modbus exception reply */
        fms_set_mode(srv, FMS_NORMAL, 0);
        pump(3.0, &samples, &n, cap);
        fms_set_mode(srv, FMS_EXCEPTION, 2);
        hmi_ui_press_setpoint_send();
        pump(1.5, &samples, &n, cap);
        printf("[ui] write answered with a Modbus exception: '%s'\n", hmi_ui_write_status_text());
        CHECK(strstr(hmi_ui_write_status_text(), "REJECTED") != NULL || strstr(hmi_ui_write_status_text(), "UNKNOWN") != NULL);
        save_ppm((snprintf(err, sizeof(err), "%s_23_write_rejected.ppm", prefix), err));
        fms_set_mode(srv, FMS_NORMAL, 0);
        pump(3.0, &samples, &n, cap);

        /* uncertain: the PLC applies the write and drops the connection without a reply */
        fms_set_holding(srv, 0, 400);
        pump(1.0, &samples, &n, cap);
        fms_set_mode(srv, FMS_APPLY_NO_REPLY, 0);
        hmi_ui_press_setpoint_send();
        pump(3.0, &samples, &n, cap);
        printf("[ui] write applied but not confirmed: '%s'\n", hmi_ui_write_status_text());
        CHECK(strstr(hmi_ui_write_status_text(), "UNKNOWN") != NULL || strstr(hmi_ui_write_status_text(), "CONFIRMED") != NULL);
        save_ppm((snprintf(err, sizeof(err), "%s_24_write_unknown.ppm", prefix), err));
        fms_set_mode(srv, FMS_NORMAL, 0);
        pump(3.0, &samples, &n, cap);

        /* a value outside its range, the largest numbers, the longest name and unit */
        {
            uint16_t port3;
            fake_modbus_t *srv3 = fms_start(&port3);                        /* its own fake PLC: the first one serves one client only */
            CHECK(srv3 != NULL);
            fms_set_holding(srv3, 20, 0xFFFFu);                              /* U32 4294967295, high word first */
            fms_set_holding(srv3, 21, 0xFFFFu);
            float big = 123456789.0f;
            uint32_t bits;
            memcpy(&bits, &big, sizeof(bits));
            fms_set_holding(srv3, 22, (uint16_t)(bits >> 16));               /* F32 */
            fms_set_holding(srv3, 23, (uint16_t)(bits & 0xFFFFu));
            fms_set_holding(srv3, 24, 0x8000u);                              /* I16 -32768 */
            fms_set_holding(srv3, 25, 1200);                                 /* 120.0 where the range is 0 to 100 */
            char big_json[2048];
            snprintf(big_json, sizeof(big_json),
                "{\"version\":1,\"plc\":{\"host\":\"127.0.0.1\",\"port\":%u,\"unit\":1,\"response_timeout_ms\":200,\"connect_timeout_ms\":500},"
                "\"tags\":["
                "{\"name\":\"HEAT_EXCHANGER_OUTLET_T\",\"address\":20,\"type\":\"u32\",\"word_order\":\"high_first\",\"units\":\"kWh/m3\",\"min\":0,\"max\":4294967295,\"poll_ms\":100},"
                "{\"name\":\"BIG_FLOAT\",\"address\":22,\"type\":\"f32\",\"word_order\":\"high_first\",\"units\":\"l/min\",\"min\":0,\"max\":1000000000,\"poll_ms\":100},"
                "{\"name\":\"NEGATIVE\",\"address\":24,\"type\":\"i16\",\"units\":\"degC\",\"min\":-32768,\"max\":32767,\"poll_ms\":100},"
                "{\"name\":\"OUT_OF_RANGE\",\"address\":25,\"type\":\"u16\",\"scale\":0.1,\"units\":\"%%\",\"min\":0,\"max\":100,\"poll_ms\":100,"
                "\"alarm\":{\"high\":80,\"deadband\":2,\"on_ms\":200,\"off_ms\":200,\"priority\":\"high\"}}"
                "]}", (unsigned)port3);
            static hmi_app_cfg_t cfg3;
            CHECK(hmi_config_parse(big_json, strlen(big_json), &cfg3, err, sizeof(err)));
            plc_libmodbus_t *plc3 = plc_libmodbus_create(cfg3.host, cfg3.port, cfg3.unit, cfg3.response_timeout_ms);
            hmi_plc_driver_t drv3;
            plc_libmodbus_driver(plc3, &drv3);
            static hmi_runtime_t rt3;
            CHECK(hmi_runtime_start(&rt3, &cfg3, &drv3, NULL, NULL));
            lv_obj_clean(lv_screen_active());
            hmi_ui_init(&rt3, HMI_ROLE_OPERATOR, NULL);
            pump(2.0, &samples, &n, cap);
            for (size_t r = 0; r < 4; r++)
                printf("[ui] row %zu: %s | %s | %s | %s\n", r, hmi_ui_row_name(r), hmi_ui_row_text(r, 0), hmi_ui_row_text(r, 1), hmi_ui_row_text(r, 2));
            CHECK(strncmp(hmi_ui_row_text(0, 0), "4294967295", 10) == 0);
            CHECK(strncmp(hmi_ui_row_text(2, 0), "-32768", 6) == 0);
            CHECK(strcmp(hmi_ui_row_name(0), "HEAT_EXCHANGER_OUTLET_T") == 0);
            CHECK(strstr(hmi_ui_row_text(3, 1), "UNCERTAIN") != NULL || strstr(hmi_ui_row_text(3, 1), "BAD") != NULL ||
                  strstr(hmi_ui_row_text(3, 1), "RANGE") != NULL);
            save_ppm((snprintf(err, sizeof(err), "%s_25_long_values_and_range.ppm", prefix), err));
            hmi_runtime_stop(&rt3);
            plc_libmodbus_destroy(plc3);
            fms_stop(srv3);
            lv_obj_clean(lv_screen_active());
            hmi_ui_init(&rt, HMI_ROLE_OPERATOR, NULL);
            pump(0.3, &samples, &n, cap);
        }
    }

    /* Responsiveness (application level, host): screen transitions and touch, with the PLC polled and the data moving
     * the whole time. See the comment above touch_read_cb for what is and is not measured. */
    {
        lv_obj_clean(lv_screen_active());
        hmi_ui_init(&rt, HMI_ROLE_OPERATOR, NULL);
        settle_us();
        const size_t ns = hmi_ui_screen_count();
        double trans_first[8], trans[256], press_fb[256], rel_scr[256];
        size_t nt = 0, np = 0, nr = 0;
        int missed = 0, wrong = 0;

        /* transitions by the test seam (what a tab press runs), the first round includes the lazy builds */
        for (int round = 0; round < 25; round++) {
            for (size_t s = 1; s <= ns; s++) {
                size_t target = s % ns;
                fms_set_holding(srv, 0, (round & 1) ? 910 : 700);        /* the alarm state keeps changing */
                double t0 = now_us();
                hmi_ui_press_tab(target);
                double t1 = settle_us();
                double lat = t1 - t0;
                if (lat < 0.0) lat = 0.0;                                  /* nothing had to be drawn after the press */
                if (hmi_ui_screen() != target) wrong++;
                if (round == 0 && s < 8) trans_first[s - 1] = lat;
                else if (nt < 256) trans[nt++] = lat;
            }
        }
        for (size_t s = 0; s < ns && s < 8; s++)
            printf("[perf] first visit of screen %u (lazy build included): %.1f ms\n", (unsigned)((s + 1) % ns),
                   trans_first[s] / 1000.0);
        print_dist("screen transition, press to last pixels", trans, nt);
        CHECK(wrong == 0);

        /* touch: press and release the ALARMS tab and the OVERVIEW tab through a pointer input device */
        lv_indev_t *ptr = lv_indev_create();
        lv_indev_set_type(ptr, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(ptr, touch_read_cb);
        hmi_ui_press_tab(HMI_UI_SCREEN_OVERVIEW);
        settle_us();
        uint32_t seed = 12345u;
        for (int i = 0; i < 120; i++) {
            bool to_alarms = (hmi_ui_screen() == HMI_UI_SCREEN_OVERVIEW);
            g_tx = to_alarms ? 300 : 100;                               /* ALARMS tab, or OVERVIEW tab */
            g_ty = 62;
            seed = seed * 1103515245u + 12345u;
            fms_set_holding(srv, 0, (i & 1) ? 910 : 700);               /* alarm and polling activity */
            double fb = inject_and_wait(true, (seed >> 16) % 4096u);    /* press */
            settle_us();
            seed = seed * 1103515245u + 12345u;
            double t_rel = now_us();
            double first = inject_and_wait(false, (seed >> 16) % 4096u); /* release */
            double sc = settle_us() - (t_rel + 0.0);
            (void)first;
            if (hmi_ui_screen() != (to_alarms ? HMI_UI_SCREEN_ALARMS : HMI_UI_SCREEN_OVERVIEW)) missed++;
            if (np < 256) { press_fb[np++] = fb; }
            if (nr < 256) { rel_scr[nr++] = first; (void)sc; }
        }
        print_dist("touch press to the first pixels of the pressed button", press_fb, np);
        print_dist("touch release to the first pixels of the new screen", rel_scr, nr);
        printf("[perf] touches: 120 pressed and released, %d did not change the screen, %d ended on the wrong screen\n",
               missed, wrong);
        CHECK(missed == 0 && wrong == 0);
        CHECK(press_fb[np - 1] < 500000.0);                           /* worst case of the sorted list: under half a second */
        /* repeated transitions leave no mark: with static data the table looks exactly as it did before 40 rounds
         * through every screen (the rows between the header and the setpoint panel are compared pixel by pixel) */
        {
            fms_set_holding(srv, 0, 700);
            hmi_ui_press_tab(HMI_UI_SCREEN_OVERVIEW);
            pump(1.5, &samples, &n, cap);
            static uint16_t before[W * H];
            memcpy(before, s_fb, sizeof(before));
            for (int round = 0; round < 40; round++)
                for (size_t s = 1; s <= ns; s++) {
                    hmi_ui_press_tab(s % ns);
                    settle_us();
                }
            pump(0.8, &samples, &n, cap);
            int diff = 0;
            for (int y = 90; y < 370; y++)
                for (int x = 0; x < W; x++)
                    if (before[y * W + x] != s_fb[y * W + x]) diff++;
            printf("[perf] after 40 rounds through every screen the table differs in %d pixels\n", diff);
            CHECK(diff == 0);
            save_ppm((snprintf(err, sizeof(err), "%s_26_after_transitions.ppm", prefix), err));
        }
        g_tdown = false;
        hmi_ui_press_tab(HMI_UI_SCREEN_OVERVIEW);
        fms_set_holding(srv, 0, 700);
        pump(1.0, &samples, &n, cap);
    }

    /* raw results */
    qsort(samples, n, sizeof(double), cmp_d);
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += samples[i];
    printf("lv_timer_handler (render+binder) samples=%u: mean=%.0f us p50=%.0f p95=%.0f p99=%.0f max=%.0f us\n",
           (unsigned)n, sum / (double)n, samples[n / 2], samples[(size_t)(n * 0.95)],
           samples[(size_t)(n * 0.99)], samples[n - 1]);
    printf("flush: calls=%u pixels=%u total_flush_time=%.0f us\n", s_flushes, s_flush_px, s_flush_us_total);
    hmi_ui_stats_t us = hmi_ui_stats();
    printf("binder ticks=%u labels_updated=%u labels_skipped_unchanged=%u\n", us.binder_ticks, us.labels_updated,
           us.labels_skipped_unchanged);
    hmi_comms_stats_t cs;
    hmi_comms_stats(&rt.comms, &cs);
    printf("comms: polls_ok=%u failed=%u exceptions=%u links_lost=%u connects_ok=%u max_poll_latency=%u ms\n",
           cs.polls_ok, cs.polls_failed, cs.exceptions, cs.links_lost, cs.connects_ok, cs.max_poll_latency_ms);
    lv_mem_monitor(&mon);
    printf("LVGL heap at end: used=%u KB, max_used=%u KB, frag=%u%%\n",
           (unsigned)((mon.total_size - mon.free_size) / 1024), (unsigned)(mon.max_used / 1024), (unsigned)mon.frag_pct);
    CHECK(mon.max_used / 1024 <= POOL_PEAK_MAX_KB);
    printf("heap allocation calls since UI build: %u (frees %u); counted at link level, "
           "CRT-internal allocations not included\n",
           alloc_count_total() - alloc_after_init, alloc_count_free());
    report_proc("end");

    hmi_runtime_stop(&rt);
    plc_libmodbus_destroy(plc);
    fms_stop(srv);
    free(samples);
    printf("\nscenario assertions: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
