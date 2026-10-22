#include "hmi_config.h"
#include "hmi_decode.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSMN_STATIC
#include "../../third_party/jsmn/jsmn.h"

#define MAX_TOKENS 1024

typedef struct {
    const char *js;
    jsmntok_t *t;
    int n;
    char *err;
    size_t err_cap;
} ctx_t;

static bool fail(ctx_t *c, const char *msg, const char *key)
{
    if (c->err && c->err_cap) {
        if (key) snprintf(c->err, c->err_cap, "%s: %s", msg, key);
        else snprintf(c->err, c->err_cap, "%s", msg);
    }
    return false;
}

/* Index of the token following the subtree rooted at i. */
static int skip(const ctx_t *c, int i)
{
    int pending = 1;
    while (pending > 0 && i < c->n) {
        /* jsmn: object size = number of keys, each key token has size 1,
         * array size = element count. A token adds its children, removes itself. */
        pending += c->t[i].size;
        pending--;
        i++;
    }
    return i;
}

static bool tok_eq(const ctx_t *c, int i, const char *s)
{
    size_t len = (size_t)(c->t[i].end - c->t[i].start);
    return c->t[i].type == JSMN_STRING && strlen(s) == len && memcmp(c->js + c->t[i].start, s, len) == 0;
}

static bool get_str_ex(ctx_t *c, int i, char *dst, size_t cap, const char *key, bool allow_empty)
{
    if (c->t[i].type != JSMN_STRING) return fail(c, "expected string", key);
    size_t len = (size_t)(c->t[i].end - c->t[i].start);
    if ((len == 0 && !allow_empty) || len >= cap) return fail(c, "string length", key);
    for (size_t k = 0; k < len; k++) {
        unsigned char ch = (unsigned char)c->js[c->t[i].start + (int)k];
        if (ch == '\\' || ch < 0x20 || ch > 0x7E) return fail(c, "illegal character", key);
    }
    memcpy(dst, c->js + c->t[i].start, len);
    dst[len] = '\0';
    return true;
}

static bool get_num(ctx_t *c, int i, double *out, const char *key)
{
    if (c->t[i].type != JSMN_PRIMITIVE) return fail(c, "expected number", key);
    char buf[40];
    size_t len = (size_t)(c->t[i].end - c->t[i].start);
    if (len == 0 || len >= sizeof(buf)) return fail(c, "number length", key);
    memcpy(buf, c->js + c->t[i].start, len);
    buf[len] = '\0';
    if (buf[0] == 't' || buf[0] == 'f' || buf[0] == 'n') return fail(c, "expected number", key);
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf || *end != '\0' || isnan(v) || isinf(v)) return fail(c, "bad number", key);
    *out = v;
    return true;
}

static bool get_uint(ctx_t *c, int i, uint32_t lo, uint32_t hi, uint32_t *out, const char *key)
{
    double v;
    if (!get_num(c, i, &v, key)) return false;
    if (v != floor(v) || v < (double)lo || v > (double)hi) return fail(c, "out of range", key);
    *out = (uint32_t)v;
    return true;
}

static bool get_bool(ctx_t *c, int i, bool *out, const char *key)
{
    if (c->t[i].type != JSMN_PRIMITIVE) return fail(c, "expected bool", key);
    size_t len = (size_t)(c->t[i].end - c->t[i].start);
    const char *p = c->js + c->t[i].start;
    if (len == 4 && memcmp(p, "true", 4) == 0) { *out = true; return true; }
    if (len == 5 && memcmp(p, "false", 5) == 0) { *out = false; return true; }
    return fail(c, "expected bool", key);
}

static bool valid_ipv4(const char *s)
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

/* ------------------------------------------------------------------ schema
 *
 * Every JSON object the loader accepts is described by a table of fields.
 * One engine (parse_object) walks an object's keys and applies the table, so
 * the rules "unknown key rejected", "duplicate key rejected", "value type and
 * range checked" exist once. Cross-field rules stay as small named
 * `finish` functions next to the table they belong to.
 *
 * Behaviour (accept/reject verdict, error text, parsed values) is pinned by
 * tests/hmi/config_golden.txt.
 */

typedef enum {
    K_STR,        /* non-empty string into char[cap] */
    K_STR_EMPTY,  /* string into char[cap], empty allowed */
    K_DBL,        /* double */
    K_U32,        /* integer in [lo, hi] */
    K_U16,
    K_U8,
    K_BOOL,
    K_ENUM,       /* string from a closed set, stored as int-sized enum */
    K_CUSTOM,     /* callback parses the value in place */
    K_DEFER       /* token index recorded for the section's finish step */
} kind_t;

typedef struct { const char *name; int value; } enum_opt_t;

enum {
    FL_NONZERO = 1,   /* numeric value must not be 0 ("zero") */
    FL_NONNEG = 2     /* numeric value must be >= 0 ("negative") */
};

#define NO_FLAG_OFF ((size_t)-1)

struct section;
typedef bool (*custom_fn)(ctx_t *c, int val, void *base);

typedef struct {
    const char *key;
    kind_t kind;
    size_t off;             /* offset of the destination inside the section's struct */
    size_t cap;             /* K_STR*: buffer size; K_ENUM: scratch buffer size */
    uint32_t lo, hi;        /* K_U* range */
    const enum_opt_t *opts; /* K_ENUM */
    const char *unknown;    /* K_ENUM: message for a value outside the set */
    size_t flag_off;        /* if not NO_FLAG_OFF: bool set true when the key is present */
    unsigned flags;
    custom_fn fn;           /* K_CUSTOM */
} field_t;

typedef struct section {
    const char *name;       /* "alarm", "write", "tag", "plc"; NULL = top level */
    const field_t *f;
    size_t nf;
    bool dup_before_value;  /* top level reports a duplicate before looking at the value */
} section_t;

#define MAX_FIELDS 16
typedef struct {
    unsigned seen;          /* bit i = field i present */
    int deferred[MAX_FIELDS];
} seen_t;

/* Compile-time check that enum fields are int sized (stored through int). */
#define ENUM_IS_INT(S, m) typedef char enum_size_##S##_##m[sizeof(((S *)0)->m) == sizeof(int) ? 1 : -1]

static bool store_enum(ctx_t *c, int val, const field_t *f, void *dst)
{
    char buf[24];
    if (f->cap > sizeof(buf)) return fail(c, "internal: enum buffer", f->key);
    if (!get_str_ex(c, val, buf, f->cap, f->key, false)) return false;
    for (const enum_opt_t *o = f->opts; o->name; o++) {
        if (!strcmp(buf, o->name)) {
            int v = o->value;
            memcpy(dst, &v, sizeof(v));
            return true;
        }
    }
    return fail(c, f->unknown, buf);
}

static bool store_field(ctx_t *c, int val, const field_t *f, void *base)
{
    char *dst = (char *)base + f->off;
    double d;
    uint32_t u;
    switch (f->kind) {
        case K_STR: return get_str_ex(c, val, dst, f->cap, f->key, false);
        case K_STR_EMPTY: return get_str_ex(c, val, dst, f->cap, f->key, true);
        case K_DBL:
            if (!get_num(c, val, &d, f->key)) return false;
            *(double *)dst = d;
            if ((f->flags & FL_NONZERO) && d == 0.0) return fail(c, "zero", f->key);
            if ((f->flags & FL_NONNEG) && d < 0.0) return fail(c, "negative", f->key);
            return true;
        case K_U32:
            if (!get_uint(c, val, f->lo, f->hi, &u, f->key)) return false;
            *(uint32_t *)dst = u;
            return true;
        case K_U16:
            if (!get_uint(c, val, f->lo, f->hi, &u, f->key)) return false;
            *(uint16_t *)dst = (uint16_t)u;
            return true;
        case K_U8:
            if (!get_uint(c, val, f->lo, f->hi, &u, f->key)) return false;
            *(uint8_t *)dst = (uint8_t)u;
            return true;
        case K_BOOL: return get_bool(c, val, (bool *)dst, f->key);
        case K_ENUM: return store_enum(c, val, f, dst);
        case K_CUSTOM: return f->fn(c, val, base);
        case K_DEFER: return true;
    }
    return false;
}

/* Walk the keys of the object at `obj` and apply the section's field table.
 * Per key the order is: key is a string, key known, (duplicate if the
 * section checks it first), value parsed and range checked, duplicate. */
static bool parse_object(ctx_t *c, int obj, const section_t *s, void *base, seen_t *st)
{
    char msg[48];
    memset(st, 0, sizeof(*st));
    for (size_t k = 0; k < MAX_FIELDS; k++) st->deferred[k] = -1;
    int i = obj + 1;
    for (int k = 0; k < c->t[obj].size; k++) {
        int key = i, val = i + 1;
        if (c->t[key].type != JSMN_STRING) return fail(c, "bad key", s->name);
        size_t fi = s->nf;
        for (size_t j = 0; j < s->nf; j++) {
            if (tok_eq(c, key, s->f[j].key)) { fi = j; break; }
        }
        if (fi == s->nf) {
            if (!s->name) return fail(c, "unknown top-level key", NULL);
            snprintf(msg, sizeof(msg), "unknown key in %s", s->name);
            return fail(c, msg, NULL);
        }
        const field_t *f = &s->f[fi];
        bool dup = (st->seen & (1u << fi)) != 0;
        if (dup && s->dup_before_value) return fail(c, "duplicate key", f->key);
        if (f->kind == K_DEFER) st->deferred[fi] = val;
        if (!store_field(c, val, f, base)) return false;
        if (dup) {
            snprintf(msg, sizeof(msg), "duplicate key in %s", s->name);
            return fail(c, msg, NULL);
        }
        if (f->flag_off != NO_FLAG_OFF) *(bool *)((char *)base + f->flag_off) = true;
        st->seen |= 1u << fi;
        i = skip(c, val);
    }
    return true;
}

static bool expect_object(ctx_t *c, int obj, const char *what)
{
    if (c->t[obj].type == JSMN_OBJECT) return true;
    char msg[40];
    snprintf(msg, sizeof(msg), "%s must be object", what);
    return fail(c, msg, NULL);
}

/* ---- alarm ---- */

enum { AL_HI, AL_HIHI, AL_LO, AL_LOLO, AL_DEADBAND, AL_ON, AL_OFF };

static const field_t ALARM_FIELDS[] = {
    { "high", K_DBL, offsetof(hmi_alarm_cfg_t, hi), 0, 0, 0, NULL, NULL, offsetof(hmi_alarm_cfg_t, en_hi), 0, NULL },
    { "high_high", K_DBL, offsetof(hmi_alarm_cfg_t, hihi), 0, 0, 0, NULL, NULL, offsetof(hmi_alarm_cfg_t, en_hihi), 0, NULL },
    { "low", K_DBL, offsetof(hmi_alarm_cfg_t, lo), 0, 0, 0, NULL, NULL, offsetof(hmi_alarm_cfg_t, en_lo), 0, NULL },
    { "low_low", K_DBL, offsetof(hmi_alarm_cfg_t, lolo), 0, 0, 0, NULL, NULL, offsetof(hmi_alarm_cfg_t, en_lolo), 0, NULL },
    { "deadband", K_DBL, offsetof(hmi_alarm_cfg_t, deadband), 0, 0, 0, NULL, NULL, NO_FLAG_OFF, FL_NONNEG, NULL },
    { "on_ms", K_U32, offsetof(hmi_alarm_cfg_t, on_ms), 0, 0, 600000, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "off_ms", K_U32, offsetof(hmi_alarm_cfg_t, off_ms), 0, 0, 600000, NULL, NULL, NO_FLAG_OFF, 0, NULL },
};
static const section_t ALARM_SECTION = { "alarm", ALARM_FIELDS, sizeof(ALARM_FIELDS) / sizeof(ALARM_FIELDS[0]), false };

static bool parse_alarm(ctx_t *c, int obj, hmi_alarm_cfg_t *a)
{
    seen_t st;
    if (!expect_object(c, obj, "alarm")) return false;
    if (!parse_object(c, obj, &ALARM_SECTION, a, &st)) return false;
    /* limits must be ordered and the deadband must not bridge two levels */
    if (a->en_hihi && a->en_hi && !(a->hihi > a->hi)) return fail(c, "high_high must exceed high", NULL);
    if (a->en_lolo && a->en_lo && !(a->lolo < a->lo)) return fail(c, "low_low must be below low", NULL);
    if (a->en_hi && a->en_lo && !(a->hi > a->lo)) return fail(c, "high must exceed low", NULL);
    if (a->en_hihi && a->en_hi && a->deadband >= a->hihi - a->hi) return fail(c, "deadband too large", NULL);
    if (a->en_lolo && a->en_lo && a->deadband >= a->lo - a->lolo) return fail(c, "deadband too large", NULL);
    return true;
}

/* ---- write ---- */

static const enum_opt_t ROLE_OPTS[] = {
    { "viewer", HMI_ROLE_VIEWER }, { "operator", HMI_ROLE_OPERATOR },
    { "engineer", HMI_ROLE_ENGINEER }, { "admin", HMI_ROLE_ADMIN }, { NULL, 0 }
};

ENUM_IS_INT(hmi_tag_cfg_t, write_role);
ENUM_IS_INT(hmi_tag_cfg_t, area);
ENUM_IS_INT(hmi_tag_cfg_t, dtype);
ENUM_IS_INT(hmi_tag_cfg_t, word_order);
ENUM_IS_INT(hmi_tag_cfg_t, byte_order);

enum { WR_ALLOWED, WR_ROLE, WR_MIN, WR_MAX };

static const field_t WRITE_FIELDS[] = {
    { "allowed", K_BOOL, offsetof(hmi_tag_cfg_t, write_allowed), 0, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "role", K_ENUM, offsetof(hmi_tag_cfg_t, write_role), 16, 0, 0, ROLE_OPTS, "unknown role", NO_FLAG_OFF, 0, NULL },
    { "min", K_DBL, offsetof(hmi_tag_cfg_t, write_min), 0, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "max", K_DBL, offsetof(hmi_tag_cfg_t, write_max), 0, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, NULL },
};
static const section_t WRITE_SECTION = { "write", WRITE_FIELDS, sizeof(WRITE_FIELDS) / sizeof(WRITE_FIELDS[0]), false };

static bool parse_write(ctx_t *c, int obj, hmi_tag_cfg_t *t)
{
    seen_t st;
    if (!expect_object(c, obj, "write")) return false;
    t->write_allowed = false;
    if (!parse_object(c, obj, &WRITE_SECTION, t, &st)) return false;
    if (t->write_allowed) {
        /* A writable tag must say who may write and the exact legal range. */
        unsigned need = (1u << WR_ROLE) | (1u << WR_MIN) | (1u << WR_MAX);
        if ((st.seen & need) != need) return fail(c, "writable tag needs role,min,max", t->name);
        if (t->write_role < HMI_ROLE_OPERATOR) return fail(c, "viewer cannot write", t->name);
        if (!(t->write_min <= t->write_max)) return fail(c, "write min>max", t->name);
        if (t->write_min < t->eng_min || t->write_max > t->eng_max) return fail(c, "write range outside eng range", t->name);
    }
    return true;
}

/* ---- tag ---- */

static const enum_opt_t AREA_OPTS[] = {
    { "holding", HMI_AREA_HOLDING }, { "input", HMI_AREA_INPUT }, { NULL, 0 }
};
static const enum_opt_t TYPE_OPTS[] = {
    { "u16", HMI_DT_U16 }, { "i16", HMI_DT_I16 }, { "u32", HMI_DT_U32 },
    { "i32", HMI_DT_I32 }, { "f32", HMI_DT_F32 }, { NULL, 0 }
};
static const enum_opt_t WORD_OPTS[] = {
    { "high_first", HMI_WORD_HIGH_FIRST }, { "low_first", HMI_WORD_LOW_FIRST }, { NULL, 0 }
};
static const enum_opt_t BYTE_OPTS[] = {
    { "big", HMI_BYTE_BIG }, { "swapped", HMI_BYTE_SWAPPED }, { NULL, 0 }
};

enum { TG_NAME, TG_UNITS, TG_AREA, TG_ADDRESS, TG_TYPE, TG_WORD, TG_BYTE, TG_SCALE, TG_OFFSET,
       TG_MIN, TG_MAX, TG_POLL, TG_ALARM, TG_WRITE };

static const field_t TAG_FIELDS[] = {
    { "name", K_STR, offsetof(hmi_tag_cfg_t, name), HMI_NAME_LEN, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "units", K_STR_EMPTY, offsetof(hmi_tag_cfg_t, units), HMI_UNIT_LEN, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "area", K_ENUM, offsetof(hmi_tag_cfg_t, area), 24, 0, 0, AREA_OPTS, "unknown area", NO_FLAG_OFF, 0, NULL },
    { "address", K_U16, offsetof(hmi_tag_cfg_t, address), 0, 0, 65535, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "type", K_ENUM, offsetof(hmi_tag_cfg_t, dtype), 24, 0, 0, TYPE_OPTS, "unknown type", NO_FLAG_OFF, 0, NULL },
    { "word_order", K_ENUM, offsetof(hmi_tag_cfg_t, word_order), 24, 0, 0, WORD_OPTS, "unknown word_order", NO_FLAG_OFF, 0, NULL },
    { "byte_order", K_ENUM, offsetof(hmi_tag_cfg_t, byte_order), 24, 0, 0, BYTE_OPTS, "unknown byte_order", NO_FLAG_OFF, 0, NULL },
    { "scale", K_DBL, offsetof(hmi_tag_cfg_t, scale), 0, 0, 0, NULL, NULL, NO_FLAG_OFF, FL_NONZERO, NULL },
    { "offset", K_DBL, offsetof(hmi_tag_cfg_t, offset), 0, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "min", K_DBL, offsetof(hmi_tag_cfg_t, eng_min), 0, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "max", K_DBL, offsetof(hmi_tag_cfg_t, eng_max), 0, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "poll_ms", K_U32, offsetof(hmi_tag_cfg_t, poll_ms), 0, 50, 3600000, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "alarm", K_DEFER, 0, 0, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "write", K_DEFER, 0, 0, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, NULL },
};
static const section_t TAG_SECTION = { "tag", TAG_FIELDS, sizeof(TAG_FIELDS) / sizeof(TAG_FIELDS[0]), false };

static bool parse_tag(ctx_t *c, int obj, hmi_tag_cfg_t *t)
{
    seen_t st;
    if (!expect_object(c, obj, "tag")) return false;
    memset(t, 0, sizeof(*t));
    t->scale = 1.0;
    t->poll_ms = 1000;
    t->eng_min = -1e12;
    t->eng_max = 1e12;
    t->write_role = HMI_ROLE_ENGINEER;
    if (!parse_object(c, obj, &TAG_SECTION, t, &st)) return false;
    if (!(st.seen & (1u << TG_NAME))) return fail(c, "tag without name", NULL);
    if (!(st.seen & (1u << TG_ADDRESS))) return fail(c, "tag without address", t->name);
    if (!(t->eng_min <= t->eng_max)) return fail(c, "min>max", t->name);
    if (st.deferred[TG_ALARM] >= 0 && !parse_alarm(c, st.deferred[TG_ALARM], &t->alarm)) return false;
    if (st.deferred[TG_WRITE] >= 0 && !parse_write(c, st.deferred[TG_WRITE], t)) return false;
    if (t->area == HMI_AREA_INPUT && t->write_allowed) return fail(c, "input register not writable", t->name);
    if ((unsigned)t->address + hmi_dtype_regs(t->dtype) > 65536u) return fail(c, "address overflow", t->name);
    return true;
}

/* ---- plc ---- */

enum { PL_HOST, PL_PORT, PL_UNIT, PL_RESP, PL_CONN, PL_ALLOWED };

/* "allowed_hosts": the only PLC addresses this device may ever connect to, as
 * dotted-quad literals. Optional for the parser (unsigned test configurations
 * need not have it); hmi_cfgsec requires it, and the host in use to be in it. */
static bool plc_allowed_hosts(ctx_t *c, int val, void *base)
{
    hmi_app_cfg_t *cfg = (hmi_app_cfg_t *)base;
    const jsmntok_t *arr = &c->t[val];
    cfg->n_allowed = 0;
    if (arr->type != JSMN_ARRAY) return fail(c, "allowed_hosts must be array", NULL);
    if (arr->size < 1 || arr->size > HMI_MAX_ALLOWED) return fail(c, "allowed_hosts count", NULL);
    for (int k = 0; k < arr->size; k++) {
        char *dst = cfg->allowed_hosts[k];
        if (!get_str_ex(c, val + 1 + k, dst, HMI_HOST_LEN, "allowed_hosts", false)) return false;
        if (!valid_ipv4(dst)) return fail(c, "allowed host must be an IPv4 literal", dst);
        for (int j = 0; j < k; j++)
            if (strcmp(cfg->allowed_hosts[j], dst) == 0) return fail(c, "duplicate allowed host", dst);
        cfg->n_allowed = (size_t)k + 1u;
    }
    return true;
}

static const field_t PLC_FIELDS[] = {
    { "host", K_STR, offsetof(hmi_app_cfg_t, host), HMI_HOST_LEN, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "port", K_U16, offsetof(hmi_app_cfg_t, port), 0, 1, 65535, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "unit", K_U8, offsetof(hmi_app_cfg_t, unit), 0, 0, 247, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "response_timeout_ms", K_U32, offsetof(hmi_app_cfg_t, response_timeout_ms), 0, 50, HMI_RESPONSE_TIMEOUT_MAX_MS, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "connect_timeout_ms", K_U32, offsetof(hmi_app_cfg_t, connect_timeout_ms), 0, 50, HMI_CONNECT_TIMEOUT_MAX_MS, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "allowed_hosts", K_CUSTOM, 0, 0, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, plc_allowed_hosts },
};
static const section_t PLC_SECTION = { "plc", PLC_FIELDS, sizeof(PLC_FIELDS) / sizeof(PLC_FIELDS[0]), false };

static bool parse_plc(ctx_t *c, int obj, hmi_app_cfg_t *cfg)
{
    seen_t st;
    if (!expect_object(c, obj, "plc")) return false;
    if (!parse_object(c, obj, &PLC_SECTION, cfg, &st)) return false;
    if ((st.seen & ((1u << PL_HOST) | (1u << PL_PORT))) != ((1u << PL_HOST) | (1u << PL_PORT)))
        return fail(c, "plc needs host and port", NULL);
    if (!valid_ipv4(cfg->host)) return fail(c, "host must be an IPv4 literal", cfg->host);
    if (!(st.seen & (1u << PL_UNIT))) return fail(c, "plc needs unit", NULL);
    if (!(st.seen & (1u << PL_RESP))) cfg->response_timeout_ms = 500;
    if (!(st.seen & (1u << PL_CONN))) cfg->connect_timeout_ms = 1000;
    return true;
}

/* ---- top level ---- */

typedef struct {
    hmi_app_cfg_t *out;
    uint32_t version;
    bool writes_enabled;
    uint32_t write_min_interval_ms;
    uint32_t write_max_per_minute;
} top_t;

static bool top_plc(ctx_t *c, int val, void *base)
{
    return parse_plc(c, val, ((top_t *)base)->out);
}

static bool top_tags(ctx_t *c, int val, void *base)
{
    hmi_app_cfg_t *out = ((top_t *)base)->out;
    const jsmntok_t *arr = &c->t[val];
    if (arr->type != JSMN_ARRAY) return fail(c, "tags must be array", NULL);
    if (arr->size < 1 || arr->size > HMI_MAX_TAGS) return fail(c, "tag count", NULL);
    int ti = val + 1;
    for (int t = 0; t < arr->size; t++) {
        if (!parse_tag(c, ti, &out->tags[t])) return false;
        ti = skip(c, ti);
        out->n_tags++;
    }
    return true;
}

enum { TOP_VERSION, TOP_WRITES, TOP_PLC, TOP_TAGS, TOP_WMIN, TOP_WMAX };

static const field_t TOP_FIELDS[] = {
    { "version", K_U32, offsetof(top_t, version), 0, 1, 1, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "writes_enabled", K_BOOL, offsetof(top_t, writes_enabled), 0, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "plc", K_CUSTOM, 0, 0, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, top_plc },
    { "tags", K_CUSTOM, 0, 0, 0, 0, NULL, NULL, NO_FLAG_OFF, 0, top_tags },
    { "write_min_interval_ms", K_U32, offsetof(top_t, write_min_interval_ms), 0, 100, 3600000, NULL, NULL, NO_FLAG_OFF, 0, NULL },
    { "write_max_per_minute", K_U32, offsetof(top_t, write_max_per_minute), 0, 1, 600, NULL, NULL, NO_FLAG_OFF, 0, NULL },
};
static const section_t TOP_SECTION = { NULL, TOP_FIELDS, sizeof(TOP_FIELDS) / sizeof(TOP_FIELDS[0]), true };

static bool parse_document(ctx_t *c, size_t len, hmi_app_cfg_t *out)
{
    if (!c->js || len == 0 || len > HMI_CONFIG_MAX_BYTES) return fail(c, "size", NULL);
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, c->js, len, c->t, MAX_TOKENS);
    if (n < 1) return fail(c, n == JSMN_ERROR_NOMEM ? "too many tokens" : "malformed JSON", NULL);
    c->n = n;
    if (c->t[0].type != JSMN_OBJECT) return fail(c, "top level must be object", NULL);
    if (skip(c, 0) != n) return fail(c, "trailing data", NULL);

    top_t top = { out, 0, false, 1000u, 30u }; /* defaults of the two optional write limits */
    seen_t st;
    if (!parse_object(c, 0, &TOP_SECTION, &top, &st)) return false;
    unsigned all = (1u << TOP_VERSION) | (1u << TOP_PLC) | (1u << TOP_TAGS);
    if ((st.seen & all) != all) return fail(c, "version, plc and tags are required", NULL);
    out->writes_enabled = top.writes_enabled;
    out->write_min_interval_ms = top.write_min_interval_ms;
    out->write_max_per_minute = top.write_max_per_minute;

    /* cross-tag checks (names unique, register spans, group building) */
    hmi_tagdb_t *db = (hmi_tagdb_t *)malloc(sizeof(*db));
    if (!db) return fail(c, "out of memory", NULL);
    bool ok = hmi_tagdb_init(db, out->tags, out->n_tags, 0);
    if (ok) hmi_tagdb_destroy(db);
    free(db);
    if (!ok) return fail(c, "tag table invalid (duplicate name or too many groups)", NULL);
    return true;
}

bool hmi_config_parse(const char *json, size_t len, hmi_app_cfg_t *out, char *err, size_t err_cap)
{
    static jsmntok_t toks[MAX_TOKENS];
    ctx_t c = { json, toks, 0, err, err_cap };
    memset(out, 0, sizeof(*out));
    if (err && err_cap) err[0] = '\0';
    if (parse_document(&c, len, out)) return true;
    memset(out, 0, sizeof(*out)); /* any rejection leaves nothing usable behind */
    return false;
}

bool hmi_config_load_file(const char *path, hmi_app_cfg_t *out, char *err, size_t err_cap)
{
    static char buf[HMI_CONFIG_MAX_BYTES + 1];
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err && err_cap) snprintf(err, err_cap, "cannot open %s", path);
        return false;
    }
    size_t n = fread(buf, 1, HMI_CONFIG_MAX_BYTES + 1, f);
    fclose(f);
    if (n > HMI_CONFIG_MAX_BYTES) {
        if (err && err_cap) snprintf(err, err_cap, "config larger than %u bytes", HMI_CONFIG_MAX_BYTES);
        return false;
    }
    return hmi_config_parse(buf, n, out, err, err_cap);
}

bool hmi_config_save_text(const char *path, const char *json, size_t len, char *err, size_t err_cap)
{
    hmi_app_cfg_t *tmp = (hmi_app_cfg_t *)malloc(sizeof(*tmp));
    if (!tmp) return false;
    bool ok = hmi_config_parse(json, len, tmp, err, err_cap);
    free(tmp);
    if (!ok) return false;
    if (!hmi_fs_atomic_write(path, json, len)) {
        if (err && err_cap) snprintf(err, err_cap, "write failed");
        return false;
    }
    return true;
}

bool hmi_config_host_allowed(const hmi_app_cfg_t *cfg)
{
    if (!cfg || cfg->n_allowed == 0 || cfg->host[0] == '\0') return false;
    for (size_t i = 0; i < cfg->n_allowed; i++)
        if (strcmp(cfg->allowed_hosts[i], cfg->host) == 0) return true;
    return false;
}
