#include "ui_priv.h"

#define AL_ROWS 8
#define AL_ROW_H 34
#define AL_ROW_Y0 28

typedef struct {
    lv_obj_t *pri, *name, *state, *age, *btn;
    char t_pri[8], t_name[HMI_NAME_LEN + 8], t_state[UI_TXT], t_age[16];
    uint32_t c_pri, c_state;
    hmi_alarm_point_t shown;    /* what this row displayed when it was last drawn */
    size_t point;
    bool used;
    bool btn_on;                /* the ACK button is enabled */
} arow_t;

static arow_t s_rows[AL_ROWS];
static size_t s_page, s_pages;
static lv_obj_t *s_pg_lbl, *s_none, *s_journal;
static char t_pg[16], t_journal[480];
static bool s_none_shown;

static void alarms_reset(void)
{
    memset(s_rows, 0, sizeof(s_rows));
    s_page = 0;
    s_pages = 1;
    s_pg_lbl = s_none = s_journal = NULL;
    memset(t_pg, 0, sizeof(t_pg));
    memset(t_journal, 0, sizeof(t_journal));
    s_none_shown = false;
}

size_t ui_alarms_page(void) { return s_page; }
size_t ui_alarms_pages(void) { return s_pages; }

size_t ui_alarms_rows(void)
{
    size_t n = 0;
    for (size_t i = 0; i < AL_ROWS; i++)
        if (s_rows[i].used) n++;
    return n;
}

const char *ui_alarms_row_text(size_t row, int column)
{
    if (row >= AL_ROWS || !s_rows[row].used) return "";
    const arow_t *r = &s_rows[row];
    return column == 0 ? r->t_pri : column == 1 ? r->t_name : column == 2 ? r->t_state : r->t_age;
}

bool ui_alarms_ack_enabled(size_t row)
{
    return row < AL_ROWS && s_rows[row].used && s_rows[row].btn_on;
}

/* What pressing the row's ACK button does. The core decides whether the role may
 * acknowledge; the button state is only a convenience. */
void ui_alarms_press_ack(size_t row)
{
    if (row >= AL_ROWS || !s_rows[row].used) return;
    ui_ack_point(s_rows[row].point, &s_rows[row].shown);
}

static void ack_cb(lv_event_t *e)
{
    ui_alarms_press_ack((size_t)(intptr_t)lv_event_get_user_data(e));
}

void ui_alarms_press_page(int dir)
{
    if (s_pages < 2) return;
    if (dir > 0) s_page = (s_page + 1) % s_pages;
    else s_page = (s_page + s_pages - 1) % s_pages;
}

static void page_cb(lv_event_t *e)
{
    ui_alarms_press_page((int)(intptr_t)lv_event_get_user_data(e));
}

static void set_row_visible(arow_t *r, bool on)
{
    lv_obj_t *all[5] = { r->pri, r->name, r->state, r->age, r->btn };
    for (int k = 0; k < 5; k++) {
        if (on) lv_obj_remove_flag(all[k], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(all[k], LV_OBJ_FLAG_HIDDEN);
    }
}

static void alarms_build(lv_obj_t *page)
{
    ui_mk_label(page, 12, 4, "PRIORITY", C_MUTED);
    ui_mk_label(page, 110, 4, "POINT", C_MUTED);
    ui_mk_label(page, 330, 4, "STATE", C_MUTED);
    ui_mk_label(page, 560, 4, "AGE", C_MUTED);
    ui_mk_button(page, 676, 0, 44, 26, "<", page_cb, (void *)(intptr_t)-1);
    s_pg_lbl = ui_mk_label(page, 726, 4, "1/1", C_TEXT);
    ui_mk_button(page, 756, 0, 40, 26, ">", page_cb, (void *)(intptr_t)1);

    for (size_t i = 0; i < AL_ROWS; i++) {
        arow_t *r = &s_rows[i];
        int y = AL_ROW_Y0 + (int)i * AL_ROW_H;
        r->pri = ui_mk_label(page, 12, y + 6, "", C_TEXT);
        r->name = ui_mk_label(page, 110, y + 6, "", C_TEXT);
        lv_label_set_long_mode(r->name, LV_LABEL_LONG_DOT);   /* a long name ends in dots instead of running into the state column */
        lv_obj_set_width(r->name, 216);
        r->state = ui_mk_label(page, 330, y + 6, "", C_TEXT);
        r->age = ui_mk_label(page, 560, y + 6, "", C_MUTED);
        r->btn = ui_mk_button(page, 676, y, 120, AL_ROW_H - 4, "ACK", ack_cb, (void *)(intptr_t)i);
        set_row_visible(r, false);
    }
    s_none = ui_mk_label(page, 12, AL_ROW_Y0 + 6, hmi_view_no_alarms_text(), C_MUTED);

    lv_obj_t *pnl = lv_obj_create(page);
    lv_obj_set_size(pnl, 800, 100);
    lv_obj_set_pos(pnl, 0, 300);
    lv_obj_set_style_bg_color(pnl, C_PANEL, 0);
    lv_obj_set_style_border_width(pnl, 0, 0);
    lv_obj_set_style_radius(pnl, 0, 0);
    lv_obj_set_style_pad_all(pnl, 0, 0);
    lv_obj_remove_flag(pnl, LV_OBJ_FLAG_SCROLLABLE);
    ui_ack_bind(ui_mk_label(pnl, 8, 6, "", C_MUTED));
    s_journal = ui_mk_label(pnl, 8, 30, "no alarm events", C_MUTED);
}

static void alarms_update(void)
{
    uint64_t now = hmi_now_ms();
    hmi_alarm_row_t list[HMI_MAX_TAGS + 1];
    size_t n = hmi_alarm_list(&ui_rt->alarms, list, HMI_MAX_TAGS + 1);

    s_pages = n ? (n + AL_ROWS - 1) / AL_ROWS : 1;
    if (s_page >= s_pages) s_page = s_pages - 1;

    const char *names[HMI_MAX_TAGS];
    size_t n_names = hmi_tagdb_count(&ui_rt->db);
    for (size_t k = 0; k < n_names; k++) names[k] = hmi_tagdb_cfg(&ui_rt->db, k)->name;

    bool may_ack = hmi_authz_allows(ui_current_role(now), HMI_ACT_ACK_ALARM, HMI_ROLE_VIEWER);
    for (size_t i = 0; i < AL_ROWS; i++) {
        arow_t *r = &s_rows[i];
        size_t li = s_page * AL_ROWS + i;
        if (li >= n) {
            if (r->used) {
                r->used = false;
                set_row_visible(r, false);
                r->t_pri[0] = r->t_name[0] = r->t_state[0] = r->t_age[0] = '\0';
                r->c_pri = r->c_state = 0;
            }
            continue;
        }
        const hmi_alarm_row_t *a = &list[li];
        bool fresh = !r->used;
        if (fresh) {
            r->used = true;
            set_row_visible(r, true);
        }
        r->point = a->point;
        r->shown = a->pt;

        char b[UI_TXT];
        ui_set_text(r->pri, r->t_pri, sizeof(r->t_pri), hmi_view_priority_text(a->priority));
        ui_set_color(r->pri, &r->c_pri, ui_tone_color(hmi_view_priority_tone(a->priority)));
        ui_set_text(r->name, r->t_name, sizeof(r->t_name), hmi_view_alarm_point_name(names, n_names, a->point));
        hmi_view_alarm_text(&a->pt, b, sizeof(b));
        ui_set_text(r->state, r->t_state, sizeof(r->t_state), b);
        ui_set_color(r->state, &r->c_state, ui_tone_color(hmi_view_alarm_tone(&a->pt)));
        hmi_view_age_text(now >= a->pt.since_ms ? now - a->pt.since_ms : 0, b, sizeof(b));
        ui_set_text(r->age, r->t_age, sizeof(r->t_age), b);

        /* Only an alarm that is waiting for acknowledgement can be acknowledged. */
        bool waiting = a->pt.state == HMI_AL_ACTIVE_UNACK || a->pt.state == HMI_AL_RTN_UNACK;
        bool on = may_ack && waiting;
        if (fresh || on != r->btn_on) {
            r->btn_on = on;
            if (on) lv_obj_remove_state(r->btn, LV_STATE_DISABLED);
            else lv_obj_add_state(r->btn, LV_STATE_DISABLED);
        }
    }

    if ((n == 0) != s_none_shown) {
        s_none_shown = (n == 0);
        if (s_none_shown) lv_obj_remove_flag(s_none, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_none, LV_OBJ_FLAG_HIDDEN);
    }

    char pb[16];
    hmi_view_page_text(s_page, s_pages, pb, sizeof(pb));
    ui_set_text(s_pg_lbl, t_pg, sizeof(t_pg), pb);

    hmi_alarm_log_t j[4];
    size_t nj = hmi_alarm_journal_copy(&ui_rt->alarms, j, 4);
    char jb[sizeof(t_journal)];
    hmi_view_journal_text(j, nj, names, n_names, jb, sizeof(jb));
    ui_set_text(s_journal, t_journal, sizeof(t_journal), jb);
}

const ui_screen_ops_t ui_screen_alarms = { "ALARMS", alarms_reset, alarms_build, alarms_update, false };
