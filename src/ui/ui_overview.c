#include "ui_priv.h"

#define ROW_H 26
#define ROWS_PER_PAGE 10
#define ROW_Y0 28

typedef struct {
    lv_obj_t *name, *value, *quality, *alarm;
    uint32_t c_value, c_quality, c_alarm;
    char t_value[UI_TXT], t_quality[UI_TXT], t_alarm[UI_TXT];
    uint32_t seen_version;
    uint8_t last_alarm_state;
    size_t idx;          /* the tag shown in this row */
    bool used;           /* false: the row is hidden (last page, fewer tags) */
    bool init;
} row_t;

static row_t s_rows[ROWS_PER_PAGE];
static size_t s_page, s_pages;
static lv_obj_t *s_pg_lbl, *s_pg_prev, *s_pg_next;
static char t_pg[16];
static lv_obj_t *s_wr_status, *s_sp_label, *s_journal, *s_store_fault;
static char t_store[UI_TXT * 2];
static uint32_t c_store;
static char t_wr[UI_TXT * 2], t_sp[UI_TXT], t_journal[480];
static uint32_t c_wr;
static int s_sp_tag = -1;
static lv_obj_t *s_sp_btn[3];      /* -, + and SEND: dimmed for a role that may not write the tag (the core refuses anyway) */
static int s_sp_may = -1;
static double s_sp_value;
static bool s_sp_inited;
static uint32_t s_last_write_id;
static uint32_t s_last_gv = 0xFFFFFFFFu;

static void overview_reset(void)
{
    memset(s_rows, 0, sizeof(s_rows));
    s_page = 0;
    s_pages = 0;
    s_pg_lbl = s_pg_prev = s_pg_next = NULL;
    memset(t_pg, 0, sizeof(t_pg));
    memset(t_wr, 0, sizeof(t_wr));
    memset(t_sp, 0, sizeof(t_sp));
    memset(t_journal, 0, sizeof(t_journal));
    memset(t_store, 0, sizeof(t_store));
    c_store = 0;
    c_wr = 0;
    s_wr_status = s_sp_label = s_journal = s_store_fault = NULL;
    s_sp_tag = -1;
    memset(s_sp_btn, 0, sizeof(s_sp_btn));
    s_sp_may = -1;
    s_sp_inited = false;
    s_last_write_id = 0;
    s_last_gv = 0xFFFFFFFFu;
}

/* ---- event handlers (UI thread) ---- */
static void sp_step_cb(lv_event_t *e)
{
    intptr_t dir = (intptr_t)lv_event_get_user_data(e);
    if (s_sp_tag < 0 || !ui_rt) return;
    s_sp_value = hmi_view_setpoint_step(hmi_tagdb_cfg(&ui_rt->db, (size_t)s_sp_tag), s_sp_value, dir > 0 ? 1 : -1);
}

static void sp_send_cb(lv_event_t *e)
{
    (void)e;
    if (s_sp_tag < 0 || !ui_rt) return;
    uint32_t id = 0;
    uint64_t now = hmi_now_ms();
    hmi_wr_submit_t r = hmi_writes_submit_as(&ui_rt->writes, (size_t)s_sp_tag, s_sp_value, ui_current_role(now),
                                             ui_current_actor(now), hmi_comms_link_health(&ui_rt->comms), now, &id);
    if (r == HMI_WRS_OK) {
        s_last_write_id = id;
    } else {
        s_last_write_id = 0;
        hmi_view_submit_refused_text(r, t_wr, sizeof(t_wr));
        lv_label_set_text(s_wr_status, t_wr);
        lv_obj_set_style_text_color(s_wr_status, ui_tone_color(HMI_TONE_BAD), 0);
    }
}

void ui_overview_press_setpoint_send(void) { sp_send_cb(NULL); }

const char *ui_overview_row_text(size_t row, int column)
{
    if (row >= ROWS_PER_PAGE || !s_rows[row].used) return "";
    const row_t *r = &s_rows[row];
    return column == 0 ? r->t_value : column == 1 ? r->t_quality : r->t_alarm;
}

const char *ui_overview_write_text(void) { return t_wr; }
bool ui_overview_send_enabled(void) { return s_sp_btn[2] && !lv_obj_has_state(s_sp_btn[2], LV_STATE_DISABLED); }

const char *ui_overview_row_name(size_t row)
{
    if (row >= ROWS_PER_PAGE || !s_rows[row].used) return "";
    return lv_label_get_text(s_rows[row].name);
}

/* Point the ten rows at the tags of the current page. Everything a row shows
 * is redrawn at the next update. */
static void bind_page(void)
{
    size_t n = hmi_tagdb_count(&ui_rt->db);
    for (size_t i = 0; i < ROWS_PER_PAGE; i++) {
        row_t *r = &s_rows[i];
        size_t idx = s_page * ROWS_PER_PAGE + i;
        r->used = idx < n;
        r->idx = idx;
        r->init = false;
        r->seen_version = 0;
        r->last_alarm_state = 0;
        r->c_value = r->c_quality = r->c_alarm = 0;
        if (!r->used) {
            lv_obj_add_flag(r->name, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(r->value, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(r->quality, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(r->alarm, LV_OBJ_FLAG_HIDDEN);
            r->t_value[0] = r->t_quality[0] = r->t_alarm[0] = '\0';
            continue;
        }
        lv_obj_remove_flag(r->name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(r->value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(r->quality, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(r->alarm, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(r->name, hmi_tagdb_cfg(&ui_rt->db, idx)->name);
        snprintf(r->t_value, sizeof(r->t_value), "---");
        snprintf(r->t_quality, sizeof(r->t_quality), "NO DATA");
        r->t_alarm[0] = '\0';
        lv_label_set_text(r->value, r->t_value);
        lv_label_set_text(r->quality, r->t_quality);
        lv_label_set_text(r->alarm, "");
    }
    if (s_pg_lbl) {
        hmi_view_page_text(s_page, s_pages, t_pg, sizeof(t_pg));
        lv_label_set_text(s_pg_lbl, t_pg);
    }
    s_last_gv = 0xFFFFFFFFu;   /* forces the snapshot to be drawn */
}

void ui_overview_press_page(int dir)
{
    if (!ui_rt || s_pages < 2) return;
    size_t p = s_page;
    if (dir > 0) p = (p + 1) % s_pages;
    else p = (p + s_pages - 1) % s_pages;
    s_page = p;
    bind_page();
}

size_t ui_overview_page(void) { return s_page; }
size_t ui_overview_pages(void) { return s_pages; }

static void page_cb(lv_event_t *e)
{
    ui_overview_press_page((int)(intptr_t)lv_event_get_user_data(e));
}

static void overview_build(lv_obj_t *page)
{
    hmi_runtime_t *rt = ui_rt;
    size_t n = hmi_tagdb_count(&rt->db);
    s_pages = (n + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    if (s_pages == 0) s_pages = 1;

    /* column headers */
    ui_mk_label(page, 12, 4, "TAG", C_MUTED);
    ui_mk_label(page, 244, 4, "VALUE", C_MUTED);
    ui_mk_label(page, 444, 4, "QUALITY", C_MUTED);
    ui_mk_label(page, 600, 4, "ALARM", C_MUTED);
    if (s_pages > 1) {
        s_pg_prev = ui_mk_button(page, 676, 0, 44, 26, "<", page_cb, (void *)(intptr_t)-1);
        s_pg_lbl = ui_mk_label(page, 726, 4, "", C_TEXT);
        s_pg_next = ui_mk_button(page, 756, 0, 40, 26, ">", page_cb, (void *)(intptr_t)1);
    }

    for (size_t i = 0; i < ROWS_PER_PAGE; i++) {
        int y = ROW_Y0 + (int)i * ROW_H;
        row_t *r = &s_rows[i];
        /* The columns have fixed widths: a name or a value that is too long ends in dots instead of running into the next column. */
        r->name = ui_mk_label(page, 12, y, "", C_TEXT);
        lv_label_set_long_mode(r->name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(r->name, 226);
        r->value = ui_mk_label(page, 244, y, "---", C_MUTED);
        lv_label_set_long_mode(r->value, LV_LABEL_LONG_DOT);
        lv_obj_set_width(r->value, 192);
        r->quality = ui_mk_label(page, 444, y, "NO DATA", C_MUTED);
        lv_label_set_long_mode(r->quality, LV_LABEL_LONG_DOT);
        lv_obj_set_width(r->quality, 150);
        r->alarm = ui_mk_label(page, 600, y, "", C_MUTED);
        lv_label_set_long_mode(r->alarm, LV_LABEL_LONG_DOT);
        lv_obj_set_width(r->alarm, 196);
    }
    for (size_t i = 0; i < n; i++) {
        if (hmi_tagdb_cfg(&rt->db, i)->write_allowed) {
            s_sp_tag = (int)i;
            break;
        }
    }
    s_page = 0;
    bind_page();

    /* bottom: setpoint + journal */
    lv_obj_t *pnl = lv_obj_create(page);
    lv_obj_set_size(pnl, 800, 100);
    lv_obj_set_pos(pnl, 0, 300);
    lv_obj_set_style_bg_color(pnl, C_PANEL, 0);
    lv_obj_set_style_border_width(pnl, 0, 0);
    lv_obj_set_style_radius(pnl, 0, 0);
    lv_obj_set_style_pad_all(pnl, 0, 0);
    lv_obj_remove_flag(pnl, LV_OBJ_FLAG_SCROLLABLE);
    if (s_sp_tag >= 0 && rt->writes_effective) {
        s_sp_label = ui_mk_label(pnl, 8, 6, "SETPOINT", C_TEXT);
        s_sp_btn[0] = ui_mk_button(pnl, 8, 32, 60, 40, "-", sp_step_cb, (void *)(intptr_t)-1);
        s_sp_btn[1] = ui_mk_button(pnl, 76, 32, 60, 40, "+", sp_step_cb, (void *)(intptr_t)1);
        s_sp_btn[2] = ui_mk_button(pnl, 144, 32, 110, 40, "SEND", sp_send_cb, NULL);
        s_wr_status = ui_mk_label(pnl, 270, 44, "", C_MUTED);
        lv_label_set_long_mode(s_wr_status, LV_LABEL_LONG_WRAP);   /* two lines at most, clear of the journal on the right */
        lv_obj_set_width(s_wr_status, 284);
    } else {
        s_sp_tag = -1;
        s_sp_label = ui_mk_label(pnl, 8, 6, "WRITES DISABLED", C_MUTED);
        s_wr_status = ui_mk_label(pnl, 8, 40, "", C_MUTED);
        lv_label_set_long_mode(s_wr_status, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_wr_status, 540);
    }
    s_journal = ui_mk_label(pnl, 560, 4, "no alarm events", C_MUTED);
    lv_obj_t *ack_lbl = ui_mk_label(pnl, 270, 6, "", C_MUTED);
    lv_label_set_long_mode(ack_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ack_lbl, 284);
    ui_ack_bind(ack_lbl);
    s_store_fault = ui_mk_label(pnl, 270, 78, "", C_BAD);
    if (ui_role != HMI_ROLE_VIEWER) { /* a claimed role must never look like a login */
        char tb[UI_TXT * 2];
        hmi_view_claimed_role_text(ui_role, tb, sizeof(tb));
        ui_mk_label(pnl, 8, 80, tb, C_WARN);
    }
}

static void overview_update(void)
{
    char buf[UI_TXT * 2];

    /* tag rows: only re-render rows whose version or alarm state changed */
    uint32_t gv = hmi_tagdb_global_version(&ui_rt->db);
    hmi_tag_value_t vals[HMI_MAX_TAGS];
    bool tags_changed = (gv != s_last_gv);
    if (tags_changed) {
        hmi_tagdb_snapshot(&ui_rt->db, vals, HMI_MAX_TAGS);
        s_last_gv = gv;
    }
    for (size_t i = 0; i < ROWS_PER_PAGE; i++) {
        row_t *r = &s_rows[i];
        if (!r->used) continue;
        hmi_alarm_point_t ap;
        hmi_alarm_get(&ui_rt->alarms, r->idx, &ap);
        bool alarm_changed = (!r->init) || r->last_alarm_state != (uint8_t)(ap.state | (ap.data_suppressed ? 0x80 : 0));
        if (!tags_changed && !alarm_changed) continue;
        if (!tags_changed && alarm_changed) hmi_tagdb_get(&ui_rt->db, r->idx, &vals[r->idx]);

        const hmi_tag_cfg_t *c = hmi_tagdb_cfg(&ui_rt->db, r->idx);
        const hmi_tag_value_t *v = &vals[r->idx];
        if (!r->init || r->seen_version != v->version) {
            hmi_view_value_text(c, v, buf, sizeof(buf));
            ui_set_text(r->value, r->t_value, sizeof(r->t_value), buf);
            ui_set_color(r->value, &r->c_value, ui_tone_color(hmi_view_value_tone(v->quality)));
            ui_set_text(r->quality, r->t_quality, sizeof(r->t_quality), hmi_view_quality_text(v));
            ui_set_color(r->quality, &r->c_quality, ui_tone_color(hmi_view_quality_tone(v->quality)));
            r->seen_version = v->version;
        }
        if (alarm_changed) {
            char ab[UI_TXT];
            hmi_view_alarm_text(&ap, ab, sizeof(ab));
            ui_set_text(r->alarm, r->t_alarm, sizeof(r->t_alarm), ab);
            ui_set_color(r->alarm, &r->c_alarm, ui_tone_color(hmi_view_alarm_tone(&ap)));
            r->last_alarm_state = (uint8_t)(ap.state | (ap.data_suppressed ? 0x80 : 0));
        }
        r->init = true;
    }

    /* setpoint panel */
    if (s_sp_tag >= 0) {
        const hmi_tag_cfg_t *c = hmi_tagdb_cfg(&ui_rt->db, (size_t)s_sp_tag);
        if (!s_sp_inited) {
            hmi_tag_value_t v;
            hmi_tagdb_get(&ui_rt->db, (size_t)s_sp_tag, &v);
            if (v.quality == HMI_Q_GOOD) {
                s_sp_value = v.eng;
                s_sp_inited = true;
            }
        }
        hmi_view_setpoint_text(c, s_sp_value, buf, sizeof(buf));
        ui_set_text(s_sp_label, t_sp, sizeof(t_sp), buf);
        /* convenience only: the core refuses a write from a role below the tag's minimum whatever the buttons show */
        int may = hmi_authz_allows(ui_current_role(hmi_now_ms()), HMI_ACT_WRITE_TAG, c->write_role) ? 1 : 0;
        if (may != s_sp_may) {
            s_sp_may = may;
            for (int b = 0; b < 3; b++) {
                if (!s_sp_btn[b]) continue;
                if (may) lv_obj_remove_state(s_sp_btn[b], LV_STATE_DISABLED);
                else lv_obj_add_state(s_sp_btn[b], LV_STATE_DISABLED);
            }
        }
    }
    if (s_store_fault) {
        if (hmi_runtime_storage_fault(ui_rt)) hmi_view_storage_fault_text(buf, sizeof(buf));
        else buf[0] = '\0';
        ui_set_text(s_store_fault, t_store, sizeof(t_store), buf);
        ui_set_color(s_store_fault, &c_store, ui_tone_color(HMI_TONE_BAD));
    }
    if (s_last_write_id) {
        hmi_write_rec_t rec;
        if (hmi_writes_status(&ui_rt->writes, s_last_write_id, &rec)) {
            char b2[UI_TXT * 2];
            hmi_view_write_status_text(&rec, b2, sizeof(b2));
            ui_set_text(s_wr_status, t_wr, sizeof(t_wr), b2);
            ui_set_color(s_wr_status, &c_wr, ui_tone_color(hmi_view_write_status_tone(rec.state)));
        }
    }

    /* journal: last 4 events */
    hmi_alarm_log_t j[4];
    size_t n = hmi_alarm_journal_copy(&ui_rt->alarms, j, 4);
    char jb[sizeof(t_journal)];
    const char *names[HMI_MAX_TAGS];
    size_t n_names = hmi_tagdb_count(&ui_rt->db);
    for (size_t k = 0; k < n_names; k++) names[k] = hmi_tagdb_cfg(&ui_rt->db, k)->name;
    hmi_view_journal_text(j, n, names, n_names, jb, sizeof(jb));
    ui_set_text(s_journal, t_journal, sizeof(t_journal), jb);
}

const ui_screen_ops_t ui_screen_overview = { "OVERVIEW", overview_reset, overview_build, overview_update, false };
