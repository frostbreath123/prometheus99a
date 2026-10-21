#include "hmi_ui.h"
#include "hmi_view.h"
#include <stdio.h>
#include <string.h>

#define ROW_H 26
#define MAX_ROWS 12
#define TXT 40

typedef struct {
    lv_obj_t *name, *value, *quality, *alarm;
    uint32_t c_value, c_quality, c_alarm;
    char t_value[TXT], t_quality[TXT], t_alarm[TXT];
    uint32_t seen_version;
    uint8_t last_alarm_state;
    bool init;
} row_t;

typedef enum { OV_HIDDEN = 0, OV_LOGIN, OV_SETUP } ov_mode_t;

static hmi_runtime_t *s_rt;
static hmi_auth_t *s_auth;          /* production: the session is the only source of the role */
static hmi_role_t s_fixed_role;     /* test builds only: used when s_auth is NULL */
static int s_last_role = -1;
static row_t s_rows[MAX_ROWS];
static size_t s_n_rows;
static lv_timer_t *s_timer;
static lv_obj_t *s_link, *s_alarm_sum, *s_wr_status, *s_sp_label, *s_journal, *s_hb, *s_ack_status;
static lv_obj_t *s_ack_btn, *s_role_lbl, *s_login_btn_lbl;
static bool s_hb_on;   /* set before hmi_ui_init; survives re-init */
static char t_hb[TXT];
static char t_ack[TXT * 2];
static char t_role[TXT * 2];
static char t_link[TXT], t_alarm_sum[TXT], t_wr[TXT * 2], t_sp[TXT], t_journal[480];
static uint32_t c_link, c_alarm_sum, c_wr, c_role;
static int s_sp_tag = -1;
static double s_sp_value;
static bool s_sp_inited;
static uint32_t s_last_write_id;
static uint32_t s_last_gv = 0xFFFFFFFFu;
static hmi_ui_stats_t s_stats;

/* login screen */
static lv_obj_t *s_ov, *s_ov_title, *s_ov_user, *s_ov_pw, *s_ov_conf, *s_ov_conf_cap, *s_ov_status, *s_ov_kb;
static ov_mode_t s_ov_mode;
static bool s_ov_pending;            /* a hash is being computed; the screen cannot be left */
static bool s_ov_countdown;          /* a lock or gap is running for s_ov_name */
static char s_ov_name[HMI_USER_NAME_MAX + 1];
static char t_ov_title[TXT], t_ov_status[160];
static uint32_t c_ov_status;

/* --- colours (flat, high contrast, ISA-101 style) --- */
#define C_BG     lv_color_hex(0x1B1F24)
#define C_PANEL  lv_color_hex(0x262B31)
#define C_TEXT   lv_color_hex(0xE6E6E6)
#define C_MUTED  lv_color_hex(0x8A9199)
#define C_GOOD   lv_color_hex(0x4CAF50)
#define C_WARN   lv_color_hex(0xFFB300)
#define C_BAD    lv_color_hex(0xE53935)

/* The only place a semantic tone becomes a colour. */
static lv_color_t tone_color(hmi_tone_t t)
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
static void set_color(lv_obj_t *lbl, uint32_t *cache, lv_color_t c)
{
    uint32_t key = (uint32_t)lv_color_to_u32(c) | 0x01000000u; /* never 0 */
    if (*cache == key) return;
    *cache = key;
    lv_obj_set_style_text_color(lbl, c, 0);
}

/* Set label text only when it differs: avoids invalidation and redraw. */
static void set_text(lv_obj_t *lbl, char *cache, size_t cap, const char *txt)
{
    if (strncmp(cache, txt, cap) == 0) {
        s_stats.labels_skipped_unchanged++;
        return;
    }
    snprintf(cache, cap, "%s", txt);
    lv_label_set_text(lbl, txt);
    s_stats.labels_updated++;
}

/* ---- the role the core is given ---- */
/* In production this is the session role and nothing else. The fixed role
 * exists only for the test/simulation entry point. */
static hmi_role_t current_role(uint64_t now)
{
    return s_auth ? hmi_auth_role(s_auth, now) : s_fixed_role;
}

/* The logged-in user's name for the audit trail ("" when nobody is logged in,
 * and always NULL for the test build's claimed role). */
static const char *current_actor(uint64_t now)
{
    return s_auth ? hmi_auth_user(s_auth, now) : NULL;
}

/* Header label, ACK button state and the login button caption follow the role. */
static void refresh_role_ui(bool force)
{
    uint64_t now = hmi_now_ms();
    hmi_role_t role = current_role(now);
    char buf[TXT * 2];
    if (s_role_lbl) {
        hmi_view_role_label(role, s_auth ? hmi_auth_user(s_auth, now) : "", buf, sizeof(buf));
        set_text(s_role_lbl, t_role, sizeof(t_role), buf);
        set_color(s_role_lbl, &c_role, tone_color(s_auth ? hmi_view_role_tone(role) : HMI_TONE_BAD));
    }
    if (!force && (int)role == s_last_role) return;
    s_last_role = (int)role;

    /* Convenience only: the core refuses the request regardless of this. */
    bool may_ack = hmi_authz_allows(role, HMI_ACT_ACK_ALARM, HMI_ROLE_VIEWER);
    if (s_ack_btn) {
        if (may_ack) lv_obj_remove_state(s_ack_btn, LV_STATE_DISABLED);
        else lv_obj_add_state(s_ack_btn, LV_STATE_DISABLED);
    }
    if (s_ack_status) {
        if (!may_ack) {
            snprintf(t_ack, sizeof(t_ack), "ACK DISABLED: %s role", hmi_role_name(role));
            lv_label_set_text(s_ack_status, t_ack);
        } else { /* a new role: an outcome shown for the previous one no longer applies */
            t_ack[0] = '\0';
            lv_label_set_text(s_ack_status, "");
        }
    }
    if (s_login_btn_lbl) lv_label_set_text(s_login_btn_lbl, role == HMI_ROLE_VIEWER ? "LOGIN" : "LOGOUT");
}

/* ---- login screen ---- */
static void ov_status(const char *txt, hmi_tone_t tone)
{
    if (!s_ov_status) return;
    set_text(s_ov_status, t_ov_status, sizeof(t_ov_status), txt);
    set_color(s_ov_status, &c_ov_status, tone_color(tone));
}

static void ov_login_status(hmi_login_result_t r, uint32_t wait_ms, const char *user)
{
    char b[160];
    hmi_view_login_text(r, wait_ms, user, b, sizeof(b));
    ov_status(b, hmi_view_login_tone(r));
}

static void ov_setup_status(hmi_setup_result_t r)
{
    char b[160];
    hmi_view_setup_text(r, s_auth ? hmi_auth_setup_reason(s_auth) : "", b, sizeof(b));
    ov_status(b, hmi_view_setup_tone(r));
}

/* Overwrite what was typed before clearing: the text widget may keep the
 * buffer it grew into. This narrows, but cannot prove away, the time a
 * password stays in the LVGL memory pool. */
static void ta_wipe(lv_obj_t *ta)
{
    if (!ta) return;
    char mask[HMI_PASSWORD_MAX + 1];
    size_t n = strlen(lv_textarea_get_text(ta));
    if (n > HMI_PASSWORD_MAX) n = HMI_PASSWORD_MAX;
    memset(mask, 'x', n);
    mask[n] = '\0';
    lv_textarea_set_text(ta, mask);
    lv_textarea_set_text(ta, "");
}

static void ov_set_title(const char *t)
{
    snprintf(t_ov_title, sizeof(t_ov_title), "%s", t);
    if (s_ov_title) lv_label_set_text(s_ov_title, t);
}

static void ov_show_confirm(bool on)
{
    if (!s_ov_conf) return;
    if (on) {
        lv_obj_remove_flag(s_ov_conf, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ov_conf_cap, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ov_conf, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ov_conf_cap, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ov_focus(lv_obj_t *ta)
{
    lv_obj_t *all[3] = { s_ov_user, s_ov_pw, s_ov_conf };
    for (int i = 0; i < 3; i++)
        if (all[i]) {
            if (all[i] == ta) lv_obj_add_state(all[i], LV_STATE_FOCUSED);
            else lv_obj_remove_state(all[i], LV_STATE_FOCUSED);
        }
    if (s_ov_kb && ta) lv_keyboard_set_textarea(s_ov_kb, ta);
}

static void ov_open(void)
{
    if (!s_auth || !s_ov || s_ov_mode != OV_HIDDEN) return;
    ta_wipe(s_ov_user);
    ta_wipe(s_ov_pw);
    ta_wipe(s_ov_conf);
    s_ov_countdown = false;
    s_ov_pending = false;
    switch (hmi_auth_store_state(s_auth)) {
        case HMI_AUTH_STORE_NONE:
            s_ov_mode = OV_SETUP;
            ov_set_title("CREATE ADMINISTRATOR");
            ov_show_confirm(true);
            ov_login_status(HMI_LOGIN_NO_STORE, 0, NULL);
            break;
        case HMI_AUTH_STORE_READY:
            s_ov_mode = OV_LOGIN;
            ov_set_title("LOG IN");
            ov_show_confirm(false);
            ov_status("", HMI_TONE_TEXT);
            break;
        default: /* damaged store: nothing can be typed that would work */
            s_ov_mode = OV_LOGIN;
            ov_set_title("LOG IN");
            ov_show_confirm(false);
            ov_login_status(HMI_LOGIN_STORE_INVALID, 0, NULL);
            break;
    }
    lv_obj_remove_flag(s_ov, LV_OBJ_FLAG_HIDDEN);
    ov_focus(s_ov_user);
}

static void ov_close(void)
{
    if (!s_ov) return;
    ta_wipe(s_ov_user);
    ta_wipe(s_ov_pw);
    ta_wipe(s_ov_conf);
    s_ov_mode = OV_HIDDEN;
    s_ov_pending = false;
    s_ov_countdown = false;
    lv_obj_add_flag(s_ov, LV_OBJ_FLAG_HIDDEN);
}

static void ov_cancel(void)
{
    if (s_ov_mode == OV_HIDDEN || s_ov_pending) return; /* a running check must finish first */
    ov_close();
}

static void ov_submit(void)
{
    if (!s_auth || s_ov_mode == OV_HIDDEN || s_ov_pending) return;
    uint64_t now = hmi_now_ms();
    const char *user = lv_textarea_get_text(s_ov_user);
    const char *pw = lv_textarea_get_text(s_ov_pw);

    if (s_ov_mode == OV_LOGIN) {
        hmi_login_info_t r = hmi_auth_login_begin(s_auth, user, pw, now);
        snprintf(s_ov_name, sizeof(s_ov_name), "%s", user);
        ta_wipe(s_ov_pw);
        if (r.result == HMI_LOGIN_PENDING) {
            s_ov_pending = true;
            ov_login_status(HMI_LOGIN_PENDING, 0, NULL);
        } else {
            s_ov_countdown = (r.result == HMI_LOGIN_LOCKED || r.result == HMI_LOGIN_WAIT);
            ov_login_status(r.result, r.wait_ms, user);
        }
        return;
    }

    /* first boot: create the administrator */
    const char *conf = lv_textarea_get_text(s_ov_conf);
    if (strcmp(pw, conf) != 0) {
        ta_wipe(s_ov_pw);
        ta_wipe(s_ov_conf);
        ov_status(hmi_view_setup_mismatch_text(), HMI_TONE_BAD);
        return;
    }
    snprintf(s_ov_name, sizeof(s_ov_name), "%s", user);
    hmi_setup_result_t sr = hmi_auth_setup_begin(s_auth, user, pw, now);
    ta_wipe(s_ov_pw);
    ta_wipe(s_ov_conf);
    if (sr == HMI_SETUP_PENDING) s_ov_pending = true;
    ov_setup_status(sr);
}

/* Collects a finished hash job (UI thread). */
static void ov_poll(uint64_t now)
{
    if (!s_auth || !hmi_auth_busy(s_auth)) return;
    hmi_auth_done_t d;
    if (!hmi_auth_poll(s_auth, now, &d)) return;
    s_ov_pending = false;
    if (d.kind == HMI_AUTH_JOB_LOGIN) {
        if (d.login.result == HMI_LOGIN_OK) {
            ov_close();
            refresh_role_ui(false);
            return;
        }
        /* A plain wrong password keeps its message; only a real lock runs a countdown. */
        s_ov_countdown = d.login.result == HMI_LOGIN_BAD && d.login.wait_ms >= HMI_AUTH_LOCK_BASE_MS;
        ov_login_status(d.login.result, d.login.wait_ms, NULL);
    } else if (d.kind == HMI_AUTH_JOB_SETUP) {
        if (d.setup == HMI_SETUP_OK) {
            ta_wipe(s_ov_user);
            s_ov_mode = OV_LOGIN; /* the account exists now: log in with it */
            ov_set_title("LOG IN");
            ov_show_confirm(false);
            ov_focus(s_ov_user);
        }
        ov_setup_status(d.setup);
    }
}

/* Keeps the "try again in M:SS" text current while a lock or gap runs. */
static void ov_countdown(uint64_t now)
{
    if (!s_ov_countdown || s_ov_mode != OV_LOGIN || s_ov_pending || !s_auth) return;
    uint32_t rem = hmi_auth_wait_ms(s_auth, s_ov_name, now);
    if (rem == 0u) {
        s_ov_countdown = false;
        ov_status("", HMI_TONE_TEXT);
        return;
    }
    ov_login_status(rem > HMI_AUTH_GAP_MS ? HMI_LOGIN_LOCKED : HMI_LOGIN_WAIT, rem, NULL);
}

static void do_logout(void)
{
    if (!s_auth) return;
    hmi_auth_logout(s_auth, hmi_now_ms());
    refresh_role_ui(false);
}

static void login_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!s_auth) return;
    if (hmi_auth_logged_in(s_auth, hmi_now_ms())) do_logout();
    else ov_open();
}

static void ov_ok_cb(lv_event_t *e)
{
    (void)e;
    ov_submit();
}

static void ov_cancel_cb(lv_event_t *e)
{
    (void)e;
    ov_cancel();
}

static void kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) ov_submit();
    else ov_cancel();
}

static void ta_click_cb(lv_event_t *e)
{
    ov_focus(lv_event_get_target_obj(e));
}

/* ---- event handlers (UI thread) ---- */
/* The core decides (hmi_alarm_ack_all checks the role and audits); this only
 * shows the outcome. */
static void do_ack_all(void)
{
    if (!s_rt || !s_ack_status) return;
    uint64_t now = hmi_now_ms();
    hmi_ack_result_t r = hmi_alarm_ack_all_as(&s_rt->alarms, now, current_role(now), current_actor(now));
    snprintf(t_ack, sizeof(t_ack), "%s", hmi_view_ack_result_text(r));
    lv_label_set_text(s_ack_status, t_ack);
    lv_obj_set_style_text_color(s_ack_status, tone_color(hmi_view_ack_result_tone(r)), 0);
}

static void ack_all_cb(lv_event_t *e)
{
    (void)e;
    do_ack_all();
}

static void sp_step_cb(lv_event_t *e)
{
    intptr_t dir = (intptr_t)lv_event_get_user_data(e);
    if (s_sp_tag < 0 || !s_rt) return;
    s_sp_value = hmi_view_setpoint_step(hmi_tagdb_cfg(&s_rt->db, (size_t)s_sp_tag), s_sp_value, dir > 0 ? 1 : -1);
}

static void sp_send_cb(lv_event_t *e)
{
    (void)e;
    if (s_sp_tag < 0 || !s_rt) return;
    uint32_t id = 0;
    uint64_t now = hmi_now_ms();
    hmi_wr_submit_t r = hmi_writes_submit_as(&s_rt->writes, (size_t)s_sp_tag, s_sp_value, current_role(now),
                                             current_actor(now), hmi_comms_link_health(&s_rt->comms), now, &id);
    if (r == HMI_WRS_OK) {
        s_last_write_id = id;
    } else {
        s_last_write_id = 0;
        hmi_view_submit_refused_text(r, t_wr, sizeof(t_wr));
        lv_label_set_text(s_wr_status, t_wr);
        lv_obj_set_style_text_color(s_wr_status, tone_color(HMI_TONE_BAD), 0);
    }
}

/* ---- binder ---- */
static void binder_cb(lv_timer_t *t)
{
    (void)t;
    s_stats.binder_ticks++;
    if (s_hb_on) {
        /* Proves the UI timer itself is running: text only changes once a second. */
        char hb[16];
        hmi_view_heartbeat_text((uint32_t)(hmi_now_ms() / 1000u), hb, sizeof(hb));
        set_text(s_hb, t_hb, sizeof(t_hb), hb);
    }
    if (s_auth) {
        /* Touch activity postpones the idle logout; the core re-checks the
         * deadline on every privileged call, this only ends the session
         * promptly and records it. */
        uint64_t now = hmi_now_ms();
        hmi_auth_note_activity(s_auth, now, lv_display_get_inactive_time(NULL));
        hmi_auth_tick(s_auth, now);
        ov_poll(now);
        ov_countdown(now);
    }
    if (s_role_lbl) refresh_role_ui(false);
    if (!s_rt) return;
    char buf[TXT * 2];

    /* link + alarm summary */
    /* Same single result the alarm engine uses (never TCP state). */
    hmi_link_health_t lh = hmi_comms_link_health(&s_rt->comms);
    hmi_view_link_text(lh, buf, sizeof(buf));
    set_text(s_link, t_link, sizeof(t_link), buf);
    set_color(s_link, &c_link, tone_color(hmi_view_link_tone(lh)));

    size_t unacked = hmi_alarm_count_unacked(&s_rt->alarms);
    hmi_view_alarm_summary_text(unacked, buf, sizeof(buf));
    set_text(s_alarm_sum, t_alarm_sum, sizeof(t_alarm_sum), buf);
    set_color(s_alarm_sum, &c_alarm_sum, tone_color(hmi_view_alarm_summary_tone(unacked)));

    /* tag rows: only re-render rows whose version or alarm state changed */
    uint32_t gv = hmi_tagdb_global_version(&s_rt->db);
    hmi_tag_value_t vals[HMI_MAX_TAGS];
    bool tags_changed = (gv != s_last_gv);
    if (tags_changed) {
        hmi_tagdb_snapshot(&s_rt->db, vals, HMI_MAX_TAGS);
        s_last_gv = gv;
    }
    for (size_t i = 0; i < s_n_rows; i++) {
        row_t *r = &s_rows[i];
        hmi_alarm_point_t ap;
        hmi_alarm_get(&s_rt->alarms, i, &ap);
        bool alarm_changed = (!r->init) || r->last_alarm_state != (uint8_t)(ap.state | (ap.data_suppressed ? 0x80 : 0));
        if (!tags_changed && !alarm_changed) continue;
        if (!tags_changed && alarm_changed) hmi_tagdb_get(&s_rt->db, i, &vals[i]);

        const hmi_tag_cfg_t *c = hmi_tagdb_cfg(&s_rt->db, i);
        const hmi_tag_value_t *v = &vals[i];
        if (!r->init || r->seen_version != v->version) {
            hmi_view_value_text(c, v, buf, sizeof(buf));
            set_text(r->value, r->t_value, sizeof(r->t_value), buf);
            set_color(r->value, &r->c_value, tone_color(hmi_view_value_tone(v->quality)));
            set_text(r->quality, r->t_quality, sizeof(r->t_quality), hmi_view_quality_text(v));
            set_color(r->quality, &r->c_quality, tone_color(hmi_view_quality_tone(v->quality)));
            r->seen_version = v->version;
        }
        if (alarm_changed) {
            char ab[TXT];
            hmi_view_alarm_text(&ap, ab, sizeof(ab));
            set_text(r->alarm, r->t_alarm, sizeof(r->t_alarm), ab);
            set_color(r->alarm, &r->c_alarm, tone_color(hmi_view_alarm_tone(&ap)));
            r->last_alarm_state = (uint8_t)(ap.state | (ap.data_suppressed ? 0x80 : 0));
        }
        r->init = true;
    }

    /* setpoint panel */
    if (s_sp_tag >= 0) {
        const hmi_tag_cfg_t *c = hmi_tagdb_cfg(&s_rt->db, (size_t)s_sp_tag);
        if (!s_sp_inited) {
            hmi_tag_value_t v;
            hmi_tagdb_get(&s_rt->db, (size_t)s_sp_tag, &v);
            if (v.quality == HMI_Q_GOOD) {
                s_sp_value = v.eng;
                s_sp_inited = true;
            }
        }
        hmi_view_setpoint_text(c, s_sp_value, buf, sizeof(buf));
        set_text(s_sp_label, t_sp, sizeof(t_sp), buf);
    }
    if (s_last_write_id) {
        hmi_write_rec_t rec;
        if (hmi_writes_status(&s_rt->writes, s_last_write_id, &rec)) {
            char b2[TXT * 2];
            hmi_view_write_status_text(&rec, b2, sizeof(b2));
            set_text(s_wr_status, t_wr, sizeof(t_wr), b2);
            set_color(s_wr_status, &c_wr, tone_color(hmi_view_write_status_tone(rec.state)));
        }
    }

    /* journal: last 4 events */
    hmi_alarm_log_t j[4];
    size_t n = hmi_alarm_journal_copy(&s_rt->alarms, j, 4);
    char jb[sizeof(t_journal)];
    const char *names[HMI_MAX_TAGS];
    size_t n_names = hmi_tagdb_count(&s_rt->db);
    for (size_t k = 0; k < n_names; k++) names[k] = hmi_tagdb_cfg(&s_rt->db, k)->name;
    hmi_view_journal_text(j, n, names, n_names, jb, sizeof(jb));
    set_text(s_journal, t_journal, sizeof(t_journal), jb);
}

hmi_ui_stats_t hmi_ui_stats(void) { return s_stats; }

void hmi_ui_enable_heartbeat(bool on)
{
    s_hb_on = on;
    if (!on && s_hb && t_hb[0]) { /* turning it off removes the mark (UI thread only) */
        t_hb[0] = '\0';
        lv_label_set_text(s_hb, "");
    }
}

const char *hmi_ui_row_text(size_t row, int column)
{
    if (row >= s_n_rows) return "";
    const row_t *r = &s_rows[row];
    return column == 0 ? r->t_value : column == 1 ? r->t_quality : r->t_alarm;
}

const char *hmi_ui_link_text(void) { return t_link; }
const char *hmi_ui_heartbeat_text(void) { return t_hb; }
const char *hmi_ui_ack_status_text(void) { return t_ack; }
void hmi_ui_press_ack_all(void) { do_ack_all(); }
void hmi_ui_press_setpoint_send(void) { sp_send_cb(NULL); }

const char *hmi_ui_role_text(void) { return t_role; }
bool hmi_ui_login_visible(void) { return s_ov_mode != OV_HIDDEN; }
const char *hmi_ui_login_title_text(void) { return t_ov_title; }
const char *hmi_ui_login_status_text(void) { return t_ov_status; }

const char *hmi_ui_login_field_text(int field)
{
    lv_obj_t *ta = field == 0 ? s_ov_user : field == 1 ? s_ov_pw : field == 2 ? s_ov_conf : NULL;
    return ta ? lv_textarea_get_text(ta) : "";
}

void hmi_ui_press_login_button(void) { login_btn_cb(NULL); }

void hmi_ui_login_fill(const char *user, const char *pw, const char *confirm)
{
    if (s_ov_mode == OV_HIDDEN) return;
    if (user) lv_textarea_set_text(s_ov_user, user);
    if (pw) lv_textarea_set_text(s_ov_pw, pw);
    if (confirm) lv_textarea_set_text(s_ov_conf, confirm);
}

void hmi_ui_login_submit(void) { ov_submit(); }
void hmi_ui_login_cancel(void) { ov_cancel(); }

/* ---- construction ---- */
static lv_obj_t *mk_label(lv_obj_t *parent, int x, int y, const char *txt, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *mk_button(lv_obj_t *parent, int x, int y, int w, int h, const char *txt, lv_event_cb_t cb, void *ud)
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

static lv_obj_t *mk_textarea(lv_obj_t *parent, int x, int y, int w, uint32_t max_len, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, w, 36);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    if (password) {
        lv_textarea_set_password_mode(ta, true);
        lv_textarea_set_password_show_time(ta, 0); /* never reveal the last character */
    }
    lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

/* Full-screen login and first-boot screen. Created last, so it covers the
 * rest of the UI and takes every touch while it is shown. */
static void build_overlay(lv_obj_t *scr)
{
    s_ov = lv_obj_create(scr);
    lv_obj_set_size(s_ov, 800, 480);
    lv_obj_set_pos(s_ov, 0, 0);
    lv_obj_set_style_bg_color(s_ov, C_BG, 0);
    lv_obj_set_style_border_width(s_ov, 0, 0);
    lv_obj_set_style_radius(s_ov, 0, 0);
    lv_obj_set_style_pad_all(s_ov, 0, 0);
    lv_obj_remove_flag(s_ov, LV_OBJ_FLAG_SCROLLABLE);

    s_ov_title = mk_label(s_ov, 20, 12, "LOG IN", C_TEXT);
    mk_label(s_ov, 20, 42, "USER NAME", C_MUTED);
    s_ov_user = mk_textarea(s_ov, 20, 62, 360, HMI_USER_NAME_MAX, false);
    lv_textarea_set_accepted_chars(s_ov_user, "abcdefghijklmnopqrstuvwxyz0123456789_.-");
    mk_label(s_ov, 20, 108, "PASSWORD", C_MUTED);
    s_ov_pw = mk_textarea(s_ov, 20, 128, 360, HMI_PASSWORD_MAX, true);
    s_ov_conf_cap = mk_label(s_ov, 20, 174, "CONFIRM PASSWORD", C_MUTED);
    s_ov_conf = mk_textarea(s_ov, 20, 194, 360, HMI_PASSWORD_MAX, true);
    mk_button(s_ov, 420, 126, 150, 40, "OK", ov_ok_cb, NULL);
    mk_button(s_ov, 420, 176, 150, 40, "CANCEL", ov_cancel_cb, NULL);
    s_ov_status = mk_label(s_ov, 20, 240, "", C_TEXT);
    lv_label_set_long_mode(s_ov_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ov_status, 760);

    s_ov_kb = lv_keyboard_create(s_ov);
    lv_obj_set_size(s_ov_kb, 800, 204);
    lv_obj_align(s_ov_kb, LV_ALIGN_BOTTOM_MID, 0, 0); /* the widget is created bottom aligned; set_pos would offset that */
    lv_obj_add_event_cb(s_ov_kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_ov_kb, kb_cb, LV_EVENT_CANCEL, NULL);

    ov_show_confirm(false);
    lv_obj_add_flag(s_ov, LV_OBJ_FLAG_HIDDEN);
}

static void ui_build(hmi_runtime_t *rt, const char *banner)
{
    memset(s_rows, 0, sizeof(s_rows));
    s_n_rows = 0;
    memset(t_link, 0, sizeof(t_link));
    memset(t_alarm_sum, 0, sizeof(t_alarm_sum));
    memset(t_wr, 0, sizeof(t_wr));
    memset(t_sp, 0, sizeof(t_sp));
    memset(t_journal, 0, sizeof(t_journal));
    memset(t_hb, 0, sizeof(t_hb));
    memset(t_ack, 0, sizeof(t_ack));
    memset(t_role, 0, sizeof(t_role));
    memset(t_ov_title, 0, sizeof(t_ov_title));
    memset(t_ov_status, 0, sizeof(t_ov_status));
    s_ack_status = NULL;
    s_ack_btn = s_role_lbl = s_login_btn_lbl = NULL;
    s_ov = s_ov_title = s_ov_user = s_ov_pw = s_ov_conf = s_ov_conf_cap = s_ov_status = s_ov_kb = NULL;
    s_ov_mode = OV_HIDDEN;
    s_ov_pending = s_ov_countdown = false;
    s_last_role = -1;
    s_rt = rt;
    s_sp_tag = -1;
    s_sp_inited = false;
    s_last_write_id = 0;
    s_last_gv = 0xFFFFFFFFu;
    c_link = c_alarm_sum = c_wr = c_role = c_ov_status = 0;
    memset(&s_stats, 0, sizeof(s_stats));
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
    mk_label(hdr, 8, 12, "PROMETHEUS99", C_TEXT);
    s_link = mk_label(hdr, 130, 12, "PLC: --", C_MUTED);
    s_hb = mk_label(hdr, 272, 12, "", C_MUTED);
    s_alarm_sum = mk_label(hdr, 326, 12, "ALARMS: --", C_MUTED);
    s_role_lbl = mk_label(hdr, 512, 12, "", C_MUTED);
    lv_label_set_long_mode(s_role_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_role_lbl, 132);
    s_ack_btn = mk_button(hdr, 650, 2, 74, 38, "ACK ALL", ack_all_cb, NULL);
    if (s_auth) {
        lv_obj_t *lb = mk_button(hdr, 728, 2, 68, 38, "LOGIN", login_btn_cb, NULL);
        s_login_btn_lbl = lv_obj_get_child(lb, 0);
    }

    if (!rt) {
        lv_obj_t *b = mk_label(scr, 20, 80, banner ? banner : "NO CONFIGURATION", C_BAD);
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(b, 760);
        mk_label(scr, 20, 140, "No PLC endpoint selected. Writes disabled.", C_MUTED);
    } else {
        /* column headers */
        mk_label(scr, 12, 50, "TAG", C_MUTED);
        mk_label(scr, 220, 50, "VALUE", C_MUTED);
        mk_label(scr, 420, 50, "QUALITY", C_MUTED);
        mk_label(scr, 580, 50, "ALARM", C_MUTED);

        s_n_rows = hmi_tagdb_count(&rt->db) < MAX_ROWS ? hmi_tagdb_count(&rt->db) : MAX_ROWS;
        for (size_t i = 0; i < s_n_rows; i++) {
            int y = 72 + (int)i * ROW_H;
            row_t *r = &s_rows[i];
            r->name = mk_label(scr, 12, y, hmi_tagdb_cfg(&rt->db, i)->name, C_TEXT);
            r->value = mk_label(scr, 220, y, "---", C_MUTED);
            r->quality = mk_label(scr, 420, y, "NO DATA", C_MUTED);
            r->alarm = mk_label(scr, 580, y, "", C_MUTED);
            snprintf(r->t_value, sizeof(r->t_value), "---");
            snprintf(r->t_quality, sizeof(r->t_quality), "NO DATA");
            r->t_alarm[0] = '\0';
            if (s_sp_tag < 0 && hmi_tagdb_cfg(&rt->db, i)->write_allowed) s_sp_tag = (int)i;
        }

        /* bottom: setpoint + journal */
        lv_obj_t *pnl = lv_obj_create(scr);
        lv_obj_set_size(pnl, 800, 100);
        lv_obj_set_pos(pnl, 0, 380);
        lv_obj_set_style_bg_color(pnl, C_PANEL, 0);
        lv_obj_set_style_border_width(pnl, 0, 0);
        lv_obj_set_style_radius(pnl, 0, 0);
        lv_obj_set_style_pad_all(pnl, 0, 0);
        lv_obj_remove_flag(pnl, LV_OBJ_FLAG_SCROLLABLE);
        if (s_sp_tag >= 0 && rt->writes_effective) {
            s_sp_label = mk_label(pnl, 8, 6, "SETPOINT", C_TEXT);
            mk_button(pnl, 8, 32, 60, 40, "-", sp_step_cb, (void *)(intptr_t)-1);
            mk_button(pnl, 76, 32, 60, 40, "+", sp_step_cb, (void *)(intptr_t)1);
            mk_button(pnl, 144, 32, 110, 40, "SEND", sp_send_cb, NULL);
            s_wr_status = mk_label(pnl, 270, 44, "", C_MUTED);
        } else {
            s_sp_tag = -1;
            s_sp_label = mk_label(pnl, 8, 6, "WRITES DISABLED", C_MUTED);
            s_wr_status = mk_label(pnl, 8, 40, "", C_MUTED);
        }
        s_journal = mk_label(pnl, 560, 4, "no alarm events", C_MUTED);
        s_ack_status = mk_label(pnl, 270, 6, "", C_MUTED);
        if (!s_auth) { /* test builds: the claimed role must never look like a login */
            char tb[TXT * 2];
            hmi_view_test_role_banner(s_fixed_role, tb, sizeof(tb));
            mk_label(pnl, 8, 80, tb, C_BAD);
        }
    }

    if (s_auth) build_overlay(scr);
    refresh_role_ui(true);
    s_timer = lv_timer_create(binder_cb, 50, NULL);
}

void hmi_ui_init(hmi_runtime_t *rt, hmi_role_t role, const char *banner)
{
    s_auth = NULL;
    s_fixed_role = role;
    ui_build(rt, banner);
}

void hmi_ui_init_auth(hmi_runtime_t *rt, hmi_auth_t *auth, const char *banner)
{
    s_auth = auth;
    s_fixed_role = HMI_ROLE_VIEWER;
    ui_build(rt, banner);
}
