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
#include "../hmi/hmi_auth.h"

/* Production entry point. The role the UI hands to the core is ALWAYS
 * hmi_auth_role(auth, now): Viewer until someone logs in, and Viewer again
 * after logout or an idle timeout. The header shows who is logged in and a
 * LOGIN / LOGOUT button; the login screen also offers first-boot creation of
 * the administrator while no user store exists.
 * rt may be NULL: the UI then shows only `banner` (e.g. config rejected) but
 * login still works. auth must outlive the UI. */
void hmi_ui_init_auth(hmi_runtime_t *rt, hmi_auth_t *auth, const char *banner);

/* Fixed-role entry point for tests and simulation ONLY (the production
 * executable reaches it only when built with HMI_TEST_ROLE, see main_linux.c).
 * `role` is a CLAIMED role, nothing is authenticated, there is no login
 * button, and a permanent "TEST ROLE ... NOT AUTHENTICATED" banner is shown. */
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
/* Test seam: runs exactly what pressing ACK ALL runs, including for roles
 * whose button is disabled, so tests can prove the core refuses. UI thread. */
void hmi_ui_press_ack_all(void);
/* Test seam: runs exactly what pressing the setpoint SEND button runs. */
void hmi_ui_press_setpoint_send(void);

/* Test seams for the login screen. Each runs exactly what the matching
 * on-screen control runs. UI thread only. */
const char *hmi_ui_role_text(void);              /* header label, e.g. "OPERATOR  op1" */
bool hmi_ui_login_visible(void);
const char *hmi_ui_login_title_text(void);       /* "LOG IN" or "CREATE ADMINISTRATOR" */
const char *hmi_ui_login_status_text(void);
const char *hmi_ui_login_field_text(int field);  /* 0 user name, 1 password, 2 confirmation */
void hmi_ui_press_login_button(void);            /* header button: opens the screen, or logs out */
void hmi_ui_login_fill(const char *user, const char *pw, const char *confirm); /* as typed */
void hmi_ui_login_submit(void);                  /* OK button / keyboard check mark */
void hmi_ui_login_cancel(void);                  /* CANCEL button / keyboard cross */

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

/* Settings screen (screen number 3) and its views. Views: 0 system, 1 accounts.
 * Accounts buttons: 0 ADD, 1 NEW (role of the next account), 2 PASSWORD, 3 ROLE, 4 REMOVE.
 * Each press runs exactly what the on-screen button runs; the core decides. */
#define HMI_UI_SCREEN_SETTINGS 3
#define HMI_UI_VIEW_SYSTEM     0
#define HMI_UI_VIEW_ACCOUNTS   1
size_t hmi_ui_settings_view(void);
size_t hmi_ui_settings_view_count(void);
void hmi_ui_press_settings_view(size_t view);
const char *hmi_ui_system_text(size_t line);     /* 0 PLC, 1 writes, 2 configuration, 3 storage, 4 clock, 5 tags, 6 build */
size_t hmi_ui_users_rows(void);
const char *hmi_ui_users_row_text(size_t row, int column);   /* 0 account, 1 role, 2 state */
void hmi_ui_users_select(size_t row);
void hmi_ui_press_users(int button);
bool hmi_ui_users_button_enabled(int button);
const char *hmi_ui_users_status_text(void);
const char *hmi_ui_users_message_text(void);     /* why the list is not shown, "" while it is */
const char *hmi_ui_users_selected(void);
int hmi_ui_users_new_role(void);                 /* a hmi_role_t */
size_t hmi_ui_users_pages(void);
size_t hmi_ui_users_page(void);
void hmi_ui_press_users_page(int dir);

/* The CONFIG view (settings view 2): an Engineer or Administrator edits the alarm settings of a tag or the
 * PLC address, then APPLY twice sends one request to the root helper through the spool directory.
 * hmi_ui_set_config_edit() is called by the program when the configuration was verified: `spool_dir`
 * is --spool (NULL: the view stays off) and `base_serial` is the serial of the signature in use.
 * Rows of a tag: 0 HIGH-HIGH, 1 HIGH, 2 LOW, 3 LOW-LOW, 4 DEADBAND, 5 ON DELAY, 6 OFF DELAY, 7 PRIORITY;
 * of the PLC (target 0): 0 HOST, 1 PORT, 2 UNIT. Keys: "0" to "9", ".", "-", "DEL", "OFF", "OK", "CANCEL".
 * Buttons: 0 APPLY, 1 DISCARD. Each press runs exactly what the on-screen control runs. */
#define HMI_UI_VIEW_CONFIG 2
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
