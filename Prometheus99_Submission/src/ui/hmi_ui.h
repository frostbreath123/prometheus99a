/**
 * @file hmi_ui.h
 * @brief LVGL v9 presentation: binds runtime tags/alarms/writes to widgets.
 *
 * UI thread only: every LVGL call and every call into this module must come
 * from the single thread that runs lv_timer_handler(). The module reads
 * runtime state exclusively through the runtime's thread-safe snapshot
 * getters and never blocks on I/O.
 */
#ifndef HMI_UI_H
#define HMI_UI_H

#include "lvgl.h"
#include "../hmi/hmi_runtime.h"

/* Builds the UI. `role` is the CLAIMED role the program was started with (--role, default viewer): the product has
 * no login, and the core checks the role on every privileged call. The header shows it. rt may be NULL: the UI then
 * shows only `banner` (for example a rejected configuration). */
void hmi_ui_init(hmi_runtime_t *rt, hmi_role_t role, const char *banner);

/* Show a once-per-second liveness mark ("HB nn") in the header. Off by
 * default so idle-flush measurements stay at zero. May be changed at run
 * time from the UI thread; turning it off clears the mark. */
void hmi_ui_enable_heartbeat(bool on);

/* Counters for measurement. */
typedef struct {
    uint32_t binder_ticks;
    uint32_t labels_updated;
    uint32_t labels_skipped_unchanged;
} hmi_ui_stats_t;
hmi_ui_stats_t hmi_ui_stats(void);

/* Diagnostic/test seam: the text currently shown in a tag row column
 * (0 value, 1 quality, 2 alarm) and in the link indicator. UI thread only. */
const char *hmi_ui_row_text(size_t row, int column);
const char *hmi_ui_link_text(void);
const char *hmi_ui_heartbeat_text(void); /* "" while the heartbeat is disabled */
const char *hmi_ui_ack_status_text(void);
const char *hmi_ui_write_status_text(void);   /* status of the last SEND: "WRITE #n: <state>" or "SEND REFUSED: <why>" */
bool hmi_ui_send_enabled(void);               /* the SEND button is shown enabled (a setpoint panel exists and the role may write the tag) */
/* Test seam: runs exactly what pressing ACK ALL runs, including for roles
 * whose button is disabled, so tests can prove the core refuses. UI thread. */
void hmi_ui_press_ack_all(void);
/* Test seam: runs exactly what pressing the setpoint SEND button runs. */
void hmi_ui_press_setpoint_send(void);

const char *hmi_ui_role_text(void);              /* header label, e.g. "OPERATOR" */

/* Test seams for the screens. Screen numbers are the tab order: 0 overview,
 * 1 alarms. Each press runs exactly what the matching on-screen control runs.
 * The row and column numbers of the alarm list are those of the current page:
 * columns 0 priority, 1 point, 2 state, 3 age. UI thread only. */
#define HMI_UI_SCREEN_OVERVIEW 0
#define HMI_UI_SCREEN_ALARMS   1
size_t hmi_ui_screen_count(void);
size_t hmi_ui_screen(void);                      /* the visible screen */
bool hmi_ui_screen_visible(size_t screen);
void hmi_ui_press_tab(size_t screen);
const char *hmi_ui_row_name(size_t row);         /* tag name in an overview row of the current page; "" if unused */
size_t hmi_ui_overview_page(void);               /* counted from 0 */
size_t hmi_ui_overview_pages(void);
void hmi_ui_press_overview_page(int dir);        /* -1 previous, +1 next */
size_t hmi_ui_alarm_rows(void);                  /* rows in use on the current page */
const char *hmi_ui_alarm_row_text(size_t row, int column);
bool hmi_ui_alarm_ack_enabled(size_t row);
void hmi_ui_press_alarm_ack(size_t row);
size_t hmi_ui_alarm_page(void);
size_t hmi_ui_alarm_pages(void);
void hmi_ui_press_alarm_page(int dir);

/* Trend screen (screen number 2). Chart values are 0 to 1000 within the drawn
 * range; the "no data" value is LV_CHART_POINT_NONE, returned as is. */
#define HMI_UI_SCREEN_TRENDS   2
size_t hmi_ui_trend_series(void);                /* the selected series, counted from 0 */
size_t hmi_ui_trend_window(void);                /* 0, 1 or 2: the window button that is selected */
size_t hmi_ui_trend_points(void);                /* chart points that hold data (120 fit on the chart) */
uint32_t hmi_ui_trend_redraws(void);             /* how often the chart was redrawn since the screen was built */
const char *hmi_ui_trend_name(void);
const char *hmi_ui_trend_status_text(void);
const char *hmi_ui_trend_axis_text(int which);   /* 0 top label, 1 bottom label */
int hmi_ui_trend_value(int which, size_t index); /* which: 0 upper line, 1 lower line; index 0 to 119 */
void hmi_ui_press_trend_series(int dir);
void hmi_ui_press_trend_window(size_t index);

/* The line the settings screen shows about the configuration in use, for example
 * "Configuration: serial 7, signed". Set by the program that checked it; may be
 * called before or after the UI is built. Copied (at most 95 characters). */
void hmi_ui_set_config_info(const char *text);

/* Settings screen (screen number 3) and its views. Views: 0 system, 1 configuration.
 * Each press runs exactly what the on-screen button runs; the core decides. */
#define HMI_UI_SCREEN_SETTINGS 3
#define HMI_UI_VIEW_SYSTEM     0
/* (view 1 is the CONFIG view below) */
size_t hmi_ui_settings_view(void);
size_t hmi_ui_settings_view_count(void);
void hmi_ui_press_settings_view(size_t view);
const char *hmi_ui_system_text(size_t line);     /* 0 PLC, 1 writes, 2 configuration, 3 storage, 4 clock, 5 tags, 6 build */

/* The CONFIG view (settings view 1): an Engineer or Administrator edits the alarm settings of a tag or the
 * PLC address, then APPLY twice sends one request to the root helper through the spool directory.
 * hmi_ui_set_config_edit() is called by the program when the configuration was verified: `spool_dir`
 * is --spool (NULL: the view stays off) and `base_serial` is the serial of the signature in use.
 * Rows of a tag: 0 HIGH-HIGH, 1 HIGH, 2 LOW, 3 LOW-LOW, 4 DEADBAND, 5 ON DELAY, 6 OFF DELAY, 7 PRIORITY;
 * of the PLC (target 0): 0 HOST, 1 PORT, 2 UNIT. Keys: "0" to "9", ".", "-", "DEL", "OFF", "OK", "CANCEL".
 * Buttons: 0 APPLY, 1 DISCARD. Each press runs exactly what the on-screen control runs. */
#define HMI_UI_VIEW_CONFIG 1
void hmi_ui_set_config_edit(const char *spool_dir, uint32_t base_serial);
size_t hmi_ui_config_targets(void);
size_t hmi_ui_config_target(void);
const char *hmi_ui_config_target_text(void);
void hmi_ui_press_config_target(int dir);
size_t hmi_ui_config_rows(void);
const char *hmi_ui_config_row_text(size_t row, int column);   /* 0 setting, 1 current value, 2 edit waiting */
void hmi_ui_press_config_row(size_t row);
bool hmi_ui_config_keypad_open(void);
const char *hmi_ui_config_keypad_text(void);
void hmi_ui_press_config_key(const char *key);
void hmi_ui_press_config(int button);
bool hmi_ui_config_button_enabled(int button);
size_t hmi_ui_config_pending(void);
const char *hmi_ui_config_status_text(void);
const char *hmi_ui_config_result_text(void);
const char *hmi_ui_config_message_text(void);    /* why the view shows nothing, "" while it does */

/* All wording and tone decisions live in hmi_view.h (pure, unit-tested); this
 * module only maps tones to colours and sets LVGL label text. */

#endif /* HMI_UI_H */
