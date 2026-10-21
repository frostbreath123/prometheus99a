/**
 * @file hmi_view.h
 * @brief Pure display view-model: runtime state -> text and a semantic tone.
 *
 * No LVGL, no globals, no I/O, no locking. Every function is a deterministic
 * function of its arguments, so the whole presentation policy (value text,
 * quality wording, alarm wording, link wording, setpoint stepping, write
 * outcome wording, journal text) is unit-testable on any host without a
 * display. The LVGL binder in hmi_ui.c only maps a tone to a colour and sets
 * label text.
 */
#ifndef HMI_VIEW_H
#define HMI_VIEW_H

#include <stddef.h>
#include <stdint.h>
#include "../hmi/hmi_comms.h"
#include "../hmi/hmi_alarm.h"
#include "../hmi/hmi_writes.h"
#include "../hmi/hmi_auth.h"

/* Semantic colour of a piece of text; the binder owns the actual colours. */
typedef enum {
    HMI_TONE_TEXT = 0,
    HMI_TONE_MUTED,
    HMI_TONE_GOOD,
    HMI_TONE_WARN,
    HMI_TONE_BAD
} hmi_tone_t;

/* Number of decimals needed to show a tag with this scale exactly (0..3). */
int hmi_view_decimals(double scale);

/* Tag value cell: "---" when quality is UNAVAILABLE or BAD, else "<v> <units>". */
void hmi_view_value_text(const hmi_tag_cfg_t *c, const hmi_tag_value_t *v, char *buf, size_t cap);
hmi_tone_t hmi_view_value_tone(hmi_quality_t q);

/* Quality cell. */
const char *hmi_view_quality_text(const hmi_tag_value_t *v);
hmi_tone_t hmi_view_quality_tone(hmi_quality_t q);

/* Alarm cell; empty string when the point is NORMAL. */
void hmi_view_alarm_text(const hmi_alarm_point_t *p, char *buf, size_t cap);
hmi_tone_t hmi_view_alarm_tone(const hmi_alarm_point_t *p);

/* Header link and alarm-summary labels. */
void hmi_view_link_text(hmi_link_health_t h, char *buf, size_t cap);
hmi_tone_t hmi_view_link_tone(hmi_link_health_t h);
void hmi_view_alarm_summary_text(size_t unacked, char *buf, size_t cap);
hmi_tone_t hmi_view_alarm_summary_tone(size_t unacked);

/* Setpoint panel. Step is 1% of the write span, never below one display unit;
 * the result is clamped to [write_min, write_max]. dir is -1 or +1. */
double hmi_view_setpoint_step(const hmi_tag_cfg_t *c, double current, int dir);
void hmi_view_setpoint_text(const hmi_tag_cfg_t *c, double value, char *buf, size_t cap);

/* Write outcome wording. */
const char *hmi_view_write_state_text(hmi_wr_state_t s);
const char *hmi_view_submit_text(hmi_wr_submit_t s);
void hmi_view_write_status_text(const hmi_write_rec_t *rec, char *buf, size_t cap);
hmi_tone_t hmi_view_write_status_tone(hmi_wr_state_t s);
void hmi_view_submit_refused_text(hmi_wr_submit_t r, char *buf, size_t cap);

/* Alarm journal text for the `n` entries in j, printed from j[n-1] down to
 * j[0] (the order hmi_alarm_journal_copy output has always been shown in).
 * `names` holds `n_names` tag names; an entry whose point is >= n_names is the comms
 * alarm. An empty journal gives "no alarm events". */
void hmi_view_journal_text(const hmi_alarm_log_t *j, size_t n, const char *const *names, size_t n_names,
                           char *buf, size_t cap);


/* Liveness mark shown in the header: "HB nn" with nn = seconds modulo 100.
 * Changes once a second, so a frozen screen is visible to the operator. */
void hmi_view_heartbeat_text(uint32_t seconds, char *buf, size_t cap);


/* Outcome of an ACK / ACK ALL request, as returned by the core. */
const char *hmi_view_ack_result_text(hmi_ack_result_t r);
hmi_tone_t hmi_view_ack_result_tone(hmi_ack_result_t r);

/* ---- login and session wording ---- */
/* Header label: "VIEWER" when logged out, else "<ROLE>  <user>" with the role in
 * capitals. A text longer than the buffer is cut, never overflowed. */
void hmi_view_role_label(hmi_role_t role, const char *user, char *buf, size_t cap);
hmi_tone_t hmi_view_role_tone(hmi_role_t role);

/* Time as M:SS, rounded UP to whole seconds so "0:00" is never shown while a
 * wait remains. */
void hmi_view_wait_text(uint32_t ms, char *buf, size_t cap);

/* One message per outcome. The wording for a wrong user name and a wrong
 * password is identical, so the screen does not reveal which accounts exist. */
void hmi_view_login_text(hmi_login_result_t r, uint32_t wait_ms, const char *user, char *buf, size_t cap);
hmi_tone_t hmi_view_login_tone(hmi_login_result_t r);
void hmi_view_setup_text(hmi_setup_result_t r, const char *reason, char *buf, size_t cap);
hmi_tone_t hmi_view_setup_tone(hmi_setup_result_t r);
const char *hmi_view_setup_mismatch_text(void);
/* Shown permanently when the process runs with --role (a test build only). */
void hmi_view_test_role_banner(hmi_role_t role, char *buf, size_t cap);

#endif /* HMI_VIEW_H */
