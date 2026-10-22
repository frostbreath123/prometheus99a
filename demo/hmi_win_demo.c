/* Live demo of the Prometheus99 HMI in a Windows window. NOT the product: a demonstration build.
 *
 * It runs the real UI (LVGL), the real runtime and the real libmodbus driver against an in-process
 * fake Modbus PLC whose values move, and shows the result in a normal window that takes mouse clicks
 * (a click stands in for a touch). The window is drawn with plain Win32 GDI; there is no extra library.
 *
 * Differences from the real device, stated plainly:
 *  - the PLC is the test fake (tests/hmi/fake_modbus_server.c); nothing real is connected;
 *  - the accounts are demo accounts, created on first start (press F1 to see them);
 *  - the configuration is not signed. A small stand-in for the root helper p99_cfg runs inside this
 *    program: it applies a change request from the CONFIG screen with the same rules (hmi_cfgedit_apply),
 *    writes the audit line and the result, then restarts the runtime with the new configuration;
 *  - trend data is in RAM, so a restart after a configuration change starts the trend again.
 *
 * Keys: F1 help and demo accounts, F2 cut or restore the PLC link, Esc closes.
 */
#include "lvgl.h"
#include "../src/ui/hmi_ui.h"
#include "../src/hmi/plc_libmodbus.h"
#include "../src/hmi/hmi_auditfmt.h"
#include "../src/hmi/hmi_cfgedit.h"
#include "../src/hmi/hmi_kdf.h"
#include "../src/hmi/hmi_users.h"
#include "../tests/hmi/fake_modbus_server.h"
#include <windows.h>
#include <direct.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define W 800
#define H 480
#define DATA_DIR "demo_data"
#define USERS_PATH DATA_DIR "/users.bin"
#define AUDIT_PATH DATA_DIR "/audit.log"
#define SPOOL_DIR DATA_DIR "/spool"

static const struct { const char *name; hmi_role_t role; const char *pw; } DEMO_USERS[] = {
    { "admin", HMI_ROLE_ADMIN, "demo-admin-1" },
    { "engineer", HMI_ROLE_ENGINEER, "demo-engineer-1" },
    { "operator", HMI_ROLE_OPERATOR, "demo-operator-1" },
};

/* ------------------------------------------------------------------ window and display */
static HWND g_hwnd;
static HDC g_memdc;
static HBITMAP g_dib;
static uint32_t *g_bits;
static volatile bool g_running = true;
static int g_mx, g_my;
static bool g_down;
static uint8_t s_buf[W * 48 * 2] __attribute__((aligned(64)));

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    const uint16_t *src = (const uint16_t *)px;
    for (int y = 0; y < h; y++) {
        uint32_t *dst = &g_bits[(a->y1 + y) * W + a->x1];
        for (int x = 0; x < w; x++) {
            uint16_t p = src[y * w + x];
            uint32_t r = ((p >> 11) & 31) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
            dst[x] = (r << 16) | (g << 8) | b;
        }
    }
    RECT rc = { a->x1, a->y1, a->x2 + 1, a->y2 + 1 };
    InvalidateRect(g_hwnd, &rc, FALSE);
    lv_display_flush_ready(d);
}

static void pointer_cb(lv_indev_t *i, lv_indev_data_t *data)
{
    (void)i;
    data->point.x = g_mx;
    data->point.y = g_my;
    data->state = g_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t tick_cb(void) { return (uint32_t)hmi_now_ms(); }

static void show_help(void)
{
    char t[1024];
    snprintf(t, sizeof(t),
             "Prometheus99 HMI demo (a demonstration build, a fake PLC)\n\n"
             "Click the screen like a touchscreen. Press LOGIN to sign in:\n\n"
             "   %s  /  %s   (administrator: accounts)\n"
             "   %s  /  %s   (engineer: CONFIG screen)\n"
             "   %s  /  %s   (operator: acknowledge, setpoint)\n\n"
             "Try: TRENDS, the alarm list, a setpoint above 80 on TANK_LEVEL, SETTINGS > CONFIG.\n\n"
             "F2 cuts or restores the PLC link.  Esc closes.",
             DEMO_USERS[0].name, DEMO_USERS[0].pw, DEMO_USERS[1].name, DEMO_USERS[1].pw, DEMO_USERS[2].name,
             DEMO_USERS[2].pw);
    MessageBoxA(g_hwnd, t, "Prometheus99 demo", MB_OK | MB_ICONINFORMATION);
}

static void toggle_link(void);

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
        case WM_LBUTTONDOWN: g_mx = (short)LOWORD(lp); g_my = (short)HIWORD(lp); g_down = true; SetCapture(h); return 0;
        case WM_MOUSEMOVE: g_mx = (short)LOWORD(lp); g_my = (short)HIWORD(lp); return 0;
        case WM_LBUTTONUP: g_mx = (short)LOWORD(lp); g_my = (short)HIWORD(lp); g_down = false; ReleaseCapture(); return 0;
        case WM_KEYDOWN:
            if (wp == VK_F1) show_help();
            else if (wp == VK_F2) toggle_link();
            else if (wp == VK_ESCAPE) g_running = false;
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, g_memdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE: g_running = false; return 0;
        default: return DefWindowProcA(h, m, wp, lp);
    }
}

static bool make_window(HINSTANCE inst)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "p99demo";
    if (!RegisterClassA(&wc)) return false;
    RECT r = { 0, 0, W, H };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_hwnd = CreateWindowA("p99demo", "Prometheus99 HMI demo  (F1 help, F2 cut the PLC link)",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!g_hwnd) return false;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;      /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(g_hwnd);
    g_memdc = CreateCompatibleDC(dc);
    g_dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&g_bits, NULL, 0);
    ReleaseDC(g_hwnd, dc);
    if (!g_dib) return false;
    memset(g_bits, 0x1B, (size_t)W * H * 4);
    SelectObject(g_memdc, g_dib);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    return true;
}

/* ------------------------------------------------------------------ the fake plant and the runtime */
static fake_modbus_t *g_plc;
static uint16_t g_port;
static bool g_link_cut;
static hmi_app_cfg_t g_cfg;
static hmi_runtime_t g_rt;
static hmi_auth_t g_auth;
static plc_libmodbus_t *g_drv;
static uint32_t g_serial = 1;

static void toggle_link(void)
{
    if (!g_plc) return;
    g_link_cut = !g_link_cut;
    fms_set_mode(g_plc, g_link_cut ? FMS_DROP_CONNECTION : FMS_NORMAL, 0);
    if (g_link_cut) fms_kick_client(g_plc);
}

/* The values move so that the trends, the alarms and the quality colours show something. */
static void plant_step(uint64_t now)
{
    static uint64_t last;
    if (now - last < 200u) return;
    last = now;
    double t = (double)now / 1000.0;
    double temp = 45.0 + 40.0 * sin(t / 12.0);          /* 5 to 85 C: crosses the high limit now and then */
    double press = 450.0 + 250.0 * sin(t / 20.0 + 1.0); /* 200 to 700 kPa */
    double flow = 1500.0 + 500.0 * sin(t / 7.0);
    fms_set_holding(g_plc, 1, (uint16_t)(int16_t)lround(temp * 10.0));
    fms_set_holding(g_plc, 2, (uint16_t)lround(press));
    fms_set_holding(g_plc, 3, (uint16_t)lround(flow));
}

static bool runtime_login_audit(void *user, const hmi_auth_audit_rec_t *rec)
{
    char line[256];
    size_t n = hmi_auth_format_audit(rec, line, sizeof(line));
    if (n == 0) return false;
    hmi_wall_t w;
    hmi_wall_now(&w);
    n = hmi_audit_stamp_line(line, n, sizeof(line), &w);
    return n != 0 && hmi_runtime_audit_append((hmi_runtime_t *)user, line, n);
}

static bool start_runtime(void)
{
    g_drv = plc_libmodbus_create(g_cfg.host, g_cfg.port, g_cfg.unit, g_cfg.response_timeout_ms);
    if (!g_drv) return false;
    hmi_plc_driver_t d;
    plc_libmodbus_driver(g_drv, &d);
    if (!hmi_runtime_start(&g_rt, &g_cfg, &d, AUDIT_PATH, NULL)) return false;
    hmi_auth_set_audit(&g_auth, runtime_login_audit, &g_rt);
    return true;
}

static void build_ui(void)
{
    lv_obj_clean(lv_screen_active());
    hmi_ui_init_auth(&g_rt, &g_auth, NULL);
    char info[96];
    snprintf(info, sizeof(info), "Configuration: demo, serial %u (not signed)", (unsigned)g_serial);
    hmi_ui_set_config_info(info);
    hmi_ui_set_config_edit(SPOOL_DIR, g_serial);
}

/* Stands in for the root helper p99_cfg and the restart of the service (see the top of this file). */
static void demo_helper_step(uint64_t now)
{
    static uint64_t last;
    if (now - last < 300u) return;
    last = now;
    static char req_path[] = SPOOL_DIR "/request", res_path[] = SPOOL_DIR "/result";
    static char text[HMI_CFGEDIT_REQUEST_MAX + 2];
    size_t len = 0;
    if (hmi_fs_read_all(req_path, text, sizeof(text) - 1u, &len) != HMI_FS_READ_OK) return;
    text[len] = '\0';
    hmi_cfgedit_request_t req;
    char err[160], res[240];
    static hmi_app_cfg_t next;
    static char cfg_text[HMI_CONFIG_MAX_BYTES + 1];
    size_t clen = 0, eff = 0;
    bool ok = hmi_cfgedit_parse(text, len, &req, err, sizeof(err));
    if (ok && req.base_serial != g_serial) {
        snprintf(err, sizeof(err), "stale request: made for serial %u, the configuration is serial %u",
                 (unsigned)req.base_serial, (unsigned)g_serial);
        ok = false;
    }
    if (ok) ok = hmi_cfgedit_apply(&req, &g_cfg, &next, cfg_text, sizeof(cfg_text), &clen, &eff, err, sizeof(err));
    if (!ok) {
        snprintf(res, sizeof(res), "rejected: %s\n", err);
        hmi_fs_atomic_write(res_path, res, strlen(res));
        remove(req_path);
        return;
    }
    char line[256];
    int n = snprintf(line, sizeof(line), "%llu CONFIG_CHANGED serial=%u from_serial=%u changes=%u by=%s\n",
                     (unsigned long long)hmi_now_ms(), (unsigned)(g_serial + 1u), (unsigned)g_serial, (unsigned)eff,
                     req.user[0] ? req.user : "-");
    hmi_wall_t w;
    hmi_wall_now(&w);
    size_t stamped = n > 0 ? hmi_audit_stamp_line(line, (size_t)n, sizeof(line), &w) : 0;
    if (stamped) hmi_runtime_audit_append(&g_rt, line, stamped);
    snprintf(res, sizeof(res), "applied serial=%u changes=%u by=%s\n", (unsigned)(g_serial + 1u), (unsigned)eff,
             req.user[0] ? req.user : "-");
    hmi_fs_atomic_write(res_path, res, strlen(res));
    remove(req_path);

    /* the restart: stop, use the new configuration, start, rebuild the screens; the login stays */
    hmi_runtime_stop(&g_rt);
    if (g_drv) plc_libmodbus_destroy(g_drv);
    g_drv = NULL;
    g_cfg = next;
    g_serial++;
    if (!start_runtime()) {
        MessageBoxA(g_hwnd, "The runtime did not restart with the new configuration.", "Prometheus99 demo", MB_OK | MB_ICONERROR);
        g_running = false;
        return;
    }
    build_ui();
}

/* ------------------------------------------------------------------ start up */
static bool seed_users(void)
{
    hmi_users_t s;
    char err[120];
    if (hmi_users_load(USERS_PATH, &s, err, sizeof(err)) == HMI_USERS_LOAD_OK && s.n > 0) return true;
    memset(&s, 0, sizeof(s));
    hmi_kdf_params_t kp = hmi_kdf_default_params();
    for (size_t i = 0; i < sizeof(DEMO_USERS) / sizeof(DEMO_USERS[0]); i++) {
        hmi_user_t u;
        if (hmi_user_make(&u, DEMO_USERS[i].name, DEMO_USERS[i].role, DEMO_USERS[i].pw, &kp, NULL) != HMI_USERS_OK) return false;
        if (hmi_users_add(&s, &u) != HMI_USERS_OK) return false;
    }
    return hmi_users_save(USERS_PATH, &s);
}

static void fail(const char *what)
{
    MessageBoxA(NULL, what, "Prometheus99 demo", MB_OK | MB_ICONERROR);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)show;
    bool no_help = cmd && strstr(cmd, "--no-help") != NULL;
    _mkdir(DATA_DIR);
    _mkdir(SPOOL_DIR);
    remove(SPOOL_DIR "/request");
    remove(SPOOL_DIR "/result");
    remove(AUDIT_PATH);

    g_plc = fms_start(&g_port);
    if (!g_plc) { fail("The fake PLC could not start."); return 1; }
    fms_set_holding(g_plc, 0, 500);   /* TANK_LEVEL 50.0 %: change it with the setpoint panel */
    fms_set_holding(g_plc, 1, 450);
    fms_set_holding(g_plc, 2, 450);
    fms_set_holding(g_plc, 3, 1500);

    char json[3072], err[160];
    snprintf(json, sizeof(json),
        "{\"version\":1,\"writes_enabled\":true,\"trend_bucket_ms\":1000,"
        "\"plc\":{\"host\":\"127.0.0.1\",\"port\":%u,\"unit\":1,\"response_timeout_ms\":300,\"connect_timeout_ms\":500,"
        "\"allowed_hosts\":[\"127.0.0.1\",\"127.0.0.2\"]},"
        "\"tags\":["
        "{\"name\":\"TANK_LEVEL\",\"address\":0,\"type\":\"u16\",\"scale\":0.1,\"units\":\"%%\",\"min\":0,\"max\":100,\"poll_ms\":200,"
        "\"alarm\":{\"high\":80,\"high_high\":95,\"low\":10,\"deadband\":2,\"on_ms\":500,\"off_ms\":500,\"priority\":\"critical\"},"
        "\"write\":{\"allowed\":true,\"role\":\"operator\",\"min\":0,\"max\":100}},"
        "{\"name\":\"TEMP\",\"trend\":true,\"address\":1,\"type\":\"i16\",\"scale\":0.1,\"units\":\"C\",\"min\":-40,\"max\":150,\"poll_ms\":200,"
        "\"alarm\":{\"high\":70,\"high_high\":82,\"deadband\":2,\"on_ms\":500,\"off_ms\":500,\"priority\":\"high\"}},"
        "{\"name\":\"PRESSURE\",\"trend\":true,\"address\":2,\"type\":\"u16\",\"units\":\"kPa\",\"min\":0,\"max\":1000,\"poll_ms\":200,"
        "\"alarm\":{\"high\":650,\"deadband\":20,\"on_ms\":500,\"off_ms\":500}},"
        "{\"name\":\"FLOW\",\"trend\":true,\"address\":3,\"type\":\"u16\",\"units\":\"l/min\",\"min\":0,\"max\":5000,\"poll_ms\":300}"
        "]}", (unsigned)g_port);
    if (!hmi_config_parse(json, strlen(json), &g_cfg, err, sizeof(err))) { fail(err); return 1; }
    if (!seed_users()) { fail("The demo accounts could not be created."); return 1; }

    hmi_auth_cfg_t ac;
    ac.users_path = USERS_PATH;
    ac.idle_ms = 600000u;           /* ten minutes, so a talk is not interrupted */
    ac.kdf = NULL;
    if (!hmi_auth_init(&g_auth, &ac, hmi_now_ms())) { fail("The account store could not be opened."); return 1; }

    if (!make_window(inst)) { fail("The window could not be created."); return 1; }
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_buf, NULL, sizeof(s_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_indev_t *ptr = lv_indev_create();
    lv_indev_set_type(ptr, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ptr, pointer_cb);

    if (!start_runtime()) { fail("The runtime could not start."); return 1; }
    build_ui();
    if (!no_help) show_help();

    while (g_running) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        uint64_t now = hmi_now_ms();
        plant_step(now);
        demo_helper_step(now);
        lv_timer_handler();
        Sleep(8);
    }

    hmi_runtime_stop(&g_rt);
    if (g_drv) plc_libmodbus_destroy(g_drv);
    hmi_auth_deinit(&g_auth);
    fms_stop(g_plc);
    return 0;
}
