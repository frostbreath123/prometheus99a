#include "ui_priv.h"

/* LOGIN and SETUP are the sign-in screens. ADD_USER and RESET_PASSWORD are the
 * same layer used by an administrator from the settings screen. */
typedef enum { OV_HIDDEN = 0, OV_LOGIN, OV_SETUP, OV_ADD_USER, OV_RESET_PASSWORD } ov_mode_t;

/* login screen */
static lv_obj_t *s_ov, *s_ov_title, *s_ov_user, *s_ov_pw, *s_ov_conf, *s_ov_conf_cap, *s_ov_status, *s_ov_kb;
static ov_mode_t s_ov_mode;
static bool s_ov_pending;            /* a hash is being computed; the screen cannot be left */
static bool s_ov_countdown;          /* a lock or gap is running for s_ov_name */
static char s_ov_name[HMI_USER_NAME_MAX + 1];
static char s_ov_target[HMI_USER_NAME_MAX + 1];   /* RESET_PASSWORD: the account whose password is set */
static hmi_role_t s_ov_new_role;                  /* ADD_USER: the role the account gets */
static char t_ov_title[UI_TXT * 2], t_ov_status[160];
static uint32_t c_ov_status;

/* ---- login screen ---- */
static void ov_status(const char *txt, hmi_tone_t tone)
{
    if (!s_ov_status) return;
    ui_set_text(s_ov_status, t_ov_status, sizeof(t_ov_status), txt);
    ui_set_color(s_ov_status, &c_ov_status, ui_tone_color(tone));
}

static void ov_login_status(hmi_login_result_t r, uint32_t wait_ms, const char *user)
{
    char b[160];
    hmi_view_login_text(r, wait_ms, user, b, sizeof(b));
    ov_status(b, hmi_view_login_tone(r));
}

static void ov_uadm_status(hmi_uadm_result_t r, hmi_view_user_action_t action)
{
    char b[160];
    hmi_view_uadm_text(r, action, ui_auth ? hmi_auth_setup_reason(ui_auth) : "", b, sizeof(b));
    ov_status(b, hmi_view_uadm_tone(r));
}

static void ov_setup_status(hmi_setup_result_t r)
{
    char b[160];
    hmi_view_setup_text(r, ui_auth ? hmi_auth_setup_reason(ui_auth) : "", b, sizeof(b));
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
    if (ta == s_ov_user && s_ov_mode == OV_RESET_PASSWORD) ta = s_ov_pw;   /* the name is fixed: type the password */
    for (int i = 0; i < 3; i++)
        if (all[i]) {
            if (all[i] == ta) lv_obj_add_state(all[i], LV_STATE_FOCUSED);
            else lv_obj_remove_state(all[i], LV_STATE_FOCUSED);
        }
    if (s_ov_kb && ta) lv_keyboard_set_textarea(s_ov_kb, ta);
}

/* The user name box can be edited, except when an account's password is set. */
static void ov_user_editable(bool on)
{
    if (!s_ov_user) return;
    if (on) lv_obj_remove_state(s_ov_user, LV_STATE_DISABLED);
    else lv_obj_add_state(s_ov_user, LV_STATE_DISABLED);
}

static void ov_open(void)
{
    if (!ui_auth || !s_ov || s_ov_mode != OV_HIDDEN) return;
    ta_wipe(s_ov_user);
    ta_wipe(s_ov_pw);
    ta_wipe(s_ov_conf);
    s_ov_countdown = false;
    s_ov_pending = false;
    ov_user_editable(true);
    switch (hmi_auth_store_state(ui_auth)) {
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

/* An administrator adds an account or sets a password from the settings screen.
 * The core refuses anyone who is not an administrator whatever this layer shows. */
static void ov_open_admin(ov_mode_t mode, const char *title)
{
    ta_wipe(s_ov_user);
    ta_wipe(s_ov_pw);
    ta_wipe(s_ov_conf);
    s_ov_countdown = false;
    s_ov_pending = false;
    s_ov_mode = mode;
    ov_set_title(title);
    ov_show_confirm(true);
    ov_status("", HMI_TONE_TEXT);
    ov_user_editable(mode == OV_ADD_USER);
    if (mode == OV_RESET_PASSWORD) lv_textarea_set_text(s_ov_user, s_ov_target);
    lv_obj_remove_flag(s_ov, LV_OBJ_FLAG_HIDDEN);
    ov_focus(mode == OV_ADD_USER ? s_ov_user : s_ov_pw);
}

bool ui_login_open_user_add(hmi_role_t role)
{
    if (!ui_auth || !s_ov || s_ov_mode != OV_HIDDEN) return false;
    char t[UI_TXT * 2];
    s_ov_new_role = role;
    snprintf(t, sizeof(t), "ADD %s ACCOUNT", hmi_view_role_caps(role));
    ov_open_admin(OV_ADD_USER, t);
    return true;
}

bool ui_login_open_password_reset(const char *name)
{
    if (!ui_auth || !s_ov || s_ov_mode != OV_HIDDEN || !name) return false;
    char t[UI_TXT * 2];
    snprintf(s_ov_target, sizeof(s_ov_target), "%s", name);
    snprintf(t, sizeof(t), "NEW PASSWORD FOR %s", name);
    ov_open_admin(OV_RESET_PASSWORD, t);
    return true;
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
    if (!ui_auth || s_ov_mode == OV_HIDDEN || s_ov_pending) return;
    uint64_t now = hmi_now_ms();
    const char *user = lv_textarea_get_text(s_ov_user);
    const char *pw = lv_textarea_get_text(s_ov_pw);

    if (s_ov_mode == OV_LOGIN) {
        hmi_login_info_t r = hmi_auth_login_begin(ui_auth, user, pw, now);
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
    if (s_ov_mode == OV_ADD_USER || s_ov_mode == OV_RESET_PASSWORD) {
        hmi_uadm_result_t ur = s_ov_mode == OV_ADD_USER
            ? hmi_auth_user_add_begin(ui_auth, user, s_ov_new_role, pw, now)
            : hmi_auth_user_password_begin(ui_auth, s_ov_target, pw, now);
        ta_wipe(s_ov_pw);
        ta_wipe(s_ov_conf);
        if (ur == HMI_UADM_PENDING) s_ov_pending = true;
        ov_uadm_status(ur, s_ov_mode == OV_ADD_USER ? HMI_VIEW_UA_ADD : HMI_VIEW_UA_PASSWORD);
        return;
    }
    snprintf(s_ov_name, sizeof(s_ov_name), "%s", user);
    hmi_setup_result_t sr = hmi_auth_setup_begin(ui_auth, user, pw, now);
    ta_wipe(s_ov_pw);
    ta_wipe(s_ov_conf);
    if (sr == HMI_SETUP_PENDING) s_ov_pending = true;
    ov_setup_status(sr);
}

/* Collects a finished hash job (UI thread). */
static void ov_poll(uint64_t now)
{
    if (!ui_auth || !hmi_auth_busy(ui_auth)) return;
    hmi_auth_done_t d;
    if (!hmi_auth_poll(ui_auth, now, &d)) return;
    s_ov_pending = false;
    if (d.kind == HMI_AUTH_JOB_LOGIN) {
        if (d.login.result == HMI_LOGIN_OK) {
            ov_close();
            ui_refresh_role(false);
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
    } else if (d.kind == HMI_AUTH_JOB_USER_ADD || d.kind == HMI_AUTH_JOB_USER_PASSWORD) {
        hmi_view_user_action_t act = d.kind == HMI_AUTH_JOB_USER_ADD ? HMI_VIEW_UA_ADD : HMI_VIEW_UA_PASSWORD;
        if (d.uadm == HMI_UADM_OK) {
            ov_close();
            ui_users_changed(act, d.uadm);
            return;
        }
        ov_uadm_status(d.uadm, act);   /* stays open: the administrator can correct it or cancel */
    }
}

/* Keeps the "try again in M:SS" text current while a lock or gap runs. */
static void ov_countdown(uint64_t now)
{
    if (!s_ov_countdown || s_ov_mode != OV_LOGIN || s_ov_pending || !ui_auth) return;
    uint32_t rem = hmi_auth_wait_ms(ui_auth, s_ov_name, now);
    if (rem == 0u) {
        s_ov_countdown = false;
        ov_status("", HMI_TONE_TEXT);
        return;
    }
    ov_login_status(rem > HMI_AUTH_GAP_MS ? HMI_LOGIN_LOCKED : HMI_LOGIN_WAIT, rem, NULL);
}

static void do_logout(void)
{
    if (!ui_auth) return;
    hmi_auth_logout(ui_auth, hmi_now_ms());
    ui_refresh_role(false);
}

void ui_login_reset(void)
{
    memset(t_ov_title, 0, sizeof(t_ov_title));
    memset(t_ov_status, 0, sizeof(t_ov_status));
    c_ov_status = 0;
    s_ov = s_ov_title = s_ov_user = s_ov_pw = s_ov_conf = s_ov_conf_cap = s_ov_status = s_ov_kb = NULL;
    s_ov_mode = OV_HIDDEN;
    s_ov_pending = s_ov_countdown = false;
    memset(s_ov_target, 0, sizeof(s_ov_target));
    s_ov_new_role = HMI_ROLE_OPERATOR;
}

/* Called by the binder: collects a finished hash and keeps a lock countdown current. */
void ui_login_tick(uint64_t now)
{
    ov_poll(now);
    ov_countdown(now);
}

void ui_login_button_cb(lv_event_t *e)
{
    (void)e;
    if (!ui_auth) return;
    if (hmi_auth_logged_in(ui_auth, hmi_now_ms())) do_logout();
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
void ui_login_build(lv_obj_t *scr)
{
    if (!ui_auth) return;
    s_ov = lv_obj_create(scr);
    lv_obj_set_size(s_ov, 800, 480);
    lv_obj_set_pos(s_ov, 0, 0);
    lv_obj_set_style_bg_color(s_ov, C_BG, 0);
    lv_obj_set_style_border_width(s_ov, 0, 0);
    lv_obj_set_style_radius(s_ov, 0, 0);
    lv_obj_set_style_pad_all(s_ov, 0, 0);
    lv_obj_remove_flag(s_ov, LV_OBJ_FLAG_SCROLLABLE);

    s_ov_title = ui_mk_label(s_ov, 20, 12, "LOG IN", C_TEXT);
    ui_mk_label(s_ov, 20, 42, "USER NAME", C_MUTED);
    s_ov_user = mk_textarea(s_ov, 20, 62, 360, HMI_USER_NAME_MAX, false);
    lv_textarea_set_accepted_chars(s_ov_user, "abcdefghijklmnopqrstuvwxyz0123456789_.-");
    ui_mk_label(s_ov, 20, 108, "PASSWORD", C_MUTED);
    s_ov_pw = mk_textarea(s_ov, 20, 128, 360, HMI_PASSWORD_MAX, true);
    s_ov_conf_cap = ui_mk_label(s_ov, 20, 174, "CONFIRM PASSWORD", C_MUTED);
    s_ov_conf = mk_textarea(s_ov, 20, 194, 360, HMI_PASSWORD_MAX, true);
    ui_mk_button(s_ov, 420, 126, 150, 40, "OK", ov_ok_cb, NULL);
    ui_mk_button(s_ov, 420, 176, 150, 40, "CANCEL", ov_cancel_cb, NULL);
    s_ov_status = ui_mk_label(s_ov, 20, 240, "", C_TEXT);
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

bool hmi_ui_login_visible(void) { return s_ov_mode != OV_HIDDEN; }
const char *hmi_ui_login_title_text(void) { return t_ov_title; }
const char *hmi_ui_login_status_text(void) { return t_ov_status; }

const char *hmi_ui_login_field_text(int field)
{
    lv_obj_t *ta = field == 0 ? s_ov_user : field == 1 ? s_ov_pw : field == 2 ? s_ov_conf : NULL;
    return ta ? lv_textarea_get_text(ta) : "";
}

void hmi_ui_press_login_button(void) { ui_login_button_cb(NULL); }

void hmi_ui_login_fill(const char *user, const char *pw, const char *confirm)
{
    if (s_ov_mode == OV_HIDDEN) return;
    if (user) lv_textarea_set_text(s_ov_user, user);
    if (pw) lv_textarea_set_text(s_ov_pw, pw);
    if (confirm) lv_textarea_set_text(s_ov_conf, confirm);
}

void hmi_ui_login_submit(void) { ov_submit(); }
void hmi_ui_login_cancel(void) { ov_cancel(); }
