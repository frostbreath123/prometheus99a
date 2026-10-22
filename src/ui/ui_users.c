#include "ui_priv.h"

#define U_ROWS 7
#define U_ROW_H 30
#define U_ROW_Y0 28
#define REMOVE_ARM_MS 5000u        /* the second press of REMOVE must come within this time */

typedef struct {
    lv_obj_t *row, *name, *role, *state;
    char t_name[HMI_USER_NAME_MAX + 1], t_role[16], t_state[24];
    bool used;
    bool selected;
} urow_t;

static urow_t s_rows[U_ROWS];
static lv_obj_t *s_msg, *s_hdr[3], *s_pg_prev, *s_pg_next, *s_pg_lbl, *s_status, *s_hint;
static lv_obj_t *s_btn[5], *s_btn_lbl[5];
static char t_msg[UI_TXT * 2], t_status[UI_TXT * 3], t_pg[16], t_newrole[24], t_remove[16];
static uint32_t c_status;
static size_t s_page, s_pages, s_count;
static char s_sel[HMI_USER_NAME_MAX + 1];         /* the selected account, by name: the list can change under it */
static hmi_role_t s_new_role;
static bool s_remove_armed;
static uint64_t s_remove_at;
static bool s_shown;                              /* the list and buttons are visible (an administrator is logged in) */
static bool s_en[5];

static void users_reset(void)
{
    memset(s_rows, 0, sizeof(s_rows));
    s_msg = s_pg_prev = s_pg_next = s_pg_lbl = s_status = s_hint = NULL;
    for (int i = 0; i < 3; i++) s_hdr[i] = NULL;
    for (int i = 0; i < 5; i++) {
        s_btn[i] = s_btn_lbl[i] = NULL;
        s_en[i] = true;
    }
    memset(t_msg, 0, sizeof(t_msg));
    memset(t_status, 0, sizeof(t_status));
    memset(t_pg, 0, sizeof(t_pg));
    memset(t_newrole, 0, sizeof(t_newrole));
    memset(t_remove, 0, sizeof(t_remove));
    c_status = 0;
    s_page = 0;
    s_pages = 1;
    s_count = 0;
    s_sel[0] = '\0';
    s_new_role = HMI_ROLE_OPERATOR;
    s_remove_armed = false;
    s_remove_at = 0;
    s_shown = false;
}

size_t ui_users_rows(void)
{
    size_t n = 0;
    for (size_t i = 0; i < U_ROWS; i++)
        if (s_rows[i].used) n++;
    return n;
}

const char *ui_users_row_text(size_t row, int column)
{
    if (row >= U_ROWS || !s_rows[row].used) return "";
    return column == 0 ? s_rows[row].t_name : column == 1 ? s_rows[row].t_role : s_rows[row].t_state;
}

const char *ui_users_status_text(void) { return t_status; }
const char *ui_users_message_text(void) { return t_msg; }
const char *ui_users_selected(void) { return s_sel; }
hmi_role_t ui_users_new_role(void) { return s_new_role; }
size_t ui_users_page(void) { return s_page; }
size_t ui_users_pages(void) { return s_pages; }
bool ui_users_button_enabled(ui_users_btn_t b) { return (unsigned)b < 5u && s_en[b]; }

static void say(const char *txt, hmi_tone_t tone)
{
    ui_set_text(s_status, t_status, sizeof(t_status), txt);
    ui_set_color(s_status, &c_status, ui_tone_color(tone));
}

static void say_result(hmi_uadm_result_t r, hmi_view_user_action_t action)
{
    char b[UI_TXT * 3];
    hmi_view_uadm_text(r, action, ui_auth ? hmi_auth_setup_reason(ui_auth) : "", b, sizeof(b));
    say(b, hmi_view_uadm_tone(r));
}

void ui_users_changed(hmi_view_user_action_t action, hmi_uadm_result_t result)
{
    s_remove_armed = false;
    say_result(result, action);
}

void ui_users_select_row(size_t row)
{
    if (row >= U_ROWS || !s_rows[row].used) return;
    if (strcmp(s_sel, s_rows[row].t_name) != 0) s_remove_armed = false;
    snprintf(s_sel, sizeof(s_sel), "%s", s_rows[row].t_name);
}

static void row_cb(lv_event_t *e)
{
    ui_users_select_row((size_t)(intptr_t)lv_event_get_user_data(e));
}

void ui_users_press_page(int dir)
{
    if (s_pages < 2) return;
    s_page = dir > 0 ? (s_page + 1) % s_pages : (s_page + s_pages - 1) % s_pages;
}

static void page_cb(lv_event_t *e)
{
    ui_users_press_page((int)(intptr_t)lv_event_get_user_data(e));
}

static hmi_role_t next_role(hmi_role_t r)
{
    return r == HMI_ROLE_OPERATOR ? HMI_ROLE_ENGINEER : r == HMI_ROLE_ENGINEER ? HMI_ROLE_ADMIN : HMI_ROLE_OPERATOR;
}

/* The role of the selected account as the list shows it. */
static bool selected_info(hmi_user_info_t *out)
{
    if (!ui_auth || !s_sel[0]) return false;
    uint64_t now = hmi_now_ms();
    size_t n = hmi_auth_user_count(ui_auth, now);
    for (size_t i = 0; i < n; i++)
        if (hmi_auth_user_info(ui_auth, i, now, out) && strcmp(out->name, s_sel) == 0) return true;
    return false;
}

/* What the buttons do. The core decides everything (administrator role, own account, last
 * administrator, audit); this only shows the outcome. */
void ui_users_press(ui_users_btn_t button)
{
    if (!ui_auth) return;
    uint64_t now = hmi_now_ms();
    hmi_user_info_t u;
    switch (button) {
        case UI_USERS_BTN_ADD:
            s_remove_armed = false;
            if (!ui_login_open_user_add(s_new_role)) say("Close the other window first", HMI_TONE_WARN);
            break;
        case UI_USERS_BTN_NEW_ROLE:
            s_new_role = next_role(s_new_role);
            break;
        case UI_USERS_BTN_PASSWORD:
            s_remove_armed = false;
            if (!selected_info(&u)) { say("Select an account first", HMI_TONE_WARN); break; }
            if (!ui_login_open_password_reset(u.name)) say("Close the other window first", HMI_TONE_WARN);
            break;
        case UI_USERS_BTN_ROLE:
            s_remove_armed = false;
            if (!selected_info(&u)) { say("Select an account first", HMI_TONE_WARN); break; }
            say_result(hmi_auth_user_set_role(ui_auth, u.name, next_role(u.role), now), HMI_VIEW_UA_ROLE);
            break;
        case UI_USERS_BTN_REMOVE:
            if (!selected_info(&u)) { s_remove_armed = false; say("Select an account first", HMI_TONE_WARN); break; }
            if (!s_remove_armed || now - s_remove_at > REMOVE_ARM_MS) {   /* the first press only asks */
                char b[UI_TXT * 3];
                s_remove_armed = true;
                s_remove_at = now;
                snprintf(b, sizeof(b), "Press REMOVE again to remove %s", u.name);
                say(b, HMI_TONE_WARN);
                break;
            }
            s_remove_armed = false;
            say_result(hmi_auth_user_remove(ui_auth, u.name, now), HMI_VIEW_UA_REMOVE);
            s_sel[0] = '\0';
            break;
    }
}

static void btn_cb(lv_event_t *e)
{
    ui_users_press((ui_users_btn_t)(intptr_t)lv_event_get_user_data(e));
}

static void set_shown(bool on)
{
    if (on == s_shown) return;
    s_shown = on;
    lv_obj_t *all[] = { s_hdr[0], s_hdr[1], s_hdr[2], s_pg_prev, s_pg_next, s_pg_lbl, s_status, s_hint,
                        s_btn[0], s_btn[1], s_btn[2], s_btn[3], s_btn[4] };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (on) lv_obj_remove_flag(all[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (on) lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    if (!on) {
        for (size_t i = 0; i < U_ROWS; i++) {
            if (s_rows[i].used) lv_obj_add_flag(s_rows[i].row, LV_OBJ_FLAG_HIDDEN);
            s_rows[i].used = false;
            s_rows[i].selected = false;
            s_rows[i].t_name[0] = s_rows[i].t_role[0] = s_rows[i].t_state[0] = '\0';
        }
    }
}

static void users_build(lv_obj_t *area)
{
    s_hdr[0] = ui_mk_label(area, 8, 4, "ACCOUNT", C_MUTED);
    s_hdr[1] = ui_mk_label(area, 250, 4, "ROLE", C_MUTED);
    s_hdr[2] = ui_mk_label(area, 380, 4, "STATE", C_MUTED);
    s_pg_prev = ui_mk_button(area, 488, 0, 44, 26, "<", page_cb, (void *)(intptr_t)-1);
    s_pg_lbl = ui_mk_label(area, 538, 4, "1/1", C_TEXT);
    s_pg_next = ui_mk_button(area, 572, 0, 44, 26, ">", page_cb, (void *)(intptr_t)1);

    for (size_t i = 0; i < U_ROWS; i++) {
        urow_t *r = &s_rows[i];
        r->row = lv_button_create(area);
        lv_obj_set_pos(r->row, 4, U_ROW_Y0 + (int)i * U_ROW_H);
        lv_obj_set_size(r->row, 612, U_ROW_H - 2);
        lv_obj_set_style_bg_color(r->row, C_PANEL, 0);
        lv_obj_set_style_radius(r->row, 0, 0);
        lv_obj_set_style_shadow_width(r->row, 0, 0);
        lv_obj_set_style_pad_all(r->row, 0, 0);
        lv_obj_add_event_cb(r->row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        r->name = ui_mk_label(r->row, 4, 3, "", C_TEXT);
        r->role = ui_mk_label(r->row, 246, 3, "", C_TEXT);
        r->state = ui_mk_label(r->row, 376, 3, "", C_WARN);
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }

    s_btn[UI_USERS_BTN_ADD] = ui_mk_button(area, 4, 246, 90, 40, "ADD", btn_cb, (void *)(intptr_t)UI_USERS_BTN_ADD);
    s_btn[UI_USERS_BTN_NEW_ROLE] = ui_mk_button(area, 100, 246, 160, 40, "", btn_cb, (void *)(intptr_t)UI_USERS_BTN_NEW_ROLE);
    s_btn[UI_USERS_BTN_PASSWORD] = ui_mk_button(area, 266, 246, 130, 40, "PASSWORD", btn_cb, (void *)(intptr_t)UI_USERS_BTN_PASSWORD);
    s_btn[UI_USERS_BTN_ROLE] = ui_mk_button(area, 402, 246, 100, 40, "ROLE", btn_cb, (void *)(intptr_t)UI_USERS_BTN_ROLE);
    s_btn[UI_USERS_BTN_REMOVE] = ui_mk_button(area, 508, 246, 108, 40, "REMOVE", btn_cb, (void *)(intptr_t)UI_USERS_BTN_REMOVE);
    for (int i = 0; i < 5; i++) s_btn_lbl[i] = lv_obj_get_child(s_btn[i], 0);

    s_status = ui_mk_label(area, 8, 296, "", C_TEXT);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, UI_SUBPAGE_W - 16);
    s_hint = ui_mk_label(area, 8, 352, "Select an account, then use the buttons. ROLE steps to the next role.", C_MUTED);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_hint, UI_SUBPAGE_W - 16);

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

static void users_update(void)
{
    uint64_t now = hmi_now_ms();
    s_count = ui_auth ? hmi_auth_user_count(ui_auth, now) : 0;   /* 0 unless the session is an administrator */
    if (s_count == 0) {
        s_sel[0] = '\0';
        s_remove_armed = false;
        set_shown(false);
        ui_set_text(s_msg, t_msg, sizeof(t_msg), ui_auth ? hmi_view_users_need_admin_text() : hmi_view_users_unavailable_text());
        return;
    }
    set_shown(true);
    t_msg[0] = '\0';

    s_pages = (s_count + U_ROWS - 1) / U_ROWS;
    if (s_page >= s_pages) s_page = s_pages - 1;
    bool sel_found = false;
    for (size_t i = 0; i < U_ROWS; i++) {
        urow_t *r = &s_rows[i];
        size_t k = s_page * U_ROWS + i;
        hmi_user_info_t u;
        if (k >= s_count || !hmi_auth_user_info(ui_auth, k, now, &u)) {
            if (r->used) {
                r->used = false;
                lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
                r->t_name[0] = r->t_role[0] = r->t_state[0] = '\0';
            }
            continue;
        }
        if (!r->used) {
            r->used = true;
            lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        }
        char st[24];
        hmi_view_account_state_text(u.wait_ms, st, sizeof(st));
        ui_set_text(r->name, r->t_name, sizeof(r->t_name), u.name);
        ui_set_text(r->role, r->t_role, sizeof(r->t_role), hmi_view_role_caps(u.role));
        ui_set_text(r->state, r->t_state, sizeof(r->t_state), st);
        bool sel = strcmp(u.name, s_sel) == 0;
        sel_found = sel_found || sel;
        if (sel != r->selected) {
            r->selected = sel;
            lv_obj_set_style_bg_color(r->row, sel ? lv_color_hex(0x3A6EA5) : C_PANEL, 0);
        }
    }
    /* the selected account may be on another page or gone: keep it only if it still exists */
    if (s_sel[0] && !sel_found) {
        hmi_user_info_t u;
        if (!selected_info(&u)) {
            s_sel[0] = '\0';
            s_remove_armed = false;
        }
    }
    if (s_remove_armed && now - s_remove_at > REMOVE_ARM_MS) s_remove_armed = false;

    char b[UI_TXT * 3];
    hmi_view_page_text(s_page, s_pages, b, sizeof(b));
    ui_set_text(s_pg_lbl, t_pg, sizeof(t_pg), b);
    snprintf(b, sizeof(b), "NEW: %s", hmi_view_role_caps(s_new_role));
    ui_set_text(s_btn_lbl[UI_USERS_BTN_NEW_ROLE], t_newrole, sizeof(t_newrole), b);
    ui_set_text(s_btn_lbl[UI_USERS_BTN_REMOVE], t_remove, sizeof(t_remove), s_remove_armed ? "CONFIRM?" : "REMOVE");

    /* Convenience only: the core refuses an administrator's own account and the last administrator anyway. */
    hmi_user_info_t cur;
    bool has = selected_info(&cur);
    bool own = has && strcmp(cur.name, hmi_auth_user(ui_auth, now)) == 0;
    enable(UI_USERS_BTN_ADD, true);
    enable(UI_USERS_BTN_NEW_ROLE, true);
    enable(UI_USERS_BTN_PASSWORD, has);
    enable(UI_USERS_BTN_ROLE, has && !own);
    enable(UI_USERS_BTN_REMOVE, has && !own);
}

const ui_screen_ops_t ui_sub_users = { "ACCOUNTS", users_reset, users_build, users_update };
