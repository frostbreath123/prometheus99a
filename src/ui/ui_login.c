/* Login with a named user and a PIN (hmi_auth.h): the header's LOGIN / LOGOUT button, the login panel with one button
 * per user and a digit keypad, the lock and idle-logout handling and their audit records. Only active when the
 * configuration has users and the PIN key is known; otherwise the claimed --role is used as before. UI thread only. */
#include "ui_priv.h"
#include "../hmi/hmi_auth.h"
#include "../hmi/hmi_auditfmt.h"
#include "../hmi/hmi_util.h"

static hmi_auth_t s_auth;
static lv_obj_t *s_panel, *s_pin_lbl, *s_msg_lbl, *s_user_lbl;
static size_t s_sel;
static char s_pin[HMI_PIN_MAX + 1];
static char t_msg[UI_TXT * 2], t_pin[HMI_PIN_MAX + 8], t_user[UI_TXT];
static uint32_t c_msg;

/* Fixed-size strings, not pointers: a table of pointers costs one relocation each in the static PIE executable. */
static const char KEYS[12][4] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "DEL", "0", "OK" };

void ui_login_configure(const hmi_user_cfg_t *users, size_t n, const uint8_t *pin_key)
{
    hmi_auth_init(&s_auth, users, n, pin_key);
}

bool ui_login_enabled(void) { return hmi_auth_enabled(&s_auth); }
hmi_role_t ui_login_role(void) { return hmi_auth_role(&s_auth); }
const char *ui_login_user(void) { return hmi_auth_user(&s_auth); }

/* One audit record. Without an audit trail (no --audit) there is nothing to record and the answer is true; with one,
 * a record that cannot be made durable returns false and the caller refuses the login (fail closed, as for writes). */
static bool audit(const char *event, const char *user, const char *detail)
{
    if (!ui_rt || ui_rt->audit_path[0] == '\0') return true;
    char line[160];
    int n = snprintf(line, sizeof(line) - HMI_WALL_STAMP_MAX, "%llu %s user=%s %s\n",
                     (unsigned long long)hmi_now_ms(), event, user ? user : "-", detail);
    if (n <= 0) return false;
    hmi_wall_t w;
    hmi_wall_now(&w);
    size_t len = hmi_audit_stamp_line(line, (size_t)n, sizeof(line), &w);
    return len > 0 && hmi_runtime_audit_append(ui_rt, line, len);
}

static void set_msg(const char *txt, hmi_tone_t tone)
{
    if (!s_msg_lbl) return;
    ui_set_text(s_msg_lbl, t_msg, sizeof(t_msg), txt);
    ui_set_color(s_msg_lbl, &c_msg, ui_tone_color(tone));
}

static void show_pin(void)
{
    char stars[HMI_PIN_MAX + 8];
    size_t n = strlen(s_pin);
    snprintf(stars, sizeof(stars), "PIN: ");
    for (size_t i = 0; i < n; i++) stars[5 + i] = '*';
    stars[5 + n] = '\0';
    if (s_pin_lbl) ui_set_text(s_pin_lbl, t_pin, sizeof(t_pin), stars);
}

static void clear_pin(void)
{
    hmi_wipe(s_pin, sizeof(s_pin));
    show_pin();
}

static void close_panel(void)
{
    clear_pin();
    if (s_panel) lv_obj_delete(s_panel);
    s_panel = s_pin_lbl = s_msg_lbl = s_user_lbl = NULL;
    memset(t_msg, 0, sizeof(t_msg));
    memset(t_pin, 0, sizeof(t_pin));
    memset(t_user, 0, sizeof(t_user));
    c_msg = 0;
}

static void select_user(size_t i)
{
    if (i >= s_auth.n) return;
    s_sel = i;
    char b[UI_TXT];
    snprintf(b, sizeof(b), "USER: %s", s_auth.users[i].name);
    if (s_user_lbl) ui_set_text(s_user_lbl, t_user, sizeof(t_user), b);
}

static void attempt(void)
{
    uint64_t now = hmi_now_ms();
    const char *name = s_sel < s_auth.n ? s_auth.users[s_sel].name : NULL;
    hmi_auth_result_t r = hmi_auth_login(&s_auth, s_sel, s_pin, now);
    clear_pin();
    char b[UI_TXT * 2];
    switch (r) {
        case HMI_AUTH_OK: {
            char d[48];
            snprintf(d, sizeof(d), "role=%s", hmi_role_name(hmi_auth_role(&s_auth)));
            if (!audit("LOGIN", name, d)) {
                hmi_auth_logout(&s_auth);
                set_msg("LOGIN REFUSED: audit unavailable", HMI_TONE_BAD);
                break;
            }
            close_panel();
            break;
        }
        case HMI_AUTH_WRONG_PIN: {
            uint32_t lock = hmi_auth_locked_for(&s_auth, now);
            (void)audit("LOGIN_FAILED", name, "reason=wrong_pin");
            if (lock) {
                char d[32];
                snprintf(d, sizeof(d), "lock_s=%u", (unsigned)(lock / 1000u));
                (void)audit("LOGIN_LOCKED", name, d);
                snprintf(b, sizeof(b), "WRONG PIN. LOCKED FOR %u s", (unsigned)(lock / 1000u));
            } else {
                snprintf(b, sizeof(b), "WRONG PIN");
            }
            set_msg(b, HMI_TONE_BAD);
            break;
        }
        case HMI_AUTH_LOCKED:
            snprintf(b, sizeof(b), "LOCKED: wait %u s", (unsigned)((hmi_auth_locked_for(&s_auth, now) + 999u) / 1000u));
            set_msg(b, HMI_TONE_BAD);
            break;
        case HMI_AUTH_BAD_FORMAT: set_msg("PIN: 4 to 8 digits", HMI_TONE_WARN); break;
        case HMI_AUTH_NO_USER: set_msg("SELECT A USER", HMI_TONE_WARN); break;
        case HMI_AUTH_DISABLED: set_msg("LOGIN NOT AVAILABLE", HMI_TONE_BAD); break;
    }
    ui_refresh_role(true);
}

void ui_login_key(char k)
{
    if (!s_panel) return;
    size_t n = strlen(s_pin);
    if (k >= '0' && k <= '9') {
        if (n < HMI_PIN_MAX) {
            s_pin[n] = k;
            s_pin[n + 1] = '\0';
        }
        show_pin();
    } else if (k == 'D') {
        if (n) s_pin[n - 1] = '\0';
        show_pin();
    } else if (k == 'K') {
        attempt();
    } else if (k == 'C') {
        close_panel();
    }
}

static void key_cb(lv_event_t *e)
{
    const char *k = KEYS[(intptr_t)lv_event_get_user_data(e)];
    ui_login_key(strcmp(k, "DEL") == 0 ? 'D' : strcmp(k, "OK") == 0 ? 'K' : k[0]);
}
static void cancel_cb(lv_event_t *e) { (void)e; ui_login_key('C'); }
static void user_cb(lv_event_t *e) { select_user((size_t)(intptr_t)lv_event_get_user_data(e)); }

void ui_login_select(size_t user) { select_user(user); }
bool ui_login_open(void) { return s_panel != NULL; }
const char *ui_login_message(void) { return t_msg; }

/* The panel covers the page area below the header; it is built when opened and deleted when closed. */
static void open_panel(void)
{
    if (s_panel) return;
    lv_obj_t *p = ui_mk_panel(lv_screen_active(), 0, ui_y(44), ui_w, ui_h - ui_y(44));
    lv_obj_set_style_bg_color(p, C_BG, 0);
    s_panel = p;
    /* make ui-sizes checks this panel from 640 x 400 up: the title wraps inside its column, a long name wraps inside
     * its button, and the chosen name ends in dots before the PIN */
    ui_label_box(ui_mk_label(p, 20, 4, "LOG IN: choose your name, enter your PIN", C_TEXT), LV_LABEL_LONG_WRAP, 330);
    for (size_t i = 0; i < s_auth.n; i++) {
        lv_obj_t *b = ui_mk_button(p, 20 + (int)(i / 4) * 160, 50 + (int)(i % 4) * 66, 150, 56, s_auth.users[i].name,
                                   user_cb, (void *)(intptr_t)i);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        ui_label_box(l, LV_LABEL_LONG_WRAP, 142);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    s_user_lbl = ui_mk_label(p, 360, 12, "USER: -", C_MUTED);
    ui_label_box(s_user_lbl, LV_LABEL_LONG_DOT, 214);
    lv_obj_set_height(s_user_lbl, lv_font_get_line_height(ui_font()));
    s_pin_lbl = ui_mk_label(p, 580, 12, "PIN: ", C_TEXT);
    for (int k = 0; k < 12; k++)
        ui_mk_button(p, 360 + (k % 3) * 100, 50 + (k / 3) * 66, 92, 56, KEYS[k], key_cb, (void *)(intptr_t)k);
    ui_mk_button(p, 670, 50, 110, 56, "CANCEL", cancel_cb, NULL);
    s_msg_lbl = ui_mk_label(p, 360, 330, "", C_MUTED);
    ui_label_box(s_msg_lbl, LV_LABEL_LONG_WRAP, 420);
    s_sel = (size_t)-1;
    if (s_auth.n == 1) select_user(0);
    uint32_t lock = hmi_auth_locked_for(&s_auth, hmi_now_ms());
    if (lock) {
        char b[UI_TXT];
        snprintf(b, sizeof(b), "LOCKED: wait %u s", (unsigned)((lock + 999u) / 1000u));
        set_msg(b, HMI_TONE_BAD);
    }
}

/* The header button: LOGIN opens the panel, LOGOUT ends the session. */
void ui_login_button(void)
{
    if (!ui_login_enabled()) return;
    const char *who = hmi_auth_user(&s_auth);
    if (who) {
        (void)audit("LOGOUT", who, "reason=operator"); /* best effort: logging out is never refused */
        hmi_auth_logout(&s_auth);
        ui_refresh_role(true);
    } else if (s_panel) {
        close_panel();
    } else {
        open_panel();
    }
}

void ui_login_button_cb(lv_event_t *e)
{
    (void)e;
    ui_login_button();
}

/* Binder tick: input activity and the idle logout. LVGL counts the time since the last touch. */
void ui_login_tick(uint64_t now)
{
    if (!ui_login_enabled()) return;
    uint32_t inactive = lv_display_get_inactive_time(NULL);
    hmi_auth_input(&s_auth, now > inactive ? now - inactive : 0);
    const char *who = hmi_auth_user(&s_auth);
    char name[HMI_NAME_LEN];
    snprintf(name, sizeof(name), "%s", who ? who : "-");
    if (hmi_auth_idle_check(&s_auth, now)) {
        (void)audit("LOGOUT", name, "reason=idle");
        ui_refresh_role(true);
    }
}

/* A rebuild of the screen (resize) deletes every object: forget the panel without touching it. */
void ui_login_reset(void)
{
    hmi_wipe(s_pin, sizeof(s_pin));
    s_panel = s_pin_lbl = s_msg_lbl = s_user_lbl = NULL;
    memset(t_msg, 0, sizeof(t_msg));
    c_msg = 0;
}
