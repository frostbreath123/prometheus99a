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

/* All wording and tone decisions live in hmi_view.h (pure, unit-tested); this
 * module only maps tones to colours and sets LVGL label text. */

#endif /* HMI_UI_H */
