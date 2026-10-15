#include "hmi_view.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int hmi_view_decimals(double scale)
{
    for (int d = 0; d <= 3; d++) {
        double m = scale * pow(10.0, d);
        if (fabs(m - floor(m + 0.5)) < 1e-9) return d;
    }
    return 3;
}

void hmi_view_value_text(const hmi_tag_cfg_t *c, const hmi_tag_value_t *v, char *buf, size_t cap)
{
    if (v->quality == HMI_Q_UNAVAILABLE || v->quality == HMI_Q_BAD) {
        snprintf(buf, cap, "---");
    } else {
        snprintf(buf, cap, "%.*f %s", hmi_view_decimals(c->scale), v->eng, c->units);
    }
}

hmi_tone_t hmi_view_value_tone(hmi_quality_t q)
{
    return q == HMI_Q_GOOD ? HMI_TONE_TEXT : HMI_TONE_MUTED;
}

const char *hmi_view_quality_text(const hmi_tag_value_t *v)
{
    switch (v->quality) {
        case HMI_Q_GOOD: return "GOOD";
        case HMI_Q_UNCERTAIN: return v->reason == HMI_REASON_OUT_OF_RANGE ? "UNCERTAIN RANGE" : "UNCERTAIN";
        case HMI_Q_BAD:
            switch (v->reason) {
                case HMI_REASON_COMM_LOSS: return "BAD COMM";
                case HMI_REASON_EXCEPTION: return "BAD PLC-EXC";
                case HMI_REASON_STALE: return "BAD STALE";
                case HMI_REASON_PROTOCOL: return "BAD PROTO";
                default: return "BAD";
            }
        default: return "NO DATA";
    }
}

hmi_tone_t hmi_view_quality_tone(hmi_quality_t q)
{
    return q == HMI_Q_GOOD ? HMI_TONE_GOOD : q == HMI_Q_UNCERTAIN ? HMI_TONE_WARN
         : q == HMI_Q_BAD ? HMI_TONE_BAD : HMI_TONE_MUTED;
}

void hmi_view_alarm_text(const hmi_alarm_point_t *p, char *buf, size_t cap)
{
    static const char *const kind[] = { "", "LO", "HI", "LOLO", "HIHI", "COMMS" };
    buf[0] = '\0';
    switch (p->state) {
        case HMI_AL_NORMAL: return;
        case HMI_AL_ACTIVE_UNACK: snprintf(buf, cap, "%s UNACK", kind[p->kind]); break;
        case HMI_AL_ACTIVE_ACK: snprintf(buf, cap, "%s ACK", kind[p->kind]); break;
        case HMI_AL_RTN_UNACK: snprintf(buf, cap, "RTN UNACK"); break;
    }
    if (p->data_suppressed) {
        size_t l = strlen(buf);
        snprintf(buf + l, cap - l, " (no data)");
    }
}

hmi_tone_t hmi_view_alarm_tone(const hmi_alarm_point_t *p)
{
    return p->state == HMI_AL_ACTIVE_UNACK ? HMI_TONE_BAD : p->state == HMI_AL_NORMAL ? HMI_TONE_MUTED : HMI_TONE_WARN;
}

void hmi_view_link_text(hmi_link_health_t h, char *buf, size_t cap)
{
    snprintf(buf, cap, "PLC: %s", hmi_link_health_text(h));
}

hmi_tone_t hmi_view_link_tone(hmi_link_health_t h)
{
    return h == HMI_LINK_HEALTHY ? HMI_TONE_GOOD : h == HMI_LINK_DEGRADED ? HMI_TONE_WARN : HMI_TONE_BAD;
}

void hmi_view_alarm_summary_text(size_t unacked, char *buf, size_t cap)
{
    snprintf(buf, cap, unacked ? "ALARMS: %u UNACK" : "ALARMS: none unacked", (unsigned)unacked);
}

hmi_tone_t hmi_view_alarm_summary_tone(size_t unacked)
{
    return unacked ? HMI_TONE_BAD : HMI_TONE_MUTED;
}

double hmi_view_setpoint_step(const hmi_tag_cfg_t *c, double current, int dir)
{
    double step = (c->write_max - c->write_min) / 100.0;
    double q = pow(10.0, -hmi_view_decimals(c->scale));
    if (step < q) step = q;
    double v = current + (double)dir * step;
    if (v < c->write_min) v = c->write_min;
    if (v > c->write_max) v = c->write_max;
    return v;
}

void hmi_view_setpoint_text(const hmi_tag_cfg_t *c, double value, char *buf, size_t cap)
{
    snprintf(buf, cap, "SETPOINT %s: %.*f %s", c->name, hmi_view_decimals(c->scale), value, c->units);
}

const char *hmi_view_write_state_text(hmi_wr_state_t s)
{
    switch (s) {
        case HMI_WR_RESERVED: return "RESERVED";
        case HMI_WR_PENDING: return "PENDING";
        case HMI_WR_SENT: return "SENT";
        case HMI_WR_CONFIRMED: return "CONFIRMED";
        case HMI_WR_REJECTED: return "REJECTED";
        case HMI_WR_EXPIRED: return "EXPIRED (not sent)";
        case HMI_WR_UNKNOWN: return "UNKNOWN (sent, no confirmation)";
        case HMI_WR_VERIFY_FAILED: return "VERIFY FAILED";
        default: return "-";
    }
}

const char *hmi_view_submit_text(hmi_wr_submit_t s)
{
    switch (s) {
        case HMI_WRS_OK: return "queued";
        case HMI_WRS_ERR_DISABLED: return "writes disabled";
        case HMI_WRS_ERR_NO_TAG: return "no such tag";
        case HMI_WRS_ERR_NOT_WRITABLE: return "tag not writable";
        case HMI_WRS_ERR_ROLE: return "role not permitted";
        case HMI_WRS_ERR_RANGE: return "value out of range";
        case HMI_WRS_ERR_NOT_REPRESENTABLE: return "value not representable";
        case HMI_WRS_ERR_LINK_DOWN: return "PLC link down";
        case HMI_WRS_ERR_QUEUE_FULL: return "queue full";
        case HMI_WRS_ERR_AUDIT: return "audit unavailable";
    }
    return "?";
}

void hmi_view_write_status_text(const hmi_write_rec_t *rec, char *buf, size_t cap)
{
    snprintf(buf, cap, "WRITE #%u: %s", (unsigned)rec->id, hmi_view_write_state_text(rec->state));
}

hmi_tone_t hmi_view_write_status_tone(hmi_wr_state_t s)
{
    return s == HMI_WR_CONFIRMED ? HMI_TONE_GOOD
         : (s == HMI_WR_PENDING || s == HMI_WR_SENT || s == HMI_WR_RESERVED) ? HMI_TONE_WARN
         : HMI_TONE_BAD;
}

void hmi_view_submit_refused_text(hmi_wr_submit_t r, char *buf, size_t cap)
{
    snprintf(buf, cap, "SEND REFUSED: %s", hmi_view_submit_text(r));
}

void hmi_view_journal_text(const hmi_alarm_log_t *j, size_t n, const char *const *names, size_t n_names,
                           char *buf, size_t cap)
{
    static const char *const ev[] = { "", "RAISED", "ESCALATED", "RTN", "ACKED", "CLEARED" };
    size_t off = 0;
    buf[0] = '\0';
    /* j[n-1] is printed first: this is the order the screen has always shown. */
    for (size_t k = n; k > 0 && off + 60 < cap; k--) {
        const hmi_alarm_log_t *e = &j[k - 1];
        const char *who = (e->point < n_names) ? names[e->point] : "PLC LINK";
        off += (size_t)snprintf(buf + off, cap - off, "%s %s\n", who, ev[e->event < 6 ? e->event : 0]);
    }
    if (!buf[0]) snprintf(buf, cap, "no alarm events");
}

void hmi_view_heartbeat_text(uint32_t seconds, char *buf, size_t cap)
{
    snprintf(buf, cap, "HB %02u", (unsigned)(seconds % 100u));
}

const char *hmi_view_ack_result_text(hmi_ack_result_t r)
{
    switch (r) {
        case HMI_ACK_OK: return "ACKNOWLEDGED";
        case HMI_ACK_NOTHING: return "NOTHING TO ACKNOWLEDGE";
        case HMI_ACK_DENIED: return "ACK DENIED: role not permitted";
        case HMI_ACK_AUDIT_FAILED: return "ACK REFUSED: audit unavailable";
    }
    return "ACK: unknown result";
}

hmi_tone_t hmi_view_ack_result_tone(hmi_ack_result_t r)
{
    return r == HMI_ACK_OK ? HMI_TONE_GOOD : r == HMI_ACK_NOTHING ? HMI_TONE_MUTED : HMI_TONE_BAD;
}
