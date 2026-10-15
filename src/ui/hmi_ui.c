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

static hmi_runtime_t *s_rt;
static hmi_role_t s_role;
static row_t s_rows[MAX_ROWS];
static size_t s_n_rows;
static lv_obj_t *s_link, *s_alarm_sum, *s_wr_status, *s_sp_label, *s_journal, *s_hb, *s_ack_status;
static bool s_hb_on;   /* set before hmi_ui_init; survives re-init */
static char t_hb[TXT];
static char t_ack[TXT * 2];
static char t_link[TXT], t_alarm_sum[TXT], t_wr[TXT * 2], t_sp[TXT], t_journal[480];
static uint32_t c_link, c_alarm_sum, c_wr;
static int s_sp_tag = -1;
static double s_sp_value;
static bool s_sp_inited;
static uint32_t s_last_write_id;
static uint32_t s_last_gv = 0xFFFFFFFFu;
static hmi_ui_stats_t s_stats;

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

/* ---- event handlers (UI thread) ---- */
/* The core decides (hmi_alarm_ack_all checks the role and audits); this only
 * shows the outcome. */
static void do_ack_all(void)
{
    if (!s_rt || !s_ack_status) return;
    hmi_ack_result_t r = hmi_alarm_ack_all(&s_rt->alarms, hmi_now_ms(), s_role);
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
    hmi_wr_submit_t r = hmi_writes_submit(&s_rt->writes, (size_t)s_sp_tag, s_sp_value, s_role,
                                          hmi_comms_link_health(&s_rt->comms), hmi_now_ms(), &id);
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

void hmi_ui_init(hmi_runtime_t *rt, hmi_role_t role, const char *banner)
{
    memset(s_rows, 0, sizeof(s_rows));
    memset(t_link, 0, sizeof(t_link));
    memset(t_alarm_sum, 0, sizeof(t_alarm_sum));
    memset(t_wr, 0, sizeof(t_wr));
    memset(t_sp, 0, sizeof(t_sp));
    memset(t_journal, 0, sizeof(t_journal));
    memset(t_hb, 0, sizeof(t_hb));
    memset(t_ack, 0, sizeof(t_ack));
    s_ack_status = NULL;
    s_rt = rt;
    s_role = role;
    s_sp_tag = -1;
    s_sp_inited = false;
    s_last_write_id = 0;
    s_last_gv = 0xFFFFFFFFu;
    c_link = c_alarm_sum = c_wr = 0;
    memset(&s_stats, 0, sizeof(s_stats));

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
    s_link = mk_label(hdr, 180, 12, "PLC: --", C_MUTED);
    s_hb = mk_label(hdr, 340, 12, "", C_MUTED);
    s_alarm_sum = mk_label(hdr, 440, 12, "ALARMS: --", C_MUTED);
    lv_obj_t *ack_btn = mk_button(hdr, 660, 2, 130, 38, "ACK ALL", ack_all_cb, NULL);
    /* Convenience only: the core refuses the request regardless of this. */
    bool may_ack = hmi_authz_allows(role, HMI_ACT_ACK_ALARM, HMI_ROLE_VIEWER);
    if (!may_ack) lv_obj_add_state(ack_btn, LV_STATE_DISABLED);

    if (!rt) {
        lv_obj_t *b = mk_label(scr, 20, 80, banner ? banner : "NO CONFIGURATION", C_BAD);
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(b, 760);
        mk_label(scr, 20, 140, "No PLC endpoint selected. Writes disabled.", C_MUTED);
        return;
    }

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
    if (!may_ack) {
        snprintf(t_ack, sizeof(t_ack), "ACK DISABLED: %s role", hmi_role_name(role));
        lv_label_set_text(s_ack_status, t_ack);
    }
    lv_timer_create(binder_cb, 50, NULL);
}
