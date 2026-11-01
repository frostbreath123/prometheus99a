#include "ui_priv.h"
#include "../hmi/hmi_auditfmt.h"
#include "../hmi/hmi_authz.h"
#include <math.h>
#include <stdlib.h>

/* The CONFIG view: an Engineer or Administrator picks the PLC or a tag, edits alarm limits,
 * deadband, delays, priority, or the PLC address, and sends ONE request for all the edits.
 * Nothing here changes the configuration: the request goes to the spool directory and the
 * root helper p99_cfg decides (hmi_cfgedit.h). Before the request leaves, the same rules are
 * run here so a bad value is shown at once, and the second press of APPLY is the confirmation. */

#define C_ROWS 8
#define C_ROW_H 30
#define C_ROW_Y0 40
#define APPLY_ARM_MS 5000u
#define KEY_MAX 15

typedef enum { K_LIMIT, K_NUMBER, K_PRIORITY, K_HOST } rowkind_t;
typedef struct {
    const char *label;
    hmi_ce_field_t field;
    rowkind_t kind;
} rowdef_t;

static const rowdef_t PLC_ROWS[] = {
    { "PLC HOST", HMI_CE_PLC_HOST, K_HOST }, { "PLC PORT", HMI_CE_PLC_PORT, K_NUMBER }, { "MODBUS UNIT", HMI_CE_PLC_UNIT, K_NUMBER },
};
static const rowdef_t TAG_ROWS[C_ROWS] = {
    { "HIGH-HIGH", HMI_CE_ALARM_HIHI, K_LIMIT }, { "HIGH", HMI_CE_ALARM_HI, K_LIMIT }, { "LOW", HMI_CE_ALARM_LO, K_LIMIT },
    { "LOW-LOW", HMI_CE_ALARM_LOLO, K_LIMIT }, { "DEADBAND", HMI_CE_ALARM_DEADBAND, K_NUMBER },
    { "ON DELAY ms", HMI_CE_ALARM_ON_MS, K_NUMBER }, { "OFF DELAY ms", HMI_CE_ALARM_OFF_MS, K_NUMBER },
    { "PRIORITY", HMI_CE_ALARM_PRIORITY, K_PRIORITY },
};

typedef struct {
    double num;
    bool off;
    char host[HMI_HOST_LEN];
} val_t;

typedef struct {
    hmi_ce_field_t field;
    char tag[HMI_NAME_LEN];            /* empty for the PLC fields */
    val_t v;
} pend_t;

typedef struct {
    lv_obj_t *row, *l_label, *l_cur, *l_pend;
    char t_label[24], t_cur[24], t_pend[32];
} crow_t;

/* Set by the program before or after the UI is built; survives a rebuild. */
static char s_spool[256];
static uint32_t s_base_serial;

static crow_t s_rows[C_ROWS];
static lv_obj_t *s_main, *s_msg, *s_rowsc, *s_kpc, *s_t_name, *s_pend_lbl, *s_status, *s_result, *s_kp_lbl;
static lv_obj_t *s_btn[2], *s_key[16];
static char t_msg[UI_TXT * 4], t_tname[UI_TXT], t_pend[24], t_status[UI_TXT * 4], t_result[UI_TXT * 5], t_kp[48];
static uint32_t c_status, c_result;
static pend_t s_pend[HMI_CFGEDIT_MAX_CHANGES];
static size_t s_npend, s_target, s_nrows_shown;
static bool s_kp_open, s_armed, s_shown, s_en[2];
static size_t s_kp_row;
static char s_kp_text[KEY_MAX + 1];
static uint64_t s_armed_at, s_poll_at;

static const char *const KEYS[16] = { "1", "2", "3", "4", "5", "DEL", "6", "7", "8", "9", "0", ".", "-", "OFF", "OK", "CANCEL" };

/* Buffers for the local check; the request can hold as many changes as the helper accepts. */
static hmi_app_cfg_t s_chk;
static char s_chk_text[HMI_CONFIG_MAX_BYTES + 1];

void ui_config_set_edit(const char *spool_dir, uint32_t base_serial)
{
    snprintf(s_spool, sizeof(s_spool), "%s", spool_dir ? spool_dir : "");
    s_base_serial = base_serial;
}

static void config_reset(void)
{
    memset(s_rows, 0, sizeof(s_rows));
    s_main = s_msg = s_rowsc = s_kpc = s_t_name = s_pend_lbl = s_status = s_result = s_kp_lbl = NULL;
    memset(s_btn, 0, sizeof(s_btn));
    memset(s_key, 0, sizeof(s_key));
    memset(t_msg, 0, sizeof(t_msg));
    memset(t_tname, 0, sizeof(t_tname));
    memset(t_pend, 0, sizeof(t_pend));
    memset(t_status, 0, sizeof(t_status));
    memset(t_result, 0, sizeof(t_result));
    memset(t_kp, 0, sizeof(t_kp));
    c_status = c_result = 0;
    memset(s_pend, 0, sizeof(s_pend));
    s_npend = 0;
    s_target = 0;
    s_nrows_shown = 0;
    s_kp_open = false;
    s_armed = false;
    s_shown = false;
    s_en[0] = s_en[1] = true;
    s_kp_row = 0;
    s_kp_text[0] = '\0';
    s_armed_at = 0;
    s_poll_at = 0;
}

/* ---- what the targets and rows are ---- */
static size_t target_count(void) { return ui_rt ? 1u + ui_rt->cfg.n_tags : 0; }

static const char *target_tag(size_t target)
{
    return (ui_rt && target >= 1 && target - 1 < ui_rt->cfg.n_tags) ? ui_rt->cfg.tags[target - 1].name : "";
}

static size_t row_count(size_t target) { return target == 0 ? sizeof(PLC_ROWS) / sizeof(PLC_ROWS[0]) : (size_t)C_ROWS; }
static const rowdef_t *row_def(size_t target, size_t row)
{
    return row < row_count(target) ? (target == 0 ? &PLC_ROWS[row] : &TAG_ROWS[row]) : NULL;
}

static bool cur_val(size_t target, const rowdef_t *d, val_t *v)
{
    memset(v, 0, sizeof(*v));
    if (!ui_rt || !d) return false;
    const hmi_app_cfg_t *c = &ui_rt->cfg;
    if (target == 0) {
        if (d->field == HMI_CE_PLC_HOST) snprintf(v->host, sizeof(v->host), "%s", c->host);
        else if (d->field == HMI_CE_PLC_PORT) v->num = c->port;
        else if (d->field == HMI_CE_PLC_UNIT) v->num = c->unit;
        else return false;
        return true;
    }
    if (target - 1 >= c->n_tags) return false;
    const hmi_alarm_cfg_t *a = &c->tags[target - 1].alarm;
    switch (d->field) {
        case HMI_CE_ALARM_HI: v->off = !a->en_hi; v->num = a->hi; break;
        case HMI_CE_ALARM_HIHI: v->off = !a->en_hihi; v->num = a->hihi; break;
        case HMI_CE_ALARM_LO: v->off = !a->en_lo; v->num = a->lo; break;
        case HMI_CE_ALARM_LOLO: v->off = !a->en_lolo; v->num = a->lolo; break;
        case HMI_CE_ALARM_DEADBAND: v->num = a->deadband; break;
        case HMI_CE_ALARM_ON_MS: v->num = a->on_ms; break;
        case HMI_CE_ALARM_OFF_MS: v->num = a->off_ms; break;
        case HMI_CE_ALARM_PRIORITY: v->num = a->priority; break;
        default: return false;
    }
    return true;
}

static bool val_same(const val_t *a, const val_t *b)
{
    return a->off == b->off && (a->off || a->num == b->num) && strcmp(a->host, b->host) == 0;
}

static void val_text(const rowdef_t *d, const val_t *v, char *buf, size_t cap)
{
    if (d->kind == K_HOST) snprintf(buf, cap, "%s", v->host);
    else if (d->kind == K_PRIORITY) snprintf(buf, cap, "%s", hmi_view_priority_caps((hmi_priority_t)(int)v->num));
    else if (v->off) snprintf(buf, cap, "off");
    else hmi_view_cfg_number_text(v->num, buf, cap);
}

/* ---- the edits waiting to be sent ---- */
static pend_t *pend_find(hmi_ce_field_t field, const char *tag)
{
    for (size_t i = 0; i < s_npend; i++)
        if (s_pend[i].field == field && strcmp(s_pend[i].tag, tag) == 0) return &s_pend[i];
    return NULL;
}

static void pend_remove(pend_t *p)
{
    size_t i = (size_t)(p - s_pend);
    memmove(&s_pend[i], &s_pend[i + 1], (s_npend - i - 1) * sizeof(s_pend[0]));
    s_npend--;
}

/* The value shown for a row: the edit if there is one, otherwise the configuration. */
static bool eff_val(size_t target, const rowdef_t *d, val_t *v)
{
    pend_t *p = pend_find(d->field, target_tag(target));
    if (p) {
        *v = p->v;
        return true;
    }
    return cur_val(target, d, v);
}

/* An edit that equals the configuration is no edit. False if there is no room. */
static bool pend_set(size_t target, const rowdef_t *d, const val_t *v)
{
    val_t cur;
    if (!cur_val(target, d, &cur)) return false;
    pend_t *p = pend_find(d->field, target_tag(target));
    s_armed = false;
    if (val_same(&cur, v)) {
        if (p) pend_remove(p);
        return true;
    }
    if (!p) {
        if (s_npend >= HMI_CFGEDIT_MAX_CHANGES) return false;
        p = &s_pend[s_npend++];
        memset(p, 0, sizeof(*p));
        p->field = d->field;
        snprintf(p->tag, sizeof(p->tag), "%s", target_tag(target));
    }
    p->v = *v;
    return true;
}

/* ---- text and state seen from outside ---- */
size_t ui_config_targets(void) { return target_count(); }
size_t ui_config_target(void) { return s_target; }
const char *ui_config_target_text(void) { return t_tname; }
size_t ui_config_rows(void) { return s_nrows_shown; }
const char *ui_config_row_text(size_t row, int column)
{
    if (row >= C_ROWS || row >= s_nrows_shown) return "";
    return column == 0 ? s_rows[row].t_label : column == 1 ? s_rows[row].t_cur : s_rows[row].t_pend;
}
bool ui_config_keypad_open(void) { return s_kp_open; }
const char *ui_config_keypad_text(void) { return s_kp_text; }
const char *ui_config_status_text(void) { return t_status; }
const char *ui_config_result_text(void) { return t_result; }
const char *ui_config_message_text(void) { return t_msg; }
size_t ui_config_pending(void) { return s_npend; }
bool ui_config_button_enabled(ui_config_btn_t b) { return (unsigned)b < 2u && s_en[b]; }

static void say(const char *txt, hmi_tone_t tone)
{
    ui_set_text(s_status, t_status, sizeof(t_status), txt);
    ui_set_color(s_status, &c_status, ui_tone_color(tone));
}

/* ---- target selector ---- */
void ui_config_press_target(int dir)
{
    size_t n = target_count();
    if (n < 2) return;
    s_target = dir > 0 ? (s_target + 1) % n : (s_target + n - 1) % n;
    s_kp_open = false;
    s_armed = false;
}

static void target_cb(lv_event_t *e) { ui_config_press_target((int)(intptr_t)lv_event_get_user_data(e)); }

/* ---- rows and the number pad ---- */
static bool needs_whole(hmi_ce_field_t f)
{
    return f == HMI_CE_PLC_PORT || f == HMI_CE_PLC_UNIT || f == HMI_CE_ALARM_ON_MS || f == HMI_CE_ALARM_OFF_MS;
}

void ui_config_press_row(size_t row)
{
    const rowdef_t *d = row_def(s_target, row);
    if (!d || !ui_rt || s_kp_open) return;
    val_t v;
    if (!eff_val(s_target, d, &v)) return;
    switch (d->kind) {
        case K_PRIORITY:
            v.num = (double)(((int)v.num + 1) % 4);
            if (!pend_set(s_target, d, &v)) say("Too many changes in one request", HMI_TONE_WARN);
            break;
        case K_HOST: {
            const hmi_app_cfg_t *c = &ui_rt->cfg;
            if (c->n_allowed == 0) { say("No allowed hosts are configured", HMI_TONE_WARN); break; }
            size_t at = c->n_allowed;
            for (size_t i = 0; i < c->n_allowed; i++)
                if (strcmp(c->allowed_hosts[i], v.host) == 0) at = i;
            snprintf(v.host, sizeof(v.host), "%s", c->allowed_hosts[(at + 1) % c->n_allowed]);   /* only hosts on the allow-list */
            if (!pend_set(s_target, d, &v)) say("Too many changes in one request", HMI_TONE_WARN);
            break;
        }
        default:
            s_kp_open = true;
            s_kp_row = row;
            s_kp_text[0] = '\0';
            say("", HMI_TONE_TEXT);
            break;
    }
}

static void row_cb(lv_event_t *e) { ui_config_press_row((size_t)(intptr_t)lv_event_get_user_data(e)); }

static bool parse_number(const char *s, double *out)
{
    if (!s[0]) return false;
    char *end = NULL;
    double v = strtod(s, &end);
    if (!end || *end != '\0' || !isfinite(v)) return false;
    *out = v;
    return true;
}

void ui_config_press_key(const char *key)
{
    if (!s_kp_open || !key) return;
    size_t n = strlen(s_kp_text);
    const rowdef_t *d = row_def(s_target, s_kp_row);
    if (strcmp(key, "CANCEL") == 0 || !d) {
        s_kp_open = false;
    } else if (strcmp(key, "DEL") == 0) {
        if (n) s_kp_text[n - 1] = '\0';
    } else if (strcmp(key, "OFF") == 0) {
        if (d->kind != K_LIMIT) return;
        val_t v;
        memset(&v, 0, sizeof(v));
        v.off = true;
        if (pend_set(s_target, d, &v)) s_kp_open = false;
        else say("Too many changes in one request", HMI_TONE_WARN);
    } else if (strcmp(key, "OK") == 0) {
        double x;
        if (!parse_number(s_kp_text, &x)) {
            say("Type a number first", HMI_TONE_WARN);
        } else if (needs_whole(d->field) && x != floor(x)) {
            say("A whole number is needed here", HMI_TONE_WARN);
        } else {
            val_t v;
            memset(&v, 0, sizeof(v));
            v.num = x;
            if (pend_set(s_target, d, &v)) s_kp_open = false;
            else say("Too many changes in one request", HMI_TONE_WARN);
        }
    } else if (strlen(key) == 1 && n < KEY_MAX) {
        char k = key[0];
        if (k >= '0' && k <= '9') { s_kp_text[n] = k; s_kp_text[n + 1] = '\0'; }
        else if (k == '-' && n == 0) { s_kp_text[0] = '-'; s_kp_text[1] = '\0'; }
        else if (k == '.' && strchr(s_kp_text, '.') == NULL) { s_kp_text[n] = '.'; s_kp_text[n + 1] = '\0'; }
    }
}

static void key_cb(lv_event_t *e) { ui_config_press_key(KEYS[(size_t)(intptr_t)lv_event_get_user_data(e)]); }

/* ---- APPLY and DISCARD ---- */
static bool audit_cb(void *user, const char *line, size_t len)
{
    (void)user;
    char b[256];
    if (!ui_rt || len + HMI_WALL_STAMP_MAX >= sizeof(b)) return false;
    memcpy(b, line, len);
    hmi_wall_t w;
    hmi_wall_now(&w);
    size_t n = hmi_audit_stamp_line(b, len, sizeof(b), &w);
    return n != 0 && hmi_runtime_audit_append(ui_rt, b, n);
}

static void build_request(hmi_cfgedit_request_t *r, uint64_t now)
{
    hmi_cfgedit_init(r, s_base_serial, ui_current_actor(now));
    for (size_t i = 0; i < s_npend; i++) {
        const pend_t *p = &s_pend[i];
        if (p->field == HMI_CE_PLC_HOST) hmi_cfgedit_set_host(r, p->v.host);
        else if (p->field == HMI_CE_ALARM_PRIORITY) hmi_cfgedit_set_priority(r, p->tag, (hmi_priority_t)(int)p->v.num);
        else if (p->v.off) hmi_cfgedit_set_limit_off(r, p->field, p->tag);
        else hmi_cfgedit_set_number(r, p->field, p->tag, p->v.num);
    }
}

void ui_config_press(ui_config_btn_t button)
{
    if (!ui_rt) return;
    uint64_t now = hmi_now_ms();
    if (button == UI_CONFIG_BTN_DISCARD) {
        s_npend = 0;
        s_armed = false;
        s_kp_open = false;
        say("Changes discarded", HMI_TONE_TEXT);
        return;
    }
    if (button != UI_CONFIG_BTN_APPLY) return;
    char b[UI_TXT * 4];
    if (s_npend == 0) {
        s_armed = false;
        hmi_view_cfgreq_text(HMI_CFGREQ_EMPTY, b, sizeof(b));
        say(b, HMI_TONE_WARN);
        return;
    }
    static hmi_cfgedit_request_t req;
    build_request(&req, now);
    if (!s_armed || now - s_armed_at > APPLY_ARM_MS) {            /* the first press checks and asks */
        char err[120];
        size_t len = 0, eff = 0;
        if (!hmi_cfgedit_apply(&req, &ui_rt->cfg, &s_chk, s_chk_text, sizeof(s_chk_text), &len, &eff, err, sizeof(err))) {
            s_armed = false;
            snprintf(b, sizeof(b), "Refused: %s", err);
            say(b, HMI_TONE_BAD);
            return;
        }
        s_armed = true;
        s_armed_at = now;
        snprintf(b, sizeof(b), "Press APPLY again to send %u change%s", (unsigned)eff, eff == 1 ? "" : "s");
        say(b, HMI_TONE_WARN);
        return;
    }
    s_armed = false;
    hmi_cfgreq_result_t r = hmi_cfgreq_submit(&req, ui_current_role(now), s_spool, audit_cb, NULL, now);
    hmi_view_cfgreq_text(r, b, sizeof(b));
    say(b, hmi_view_cfgreq_tone(r));
    if (r == HMI_CFGREQ_OK) {
        s_npend = 0;
        s_poll_at = 0;                                            /* look at the spool directory at the next update */
    }
}

static void btn_cb(lv_event_t *e) { ui_config_press((ui_config_btn_t)(intptr_t)lv_event_get_user_data(e)); }

/* ---- the view ---- */
static void set_shown(bool on)
{
    if (on == s_shown) return;
    s_shown = on;
    if (on) {
        lv_obj_remove_flag(s_main, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_main, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *mk_container(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static void config_build(lv_obj_t *area)
{
    s_main = mk_container(area, 0, 0, UI_SUBPAGE_W, UI_PAGE_H);
    ui_mk_button(s_main, 4, 0, 44, 32, "<", target_cb, (void *)(intptr_t)-1);
    s_t_name = ui_mk_label(s_main, 56, 6, "", C_TEXT);
    lv_label_set_long_mode(s_t_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_t_name, 380);
    ui_mk_button(s_main, 442, 0, 44, 32, ">", target_cb, (void *)(intptr_t)1);
    s_pend_lbl = ui_mk_label(s_main, 496, 6, "", C_WARN);

    s_rowsc = mk_container(s_main, 0, C_ROW_Y0, UI_SUBPAGE_W, C_ROWS * C_ROW_H);
    for (size_t i = 0; i < C_ROWS; i++) {
        crow_t *r = &s_rows[i];
        r->row = lv_button_create(s_rowsc);
        lv_obj_set_pos(r->row, 4, (int)i * C_ROW_H);
        lv_obj_set_size(r->row, 612, C_ROW_H - 2);
        lv_obj_set_style_bg_color(r->row, C_PANEL, 0);
        lv_obj_set_style_radius(r->row, 0, 0);
        lv_obj_set_style_shadow_width(r->row, 0, 0);
        lv_obj_set_style_pad_all(r->row, 0, 0);
        lv_obj_add_event_cb(r->row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        r->l_label = ui_mk_label(r->row, 6, 4, "", C_TEXT);
        r->l_cur = ui_mk_label(r->row, 230, 4, "", C_TEXT);
        r->l_pend = ui_mk_label(r->row, 400, 4, "", C_WARN);
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }

    s_kpc = mk_container(s_main, 0, C_ROW_Y0, UI_SUBPAGE_W, 250);
    s_kp_lbl = ui_mk_label(s_kpc, 8, 4, "", C_TEXT);
    lv_label_set_long_mode(s_kp_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_kp_lbl, UI_SUBPAGE_W - 16);
    for (size_t i = 0; i < 16; i++) {
        int cx = 4 + (int)(i % 6) * 102, cy = 44 + (int)(i / 6) * 54;
        int w = strlen(KEYS[i]) > 3 ? 150 : 96;
        s_key[i] = ui_mk_button(s_kpc, cx, cy, w, 46, KEYS[i], key_cb, (void *)(intptr_t)i);
    }
    lv_obj_add_flag(s_kpc, LV_OBJ_FLAG_HIDDEN);

    s_status = ui_mk_label(s_main, 8, 290, "", C_TEXT);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, UI_SUBPAGE_W - 16);
    s_result = ui_mk_label(s_main, 8, 320, "", C_MUTED);
    lv_label_set_long_mode(s_result, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_result, UI_SUBPAGE_W - 16);
    s_btn[UI_CONFIG_BTN_APPLY] = ui_mk_button(s_main, 4, 352, 200, 44, "APPLY", btn_cb, (void *)(intptr_t)UI_CONFIG_BTN_APPLY);
    s_btn[UI_CONFIG_BTN_DISCARD] = ui_mk_button(s_main, 212, 352, 160, 44, "DISCARD", btn_cb, (void *)(intptr_t)UI_CONFIG_BTN_DISCARD);

    s_msg = ui_mk_label(area, 8, 60, "", C_MUTED);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_msg, UI_SUBPAGE_W - 16);
    s_shown = true;       /* force the first set_shown to act */
    set_shown(false);
}

static void enable(int b, bool on)
{
    if (s_en[b] == on && s_btn[b]) return;
    s_en[b] = on;
    if (on) lv_obj_remove_state(s_btn[b], LV_STATE_DISABLED);
    else lv_obj_add_state(s_btn[b], LV_STATE_DISABLED);
}

static void config_update(void)
{
    uint64_t now = hmi_now_ms();
    bool may = hmi_authz_allows(ui_current_role(now), HMI_ACT_EDIT_SETTINGS, HMI_ROLE_VIEWER);
    if (!s_spool[0] || !may) {
        if (s_npend || s_kp_open) {                 /* a session that ended leaves nothing half done */
            s_npend = 0;
            s_kp_open = false;
            s_armed = false;
            ui_set_text(s_status, t_status, sizeof(t_status), "");
        }
        set_shown(false);
        ui_set_text(s_msg, t_msg, sizeof(t_msg),
                    !s_spool[0] ? hmi_view_config_unavailable_text() : hmi_view_config_need_engineer_text());
        s_nrows_shown = 0;
        return;
    }
    set_shown(true);
    t_msg[0] = '\0';
    if (s_armed && now - s_armed_at > APPLY_ARM_MS) s_armed = false;

    size_t nt = target_count();
    if (s_target >= nt) s_target = 0;
    char b[UI_TXT * 3];
    if (s_target == 0) snprintf(b, sizeof(b), "PLC connection");
    else snprintf(b, sizeof(b), "TAG %s  (%u/%u)", target_tag(s_target), (unsigned)s_target, (unsigned)(nt - 1));
    ui_set_text(s_t_name, t_tname, sizeof(t_tname), b);
    snprintf(b, sizeof(b), s_npend ? "%u waiting" : "", (unsigned)s_npend);
    ui_set_text(s_pend_lbl, t_pend, sizeof(t_pend), b);

    size_t nr = row_count(s_target);
    s_nrows_shown = nr;
    if (s_kp_open) lv_obj_add_flag(s_rowsc, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_rowsc, LV_OBJ_FLAG_HIDDEN);
    if (s_kp_open) lv_obj_remove_flag(s_kpc, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_kpc, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < C_ROWS; i++) {
        crow_t *r = &s_rows[i];
        const rowdef_t *d = i < nr ? row_def(s_target, i) : NULL;
        val_t cur, eff;
        if (!d || !cur_val(s_target, d, &cur) || !eff_val(s_target, d, &eff)) {
            lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
            r->t_label[0] = r->t_cur[0] = r->t_pend[0] = '\0';
            continue;
        }
        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        ui_set_text(r->l_label, r->t_label, sizeof(r->t_label), d->label);
        val_text(d, &cur, b, sizeof(b));
        ui_set_text(r->l_cur, r->t_cur, sizeof(r->t_cur), b);
        if (val_same(&cur, &eff)) b[0] = '\0';
        else {
            char v[24];
            val_text(d, &eff, v, sizeof(v));
            snprintf(b, sizeof(b), "-> %s", v);
        }
        ui_set_text(r->l_pend, r->t_pend, sizeof(r->t_pend), b);
    }
    if (s_kp_open) {
        const rowdef_t *d = row_def(s_target, s_kp_row);
        snprintf(b, sizeof(b), "%s: %s_", d ? d->label : "", s_kp_text);
        ui_set_text(s_kp_lbl, t_kp, sizeof(t_kp), b);
        for (size_t i = 0; i < 16; i++)
            if (strcmp(KEYS[i], "OFF") == 0) {
                if (d && d->kind == K_LIMIT) lv_obj_remove_state(s_key[i], LV_STATE_DISABLED);
                else lv_obj_add_state(s_key[i], LV_STATE_DISABLED);
            }
    }
    enable(UI_CONFIG_BTN_APPLY, s_npend > 0 && !s_kp_open);
    enable(UI_CONFIG_BTN_DISCARD, s_npend > 0 || s_kp_open);

    /* what the helper did with the last request: looked at once a second */
    if (now - s_poll_at >= 1000u || s_poll_at == 0) {
        s_poll_at = now ? now : 1u;
        char line[160];
        hmi_cfgreq_state_t st = hmi_cfgreq_state(s_spool, line, sizeof(line));
        char txt[UI_TXT * 5];
        hmi_view_cfgstate_text(st, line, txt, sizeof(txt));
        char full[UI_TXT * 5 + 16];
        snprintf(full, sizeof(full), "%s%s", txt[0] ? "Last request: " : "", txt);
        ui_set_text(s_result, t_result, sizeof(t_result), full);
        ui_set_color(s_result, &c_result, ui_tone_color(hmi_view_cfgstate_tone(st)));
    }
}

const ui_screen_ops_t ui_sub_config = { "CONFIG", config_reset, config_build, config_update, true };
