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
#include "../hmi/hmi_cfgedit.h"
#include "../hmi/hmi_authz.h"

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

/* Alarm priority: "LOW", "MED", "HIGH" or "CRIT" (a value outside the enum gives "?"). */
const char *hmi_view_priority_text(hmi_priority_t p);
hmi_tone_t hmi_view_priority_tone(hmi_priority_t p);

/* Name of an alarm point in a list: the tag name, or "PLC LINK" for the
 * communications alarm (any point at or beyond n_names). */
const char *hmi_view_alarm_point_name(const char *const *names, size_t n_names, size_t point);
/* Time since an alarm last changed, from a duration in milliseconds: "0s" to
 * "59s", "1m00s" to "59m59s", "1h00m" to "99h59m", and ">99h" beyond. */
void hmi_view_age_text(uint64_t ms, char *buf, size_t cap);
/* "3/7" for the third of seven pages (page is counted from 0); "0/0" with no pages. */
void hmi_view_page_text(size_t page, size_t pages, char *buf, size_t cap);
const char *hmi_view_no_alarms_text(void);
/* Shown instead of acknowledging when the alarm changed after it was drawn. */
const char *hmi_view_ack_changed_text(void);

/* A value on a chart axis, short enough for a 58 pixel label: at most four
 * significant digits ("50.04", "1235", "0.0001235", "1.235e+07"). */
void hmi_view_axis_text(double v, char *buf, size_t cap);
/* The line under a trend chart. With data: "<name>  min <lo> max <hi> <units>
 * (last <span>)", the numbers with `decimals` places. Without: "<name>: no good
 * data in the last <span>". `span` is text such as "10m00s". */
void hmi_view_trend_status_text(const char *name, const char *units, int decimals, bool any, double lo, double hi,
                                const char *span, char *buf, size_t cap);
const char *hmi_view_no_trend_text(void);   /* shown when no tag has "trend": true */

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

/* Banner shown while the storage worker cannot write its files. */
void hmi_view_storage_fault_text(char *buf, size_t cap);

/* ---- the claimed role ---- */
/* Header label: the role in capitals ("OPERATOR"). A text longer than the buffer is cut, never overflowed. */
void hmi_view_role_label(hmi_role_t role, char *buf, size_t cap);

/* ---- changing settings from the screen (the CONFIG view) ---- */
/* One line for the outcome of handing a request to the configuration helper. */
void hmi_view_cfgreq_text(hmi_cfgreq_result_t r, char *buf, size_t cap);
hmi_tone_t hmi_view_cfgreq_tone(hmi_cfgreq_result_t r);
/* One line for what the spool directory shows afterwards. `line` is the first line of the
 * result file as hmi_cfgreq_state() returned it. Empty for HMI_CFGREQ_NONE. */
void hmi_view_cfgstate_text(hmi_cfgreq_state_t s, const char *line, char *buf, size_t cap);
hmi_tone_t hmi_view_cfgstate_tone(hmi_cfgreq_state_t s);
/* A number as the CONFIG view shows it: at most six significant digits, no trailing zeros. */
void hmi_view_cfg_number_text(double v, char *buf, size_t cap);
const char *hmi_view_priority_caps(hmi_priority_t p);   /* "LOW", "MEDIUM", "HIGH", "CRITICAL", "?" */
const char *hmi_view_config_need_engineer_text(void);
const char *hmi_view_config_unavailable_text(void);     /* no spool directory: not set up */

/* ---- the system lines on the settings screen ---- */
void hmi_view_plc_text(const char *host, unsigned port, unsigned unit, char *buf, size_t cap);
/* Why writes are on or off. master: the "writes_enabled" switch of the configuration. */
void hmi_view_writes_text(bool master, bool effective, char *buf, size_t cap);
void hmi_view_storage_text(bool fault, uint32_t written, uint32_t failed, char *buf, size_t cap);
void hmi_view_clock_text(bool valid, bool synced, char *buf, size_t cap);
void hmi_view_tags_text(size_t tags, size_t trended, size_t with_alarm, char *buf, size_t cap);

/* One line about the claimed role (the program has no login): "Role: VIEWER (read only)" or the role with a warning that nothing authenticates it. */
void hmi_view_claimed_role_text(hmi_role_t role, char *buf, size_t cap);

#endif /* HMI_VIEW_H */
