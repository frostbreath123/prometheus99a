/**
 * @file ui_priv.h
 * @brief Private interface between the files of the LVGL presentation layer.
 *
 * Not part of the public UI interface (that is hmi_ui.h). Every function here
 * must be called from the UI thread only. hmi_ui.c owns the shared state and
 * the header; each screen lives in its own file and is built once.
 */
#ifndef UI_PRIV_H
#define UI_PRIV_H

#include "hmi_ui.h"
#include "hmi_view.h"
#include <stdio.h>
#include <string.h>

#define UI_TXT 40

/* --- colours (flat, high contrast, ISA-101 style) --- */
#define C_BG     lv_color_hex(0x1B1F24)
#define C_PANEL  lv_color_hex(0x262B31)
#define C_TEXT   lv_color_hex(0xE6E6E6)
#define C_MUTED  lv_color_hex(0x8A9199)
#define C_GOOD   lv_color_hex(0x4CAF50)
#define C_WARN   lv_color_hex(0xFFB300)
#define C_BAD    lv_color_hex(0xE53935)

/* State shared by all screens, defined in hmi_ui.c. */
extern hmi_runtime_t *ui_rt;       /* NULL when no configuration was accepted */
extern hmi_auth_t *ui_auth;        /* production: the session is the only source of the role */
extern hmi_role_t ui_fixed_role;   /* test builds only: used when ui_auth is NULL */
extern hmi_ui_stats_t ui_stats;

/* Helpers, defined in hmi_ui.c. */
lv_color_t ui_tone_color(hmi_tone_t t);
void ui_set_color(lv_obj_t *lbl, uint32_t *cache, lv_color_t c);
void ui_set_text(lv_obj_t *lbl, char *cache, size_t cap, const char *txt);
hmi_role_t ui_current_role(uint64_t now);
const char *ui_current_actor(uint64_t now);
void ui_refresh_role(bool force);
lv_obj_t *ui_mk_label(lv_obj_t *parent, int x, int y, const char *txt, lv_color_t col);
lv_obj_t *ui_mk_button(lv_obj_t *parent, int x, int y, int w, int h, const char *txt, lv_event_cb_t cb, void *ud);

/* ui_login.c: login and first-boot screen, drawn over everything else. */
void ui_login_reset(void);
void ui_login_build(lv_obj_t *scr);
void ui_login_button_cb(lv_event_t *e);   /* header button: opens the screen, or logs out */
void ui_login_tick(uint64_t now);
/* The same layer for an administrator on the settings screen. False if it is already open. */
bool ui_login_open_user_add(hmi_role_t role);
bool ui_login_open_password_reset(const char *name);

/* ui_ack.c: acknowledgement requests and the outcome text. The outcome is
 * shown on every bound label (one per screen that has an ACK status line). */
void ui_ack_reset(void);
void ui_ack_bind(lv_obj_t *status_label);
void ui_ack_all(void);
void ui_ack_all_cb(lv_event_t *e);
/* Acknowledge one alarm point. `shown` is the state the operator saw when
 * pressing: if the alarm has changed since it was drawn nothing is acknowledged. */
void ui_ack_point(size_t point, const hmi_alarm_point_t *shown);
void ui_ack_role_changed(bool may_ack, hmi_role_t role);
void ui_ack_poll(void);
const char *ui_ack_text(void);

/* ---- screens ----
 * A screen is built once into its own page and updated only while it is the
 * visible one (nothing on a hidden screen is touched by the binder). A screen
 * is added by writing its file and adding its table to SCREENS in ui_nav.c. */
#define UI_PAGE_Y 80   /* the pages start below the header and the tab bar */
#define UI_PAGE_H 400

typedef struct {
    const char *title;                 /* tab caption */
    void (*reset)(void);               /* forget every widget pointer and cached text (before a rebuild) */
    void (*build)(lv_obj_t *page);     /* page: 800 x UI_PAGE_H, coordinates are relative to it */
    void (*update)(void);              /* binder tick while visible; ui_rt is not NULL */
} ui_screen_ops_t;

extern const ui_screen_ops_t ui_screen_overview;
extern const ui_screen_ops_t ui_screen_alarms;
extern const ui_screen_ops_t ui_screen_trends;
extern const ui_screen_ops_t ui_screen_settings;

/* The views of the settings screen use the same shape: a page of 624 x UI_PAGE_H. */
#define UI_SUBPAGE_W 624
extern const ui_screen_ops_t ui_sub_system;
extern const ui_screen_ops_t ui_sub_users;
extern const ui_screen_ops_t ui_sub_config;

/* ui_nav.c: tab bar and page switching. */
void ui_nav_reset(void);
void ui_nav_build(lv_obj_t *scr);
void ui_nav_update(void);
size_t ui_nav_count(void);
size_t ui_nav_current(void);
bool ui_nav_select(size_t index);
bool ui_nav_visible(size_t index);

/* ui_overview.c: tag table (paged), setpoint panel, journal, storage banner. */
const char *ui_overview_row_text(size_t row, int column);
const char *ui_overview_row_name(size_t row);
void ui_overview_press_setpoint_send(void);
size_t ui_overview_page(void);
size_t ui_overview_pages(void);
void ui_overview_press_page(int dir);

/* ui_trends.c: min/max chart of one trended tag over a selectable window. */
size_t ui_trends_series(void);
size_t ui_trends_window(void);
size_t ui_trends_points(void);                 /* chart points that hold data */
uint32_t ui_trends_redraws(void);              /* chart redraws since the screen was built */
const char *ui_trends_name(void);
const char *ui_trends_status_text(void);
const char *ui_trends_axis_text(int which);    /* 0 top, 1 bottom */
int32_t ui_trends_chart_value(int which, size_t index);   /* 0 upper line, 1 lower line; LV_CHART_POINT_NONE = gap */
void ui_trends_press_series(int dir);
void ui_trends_press_window(size_t index);

/* ui_settings.c: the settings screen (a list of views on the left, one view on the right). */
size_t ui_settings_view(void);
size_t ui_settings_view_count(void);
bool ui_settings_select(size_t view);
const char *ui_settings_system_text(size_t line);
const char *ui_config_info_text(void);       /* set by hmi_ui_set_config_info() */
void ui_config_info_set(const char *text);

/* ui_users.c: accounts, for an administrator. The list is read from hmi_auth at every
 * update, so it is never out of date. */
size_t ui_users_rows(void);                  /* rows in use on the current page */
const char *ui_users_row_text(size_t row, int column);    /* 0 name, 1 role, 2 state */
void ui_users_select_row(size_t row);
typedef enum { UI_USERS_BTN_ADD = 0, UI_USERS_BTN_NEW_ROLE, UI_USERS_BTN_PASSWORD, UI_USERS_BTN_ROLE, UI_USERS_BTN_REMOVE } ui_users_btn_t;
void ui_users_press(ui_users_btn_t button);
bool ui_users_button_enabled(ui_users_btn_t button);
const char *ui_users_status_text(void);
const char *ui_users_message_text(void);     /* why the list is not shown, "" when it is */
const char *ui_users_selected(void);
hmi_role_t ui_users_new_role(void);
size_t ui_users_page(void);
size_t ui_users_pages(void);
void ui_users_press_page(int dir);
/* The settings view hears of a finished change from the layer that took the password. */
void ui_users_changed(hmi_view_user_action_t action, hmi_uadm_result_t result);

/* ui_config.c: changing alarm limits, deadband, delays, priority and the PLC address. Nothing is changed
 * here: the edits become one request for the root helper (hmi_cfgedit.h). */
typedef enum { UI_CONFIG_BTN_APPLY = 0, UI_CONFIG_BTN_DISCARD } ui_config_btn_t;
void ui_config_set_edit(const char *spool_dir, uint32_t base_serial);   /* NULL or "" turns the view off */
size_t ui_config_targets(void);              /* the PLC and every tag */
size_t ui_config_target(void);
const char *ui_config_target_text(void);
void ui_config_press_target(int dir);
size_t ui_config_rows(void);                 /* rows shown for the current target */
const char *ui_config_row_text(size_t row, int column);   /* 0 setting, 1 current value, 2 edit waiting ("-> 82") */
void ui_config_press_row(size_t row);        /* cycles priority and host, opens the number pad for the rest */
bool ui_config_keypad_open(void);
const char *ui_config_keypad_text(void);
void ui_config_press_key(const char *key);   /* "0".."9", ".", "-", "DEL", "OFF", "OK", "CANCEL" */
void ui_config_press(ui_config_btn_t button);
bool ui_config_button_enabled(ui_config_btn_t button);
size_t ui_config_pending(void);              /* edits waiting to be sent */
const char *ui_config_status_text(void);
const char *ui_config_result_text(void);     /* what the helper did with the last request */
const char *ui_config_message_text(void);    /* why the view shows nothing, "" while it does */

/* ui_alarms.c: the alarm list, most urgent first, with a per-alarm ACK. */
size_t ui_alarms_rows(void);
const char *ui_alarms_row_text(size_t row, int column);
bool ui_alarms_ack_enabled(size_t row);
void ui_alarms_press_ack(size_t row);
size_t ui_alarms_page(void);
size_t ui_alarms_pages(void);
void ui_alarms_press_page(int dir);

#endif /* UI_PRIV_H */
