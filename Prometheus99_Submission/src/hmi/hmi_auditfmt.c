#include "hmi_auditfmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hmi_wall_now(hmi_wall_t *w)
{
    memset(w, 0, sizeof(*w));
    w->valid = hmi_wall_ms(&w->unix_ms, &w->synced);
    if (!w->valid) w->synced = false;
}

/* Civil date from days since 1970-01-01 (proleptic Gregorian calendar). */
static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

size_t hmi_wall_format(const hmi_wall_t *w, char *buf, size_t cap)
{
    int n;
    if (!w->valid) {
        n = snprintf(buf, cap, "wall=- sync=no");
    } else {
        uint64_t secs = w->unix_ms / 1000u;
        unsigned ms = (unsigned)(w->unix_ms % 1000u);
        int y, mo, d;
        civil_from_days((int64_t)(secs / 86400u), &y, &mo, &d);
        unsigned sod = (unsigned)(secs % 86400u);
        n = snprintf(buf, cap, "wall=%04d-%02d-%02dT%02u:%02u:%02u.%03uZ sync=%s", y, mo, d, sod / 3600u,
                     (sod / 60u) % 60u, sod % 60u, ms, w->synced ? "ok" : "no");
    }
    if (n <= 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

static const char *quality_word(unsigned q)
{
    switch (q) {
        case HMI_Q_GOOD: return "GOOD";
        case HMI_Q_UNCERTAIN: return "UNCERTAIN";
        case HMI_Q_BAD: return "BAD";
        default: return "UNAVAILABLE";
    }
}

/* Appends " <stamp>\n" to a line of length off. Returns the new length or 0. */
static size_t finish_line(char *buf, size_t cap, size_t off, const hmi_wall_t *w)
{
    if (off + 2u >= cap) return 0;
    buf[off++] = ' ';
    size_t k = hmi_wall_format(w, buf + off, cap - off);
    if (k == 0 || off + k + 2u > cap) return 0;
    off += k;
    buf[off++] = '\n';
    buf[off] = '\0';
    return off;
}

static uint32_t crc32_bytes(const char *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint8_t)p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

static const char *const k_event[] = { NULL, "RAISED", "ESCALATED", "RTN", "ACKED", "CLEARED" };
static const char *const k_kind[] = { "NONE", "LO", "HI", "LOLO", "HIHI", "COMMS" };
static const char *const k_state[] = { "NORMAL", "ACTIVE_UNACK", "ACTIVE_ACK", "RTN_UNACK" };

size_t hmi_audit_format_journal(char *buf, size_t cap, const hmi_alarm_log_t *e, const char *const *names,
                                size_t n_names, const hmi_wall_t *w)
{
    if (e->event < 1 || e->event > HMI_ALE_CLEARED || e->kind > HMI_ALK_COMMS || e->state_after > HMI_AL_RTN_UNACK)
        return 0;
    const char *who = e->point < n_names ? names[e->point] : "PLC_LINK";
    int n = snprintf(buf, cap, "%llu ALARM_EVENT seq=%u point=%s event=%s kind=%s state=%s value=%.9g ",
                     (unsigned long long)e->ts_ms, (unsigned)e->seq, who, k_event[e->event], k_kind[e->kind],
                     k_state[e->state_after], e->value);
    if (n <= 0 || (size_t)n >= cap) return 0;
    size_t k = hmi_wall_format(w, buf + n, cap - (size_t)n);
    if (k == 0) return 0;
    size_t off = (size_t)n + k;
    uint32_t crc = crc32_bytes(buf, off);
    int m = snprintf(buf + off, cap - off, " crc=%08X\n", (unsigned)crc);
    if (m <= 0 || (size_t)m >= cap - off) return 0;
    return off + (size_t)m;
}

/* The text after "key=" in tok (which is one space-separated token), or NULL. */
static const char *field_value(const char *tok, size_t tlen, const char *key)
{
    size_t kl = strlen(key);
    if (tlen <= kl || memcmp(tok, key, kl) != 0 || tok[kl] != '=') return NULL;
    return tok + kl + 1;
}

static int index_of(const char *const *table, size_t n, const char *s, size_t len)
{
    for (size_t i = 0; i < n; i++) {
        if (table[i] && strlen(table[i]) == len && memcmp(table[i], s, len) == 0) return (int)i;
    }
    return -1;
}

bool hmi_audit_parse_journal(const char *line, size_t len, const char *const *names, size_t n_names,
                             hmi_alarm_log_t *out)
{
    if (len < 20 || len > 400) return false;
    /* the CRC: the last token, " crc=" and exactly eight upper-case hex digits */
    if (len < 13 || memcmp(line + len - 13, " crc=", 5) != 0) return false;
    uint32_t want = 0;
    for (size_t i = len - 8; i < len; i++) {
        char c = line[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return false;
        want = (want << 4) | d;
    }
    if (crc32_bytes(line, len - 13) != want) return false;

    /* split the covered text into tokens: ts, ALARM_EVENT, 6 fields, 2 stamp tokens */
    const char *tok[16];
    size_t tl[16];
    size_t nt = 0, pos = 0, end = len - 13;
    while (pos < end) {
        size_t s = pos;
        while (pos < end && line[pos] != ' ') pos++;
        if (nt >= 16 || pos == s) return false; /* too many tokens, or an empty one (double space) */
        tok[nt] = line + s;
        tl[nt++] = pos - s;
        pos++; /* skip the space */
    }
    if (nt != 10 || tl[1] != 11 || memcmp(tok[1], "ALARM_EVENT", 11) != 0) return false;
    if (!field_value(tok[8], tl[8], "wall") || !field_value(tok[9], tl[9], "sync")) return false;

    char num[40];
    uint64_t ts = 0;
    if (tl[0] == 0 || tl[0] > 19) return false;
    for (size_t i = 0; i < tl[0]; i++) {
        if (tok[0][i] < '0' || tok[0][i] > '9') return false;
        ts = ts * 10u + (uint64_t)(tok[0][i] - '0');
    }
    const char *v = field_value(tok[2], tl[2], "seq");
    if (!v) return false;
    uint64_t seq = 0;
    size_t vl = tl[2] - 4;
    if (vl == 0 || vl > 10) return false;
    for (size_t i = 0; i < vl; i++) {
        if (v[i] < '0' || v[i] > '9') return false;
        seq = seq * 10u + (uint64_t)(v[i] - '0');
    }
    if (seq == 0 || seq > 0xFFFFFFFFull) return false;

    v = field_value(tok[3], tl[3], "point");
    if (!v) return false;
    size_t pl = tl[3] - 6;
    int point = -1;
    if (pl == 8 && memcmp(v, "PLC_LINK", 8) == 0) point = (int)n_names;
    for (size_t i = 0; i < n_names && point < 0; i++) {
        if (strlen(names[i]) == pl && memcmp(names[i], v, pl) == 0) point = (int)i;
    }
    if (point < 0 || point > 255) return false;

    v = field_value(tok[4], tl[4], "event");
    int ev = v ? index_of(k_event, sizeof(k_event) / sizeof(k_event[0]), v, tl[4] - 6) : -1;
    v = field_value(tok[5], tl[5], "kind");
    int kd = v ? index_of(k_kind, sizeof(k_kind) / sizeof(k_kind[0]), v, tl[5] - 5) : -1;
    v = field_value(tok[6], tl[6], "state");
    int st = v ? index_of(k_state, sizeof(k_state) / sizeof(k_state[0]), v, tl[6] - 6) : -1;
    if (ev < 1 || kd < 0 || st < 0) return false;

    v = field_value(tok[7], tl[7], "value");
    if (!v || tl[7] - 6 == 0 || tl[7] - 6 >= sizeof(num)) return false;
    memcpy(num, v, tl[7] - 6);
    num[tl[7] - 6] = '\0';
    char *endp = NULL;
    double value = strtod(num, &endp);
    if (endp == num || *endp != '\0') return false;

    out->seq = (uint32_t)seq;
    out->ts_ms = ts;
    out->point = (uint8_t)point;
    out->kind = (uint8_t)kd;
    out->event = (uint8_t)ev;
    out->state_after = (uint8_t)st;
    out->value = value;
    return true;
}

size_t hmi_audit_stamp_line(char *line, size_t len, size_t cap, const hmi_wall_t *w)
{
    char st[HMI_WALL_STAMP_MAX];
    if (len == 0 || len >= cap || line[len - 1] != '\n') return 0;
    size_t k = hmi_wall_format(w, st, sizeof(st));
    if (k == 0 || len - 1u + 1u + k + 2u > cap) return 0;
    len--;
    line[len++] = ' ';
    memcpy(line + len, st, k);
    len += k;
    line[len++] = '\n';
    line[len] = '\0';
    return len;
}

size_t hmi_audit_format_boot(char *buf, size_t cap, uint64_t mono_ms, const hmi_wall_t *w)
{
    int n = snprintf(buf, cap, "%llu BOOT", (unsigned long long)mono_ms);
    if (n <= 0 || (size_t)n >= cap) return 0;
    return finish_line(buf, cap, (size_t)n, w);
}

size_t hmi_audit_format_write(char *buf, size_t cap, const hmi_write_rec_t *r, const char *event, const hmi_wall_t *w)
{
    char old[32];
    if (r->old_quality == HMI_Q_GOOD) snprintf(old, sizeof(old), "%.6g", r->old_eng);
    else snprintf(old, sizeof(old), "-"); /* no trustworthy previous value to record */
    int n = snprintf(buf, cap,
                     "%llu %s id=%u tag=%u role=%d eng=%.6g raw=0x%08X rb=0x%08X exc=%u user=%s old=%s oldq=%s",
                     (unsigned long long)r->t_submit_ms, event, (unsigned)r->id, (unsigned)r->tag, (int)r->role, r->eng,
                     (unsigned)r->raw, (unsigned)r->readback_raw, (unsigned)r->exc, r->actor, old,
                     quality_word(r->old_quality));
    if (n <= 0 || (size_t)n >= cap) return 0;
    return finish_line(buf, cap, (size_t)n, w);
}

size_t hmi_audit_format_ack(char *buf, size_t cap, const hmi_ack_audit_t *rec, const char *const *names,
                            size_t n_names, const hmi_wall_t *w)
{
    int n = snprintf(buf, cap, "%llu ALARM_ACK role=%s action=%s result=%s n=%u points=", (unsigned long long)rec->t_ms,
                     hmi_role_name(rec->role), rec->all ? "ACK_ALL" : "ACK",
                     rec->result == HMI_ACK_OK ? "GRANTED" : "DENIED", (unsigned)rec->n_points);
    if (n <= 0 || (size_t)n >= cap) return 0;
    size_t off = (size_t)n;
    /* Room that must stay free: " user=<name>", the stamp, the newline. */
    size_t reserve = 8u + strlen(rec->actor) + 1u + HMI_WALL_STAMP_MAX + 2u;
    for (size_t i = 0; i < rec->n_points; i++) {
        const char *who = rec->points[i] < n_names ? names[rec->points[i]] : "PLC_LINK";
        size_t need = strlen(who) + 1u;
        if (off + need + 12u + reserve >= cap) { /* list would not fit: say how many were left out */
            off += (size_t)snprintf(buf + off, cap - off, ",+%u_more", (unsigned)(rec->n_points - i));
            break;
        }
        off += (size_t)snprintf(buf + off, cap - off, "%s%s", i ? "," : "", who);
    }
    if (off + reserve >= cap) return 0; /* never write a line without its user */
    off += (size_t)snprintf(buf + off, cap - off, " user=%s", rec->actor);
    return finish_line(buf, cap, off, w);
}
