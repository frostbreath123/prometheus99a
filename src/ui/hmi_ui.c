#include "ui_priv.h"

hmi_runtime_t *ui_rt;
hmi_auth_t *ui_auth;
hmi_role_t ui_fixed_role;
hmi_ui_stats_t ui_stats;

static int s_last_role = -1;
static lv_timer_t *s_timer;
static lv_obj_t *s_link, *s_alarm_sum, *s_hb;
static lv_obj_t *s_ack_btn, *s_role_lbl, *s_login_btn_lbl;
static bool s_hb_on;   /* set before hmi_ui_init; survives re-init */
static char t_hb[UI_TXT];
static char t_role[UI_TXT * 2];
static char t_link[UI_TXT], t_alarm_sum[UI_TXT];
static uint32_t c_link, c_alarm_sum, c_role;

/* The only place a semantic tone becomes a colour. */
lv_color_t ui_tone_color(hmi_tone_t t)
{
    switch (t) {
        case HMI_TONE_GOOD: return C_GOOD;
        case HMI_TONE_WARN: return C_WARN;
        case HMI_TONE_BAD: return C_BAD;
        case HMI_TONE_MUTED: return C_MUTED;
        default: return C_TEXT;
    }
}

/* Set a text colour only when it changed: every style write may invalidate. */
void ui_set_color(lv_obj_t *lbl, uint32_t *cache, lv_color_t c)
{
    uint32_t key = (uint32_t)lv_color_to_u32(c) | 0x01000000u; /* never 0 */
    if (*cache == key) return;
    *cache = key;
    lv_obj_set_style_text_color(lbl, c, 0);
}

/* Set label text only when it differs: avoids invalidation and redraw. */
void ui_set_text(lv_obj_t *lbl, char *cache, size_t cap, const char *txt)
{
    if (strncmp(cache, txt, cap) == 0) {
        ui_stats.labels_skipped_unchanged++;
        return;
    }
    snprintf(cache, cap, "%s", txt);
    lv_label_set_text(lbl, txt);
    ui_stats.labels_updated++;
}

/* ---- the role the core is given ---- */
/* In production this is the session role and nothing else. The fixed role
 * exists only for the test/simulation entry point. */
hmi_role_t ui_current_role(uint64_t now)
{
    return ui_auth ? hmi_auth_role(ui_auth, now) : ui_fixed_role;
}

/* The logged-in user's name for the audit trail ("" when nobody is logged in,
 * and always NULL for the test build's claimed role). */
const char *ui_current_actor(uint64_t now)
{
    return ui_auth ? hmi_auth_user(ui_auth, now) : NULL;
}

/* Header label, ACK button state and the login button caption follow the role. */
void ui_refresh_role(bool force)
{
    uint64_t now = hmi_now_ms();
    hmi_role_t role = ui_current_role(now);
    char buf[UI_TXT * 2];
    if (s_role_lbl) {
        hmi_view_role_label(role, ui_auth ? hmi_auth_user(ui_auth, now) : "", buf, sizeof(buf));
        ui_set_text(s_role_lbl, t_role, sizeof(t_role), buf);
        ui_set_color(s_role_lbl, &c_role, ui_tone_color(ui_auth ? hmi_view_role_tone(role) : HMI_TONE_BAD));
    }
    if (!force && (int)role == s_last_role) return;
    s_last_role = (int)role;

    /* Convenience only: the core refuses the request regardless of this. */
    bool may_ack = hmi_authz_allows(role, HMI_ACT_ACK_ALARM, HMI_ROLE_VIEWER);
    if (s_ack_btn) {
        if (may_ack) lv_obj_remove_state(s_ack_btn, LV_STATE_DISABLED);
        else lv_obj_add_state(s_ack_btn, LV_STATE_DISABLED);
    }
    ui_ack_role_changed(may_ack, role);
    if (s_login_btn_lbl) lv_label_set_text(s_login_btn_lbl, role == HMI_ROLE_VIEWER ? "LOGIN" : "LOGOUT");
}

/* ---- binder ---- */
static void binder_cb(lv_timer_t *t)
{
    (void)t;
    ui_stats.binder_ticks++;
    if (s_hb_on) {
        /* Proves the UI timer itself is running: text only changes once a second. */
        char hb[16];
        hmi_view_heartbeat_text((uint32_t)(hmi_now_ms() / 1000u), hb, sizeof(hb));
        ui_set_text(s_hb, t_hb, sizeof(t_hb), hb);
    }
    if (ui_auth) {
        /* Touch activity postpones the idle logout; the core re-checks the
         * deadline on every privileged call, this only ends the session
         * promptly and records it. */
        uint64_t now = hmi_now_ms();
        hmi_auth_note_activity(ui_auth, now, lv_display_get_inactive_time(NULL));
        hmi_auth_tick(ui_auth, now);
        ui_login_tick(now);
    }
    if (s_role_lbl) ui_refresh_role(false);
    if (!ui_rt) return;
    char buf[UI_TXT * 2];

    /* link + alarm summary */
    /* Same single result the alarm engine uses (never TCP state). */
    hmi_link_health_t lh = hmi_comms_link_health(&ui_rt->comms);
    hmi_view_link_text(lh, buf, sizeof(buf));
    ui_set_text(s_link, t_link, sizeof(t_link), buf);
    ui_set_color(s_link, &c_link, ui_tone_color(hmi_view_link_tone(lh)));

    size_t unacked = hmi_alarm_count_unacked(&ui_rt->alarms);
    hmi_view_alarm_summary_text(unacked, buf, sizeof(buf));
    ui_set_text(s_alarm_sum, t_alarm_sum, sizeof(t_alarm_sum), buf);
    ui_set_color(s_alarm_sum, &c_alarm_sum, ui_tone_color(hmi_view_alarm_summary_tone(unacked)));

    ui_ack_poll();
    ui_nav_update();
}

hmi_ui_stats_t hmi_ui_stats(void) { return ui_stats; }

void hmi_ui_enable_heartbeat(bool on)
{
    s_hb_on = on;
    if (!on && s_hb && t_hb[0]) { /* turning it off removes the mark (UI thread only) */
        t_hb[0] = '\0';
        lv_label_set_text(s_hb, "");
    }
}

const char *hmi_ui_row_text(size_t row, int column) { return ui_overview_row_text(row, column); }
const char *hmi_ui_link_text(void) { return t_link; }
const char *hmi_ui_heartbeat_text(void) { return t_hb; }
const char *hmi_ui_ack_status_text(void) { return ui_ack_text(); }
void hmi_ui_press_ack_all(void) { ui_ack_all(); }
void hmi_ui_press_setpoint_send(void) { ui_overview_press_setpoint_send(); }

const char *hmi_ui_role_text(void) { return t_role; }

/* ---- screen test seams ---- */
size_t hmi_ui_screen_count(void) { return ui_nav_count(); }
size_t hmi_ui_screen(void) { return ui_nav_current(); }
bool hmi_ui_screen_visible(size_t screen) { return ui_nav_visible(screen); }
void hmi_ui_press_tab(size_t screen) { ui_nav_select(screen); }
const char *hmi_ui_row_name(size_t row) { return ui_overview_row_name(row); }
size_t hmi_ui_overview_page(void) { return ui_overview_page(); }
size_t hmi_ui_overview_pages(void) { return ui_overview_pages(); }
void hmi_ui_press_overview_page(int dir) { ui_overview_press_page(dir); }
size_t hmi_ui_alarm_rows(void) { return ui_alarms_rows(); }
const char *hmi_ui_alarm_row_text(size_t row, int column) { return ui_alarms_row_text(row, column); }
bool hmi_ui_alarm_ack_enabled(size_t row) { return ui_alarms_ack_enabled(row); }
void hmi_ui_press_alarm_ack(size_t row) { ui_alarms_press_ack(row); }
size_t hmi_ui_alarm_page(void) { return ui_alarms_page(); }
size_t hmi_ui_alarm_pages(void) { return ui_alarms_pages(); }
void hmi_ui_press_alarm_page(int dir) { ui_alarms_press_page(dir); }
size_t hmi_ui_trend_series(void) { return ui_trends_series(); }
size_t hmi_ui_trend_window(void) { return ui_trends_window(); }
size_t hmi_ui_trend_points(void) { return ui_trends_points(); }
uint32_t hmi_ui_trend_redraws(void) { return ui_trends_redraws(); }
const char *hmi_ui_trend_name(void) { return ui_trends_name(); }
const char *hmi_ui_trend_status_text(void) { return ui_trends_status_text(); }
const char *hmi_ui_trend_axis_text(int which) { return ui_trends_axis_text(which); }
int hmi_ui_trend_value(int which, size_t index) { return (int)ui_trends_chart_value(which, index); }
void hmi_ui_set_config_info(const char *text) { ui_config_info_set(text); }
size_t hmi_ui_settings_view(void) { return ui_settings_view(); }
size_t hmi_ui_settings_view_count(void) { return ui_settings_view_count(); }
void hmi_ui_press_settings_view(size_t view) { ui_settings_select(view); }
const char *hmi_ui_system_text(size_t line) { return ui_settings_system_text(line); }
size_t hmi_ui_users_rows(void) { return ui_users_rows(); }
const char *hmi_ui_users_row_text(size_t row, int column) { return ui_users_row_text(row, column); }
void hmi_ui_users_select(size_t row) { ui_users_select_row(row); }
void hmi_ui_press_users(int button) { ui_users_press((ui_users_btn_t)button); }
bool hmi_ui_users_button_enabled(int button) { return ui_users_button_enabled((ui_users_btn_t)button); }
const char *hmi_ui_users_status_text(void) { return ui_users_status_text(); }
const char *hmi_ui_users_message_text(void) { return ui_users_message_text(); }
const char *hmi_ui_users_selected(void) { return ui_users_selected(); }
int hmi_ui_users_new_role(void) { return (int)ui_users_new_role(); }
size_t hmi_ui_users_pages(void) { return ui_users_pages(); }
size_t hmi_ui_users_page(void) { return ui_users_page(); }
void hmi_ui_press_users_page(int dir) { ui_users_press_page(dir); }
void hmi_ui_set_config_edit(const char *spool_dir, uint32_t base_serial) { ui_config_set_edit(spool_dir, base_serial); }
size_t hmi_ui_config_targets(void) { return ui_config_targets(); }
size_t hmi_ui_config_target(void) { return ui_config_target(); }
const char *hmi_ui_config_target_text(void) { return ui_config_target_text(); }
void hmi_ui_press_config_target(int dir) { ui_config_press_target(dir); }
size_t hmi_ui_config_rows(void) { return ui_config_rows(); }
const char *hmi_ui_config_row_text(size_t row, int column) { return ui_config_row_text(row, column); }
void hmi_ui_press_config_row(size_t row) { ui_config_press_row(row); }
bool hmi_ui_config_keypad_open(void) { return ui_config_keypad_open(); }
const char *hmi_ui_config_keypad_text(void) { return ui_config_keypad_text(); }
void hmi_ui_press_config_key(const char *key) { ui_config_press_key(key); }
void hmi_ui_press_config(int button) { ui_config_press((ui_config_btn_t)button); }
bool hmi_ui_config_button_enabled(int button) { return ui_config_button_enabled((ui_config_btn_t)button); }
size_t hmi_ui_config_pending(void) { return ui_config_pending(); }
const char *hmi_ui_config_status_text(void) { return ui_config_status_text(); }
const char *hmi_ui_config_result_text(void) { return ui_config_result_text(); }
const char *hmi_ui_config_message_text(void) { return ui_config_message_text(); }
void hmi_ui_press_trend_series(int dir) { ui_trends_press_series(dir); }
void hmi_ui_press_trend_window(size_t index) { ui_trends_press_window(index); }

/* ---- construction ---- */
lv_obj_t *ui_mk_label(lv_obj_t *parent, int x, int y, const char *txt, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *ui_mk_button(lv_obj_t *parent, int x, int y, int w, int h, const char *txt, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

static void ui_build(hmi_runtime_t *rt, const char *banner)
{
    memset(t_link, 0, sizeof(t_link));
    memset(t_alarm_sum, 0, sizeof(t_alarm_sum));
    memset(t_hb, 0, sizeof(t_hb));
    memset(t_role, 0, sizeof(t_role));
    ui_nav_reset();
    ui_ack_reset();
    ui_login_reset();
    s_ack_btn = s_role_lbl = s_login_btn_lbl = NULL;
    s_last_role = -1;
    ui_rt = rt;
    c_link = c_alarm_sum = c_role = 0;
    memset(&ui_stats, 0, sizeof(ui_stats));
    if (s_timer) { /* a re-init must not leave the previous binder running */
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 800, 44);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_PANEL, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    ui_mk_label(hdr, 8, 12, "PROMETHEUS99", C_TEXT);
    s_link = ui_mk_label(hdr, 130, 12, "PLC: --", C_MUTED);
    s_hb = ui_mk_label(hdr, 272, 12, "", C_MUTED);
    s_alarm_sum = ui_mk_label(hdr, 326, 12, "ALARMS: --", C_MUTED);
    s_role_lbl = ui_mk_label(hdr, 512, 12, "", C_MUTED);
    lv_label_set_long_mode(s_role_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_role_lbl, 132);
    s_ack_btn = ui_mk_button(hdr, 650, 2, 74, 38, "ACK ALL", ui_ack_all_cb, NULL);
    if (ui_auth) {
        lv_obj_t *lb = ui_mk_button(hdr, 728, 2, 68, 38, "LOGIN", ui_login_button_cb, NULL);
        s_login_btn_lbl = lv_obj_get_child(lb, 0);
    }

    if (!rt) {
        lv_obj_t *b = ui_mk_label(scr, 20, 80, banner ? banner : "NO CONFIGURATION", C_BAD);
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(b, 760);
        ui_mk_label(scr, 20, 140, "No PLC endpoint selected. Writes disabled.", C_MUTED);
    } else {
        ui_nav_build(scr);
    }

    ui_login_build(scr);
    ui_refresh_role(true);
    s_timer = lv_timer_create(binder_cb, 50, NULL);
}

void hmi_ui_init(hmi_runtime_t *rt, hmi_role_t role, const char *banner)
{
    ui_auth = NULL;
    ui_fixed_role = role;
    ui_build(rt, banner);
}

void hmi_ui_init_auth(hmi_runtime_t *rt, hmi_auth_t *auth, const char *banner)
{
    ui_auth = auth;
    ui_fixed_role = HMI_ROLE_VIEWER;
    ui_build(rt, banner);
}
