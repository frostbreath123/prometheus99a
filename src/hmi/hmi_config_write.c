#include "hmi_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* The text this module writes is read back by hmi_config_parse, which is the
 * only judge of what is valid. Each value is written so that the parser gets
 * exactly the number that is in memory, and a field that holds its default is
 * left out, as a hand-written file would leave it out. That also keeps the
 * result within the parser's token budget for a configuration that was near it. */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ok;
} out_t;

static void put(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(out_t *o, const char *fmt, ...)
{
    if (!o->ok) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len) {
        o->ok = false;
        return;
    }
    o->len += (size_t)n;
}

/* The shortest text of a double that reads back as the same double. */
static void fmt_double(double v, char *buf, size_t cap)
{
    for (int digits = 15; digits <= 17; digits++) {
        snprintf(buf, cap, "%.*g", digits, v);
        if (strtod(buf, NULL) == v) return;
    }
}

/* A string the parser accepts is printable ASCII without a backslash, and it
 * cannot hold a double quote either (that would have ended the string). */
static bool string_safe(const char *s)
{
    for (; *s; s++)
        if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) return false;
    return true;
}

static const char *const AREA[] = { "holding", "input" };
static const char *const DTYPE[] = { "u16", "i16", "u32", "i32", "f32" };
static const char *const WORD[] = { "high_first", "low_first" };
static const char *const BYTE_ORDER_NAME[] = { "big", "swapped" };
static const char *const ROLE[] = { "viewer", "operator", "engineer", "admin" };
static const char *const PRIORITY[] = { "low", "medium", "high", "critical" };

static bool enum_ok(int v, size_t n) { return v >= 0 && (size_t)v < n; }

static void put_double(out_t *o, const char *key, double v)
{
    char b[40];
    fmt_double(v, b, sizeof(b));
    put(o, ",\"%s\":%s", key, b);
}

static void put_alarm(out_t *o, const hmi_alarm_cfg_t *a)
{
    bool any = a->en_hi || a->en_hihi || a->en_lo || a->en_lolo || a->deadband != 0.0 || a->on_ms || a->off_ms ||
               a->priority != HMI_PRI_MEDIUM;
    if (!any) return;
    /* Members that hold their default are left out (this keeps a large file inside the parser's token budget).
     * Every member writes a leading comma, and the one after the opening brace is removed at the end. */
    put(o, ",\"alarm\":{");
    size_t first = o->len;
    if (a->on_ms) put(o, ",\"on_ms\":%u", (unsigned)a->on_ms);
    if (a->off_ms) put(o, ",\"off_ms\":%u", (unsigned)a->off_ms);
    if (a->deadband != 0.0) put_double(o, "deadband", a->deadband);
    if (a->en_hi) put_double(o, "high", a->hi);
    if (a->en_hihi) put_double(o, "high_high", a->hihi);
    if (a->en_lo) put_double(o, "low", a->lo);
    if (a->en_lolo) put_double(o, "low_low", a->lolo);
    if (a->priority != HMI_PRI_MEDIUM && (unsigned)a->priority < 4u) put(o, ",\"priority\":\"%s\"", PRIORITY[a->priority]);
    if (o->ok && o->len > first && o->buf[first] == ',') {
        memmove(o->buf + first, o->buf + first + 1, o->len - first);   /* includes the terminating NUL */
        o->len--;
    }
    put(o, "}");
}

static void put_write(out_t *o, const hmi_tag_cfg_t *t)
{
    /* Without write.allowed the object is only needed to keep a role or limits that a file gave anyway. */
    bool any = t->write_allowed || t->write_role != HMI_ROLE_ENGINEER || t->write_min != 0.0 || t->write_max != 0.0;
    if (!any) return;
    put(o, ",\"write\":{\"allowed\":%s", t->write_allowed ? "true" : "false");
    if ((unsigned)t->write_role < 4u) put(o, ",\"role\":\"%s\"", ROLE[t->write_role]);
    put_double(o, "min", t->write_min);
    put_double(o, "max", t->write_max);
    put(o, "}");
}

size_t hmi_config_serialize(const hmi_app_cfg_t *cfg, char *buf, size_t cap)
{
    if (!cfg || !buf || cap == 0 || cfg->n_tags == 0 || cfg->n_tags > HMI_MAX_TAGS) return 0;
    if (!string_safe(cfg->host)) return 0;
    out_t o = { buf, cap, 0, true };
    put(&o, "{\"version\":1,\"writes_enabled\":%s,\"write_min_interval_ms\":%u,\"write_max_per_minute\":%u,"
            "\"trend_bucket_ms\":%u,\n", cfg->writes_enabled ? "true" : "false", (unsigned)cfg->write_min_interval_ms,
        (unsigned)cfg->write_max_per_minute, (unsigned)cfg->trend_bucket_ms);
    put(&o, "\"plc\":{\"host\":\"%s\",\"port\":%u,\"unit\":%u,\"response_timeout_ms\":%u,\"connect_timeout_ms\":%u",
        cfg->host, (unsigned)cfg->port, (unsigned)cfg->unit, (unsigned)cfg->response_timeout_ms,
        (unsigned)cfg->connect_timeout_ms);
    if (cfg->n_allowed > 0) {
        put(&o, ",\"allowed_hosts\":[");
        for (size_t i = 0; i < cfg->n_allowed; i++) {
            if (!string_safe(cfg->allowed_hosts[i])) return 0;
            put(&o, "%s\"%s\"", i ? "," : "", cfg->allowed_hosts[i]);
        }
        put(&o, "]");
    }
    put(&o, "},\n\"tags\":[\n");
    for (size_t i = 0; i < cfg->n_tags; i++) {
        const hmi_tag_cfg_t *t = &cfg->tags[i];
        if (!string_safe(t->name) || !string_safe(t->units)) return 0;
        if (!enum_ok((int)t->area, 2) || !enum_ok((int)t->dtype, 5) || !enum_ok((int)t->word_order, 2) ||
            !enum_ok((int)t->byte_order, 2))
            return 0;
        put(&o, "%s{\"name\":\"%s\",\"address\":%u", i ? ",\n" : "", t->name, (unsigned)t->address);
        if (t->units[0]) put(&o, ",\"units\":\"%s\"", t->units);
        if (t->area != HMI_AREA_HOLDING) put(&o, ",\"area\":\"%s\"", AREA[t->area]);
        if (t->dtype != HMI_DT_U16) put(&o, ",\"type\":\"%s\"", DTYPE[t->dtype]);
        if (t->word_order != HMI_WORD_HIGH_FIRST) put(&o, ",\"word_order\":\"%s\"", WORD[t->word_order]);
        if (t->byte_order != HMI_BYTE_BIG) put(&o, ",\"byte_order\":\"%s\"", BYTE_ORDER_NAME[t->byte_order]);
        if (t->scale != 1.0) put_double(&o, "scale", t->scale);
        if (t->offset != 0.0) put_double(&o, "offset", t->offset);
        if (t->eng_min != -1e12) put_double(&o, "min", t->eng_min);
        if (t->eng_max != 1e12) put_double(&o, "max", t->eng_max);
        if (t->poll_ms != 1000u) put(&o, ",\"poll_ms\":%u", (unsigned)t->poll_ms);
        if (t->trend) put(&o, ",\"trend\":true");
        put_alarm(&o, &t->alarm);
        put_write(&o, t);
        put(&o, "}");
    }
    put(&o, "\n]}\n");
    if (!o.ok || o.len + 1 > cap) return 0;
    return o.len;
}

static bool tag_same(const hmi_tag_cfg_t *a, const hmi_tag_cfg_t *b)
{
    return strcmp(a->name, b->name) == 0 && strcmp(a->units, b->units) == 0 && a->area == b->area &&
           a->address == b->address && a->dtype == b->dtype && a->word_order == b->word_order &&
           a->byte_order == b->byte_order && a->scale == b->scale && a->offset == b->offset &&
           a->eng_min == b->eng_min && a->eng_max == b->eng_max && a->poll_ms == b->poll_ms && a->trend == b->trend &&
           a->alarm.en_hi == b->alarm.en_hi && a->alarm.en_hihi == b->alarm.en_hihi &&
           a->alarm.en_lo == b->alarm.en_lo && a->alarm.en_lolo == b->alarm.en_lolo && a->alarm.hi == b->alarm.hi &&
           a->alarm.hihi == b->alarm.hihi && a->alarm.lo == b->alarm.lo && a->alarm.lolo == b->alarm.lolo &&
           a->alarm.deadband == b->alarm.deadband && a->alarm.on_ms == b->alarm.on_ms &&
           a->alarm.off_ms == b->alarm.off_ms && a->alarm.priority == b->alarm.priority &&
           a->write_allowed == b->write_allowed && a->write_role == b->write_role &&
           a->write_min == b->write_min && a->write_max == b->write_max;
}

bool hmi_config_equal(const hmi_app_cfg_t *a, const hmi_app_cfg_t *b)
{
    if (strcmp(a->host, b->host) != 0 || a->port != b->port || a->unit != b->unit ||
        a->response_timeout_ms != b->response_timeout_ms || a->connect_timeout_ms != b->connect_timeout_ms ||
        a->writes_enabled != b->writes_enabled || a->write_min_interval_ms != b->write_min_interval_ms ||
        a->write_max_per_minute != b->write_max_per_minute || a->trend_bucket_ms != b->trend_bucket_ms ||
        a->n_allowed != b->n_allowed || a->n_tags != b->n_tags)
        return false;
    for (size_t i = 0; i < a->n_allowed; i++)
        if (strcmp(a->allowed_hosts[i], b->allowed_hosts[i]) != 0) return false;
    for (size_t i = 0; i < a->n_tags; i++)
        if (!tag_same(&a->tags[i], &b->tags[i])) return false;
    return true;
}
