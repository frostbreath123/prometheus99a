/* Headless host harness: real LVGL v9.2.2 + real UI binder + real libmodbus
 * runtime against the fake Modbus server. Renders into memory (no window),
 * drives a scripted scenario, writes PPM screenshots and prints raw
 * measurements. HOST measurements only: not representative of a Raspberry Pi. */
#include "lvgl.h"
#include "../../src/ui/hmi_ui.h"
#include "../../src/hmi/plc_libmodbus.h"
#include "fake_modbus_server.h"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 800
#define H 480

uint32_t alloc_count_total(void);
uint32_t alloc_count_free(void);
static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static uint16_t s_fb[W * H];
static uint8_t s_buf[W * 48 * 2] __attribute__((aligned(64)));
static uint32_t s_flushes, s_flush_px;
static double s_flush_us_total;

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
        "{\"version\":1,\"writes_enabled\":true,"
        "\"plc\":{\"host\":\"127.0.0.1\",\"port\":%u,\"unit\":1,\"response_timeout_ms\":200,\"connect_timeout_ms\":500},"
        "\"tags\":["
        "{\"name\":\"TANK_LEVEL\",\"address\":0,\"type\":\"u16\",\"scale\":0.1,\"units\":\"%%\",\"min\":0,\"max\":100,\"poll_ms\":100,"
        "\"alarm\":{\"high\":80,\"high_high\":95,\"low\":10,\"deadband\":2,\"on_ms\":300,\"off_ms\":300},"
        "\"write\":{\"allowed\":true,\"role\":\"operator\",\"min\":0,\"max\":100}},"
        "{\"name\":\"TEMP\",\"address\":1,\"type\":\"i16\",\"scale\":0.1,\"units\":\"C\",\"min\":-40,\"max\":150,\"poll_ms\":100},"
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
        hmi_alarm_ack_all(&rt.alarms, hmi_now_ms(), HMI_ROLE_OPERATOR);
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

    /* Authentication through the UI path: the role handed to the core is the
     * session role. First boot creates the administrator, a wrong password is
     * refused and shown identically for any account, a correct one opens the
     * session, an idle session ends, and every step is in the audit file. */
    {
        static char users_path[256], lock_path[272];
        snprintf(users_path, sizeof(users_path), "%s_users.bin", prefix);
        snprintf(lock_path, sizeof(lock_path), "%s.lock", users_path);
        remove(users_path);
        remove(lock_path);
        static hmi_auth_t auth;
        const hmi_kdf_params_t cheap = { 1, 64, 1 };
        hmi_auth_cfg_t acfg;
        acfg.users_path = users_path;
        acfg.idle_ms = 6000;
        acfg.kdf = &cheap;
        CHECK(hmi_auth_init(&auth, &acfg, hmi_now_ms()));
        hmi_auth_set_audit(&auth, hmi_auth_file_audit, (void *)audit_path);

        lv_obj_clean(lv_screen_active());
        hmi_ui_init_auth(&rt, &auth, NULL);
        lv_mem_monitor(&mon);
        printf("LVGL heap with the login screen built: used=%u KB of %u KB\n",
               (unsigned)((mon.total_size - mon.free_size) / 1024), (unsigned)(mon.total_size / 1024));
        pump(0.3, &samples, &n, cap);
        CHECK(strcmp(hmi_ui_role_text(), "VIEWER") == 0);
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACK DISABLED: viewer role") == 0);
        CHECK(!hmi_ui_login_visible());
        hmi_ui_press_ack_all(); /* with no session the core refuses */
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACK DENIED: role not permitted") == 0);

        /* first boot */
        hmi_ui_press_login_button();
        pump(0.3, &samples, &n, cap);
        CHECK(hmi_ui_login_visible());
        CHECK(strcmp(hmi_ui_login_title_text(), "CREATE ADMINISTRATOR") == 0);
        CHECK(strstr(hmi_ui_login_status_text(), "No accounts") != NULL);
        save_ppm((snprintf(err, sizeof(err), "%s_7_first_boot.ppm", prefix), err));
        hmi_ui_login_fill("admin", "short", "short");
        hmi_ui_login_submit();
        CHECK(strstr(hmi_ui_login_status_text(), "too short") != NULL);
        CHECK(hmi_ui_login_field_text(1)[0] == '\0' && hmi_ui_login_field_text(2)[0] == '\0'); /* wiped */
        hmi_ui_login_fill("admin", "admin-pass-1", "admin-pass-2");
        hmi_ui_login_submit();
        CHECK(strstr(hmi_ui_login_status_text(), "do not match") != NULL);
        hmi_ui_login_fill("Admin Name!", NULL, NULL);                  /* the keyboard filter drops what is not allowed */
        CHECK(strcmp(hmi_ui_login_field_text(0), "dminame") == 0);     /* only a-z 0-9 _ . - are accepted */
        hmi_ui_login_fill("admin", "admin-pass-1", "admin-pass-1");
        hmi_ui_login_submit();
        for (int i = 0; i < 40 && strstr(hmi_ui_login_status_text(), "Administrator created") == NULL; i++)
            pump(0.1, &samples, &n, cap);
        CHECK(strstr(hmi_ui_login_status_text(), "Administrator created") != NULL);
        CHECK(strcmp(hmi_ui_login_title_text(), "LOG IN") == 0);
        CHECK(hmi_auth_store_state(&auth) == HMI_AUTH_STORE_READY);
        CHECK(strcmp(hmi_ui_role_text(), "VIEWER") == 0);              /* creating does not log in */

        /* wrong password */
        hmi_ui_login_fill("admin", "wrong-pass-12", NULL);
        hmi_ui_login_submit();
        for (int i = 0; i < 40 && strstr(hmi_ui_login_status_text(), "Wrong user") == NULL; i++)
            pump(0.1, &samples, &n, cap);
        printf("[ui] wrong password: '%s'\n", hmi_ui_login_status_text());
        CHECK(strcmp(hmi_ui_login_status_text(), "Wrong user name or password") == 0);
        CHECK(strcmp(hmi_ui_role_text(), "VIEWER") == 0);
        CHECK(hmi_ui_login_field_text(1)[0] == '\0');
        /* an unknown user gets the very same wording */
        pump(1.1, &samples, &n, cap);
        hmi_ui_login_fill("nobody", "wrong-pass-12", NULL);
        hmi_ui_login_submit();
        for (int i = 0; i < 40 && strstr(hmi_ui_login_status_text(), "Wrong user") == NULL; i++)
            pump(0.1, &samples, &n, cap);
        CHECK(strcmp(hmi_ui_login_status_text(), "Wrong user name or password") == 0);
        /* too soon after a failure */
        hmi_ui_login_fill("admin", "admin-pass-1", NULL);
        hmi_ui_login_submit();
        CHECK(strstr(hmi_ui_login_status_text(), "Please wait") != NULL);
        CHECK(strcmp(hmi_ui_role_text(), "VIEWER") == 0);              /* the right password was not accepted */
        save_ppm((snprintf(err, sizeof(err), "%s_8_login_refused.ppm", prefix), err));
        pump(1.4, &samples, &n, cap);                                  /* the countdown ends by itself */
        CHECK(hmi_ui_login_status_text()[0] == '\0');

        /* cancel and reopen */
        hmi_ui_login_cancel();
        CHECK(!hmi_ui_login_visible());

        /* correct login */
        hmi_ui_press_login_button();
        hmi_ui_login_fill("admin", "admin-pass-1", NULL);
        hmi_ui_login_submit();
        for (int i = 0; i < 40 && hmi_ui_login_visible(); i++) pump(0.1, &samples, &n, cap);
        CHECK(!hmi_ui_login_visible());
        pump(0.2, &samples, &n, cap);
        printf("[ui] logged in: header='%s'\n", hmi_ui_role_text());
        CHECK(strcmp(hmi_ui_role_text(), "ADMIN  admin") == 0);
        CHECK(hmi_auth_role(&auth, hmi_now_ms()) == HMI_ROLE_ADMIN);
        hmi_ui_press_ack_all();
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACK DENIED: role not permitted") != 0);

        /* what the admin does is recorded under the admin's name: an
         * acknowledgement (the alarm returns to normal and waits for it) and a write */
        fms_set_holding(srv, 0, 700);          /* back to normal ... */
        pump(1.0, &samples, &n, cap);
        fms_set_holding(srv, 0, 910);          /* ... and a new high alarm that waits for acknowledgement */
        pump(1.5, &samples, &n, cap);
        CHECK(hmi_alarm_count_unacked(&rt.alarms) >= 1);
        hmi_ui_press_ack_all();
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACKNOWLEDGED") == 0);
        uint32_t writes_before = fms_write_count(srv);
        hmi_ui_press_setpoint_send();
        pump(1.0, &samples, &n, cap);
        CHECK(fms_write_count(srv) == writes_before + 1);
        save_ppm((snprintf(err, sizeof(err), "%s_9_logged_in.ppm", prefix), err));

        /* idle logout: no touch input in this harness, so the deadline runs out */
        pump(6.5, &samples, &n, cap);
        CHECK(strcmp(hmi_ui_role_text(), "VIEWER") == 0);
        hmi_ui_press_ack_all();
        CHECK(strcmp(hmi_ui_ack_status_text(), "ACK DENIED: role not permitted") == 0);

        /* explicit logout with the header button */
        hmi_ui_press_login_button();
        hmi_ui_login_fill("admin", "admin-pass-1", NULL);
        hmi_ui_login_submit();
        for (int i = 0; i < 40 && hmi_ui_login_visible(); i++) pump(0.1, &samples, &n, cap);
        CHECK(strcmp(hmi_ui_role_text(), "ADMIN  admin") == 0);
        hmi_ui_press_login_button();
        pump(0.2, &samples, &n, cap);
        CHECK(strcmp(hmi_ui_role_text(), "VIEWER") == 0);

        /* settled again: an idle screen with the login machinery present stays idle */
        pump(0.5, &samples, &n, cap);
        f0 = s_flushes;
        pump(1.5, &samples, &n, cap);
        CHECK(s_flushes - f0 == 0);

        FILE *af = fopen(audit_path, "rb");
        static char atext[65536];
        size_t alen = af ? fread(atext, 1, sizeof(atext) - 1, af) : 0;
        if (af) fclose(af);
        atext[alen] = '\0';
        CHECK(strstr(atext, "USER_CREATE user=admin role=admin") != NULL);
        CHECK(strstr(atext, "LOGIN_FAIL user=admin failures=1") != NULL);
        CHECK(strstr(atext, "LOGIN_FAIL user=? failures=1") != NULL);
        CHECK(strstr(atext, "LOGIN_OK user=admin role=admin") != NULL);
        CHECK(strstr(atext, "IDLE_LOGOUT user=admin role=admin") != NULL);
        CHECK(strstr(atext, " LOGOUT user=admin role=admin") != NULL);
        CHECK(strstr(atext, "ALARM_ACK role=viewer action=ACK_ALL result=DENIED") != NULL);
        CHECK(strstr(atext, "ALARM_ACK role=admin action=ACK_ALL result=GRANTED") != NULL);
        CHECK(strstr(atext, " user=admin") != NULL);                  /* acknowledgement and write lines name the user */
        {
            const char *wl = strstr(atext, " RESERVED id=");
            const char *last_res = NULL;
            while (wl) { last_res = wl; wl = strstr(wl + 1, " RESERVED id="); }
            CHECK(last_res != NULL && strstr(last_res, "role=3") != NULL);          /* role 3 = admin */
            const char *eol = last_res ? strchr(last_res, 10) : NULL;
            CHECK(eol != NULL && eol - last_res > 10 && strncmp(eol - 11, " user=admin", 11) == 0);
        }
        CHECK(strstr(atext, "pass") == NULL && strstr(atext, "nobody") == NULL); /* no credentials, no unknown names */
        hmi_auth_deinit(&auth);
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
