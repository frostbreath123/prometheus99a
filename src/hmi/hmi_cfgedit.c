#include "hmi_cfgedit.h"
#include "hmi_platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ field table */
typedef struct {
    const char *suffix;      /* after "tag.<name>.alarm." */
    hmi_ce_field_t field;
    bool is_limit;           /* may be "off" */
} alarm_key_t;

static const alarm_key_t ALARM_KEYS[] = {
    { "high", HMI_CE_ALARM_HI, true },
    { "high_high", HMI_CE_ALARM_HIHI, true },
    { "low", HMI_CE_ALARM_LO, true },
    { "low_low", HMI_CE_ALARM_LOLO, true },
    { "deadband", HMI_CE_ALARM_DEADBAND, false },
    { "on_ms", HMI_CE_ALARM_ON_MS, false },
    { "off_ms", HMI_CE_ALARM_OFF_MS, false },
    { "priority", HMI_CE_ALARM_PRIORITY, false },
};
#define N_ALARM_KEYS (sizeof(ALARM_KEYS) / sizeof(ALARM_KEYS[0]))

static const char *const PRIORITY_NAME[] = { "low", "medium", "high", "critical" };

static const alarm_key_t *alarm_key_for(hmi_ce_field_t f)
{
    for (size_t i = 0; i < N_ALARM_KEYS; i++)
        if (ALARM_KEYS[i].field == f) return &ALARM_KEYS[i];
    return NULL;
}

/* ---------------------------------------------------------------- strings */
static bool text_char_ok(char c) { return c >= 0x20 && c <= 0x7E; }

/* A tag name as the configuration allows it: printable ASCII, no quote or backslash. */
static bool tag_name_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= HMI_NAME_LEN) return false;
    for (size_t i = 0; i < n; i++)
        if (!text_char_ok(s[i]) || s[i] == '"' || s[i] == '\\' || s[i] == '=' ) return false;
    return true;
}

static bool ipv4_ok(const char *s)
{
    int parts = 0;
    const char *p = s;
    while (*p) {
        if (*p < '0' || *p > '9') return false;
        int v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            if (++digits > 3 || v > 255) return false;
            p++;
        }
        parts++;
        if (*p == '.') {
            p++;
            if (*p == '\0') return false;
        } else if (*p != '\0') {
            return false;
        }
    }
    return parts == 4;
}

/* Digits, an optional fraction and an optional exponent; nothing else. */
static bool parse_number(const char *s, size_t len, double *out)
{
    if (len == 0 || len > 30) return false;
    size_t i = 0;
    if (s[i] == '-') i++;
    size_t d0 = i;
    while (i < len && s[i] >= '0' && s[i] <= '9') i++;
    if (i == d0) return false;
    if (i < len && s[i] == '.') {
        i++;
        size_t f0 = i;
        while (i < len && s[i] >= '0' && s[i] <= '9') i++;
        if (i == f0) return false;
    }
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) i++;
        size_t e0 = i;
        while (i < len && s[i] >= '0' && s[i] <= '9') i++;
        if (i == e0) return false;
    }
    if (i != len) return false;
    char buf[32];
    memcpy(buf, s, len);
    buf[len] = '\0';
    double v = strtod(buf, NULL);
    if ((v - v) != 0.0) return false;   /* not a finite number */
    *out = v;
    return true;
}

static bool parse_uint(const char *s, size_t len, uint32_t *out)
{
    if (len == 0 || len > 10) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10u + (uint64_t)(s[i] - '0');
    }
    if (v > 0xFFFFFFFFu) return false;
    *out = (uint32_t)v;
    return true;
}

static void fmt_double(double v, char *buf, size_t cap)
{
    for (int digits = 15; digits <= 17; digits++) {
        snprintf(buf, cap, "%.*g", digits, v);
        if (strtod(buf, NULL) == v) return;
    }
}

/* ---------------------------------------------------------------- building */
void hmi_cfgedit_init(hmi_cfgedit_request_t *r, uint32_t base_serial, const char *user)
{
    memset(r, 0, sizeof(*r));
    r->base_serial = base_serial;
    hmi_actor_copy(r->user, user);
}

static hmi_ce_change_t *slot(hmi_cfgedit_request_t *r, hmi_ce_field_t field, const char *tag)
{
    for (size_t i = 0; i < r->n; i++)
        if (r->c[i].field == field && strcmp(r->c[i].tag, tag) == 0) return &r->c[i];
    if (r->n >= HMI_CFGEDIT_MAX_CHANGES) return NULL;
    hmi_ce_change_t *c = &r->c[r->n++];
    memset(c, 0, sizeof(*c));
    c->field = field;
    snprintf(c->tag, sizeof(c->tag), "%s", tag);
    return c;
}

bool hmi_cfgedit_set_host(hmi_cfgedit_request_t *r, const char *host)
{
    if (!host || !ipv4_ok(host) || strlen(host) >= HMI_HOST_LEN) return false;
    hmi_ce_change_t *c = slot(r, HMI_CE_PLC_HOST, "");
    if (!c) return false;
    snprintf(c->host, sizeof(c->host), "%s", host);
    return true;
}

bool hmi_cfgedit_set_number(hmi_cfgedit_request_t *r, hmi_ce_field_t field, const char *tag, double value)
{
    bool plc = field == HMI_CE_PLC_PORT || field == HMI_CE_PLC_UNIT;
    if ((value - value) != 0.0) return false;
    if (plc) {
        tag = "";
    } else {
        const alarm_key_t *k = alarm_key_for(field);
        if (!k || field == HMI_CE_ALARM_PRIORITY || !tag || !tag_name_ok(tag)) return false;
    }
    hmi_ce_change_t *c = slot(r, field, tag);
    if (!c) return false;
    c->num = value;
    c->off = false;
    return true;
}

bool hmi_cfgedit_set_limit_off(hmi_cfgedit_request_t *r, hmi_ce_field_t field, const char *tag)
{
    const alarm_key_t *k = alarm_key_for(field);
    if (!k || !k->is_limit || !tag || !tag_name_ok(tag)) return false;
    hmi_ce_change_t *c = slot(r, field, tag);
    if (!c) return false;
    c->num = 0;
    c->off = true;
    return true;
}

bool hmi_cfgedit_set_priority(hmi_cfgedit_request_t *r, const char *tag, hmi_priority_t p)
{
    if ((unsigned)p > 3u || !tag || !tag_name_ok(tag)) return false;
    hmi_ce_change_t *c = slot(r, HMI_CE_ALARM_PRIORITY, tag);
    if (!c) return false;
    c->num = (double)p;
    c->off = false;
    return true;
}

/* ---------------------------------------------------------------- format */
size_t hmi_cfgedit_format(const hmi_cfgedit_request_t *r, char *buf, size_t cap)
{
    if (!r || !buf || cap == 0) return 0;
    size_t len = 0;
#define EMIT(...)                                                      \
    do {                                                               \
        int n_ = snprintf(buf + len, cap - len, __VA_ARGS__);          \
        if (n_ < 0 || (size_t)n_ >= cap - len) {                       \
            return 0;                                                  \
        }                                                              \
        len += (size_t)n_;                                             \
    } while (0)
    EMIT("P99REQ 1\nbase_serial=%u\nuser=%s\n", (unsigned)r->base_serial, r->user[0] ? r->user : "-");
    for (size_t i = 0; i < r->n; i++) {
        const hmi_ce_change_t *c = &r->c[i];
        char num[40];
        switch (c->field) {
            case HMI_CE_PLC_HOST: EMIT("plc.host=%s\n", c->host); break;
            case HMI_CE_PLC_PORT: EMIT("plc.port=%u\n", (unsigned)c->num); break;
            case HMI_CE_PLC_UNIT: EMIT("plc.unit=%u\n", (unsigned)c->num); break;
            default: {
                const alarm_key_t *k = alarm_key_for(c->field);
                if (!k) return 0;
                if (c->off) {
                    EMIT("tag.%s.alarm.%s=off\n", c->tag, k->suffix);
                } else if (c->field == HMI_CE_ALARM_PRIORITY) {
                    if ((unsigned)c->num > 3u) return 0;
                    EMIT("tag.%s.alarm.%s=%s\n", c->tag, k->suffix, PRIORITY_NAME[(unsigned)c->num]);
                } else if (c->field == HMI_CE_ALARM_ON_MS || c->field == HMI_CE_ALARM_OFF_MS) {
                    EMIT("tag.%s.alarm.%s=%u\n", c->tag, k->suffix, (unsigned)c->num);
                } else {
                    fmt_double(c->num, num, sizeof(num));
                    EMIT("tag.%s.alarm.%s=%s\n", c->tag, k->suffix, num);
                }
            }
        }
    }
    EMIT("END\n");
#undef EMIT
    return len;
}

/* ---------------------------------------------------------------- parse */
static bool bad(char *err, size_t cap, const char *msg, const char *detail)
{
    if (err && cap) {
        if (detail) snprintf(err, cap, "%s: %s", msg, detail);
        else snprintf(err, cap, "%s", msg);
    }
    return false;
}

static bool line_is(const char *line, size_t n, const char *lit)
{
    return strlen(lit) == n && memcmp(line, lit, n) == 0;
}

static bool starts(const char *line, size_t n, const char *prefix)
{
    size_t p = strlen(prefix);
    return n > p && memcmp(line, prefix, p) == 0;
}

static bool parse_change(const char *line, size_t n, hmi_ce_change_t *c, char *err, size_t cap)
{
    const char *eq = memchr(line, '=', n);
    if (!eq || eq == line || (size_t)(eq - line) + 1u >= n) return bad(err, cap, "malformed line", NULL);
    size_t klen = (size_t)(eq - line);
    const char *val = eq + 1;
    size_t vlen = n - klen - 1u;
    char key[HMI_CFGEDIT_LINE_MAX];
    if (klen >= sizeof(key)) return bad(err, cap, "key too long", NULL);
    memcpy(key, line, klen);
    key[klen] = '\0';
    memset(c, 0, sizeof(*c));

    if (strcmp(key, "plc.host") == 0) {
        if (vlen >= HMI_HOST_LEN) return bad(err, cap, "bad value", key);
        memcpy(c->host, val, vlen);
        c->host[vlen] = '\0';
        if (!ipv4_ok(c->host)) return bad(err, cap, "bad value", key);
        c->field = HMI_CE_PLC_HOST;
        return true;
    }
    if (strcmp(key, "plc.port") == 0 || strcmp(key, "plc.unit") == 0) {
        uint32_t u;
        if (!parse_uint(val, vlen, &u)) return bad(err, cap, "bad value", key);
        c->field = key[4] == 'p' ? HMI_CE_PLC_PORT : HMI_CE_PLC_UNIT;
        c->num = (double)u;
        return true;
    }
    if (strncmp(key, "tag.", 4) == 0) {
        /* the tag name may hold dots: the field is what follows the last ".alarm." */
        const char *at = NULL;
        for (const char *p = key + 4; (p = strstr(p, ".alarm.")) != NULL; p++) at = p;
        if (!at) return bad(err, cap, "unknown key", key);
        size_t nlen = (size_t)(at - (key + 4));
        if (nlen == 0 || nlen >= HMI_NAME_LEN) return bad(err, cap, "unknown key", key);
        memcpy(c->tag, key + 4, nlen);
        c->tag[nlen] = '\0';
        if (!tag_name_ok(c->tag)) return bad(err, cap, "unknown key", key);
        const char *suffix = at + 7;
        const alarm_key_t *k = NULL;
        for (size_t i = 0; i < N_ALARM_KEYS; i++)
            if (strcmp(suffix, ALARM_KEYS[i].suffix) == 0) k = &ALARM_KEYS[i];
        if (!k) return bad(err, cap, "unknown key", key);
        c->field = k->field;
        if (k->is_limit && vlen == 3 && memcmp(val, "off", 3) == 0) {
            c->off = true;
            return true;
        }
        if (k->field == HMI_CE_ALARM_PRIORITY) {
            for (size_t i = 0; i < 4; i++) {
                if (strlen(PRIORITY_NAME[i]) == vlen && memcmp(PRIORITY_NAME[i], val, vlen) == 0) {
                    c->num = (double)i;
                    return true;
                }
            }
            return bad(err, cap, "bad value", key);
        }
        if (k->field == HMI_CE_ALARM_ON_MS || k->field == HMI_CE_ALARM_OFF_MS) {
            uint32_t u;
            if (!parse_uint(val, vlen, &u)) return bad(err, cap, "bad value", key);
            c->num = (double)u;
            return true;
        }
        if (!parse_number(val, vlen, &c->num)) return bad(err, cap, "bad value", key);
        return true;
    }
    return bad(err, cap, "unknown key", key);
}

static bool parse_request(const char *text, size_t len, hmi_cfgedit_request_t *out, char *err, size_t err_cap);

/* A refused request leaves nothing behind, whichever path refused it. */
bool hmi_cfgedit_parse(const char *text, size_t len, hmi_cfgedit_request_t *out, char *err, size_t err_cap)
{
    if (parse_request(text, len, out, err, err_cap)) return true;
    memset(out, 0, sizeof(*out));
    return false;
}

static bool parse_request(const char *text, size_t len, hmi_cfgedit_request_t *out, char *err, size_t err_cap)
{
    if (err && err_cap) err[0] = '\0';
    memset(out, 0, sizeof(*out));
    if (!text || len == 0) return bad(err, err_cap, "empty request", NULL);
    if (len > HMI_CFGEDIT_REQUEST_MAX) return bad(err, err_cap, "request too large", NULL);
    for (size_t i = 0; i < len; i++)
        if (text[i] != '\n' && !text_char_ok(text[i])) return bad(err, err_cap, "illegal character", NULL);

    size_t pos = 0;
    int stage = 0;                      /* 0 magic, 1 base_serial, 2 user, 3 changes */
    bool have_end = false;
    while (pos < len && !have_end) {
        const char *line = text + pos;
        const char *nl = memchr(line, '\n', len - pos);
        if (!nl) return bad(err, err_cap, "the request is cut off (no line break)", NULL);
        size_t n = (size_t)(nl - line);
        if (n >= HMI_CFGEDIT_LINE_MAX) return bad(err, err_cap, "line too long", NULL);
        pos += n + 1u;
        if (stage == 0) {
            if (!line_is(line, n, "P99REQ 1")) return bad(err, err_cap, "not a request (wrong first line)", NULL);
            stage = 1;
        } else if (stage == 1) {
            uint32_t s;
            if (!starts(line, n, "base_serial=") || !parse_uint(line + 12, n - 12, &s))
                return bad(err, err_cap, "base_serial is missing or not a number", NULL);
            out->base_serial = s;
            stage = 2;
        } else if (stage == 2) {
            if (!starts(line, n, "user=")) return bad(err, err_cap, "user is missing", NULL);
            char u[HMI_CFGEDIT_LINE_MAX];
            memcpy(u, line + 5, n - 5);
            u[n - 5] = '\0';
            hmi_actor_copy(out->user, u);   /* anything that is not an account name becomes "-" */
            stage = 3;
        } else if (line_is(line, n, "END")) {
            have_end = true;
        } else {
            if (out->n >= HMI_CFGEDIT_MAX_CHANGES) return bad(err, err_cap, "too many changes", NULL);
            hmi_ce_change_t c;
            if (!parse_change(line, n, &c, err, err_cap)) {
                memset(out, 0, sizeof(*out));
                return false;
            }
            for (size_t i = 0; i < out->n; i++) {
                if (out->c[i].field == c.field && strcmp(out->c[i].tag, c.tag) == 0) {
                    memset(out, 0, sizeof(*out));
                    return bad(err, err_cap, "the same setting twice", c.tag[0] ? c.tag : "plc");
                }
            }
            out->c[out->n++] = c;
        }
    }
    if (!have_end) {
        memset(out, 0, sizeof(*out));
        return bad(err, err_cap, "the request has no END line", NULL);
    }
    if (pos != len) {
        memset(out, 0, sizeof(*out));
        return bad(err, err_cap, "bytes after END", NULL);
    }
    return true;
}

/* ---------------------------------------------------------------- apply */
static bool apply_one(const hmi_ce_change_t *c, hmi_app_cfg_t *w, char *err, size_t cap, bool *changed)
{
    *changed = false;
    if (c->field == HMI_CE_PLC_HOST) {
        if (!ipv4_ok(c->host)) return bad(err, cap, "plc.host is not an IPv4 address", NULL);
        bool allowed = false;
        for (size_t i = 0; i < w->n_allowed; i++)
            if (strcmp(w->allowed_hosts[i], c->host) == 0) allowed = true;
        if (!allowed) return bad(err, cap, "plc.host is not in plc.allowed_hosts", c->host);
        *changed = strcmp(w->host, c->host) != 0;
        snprintf(w->host, sizeof(w->host), "%s", c->host);
        return true;
    }
    if (c->field == HMI_CE_PLC_PORT) {
        if (c->num < 1 || c->num > 65535 || c->num != floor(c->num)) return bad(err, cap, "plc.port must be 1 to 65535", NULL);
        *changed = w->port != (uint16_t)c->num;
        w->port = (uint16_t)c->num;
        return true;
    }
    if (c->field == HMI_CE_PLC_UNIT) {
        if (c->num < 0 || c->num > 247 || c->num != floor(c->num)) return bad(err, cap, "plc.unit must be 0 to 247", NULL);
        *changed = w->unit != (uint8_t)c->num;
        w->unit = (uint8_t)c->num;
        return true;
    }
    const alarm_key_t *k = alarm_key_for(c->field);
    if (!k) return bad(err, cap, "this setting cannot be changed from the device", NULL);
    hmi_tag_cfg_t *t = NULL;
    for (size_t i = 0; i < w->n_tags; i++)
        if (strcmp(w->tags[i].name, c->tag) == 0) t = &w->tags[i];
    if (!t) return bad(err, cap, "no such tag", c->tag);
    hmi_alarm_cfg_t *a = &t->alarm;
    switch (c->field) {
        case HMI_CE_ALARM_HI: case HMI_CE_ALARM_HIHI: case HMI_CE_ALARM_LO: case HMI_CE_ALARM_LOLO: {
            bool *en = c->field == HMI_CE_ALARM_HI ? &a->en_hi : c->field == HMI_CE_ALARM_HIHI ? &a->en_hihi
                     : c->field == HMI_CE_ALARM_LO ? &a->en_lo : &a->en_lolo;
            double *val = c->field == HMI_CE_ALARM_HI ? &a->hi : c->field == HMI_CE_ALARM_HIHI ? &a->hihi
                        : c->field == HMI_CE_ALARM_LO ? &a->lo : &a->lolo;
            if (c->off) {
                *changed = *en;
                *en = false;
                *val = 0.0;
                return true;
            }
            if (c->num < t->eng_min || c->num > t->eng_max)
                return bad(err, cap, "an alarm limit must lie inside the tag's min and max", c->tag);
            *changed = !*en || *val != c->num;
            *en = true;
            *val = c->num;
            return true;
        }
        case HMI_CE_ALARM_DEADBAND:
            if (c->num < 0.0 || c->num > t->eng_max - t->eng_min) return bad(err, cap, "deadband must be 0 or more and no larger than the tag range", c->tag);
            *changed = a->deadband != c->num;
            a->deadband = c->num;
            return true;
        case HMI_CE_ALARM_ON_MS:
        case HMI_CE_ALARM_OFF_MS: {
            if (c->num < 0 || c->num > 600000 || c->num != floor(c->num)) return bad(err, cap, "the delay must be 0 to 600000 ms", c->tag);
            uint32_t *d = c->field == HMI_CE_ALARM_ON_MS ? &a->on_ms : &a->off_ms;
            *changed = *d != (uint32_t)c->num;
            *d = (uint32_t)c->num;
            return true;
        }
        case HMI_CE_ALARM_PRIORITY:
            if (c->num < 0 || c->num > 3 || c->num != floor(c->num)) return bad(err, cap, "bad priority", c->tag);
            *changed = a->priority != (hmi_priority_t)(int)c->num;
            a->priority = (hmi_priority_t)(int)c->num;
            return true;
        default:
            return bad(err, cap, "this setting cannot be changed from the device", NULL);
    }
}

bool hmi_cfgedit_apply(const hmi_cfgedit_request_t *req, const hmi_app_cfg_t *base, hmi_app_cfg_t *out,
                       char *text, size_t text_cap, size_t *text_len, size_t *n_effective, char *err, size_t err_cap)
{
    hmi_app_cfg_t work;                 /* about 13 KB of stack, as the callers have megabytes */
    if (err && err_cap) err[0] = '\0';
    memset(out, 0, sizeof(*out));
    if (text_len) *text_len = 0;
    if (n_effective) *n_effective = 0;
    if (!req || !base || !text || text_cap < HMI_CONFIG_MAX_BYTES + 1u) return bad(err, err_cap, "internal: bad arguments", NULL);
    if (req->n == 0) return bad(err, err_cap, "the request holds no change", NULL);
    if (base->n_tags == 0) return bad(err, err_cap, "no configuration to change", NULL);

    work = *base;
    size_t effective = 0;
    for (size_t i = 0; i < req->n; i++) {
        bool changed = false;
        if (!apply_one(&req->c[i], &work, err, err_cap, &changed)) return false;
        if (changed) effective++;
    }
    if (effective == 0) return bad(err, err_cap, "nothing would change", NULL);

    size_t n = hmi_config_serialize(&work, text, text_cap);
    if (n == 0) return bad(err, err_cap, "the changed configuration cannot be written out", NULL);
    char perr[120];
    if (!hmi_config_parse(text, n, out, perr, sizeof(perr))) {
        memset(out, 0, sizeof(*out));
        return bad(err, err_cap, "the changed configuration is invalid", perr);
    }
    if (!hmi_config_host_allowed(out)) {
        memset(out, 0, sizeof(*out));
        return bad(err, err_cap, "plc.host is not in plc.allowed_hosts", NULL);
    }
    if (!hmi_config_equal(out, &work)) {          /* what is read back is what was meant */
        memset(out, 0, sizeof(*out));
        return bad(err, err_cap, "internal: the configuration did not read back the same", NULL);
    }
    if (text_len) *text_len = n;
    if (n_effective) *n_effective = effective;
    return true;
}

/* ----------------------------------------------------- the service side */
static bool spool_path(const char *dir, const char *name, char *out, size_t cap)
{
    if (!dir || !dir[0]) return false;
    size_t n = strlen(dir);
    int w = snprintf(out, cap, "%s%s%s", dir, (dir[n - 1] == '/') ? "" : "/", name);
    return w > 0 && (size_t)w < cap;
}

hmi_cfgreq_result_t hmi_cfgreq_submit(const hmi_cfgedit_request_t *req, hmi_role_t role, const char *spool_dir,
                                      hmi_cfgreq_audit_fn audit, void *audit_user, uint64_t now_ms)
{
    char line[160];
    if (!hmi_authz_allows(role, HMI_ACT_EDIT_SETTINGS, HMI_ROLE_VIEWER)) {
        if (audit) {
            int n = snprintf(line, sizeof(line), "%llu CONFIG_REQUEST_DENIED by=%s\n", (unsigned long long)now_ms,
                             req && req->user[0] ? req->user : "-");
            if (n > 0 && (size_t)n < sizeof(line)) audit(audit_user, line, (size_t)n);   /* best effort */
        }
        return HMI_CFGREQ_DENIED;
    }
    if (!req || req->n == 0) return HMI_CFGREQ_EMPTY;
    char path[320];
    if (!spool_path(spool_dir, "request", path, sizeof(path))) return HMI_CFGREQ_WRITE_FAILED;
    uint8_t probe[1];
    size_t got = 0;
    if (hmi_fs_read_all(path, probe, sizeof(probe), &got) != HMI_FS_READ_ABSENT) return HMI_CFGREQ_BUSY;

    char text[HMI_CFGEDIT_REQUEST_MAX + 1];
    size_t len = hmi_cfgedit_format(req, text, sizeof(text));
    if (len == 0) return HMI_CFGREQ_TOO_LARGE;
    if (audit) {
        int n = snprintf(line, sizeof(line), "%llu CONFIG_REQUEST by=%s changes=%u base_serial=%u\n",
                         (unsigned long long)now_ms, req->user[0] ? req->user : "-", (unsigned)req->n,
                         (unsigned)req->base_serial);
        if (n <= 0 || (size_t)n >= sizeof(line) || !audit(audit_user, line, (size_t)n)) return HMI_CFGREQ_AUDIT_FAILED;
    }
    if (!hmi_fs_atomic_write(path, text, len)) {
        if (audit) {
            int n = snprintf(line, sizeof(line), "%llu CONFIG_REQUEST_FAILED by=%s\n", (unsigned long long)now_ms,
                             req->user[0] ? req->user : "-");
            if (n > 0 && (size_t)n < sizeof(line)) audit(audit_user, line, (size_t)n);
        }
        return HMI_CFGREQ_WRITE_FAILED;
    }
    return HMI_CFGREQ_OK;
}

hmi_cfgreq_state_t hmi_cfgreq_state(const char *spool_dir, char *text, size_t text_cap)
{
    char path[320];
    uint8_t probe[1];
    size_t got = 0;
    if (text && text_cap) text[0] = '\0';
    if (!spool_path(spool_dir, "request", path, sizeof(path))) return HMI_CFGREQ_NONE;
    if (hmi_fs_read_all(path, probe, sizeof(probe), &got) != HMI_FS_READ_ABSENT) return HMI_CFGREQ_PENDING;
    if (!spool_path(spool_dir, "result", path, sizeof(path))) return HMI_CFGREQ_NONE;
    char buf[200];
    hmi_fs_read_t r = hmi_fs_read_all(path, buf, sizeof(buf) - 1u, &got);
    if (r == HMI_FS_READ_ABSENT) return HMI_CFGREQ_NONE;
    if (r != HMI_FS_READ_OK && r != HMI_FS_READ_TOO_BIG) {
        if (text && text_cap) snprintf(text, text_cap, "rejected: the result cannot be read");
        return HMI_CFGREQ_REJECTED;
    }
    if (r == HMI_FS_READ_TOO_BIG) got = sizeof(buf) - 1u;
    buf[got] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    for (char *p = buf; *p; p++)
        if (!text_char_ok(*p)) *p = '?';
    if (text && text_cap) snprintf(text, text_cap, "%s", buf);
    return strncmp(buf, "applied", 7) == 0 ? HMI_CFGREQ_APPLIED : HMI_CFGREQ_REJECTED;
}
