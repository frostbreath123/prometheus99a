#include "ui_priv.h"
#include <math.h>
#include "../hmi/hmi_trend.h"

#define CHART_POINTS 120        /* points on the chart; a longer window is squeezed into them */
#define CHART_RANGE  1000       /* values are mapped to 0 .. CHART_RANGE, the axis labels show the real ones */
#define N_WINDOWS 3

/* Window = CHART_POINTS * factor buckets. With the default 5 s bucket: 10, 20 and 40 minutes. */
static const size_t FACTOR[N_WINDOWS] = { 1, 2, 4 };

static lv_obj_t *s_chart, *s_name, *s_status, *s_none, *s_axis_hi, *s_axis_lo;
static lv_obj_t *s_btn_prev, *s_btn_next, *s_win_btn[N_WINDOWS], *s_win_lbl[N_WINDOWS];
static lv_chart_series_t *s_ser_max, *s_ser_min;
static char t_name[HMI_NAME_LEN + 4], t_status[UI_TXT * 3], t_hi[16], t_lo[16];
static size_t s_series, s_win;
static uint32_t s_rev;
static bool s_dirty;
static size_t s_points;         /* points of the chart that hold data */
static uint32_t s_redraws;      /* how often the chart was redrawn (a measurement for tests) */

static hmi_trend_bucket_t s_raw[HMI_TREND_POINTS];
static hmi_trend_bucket_t s_dec[CHART_POINTS];

static void trends_reset(void)
{
    s_chart = s_name = s_status = s_none = s_axis_hi = s_axis_lo = NULL;
    s_btn_prev = s_btn_next = NULL;
    for (int i = 0; i < N_WINDOWS; i++) s_win_btn[i] = s_win_lbl[i] = NULL;
    s_ser_max = s_ser_min = NULL;
    memset(t_name, 0, sizeof(t_name));
    memset(t_status, 0, sizeof(t_status));
    memset(t_hi, 0, sizeof(t_hi));
    memset(t_lo, 0, sizeof(t_lo));
    s_series = 0;
    s_win = 0;
    s_rev = 0;
    s_dirty = true;
    s_points = 0;
    s_redraws = 0;
}

size_t ui_trends_series(void) { return s_series; }
size_t ui_trends_window(void) { return s_win; }
size_t ui_trends_points(void) { return s_points; }
uint32_t ui_trends_redraws(void) { return s_redraws; }
const char *ui_trends_name(void) { return t_name; }
const char *ui_trends_status_text(void) { return t_status; }
const char *ui_trends_axis_text(int which) { return which == 0 ? t_hi : t_lo; }

int32_t ui_trends_chart_value(int which, size_t index)
{
    if (!s_chart || index >= CHART_POINTS) return LV_CHART_POINT_NONE;
    const int32_t *y = lv_chart_get_y_array(s_chart, which == 0 ? s_ser_max : s_ser_min);
    return y ? y[index] : LV_CHART_POINT_NONE;
}

/* The chart, the two axis labels and the line under it follow the selected
 * series and window. Called when a bucket was closed or the selection changed. */
static void redraw(void)
{
    hmi_trend_t *tr = &ui_rt->trend;
    int tag = hmi_trend_series_tag(tr, s_series);
    if (tag < 0) return;
    const hmi_tag_cfg_t *cfg = hmi_tagdb_cfg(&ui_rt->db, (size_t)tag);
    size_t factor = FACTOR[s_win];
    size_t want = CHART_POINTS * factor;
    if (want > HMI_TREND_POINTS) want = HMI_TREND_POINTS;
    size_t n = hmi_trend_copy(tr, s_series, s_raw, want);
    size_t m = hmi_trend_decimate(s_raw, n, factor, s_dec, CHART_POINTS);

    double lo = 0, hi = 0;
    bool any = hmi_trend_range(s_dec, m, &lo, &hi);
    double plo = lo, phi = hi;
    if (any) {            /* room above and below, and a flat line is not a division by zero */
        double span = hi - lo;
        double pad = span > 1e-9 ? span * 0.1 : (fabs(lo) * 0.05 > 0.5 ? fabs(lo) * 0.05 : 0.5);
        plo = lo - pad;
        phi = hi + pad;
    }

    s_redraws++;
    size_t offset = CHART_POINTS - m;            /* the newest bucket is the right-most point */
    s_points = 0;
    for (size_t i = 0; i < CHART_POINTS; i++) {
        int32_t vmax = LV_CHART_POINT_NONE, vmin = LV_CHART_POINT_NONE;
        if (i >= offset && any && hmi_trend_bucket_valid(&s_dec[i - offset])) {
            vmax = (int32_t)(((double)s_dec[i - offset].hi - plo) / (phi - plo) * CHART_RANGE + 0.5);
            vmin = (int32_t)(((double)s_dec[i - offset].lo - plo) / (phi - plo) * CHART_RANGE + 0.5);
            s_points++;
        }
        lv_chart_set_value_by_id(s_chart, s_ser_max, (uint32_t)i, vmax);
        lv_chart_set_value_by_id(s_chart, s_ser_min, (uint32_t)i, vmin);
    }
    lv_chart_refresh(s_chart);

    char b[UI_TXT * 3], span[16];
    if (any) {
        hmi_view_axis_text(phi, b, sizeof(b));
        ui_set_text(s_axis_hi, t_hi, sizeof(t_hi), b);
        hmi_view_axis_text(plo, b, sizeof(b));
        ui_set_text(s_axis_lo, t_lo, sizeof(t_lo), b);
    } else {
        ui_set_text(s_axis_hi, t_hi, sizeof(t_hi), "--");
        ui_set_text(s_axis_lo, t_lo, sizeof(t_lo), "--");
    }
    hmi_view_age_text((uint64_t)want * hmi_trend_bucket_ms(tr), span, sizeof(span));
    hmi_view_trend_status_text(cfg->name, cfg->units, hmi_view_decimals(cfg->scale), any, lo, hi, span, b, sizeof(b));
    ui_set_text(s_status, t_status, sizeof(t_status), b);
    ui_set_text(s_name, t_name, sizeof(t_name), cfg->name);
}

static void window_labels(void)
{
    char b[16];
    for (int i = 0; i < N_WINDOWS; i++) {
        size_t w = CHART_POINTS * FACTOR[i];
        if (w > HMI_TREND_POINTS) w = HMI_TREND_POINTS;
        hmi_view_age_text((uint64_t)w * hmi_trend_bucket_ms(&ui_rt->trend), b, sizeof(b));
        lv_label_set_text(s_win_lbl[i], b);
        lv_obj_set_style_bg_color(s_win_btn[i], (size_t)i == s_win ? C_ACCENT : C_PANEL, 0);
    }
}

void ui_trends_press_series(int dir)
{
    size_t ns = ui_rt ? hmi_trend_series_count(&ui_rt->trend) : 0;
    if (ns < 2) return;
    s_series = dir > 0 ? (s_series + 1) % ns : (s_series + ns - 1) % ns;
    s_dirty = true;
}

void ui_trends_press_window(size_t index)
{
    if (index >= N_WINDOWS || !ui_rt || !s_win_btn[0]) return;
    s_win = index;
    window_labels();
    s_dirty = true;
}

static void series_cb(lv_event_t *e)
{
    ui_trends_press_series((int)(intptr_t)lv_event_get_user_data(e));
}

static void window_cb(lv_event_t *e)
{
    ui_trends_press_window((size_t)(intptr_t)lv_event_get_user_data(e));
}

static void trends_build(lv_obj_t *page)
{
    size_t ns = hmi_trend_series_count(&ui_rt->trend);

    s_btn_prev = ui_mk_button(page, 12, 0, 44, 30, "<", series_cb, (void *)(intptr_t)-1);
    s_name = ui_mk_label(page, 64, 6, "", C_TEXT);
    s_btn_next = ui_mk_button(page, 270, 0, 44, 30, ">", series_cb, (void *)(intptr_t)1);
    for (int i = 0; i < N_WINDOWS; i++) {
        s_win_btn[i] = ui_mk_button(page, 520 + i * 92, 0, 88, 30, "", window_cb, (void *)(intptr_t)i);
        s_win_lbl[i] = lv_obj_get_child(s_win_btn[i], 0);
    }
    window_labels();

    s_chart = lv_chart_create(page);
    lv_obj_set_pos(s_chart, 64, 40);
    lv_obj_set_size(s_chart, 724, 236);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, CHART_RANGE);
    lv_chart_set_div_line_count(s_chart, 5, 7);
    lv_obj_set_style_bg_color(s_chart, C_PANEL, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_radius(s_chart, 0, 0);
    lv_obj_set_style_line_color(s_chart, C_LINE, LV_PART_MAIN);   /* grid */
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);                    /* no marker per point */
    s_ser_max = lv_chart_add_series(s_chart, C_WARN, LV_CHART_AXIS_PRIMARY_Y);
    s_ser_min = lv_chart_add_series(s_chart, C_SERIES, LV_CHART_AXIS_PRIMARY_Y);
    for (uint32_t i = 0; i < CHART_POINTS; i++) {
        lv_chart_set_value_by_id(s_chart, s_ser_max, i, LV_CHART_POINT_NONE);
        lv_chart_set_value_by_id(s_chart, s_ser_min, i, LV_CHART_POINT_NONE);
    }

    s_axis_hi = ui_mk_label(page, 2, 40, "--", C_MUTED);
    s_axis_lo = ui_mk_label(page, 2, 258, "--", C_MUTED);
    s_status = ui_mk_label(page, 12, 284, "", C_TEXT);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_status, 776);
    lv_obj_t *hint1 = ui_mk_label(page, 12, 312, "Upper line: highest value in each period. Lower line: lowest.", C_MUTED);
    lv_obj_t *hint2 = ui_mk_label(page, 12, 336, "A break in the lines means no good data in that period.", C_MUTED);

    s_none = ui_mk_label(page, 12, 100, hmi_view_no_trend_text(), C_MUTED);
    lv_label_set_long_mode(s_none, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_none, 760);
    if (ns == 0) {
        lv_obj_t *hide[] = { s_btn_prev, s_name, s_btn_next, s_win_btn[0], s_win_btn[1], s_win_btn[2], s_chart, s_axis_hi, s_axis_lo, s_status, hint1, hint2 };
        for (size_t i = 0; i < sizeof(hide) / sizeof(hide[0]); i++) lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_none, LV_OBJ_FLAG_HIDDEN);
        if (ns < 2) {
            lv_obj_add_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_dirty = true;
}

/* A chart is redrawn only when a bucket was closed or the selection changed:
 * between two buckets nothing is touched, so an idle trend screen costs nothing. */
static void trends_update(void)
{
    if (hmi_trend_series_count(&ui_rt->trend) == 0) return;
    uint32_t rev = hmi_trend_rev(&ui_rt->trend);
    if (!s_dirty && rev == s_rev) return;
    s_dirty = false;
    s_rev = rev;
    redraw();
}

const ui_screen_ops_t ui_screen_trends = { "TRENDS", trends_reset, trends_build, trends_update, true };
