#include "ui_priv.h"
#include "../hmi/hmi_auditfmt.h"

#define VIEW_X 176               /* the views start right of the list of views */
#define BTN_W 150
#define BTN_H 44
#define SYS_LINES 7

/* The views, in the order of the buttons. A new view is one more line here. */
static const ui_screen_ops_t *const VIEWS[] = { &ui_sub_system, &ui_sub_users, &ui_sub_config };
#define N_VIEWS (sizeof(VIEWS) / sizeof(VIEWS[0]))

static lv_obj_t *s_area[N_VIEWS];
static lv_obj_t *s_btn[N_VIEWS];
static size_t s_view;
static char s_config_info[96];   /* set before or after the UI is built; survives a rebuild */

void ui_config_info_set(const char *text)
{
    snprintf(s_config_info, sizeof(s_config_info), "%s", text ? text : "");
}

const char *ui_config_info_text(void) { return s_config_info; }

size_t ui_settings_view(void) { return s_view; }
size_t ui_settings_view_count(void) { return N_VIEWS; }

static void show_view(void)
{
    for (size_t i = 0; i < N_VIEWS; i++) {
        if (!s_area[i]) continue;
        if (i == s_view) lv_obj_remove_flag(s_area[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_area[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_btn[i], i == s_view ? lv_color_hex(0x3A6EA5) : C_PANEL, 0);
    }
}

static void settings_update(void);

bool ui_settings_select(size_t view)
{
    if (view >= N_VIEWS || !s_area[view]) return false;
    if (view == s_view) return true;
    s_view = view;
    show_view();
    settings_update();
    return true;
}

static void view_cb(lv_event_t *e)
{
    ui_settings_select((size_t)(intptr_t)lv_event_get_user_data(e));
}

/* ---- the system view: read-only facts about this device ---- */
static lv_obj_t *s_sys[SYS_LINES];
static char t_sys[SYS_LINES][UI_TXT * 3];
static uint32_t c_sys[SYS_LINES];
static uint64_t s_sys_at;           /* when the lines were last refreshed */
static bool s_sys_done;

static void system_reset(void)
{
    memset(s_sys, 0, sizeof(s_sys));
    memset(t_sys, 0, sizeof(t_sys));
    memset(c_sys, 0, sizeof(c_sys));
    s_sys_at = 0;
    s_sys_done = false;
}

const char *ui_settings_system_text(size_t line)
{
    return line < SYS_LINES ? t_sys[line] : "";
}

static void system_build(lv_obj_t *area)
{
    ui_mk_label(area, 8, 4, "THIS DEVICE", C_MUTED);
    for (size_t i = 0; i < SYS_LINES; i++) {
        s_sys[i] = ui_mk_label(area, 8, 34 + (int)i * 30, "", C_TEXT);
        lv_label_set_long_mode(s_sys[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_sys[i], UI_SUBPAGE_W - 16);
    }
}

static void sys_line(size_t i, const char *txt, hmi_tone_t tone)
{
    ui_set_text(s_sys[i], t_sys[i], sizeof(t_sys[i]), txt);
    ui_set_color(s_sys[i], &c_sys[i], ui_tone_color(tone));
}

/* Once a second at most: nothing here changes faster. */
static void system_update(void)
{
    uint64_t now = hmi_now_ms();
    if (s_sys_done && now - s_sys_at < 1000u) return;
    s_sys_done = true;
    s_sys_at = now;
    char b[UI_TXT * 3];
    const hmi_app_cfg_t *cfg = &ui_rt->cfg;

    hmi_view_plc_text(cfg->host, cfg->port, cfg->unit, b, sizeof(b));
    sys_line(0, b, HMI_TONE_TEXT);
    hmi_view_writes_text(cfg->writes_enabled, ui_rt->writes_effective, b, sizeof(b));
    sys_line(1, b, ui_rt->writes_effective ? HMI_TONE_GOOD : HMI_TONE_MUTED);
    sys_line(2, s_config_info[0] ? s_config_info : "Configuration: (not reported)", HMI_TONE_TEXT);

    hmi_runtime_storage_info_t si;
    hmi_runtime_storage_info(ui_rt, &si);
    bool fault = hmi_runtime_storage_fault(ui_rt);
    hmi_view_storage_text(fault, si.store.written, si.store.failed, b, sizeof(b));
    sys_line(3, ui_rt->store_started ? b : "Storage: no files are kept (no audit or alarm path given)",
             fault ? HMI_TONE_BAD : HMI_TONE_TEXT);

    hmi_wall_t w;
    hmi_wall_now(&w);
    hmi_view_clock_text(w.valid, w.synced, b, sizeof(b));
    sys_line(4, b, (w.valid && w.synced) ? HMI_TONE_TEXT : HMI_TONE_WARN);

    size_t n = hmi_tagdb_count(&ui_rt->db), alarms = 0;
    for (size_t i = 0; i < n; i++) {
        const hmi_alarm_cfg_t *a = &hmi_tagdb_cfg(&ui_rt->db, i)->alarm;
        if (a->en_hi || a->en_hihi || a->en_lo || a->en_lolo) alarms++;
    }
    hmi_view_tags_text(n, hmi_trend_series_count(&ui_rt->trend), alarms, b, sizeof(b));
    sys_line(5, b, HMI_TONE_TEXT);

    sys_line(6, ui_auth ? "Build: production, roles come from logging in"
                        : "Build: TEST build, roles are claimed and NOT authenticated",
             ui_auth ? HMI_TONE_TEXT : HMI_TONE_BAD);
}

const ui_screen_ops_t ui_sub_system = { "SYSTEM", system_reset, system_build, system_update };

/* ---- the screen ---- */
static void settings_reset(void)
{
    for (size_t i = 0; i < N_VIEWS; i++) {
        s_area[i] = NULL;
        s_btn[i] = NULL;
        VIEWS[i]->reset();
    }
    s_view = 0;
}

static void settings_build(lv_obj_t *page)
{
    for (size_t i = 0; i < N_VIEWS; i++) {
        s_btn[i] = ui_mk_button(page, 12, 8 + (int)i * (BTN_H + 8), BTN_W, BTN_H, VIEWS[i]->title, view_cb, (void *)(intptr_t)i);
        lv_obj_t *a = lv_obj_create(page);
        lv_obj_set_size(a, UI_SUBPAGE_W, UI_PAGE_H);
        lv_obj_set_pos(a, VIEW_X, 0);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(a, 0, 0);
        lv_obj_set_style_radius(a, 0, 0);
        lv_obj_set_style_pad_all(a, 0, 0);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE);
        s_area[i] = a;
        VIEWS[i]->build(a);
    }
    s_view = 0;
    show_view();
}

static void settings_update(void)
{
    if (s_view < N_VIEWS && s_area[s_view]) VIEWS[s_view]->update();
}

const ui_screen_ops_t ui_screen_settings = { "SETTINGS", settings_reset, settings_build, settings_update };
