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

/* rt may be NULL: the UI then shows only `banner` (e.g. config rejected).
 * role is a CLAIMED role (see hmi_authz.h): the UI dims controls the role may
 * not use, but the core is what refuses. */
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

/* All wording and tone decisions live in hmi_view.h (pure, unit-tested); this
 * module only maps tones to colours and sets LVGL label text. */

#endif /* HMI_UI_H */
