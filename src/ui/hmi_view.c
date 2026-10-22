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

const char *hmi_view_alarm_point_name(const char *const *names, size_t n_names, size_t point)
{
    return point < n_names ? names[point] : "PLC LINK";
}

void hmi_view_age_text(uint64_t ms, char *buf, size_t cap)
{
    uint64_t s = ms / 1000u;
    if (s < 60u) snprintf(buf, cap, "%us", (unsigned)s);
    else if (s < 3600u) snprintf(buf, cap, "%um%02us", (unsigned)(s / 60u), (unsigned)(s % 60u));
    else if (s < 100u * 3600u) snprintf(buf, cap, "%uh%02um", (unsigned)(s / 3600u), (unsigned)((s % 3600u) / 60u));
    else snprintf(buf, cap, ">99h");
}

void hmi_view_page_text(size_t page, size_t pages, char *buf, size_t cap)
{
    if (pages == 0) snprintf(buf, cap, "0/0");
    else snprintf(buf, cap, "%u/%u", (unsigned)(page + 1), (unsigned)pages);
}

const char *hmi_view_no_alarms_text(void) { return "no active alarms"; }

const char *hmi_view_ack_changed_text(void) { return "ALARM CHANGED: check the list"; }

void hmi_view_axis_text(double v, char *buf, size_t cap)
{
    snprintf(buf, cap, "%.4g", v);
}

void hmi_view_trend_status_text(const char *name, const char *units, int decimals, bool any, double lo, double hi,
                                const char *span, char *buf, size_t cap)
{
    if (decimals < 0) decimals = 0;
    if (decimals > 3) decimals = 3;
    if (!any) snprintf(buf, cap, "%s: no good data in the last %s", name, span);
    else snprintf(buf, cap, "%s  min %.*f  max %.*f %s  (last %s)", name, decimals, lo, decimals, hi, units, span);
}

const char *hmi_view_no_trend_text(void)
{
    return "No tag is trended. Set \"trend\": true on a tag in the configuration.";
}

const char *hmi_view_priority_text(hmi_priority_t p)
{
    switch (p) {
        case HMI_PRI_LOW: return "LOW";
        case HMI_PRI_MEDIUM: return "MED";
        case HMI_PRI_HIGH: return "HIGH";
        case HMI_PRI_CRITICAL: return "CRIT";
    }
    return "?";
}

hmi_tone_t hmi_view_priority_tone(hmi_priority_t p)
{
    return p == HMI_PRI_CRITICAL ? HMI_TONE_BAD : p == HMI_PRI_HIGH ? HMI_TONE_WARN
         : p == HMI_PRI_MEDIUM ? HMI_TONE_TEXT : HMI_TONE_MUTED;
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
        case HMI_WR_RESERVED: return "RECORDING (not sent yet)";
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
        case HMI_WRS_ERR_RATE_LIMITED: return "too many writes, wait";
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
        case HMI_ACK_PENDING: return "ACK PENDING: recording";
    }
    return "ACK: unknown result";
}

hmi_tone_t hmi_view_ack_result_tone(hmi_ack_result_t r)
{
    return r == HMI_ACK_OK ? HMI_TONE_GOOD
         : r == HMI_ACK_NOTHING ? HMI_TONE_MUTED
         : r == HMI_ACK_PENDING ? HMI_TONE_WARN
         : HMI_TONE_BAD;
}

void hmi_view_storage_fault_text(char *buf, size_t cap)
{
    snprintf(buf, cap, "STORAGE FAULT: records not saved");
}

/* ---- login and session wording ---- */
static void upper_into(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) {
        char c = src[i];
        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    dst[i] = '\0';
}

void hmi_view_role_label(hmi_role_t role, const char *user, char *buf, size_t cap)
{
    if (cap == 0) return;
    char r[16];
    upper_into(r, sizeof(r), hmi_role_name(role));
    if (role == HMI_ROLE_VIEWER || !user || !user[0]) snprintf(buf, cap, "%s", r);
    else snprintf(buf, cap, "%s  %s", r, user);
}

hmi_tone_t hmi_view_role_tone(hmi_role_t role)
{
    return role == HMI_ROLE_VIEWER ? HMI_TONE_MUTED : HMI_TONE_GOOD;
}

void hmi_view_wait_text(uint32_t ms, char *buf, size_t cap)
{
    uint32_t s = ms / 1000u + ((ms % 1000u) ? 1u : 0u);
    snprintf(buf, cap, "%u:%02u", (unsigned)(s / 60u), (unsigned)(s % 60u));
}

void hmi_view_login_text(hmi_login_result_t r, uint32_t wait_ms, const char *user, char *buf, size_t cap)
{
    char w[16];
    hmi_view_wait_text(wait_ms, w, sizeof(w));
    switch (r) {
        case HMI_LOGIN_OK: snprintf(buf, cap, "Logged in as %s", user ? user : ""); break;
        case HMI_LOGIN_PENDING: snprintf(buf, cap, "Checking, please wait"); break;
        case HMI_LOGIN_BAD:
            if (wait_ms >= HMI_AUTH_LOCK_BASE_MS)
                snprintf(buf, cap, "Wrong user name or password. Locked, try again in %s", w);
            else
                snprintf(buf, cap, "Wrong user name or password");
            break;
        case HMI_LOGIN_LOCKED: snprintf(buf, cap, "Too many failed attempts. Try again in %s", w); break;
        case HMI_LOGIN_WAIT: snprintf(buf, cap, "Please wait %s before trying again", w); break;
        case HMI_LOGIN_BUSY: snprintf(buf, cap, "Checking, please wait"); break;
        case HMI_LOGIN_NO_STORE: snprintf(buf, cap, "No accounts yet. Create the administrator."); break;
        case HMI_LOGIN_STORE_INVALID:
            snprintf(buf, cap, "User store is damaged. Login is disabled. An administrator must repair it.");
            break;
        case HMI_LOGIN_SESSION_ACTIVE: snprintf(buf, cap, "Already logged in"); break;
        case HMI_LOGIN_AUDIT_FAILED: snprintf(buf, cap, "Login refused: the audit log cannot be written"); break;
        case HMI_LOGIN_INPUT: snprintf(buf, cap, "Password is too long (64 characters at most)"); break;
        case HMI_LOGIN_ERROR: snprintf(buf, cap, "Internal error, please try again"); break;
        default: snprintf(buf, cap, "Login: unknown result"); break;
    }
}

hmi_tone_t hmi_view_login_tone(hmi_login_result_t r)
{
    switch (r) {
        case HMI_LOGIN_OK: return HMI_TONE_GOOD;
        case HMI_LOGIN_PENDING:
        case HMI_LOGIN_BUSY:
        case HMI_LOGIN_NO_STORE:
        case HMI_LOGIN_SESSION_ACTIVE: return HMI_TONE_TEXT;
        case HMI_LOGIN_LOCKED:
        case HMI_LOGIN_WAIT: return HMI_TONE_WARN;
        default: return HMI_TONE_BAD;
    }
}

void hmi_view_setup_text(hmi_setup_result_t r, const char *reason, char *buf, size_t cap)
{
    switch (r) {
        case HMI_SETUP_OK: snprintf(buf, cap, "Administrator created. You can log in now."); break;
        case HMI_SETUP_PENDING: snprintf(buf, cap, "Creating the account, please wait"); break;
        case HMI_SETUP_NOT_FIRST_BOOT: snprintf(buf, cap, "An account store already exists"); break;
        case HMI_SETUP_NAME: snprintf(buf, cap, "User name: 3 to 24 characters, a-z 0-9 _ . -"); break;
        case HMI_SETUP_PASSWORD: snprintf(buf, cap, "%s", (reason && reason[0]) ? reason : "Password rejected"); break;
        case HMI_SETUP_BUSY: snprintf(buf, cap, "Working, please wait"); break;
        case HMI_SETUP_AUDIT_FAILED: snprintf(buf, cap, "Not created: the audit log cannot be written"); break;
        case HMI_SETUP_SAVE_FAILED: snprintf(buf, cap, "Not created: the account file cannot be saved"); break;
        case HMI_SETUP_ERROR: snprintf(buf, cap, "Internal error, please try again"); break;
        default: snprintf(buf, cap, "Setup: unknown result"); break;
    }
}

hmi_tone_t hmi_view_setup_tone(hmi_setup_result_t r)
{
    if (r == HMI_SETUP_OK) return HMI_TONE_GOOD;
    if (r == HMI_SETUP_PENDING || r == HMI_SETUP_BUSY) return HMI_TONE_TEXT;
    return HMI_TONE_BAD;
}

const char *hmi_view_setup_mismatch_text(void) { return "The two passwords do not match"; }

void hmi_view_uadm_text(hmi_uadm_result_t r, hmi_view_user_action_t action, const char *reason, char *buf, size_t cap)
{
    static const char *const done[] = { "Account added", "Password changed", "Role changed", "Account removed" };
    switch (r) {
        case HMI_UADM_OK: snprintf(buf, cap, "%s", (unsigned)action < 4u ? done[action] : "Done"); break;
        case HMI_UADM_PENDING: snprintf(buf, cap, "Working, please wait"); break;
        case HMI_UADM_DENIED: snprintf(buf, cap, "Not allowed: log in as an administrator"); break;
        case HMI_UADM_BUSY: snprintf(buf, cap, "Busy, please try again"); break;
        case HMI_UADM_NO_STORE: snprintf(buf, cap, "There is no usable account store"); break;
        case HMI_UADM_NAME: snprintf(buf, cap, "User name: 3 to 24 characters, a-z 0-9 _ . -"); break;
        case HMI_UADM_PASSWORD: snprintf(buf, cap, "%s", (reason && reason[0]) ? reason : "Password rejected"); break;
        case HMI_UADM_ROLE: snprintf(buf, cap, "That role cannot be given"); break;
        case HMI_UADM_EXISTS: snprintf(buf, cap, "That account already exists"); break;
        case HMI_UADM_NOT_FOUND: snprintf(buf, cap, "There is no such account"); break;
        case HMI_UADM_FULL: snprintf(buf, cap, "No room: %d accounts is the most", HMI_USERS_MAX); break;
        case HMI_UADM_LAST_ADMIN: snprintf(buf, cap, "The last administrator cannot be removed or changed"); break;
        case HMI_UADM_SELF: snprintf(buf, cap, "Not allowed on your own account"); break;
        case HMI_UADM_AUDIT_FAILED: snprintf(buf, cap, "Not changed: the audit log cannot be written"); break;
        case HMI_UADM_SAVE_FAILED: snprintf(buf, cap, "Not changed: the account file cannot be saved"); break;
        case HMI_UADM_ERROR: snprintf(buf, cap, "Internal error, please try again"); break;
        default: snprintf(buf, cap, "Account change: unknown result"); break;
    }
}

hmi_tone_t hmi_view_uadm_tone(hmi_uadm_result_t r)
{
    switch (r) {
        case HMI_UADM_OK: return HMI_TONE_GOOD;
        case HMI_UADM_PENDING: return HMI_TONE_TEXT;
        case HMI_UADM_BUSY: case HMI_UADM_NAME: case HMI_UADM_PASSWORD: case HMI_UADM_ROLE: case HMI_UADM_EXISTS:
        case HMI_UADM_NOT_FOUND: case HMI_UADM_FULL: case HMI_UADM_LAST_ADMIN: case HMI_UADM_SELF:
            return HMI_TONE_WARN;
        default: return HMI_TONE_BAD;
    }
}

void hmi_view_cfgreq_text(hmi_cfgreq_result_t r, char *buf, size_t cap)
{
    switch (r) {
        case HMI_CFGREQ_OK: snprintf(buf, cap, "Request sent. Waiting for the configuration helper"); break;
        case HMI_CFGREQ_DENIED: snprintf(buf, cap, "Not allowed: log in as an engineer or administrator"); break;
        case HMI_CFGREQ_EMPTY: snprintf(buf, cap, "Nothing to send: no change was made"); break;
        case HMI_CFGREQ_BUSY: snprintf(buf, cap, "An earlier request has not been dealt with yet"); break;
        case HMI_CFGREQ_AUDIT_FAILED: snprintf(buf, cap, "Not sent: the audit log cannot be written"); break;
        case HMI_CFGREQ_WRITE_FAILED: snprintf(buf, cap, "Not sent: the spool directory cannot be written"); break;
        case HMI_CFGREQ_TOO_LARGE: snprintf(buf, cap, "Not sent: too many changes in one request"); break;
        default: snprintf(buf, cap, "Request: unknown result"); break;
    }
}

hmi_tone_t hmi_view_cfgreq_tone(hmi_cfgreq_result_t r)
{
    switch (r) {
        case HMI_CFGREQ_OK: return HMI_TONE_GOOD;
        case HMI_CFGREQ_EMPTY: case HMI_CFGREQ_BUSY: return HMI_TONE_WARN;
        default: return HMI_TONE_BAD;
    }
}

void hmi_view_cfgstate_text(hmi_cfgreq_state_t s, const char *line, char *buf, size_t cap)
{
    switch (s) {
        case HMI_CFGREQ_PENDING: snprintf(buf, cap, "Waiting for the configuration helper"); break;
        case HMI_CFGREQ_APPLIED: snprintf(buf, cap, "%s. The service restarts to use it", (line && line[0]) ? line : "applied"); break;
        case HMI_CFGREQ_REJECTED: snprintf(buf, cap, "%s", (line && line[0]) ? line : "rejected"); break;
        default: buf[0] = '\0'; break;
    }
}

hmi_tone_t hmi_view_cfgstate_tone(hmi_cfgreq_state_t s)
{
    return s == HMI_CFGREQ_APPLIED ? HMI_TONE_GOOD : s == HMI_CFGREQ_REJECTED ? HMI_TONE_BAD : HMI_TONE_TEXT;
}

void hmi_view_cfg_number_text(double v, char *buf, size_t cap)
{
    snprintf(buf, cap, "%.6g", v);
}

const char *hmi_view_priority_caps(hmi_priority_t p)
{
    switch (p) {
        case HMI_PRI_LOW: return "LOW";
        case HMI_PRI_MEDIUM: return "MEDIUM";
        case HMI_PRI_HIGH: return "HIGH";
        case HMI_PRI_CRITICAL: return "CRITICAL";
        default: return "?";
    }
}

const char *hmi_view_config_need_engineer_text(void) { return "Changing settings needs an engineer or an administrator. Log in first."; }
const char *hmi_view_config_unavailable_text(void) { return "Changing settings from the screen is not set up on this device (no spool directory was given)."; }

const char *hmi_view_role_caps(hmi_role_t role)
{
    switch (role) {
        case HMI_ROLE_VIEWER: return "VIEWER";
        case HMI_ROLE_OPERATOR: return "OPERATOR";
        case HMI_ROLE_ENGINEER: return "ENGINEER";
        case HMI_ROLE_ADMIN: return "ADMIN";
    }
    return "?";
}

void hmi_view_account_state_text(uint32_t wait_ms, char *buf, size_t cap)
{
    if (wait_ms == 0) {
        if (cap) buf[0] = '\0';
        return;
    }
    char w[16];
    hmi_view_wait_text(wait_ms, w, sizeof(w));
    snprintf(buf, cap, "LOCKED %s", w);
}

const char *hmi_view_users_need_admin_text(void) { return "Log in as an administrator to manage accounts."; }
const char *hmi_view_users_unavailable_text(void) { return "Accounts are not available in this test build."; }

void hmi_view_plc_text(const char *host, unsigned port, unsigned unit, char *buf, size_t cap)
{
    snprintf(buf, cap, "PLC %s:%u  unit %u", host, port, unit);
}

void hmi_view_writes_text(bool master, bool effective, char *buf, size_t cap)
{
    if (effective) snprintf(buf, cap, "Writes: enabled");
    else if (!master) snprintf(buf, cap, "Writes: off (switched off in the configuration)");
    else snprintf(buf, cap, "Writes: off (no audit trail to record them in)");
}

void hmi_view_storage_text(bool fault, uint32_t written, uint32_t failed, char *buf, size_t cap)
{
    if (fault) snprintf(buf, cap, "Storage: FAULT, records are not saved (%u failed)", (unsigned)failed);
    else snprintf(buf, cap, "Storage: ok, %u records written", (unsigned)written);
}

void hmi_view_clock_text(bool valid, bool synced, char *buf, size_t cap)
{
    if (!valid) snprintf(buf, cap, "Clock: cannot be read");
    else if (synced) snprintf(buf, cap, "Clock: synchronised with a time source");
    else snprintf(buf, cap, "Clock: NOT synchronised, dates are not reliable");
}

void hmi_view_tags_text(size_t tags, size_t trended, size_t with_alarm, char *buf, size_t cap)
{
    snprintf(buf, cap, "Tags: %u   with alarms: %u   trended: %u", (unsigned)tags, (unsigned)with_alarm, (unsigned)trended);
}

void hmi_view_test_role_banner(hmi_role_t role, char *buf, size_t cap)
{
    snprintf(buf, cap, "TEST ROLE %s - NOT AUTHENTICATED", hmi_role_name(role));
}
