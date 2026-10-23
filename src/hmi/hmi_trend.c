#include "hmi_trend.h"
#include <stddef.h>
#include <string.h>

/* A bucket that holds no data: lo > hi. */
static const hmi_trend_bucket_t GAP = { 1.0f, 0.0f };

/* NaN and infinity are not data. */
static bool finite_value(double v)
{
    return (v - v) == 0.0;
}

bool hmi_trend_init(hmi_trend_t *t, const hmi_tag_cfg_t *tags, size_t n_tags, uint32_t bucket_ms, uint64_t now_ms)
{
    /* Only the series in use are cleared. Each holds a ring of HMI_TREND_POINTS buckets that is read only up to its
     * `count`, so clearing the unused ones would just make their pages resident (about 4 KB per series). */
    size_t used = 0;
    for (size_t i = 0; tags && i < n_tags; i++)
        if (tags[i].trend) used++;
    if (used > HMI_TREND_MAX_SERIES) used = HMI_TREND_MAX_SERIES;
    memset(t->s, 0, used * sizeof(t->s[0]));
    memset(&t->n_series, 0, sizeof(*t) - offsetof(hmi_trend_t, n_series));
    if (bucket_ms < HMI_TREND_BUCKET_MIN_MS || bucket_ms > HMI_TREND_BUCKET_MAX_MS) return false;
    if (!tags && n_tags) return false;
    for (size_t i = 0; i < n_tags; i++) {
        if (!tags[i].trend) continue;
        if (t->n_series >= HMI_TREND_MAX_SERIES || i > 255u) return false;
        t->s[t->n_series].tag = (uint8_t)i;
        t->n_series++;
    }
    t->bucket_ms = bucket_ms;
    t->t0_ms = now_ms;
    if (!hmi_mutex_init(&t->lock)) return false;
    t->ready = true;
    return true;
}

void hmi_trend_destroy(hmi_trend_t *t)
{
    if (t->ready) hmi_mutex_destroy(&t->lock);
    t->ready = false;
}

static void push(hmi_trend_series_t *s, hmi_trend_bucket_t b)
{
    s->ring[s->head] = b;
    s->head = (s->head + 1u) % HMI_TREND_POINTS;
    if (s->count < HMI_TREND_POINTS) s->count++;
}

/* Close the open bucket of every series and open the next one. */
static void close_open(hmi_trend_t *t)
{
    for (size_t i = 0; i < t->n_series; i++) {
        hmi_trend_series_t *s = &t->s[i];
        hmi_trend_bucket_t b = GAP;
        if (s->open_valid) {
            b.lo = (float)s->open_lo;
            b.hi = (float)s->open_hi;
        }
        push(s, b);
        s->open_valid = false;
    }
    t->open_id++;
    t->rev++;
}

void hmi_trend_step(hmi_trend_t *t, uint64_t now_ms, const hmi_tag_value_t *vals, size_t n_vals)
{
    if (!t->ready) return;
    hmi_mutex_lock(&t->lock);
    if (now_ms < t->t0_ms) now_ms = t->t0_ms;   /* the clock never runs backwards; do not trust a caller that does */
    uint64_t target = (now_ms - t->t0_ms) / t->bucket_ms;

    /* Close the buckets whose time has passed before the new value is recorded:
     * it belongs to the bucket that is open now, not to one that has ended. */
    if (target > t->open_id) {
        uint64_t missed = target - t->open_id;
        close_open(t);                  /* the open bucket, with whatever it collected */
        missed--;
        if (missed > HMI_TREND_POINTS) { /* everything older would scroll out anyway */
            t->open_id += missed - HMI_TREND_POINTS;
            missed = HMI_TREND_POINTS;
        }
        while (missed--) close_open(t); /* buckets in which nothing was sampled: gaps */
    }

    for (size_t i = 0; i < t->n_series; i++) {
        hmi_trend_series_t *s = &t->s[i];
        if (s->tag >= n_vals || vals[s->tag].quality != HMI_Q_GOOD || !finite_value(vals[s->tag].eng)) continue;
        double v = vals[s->tag].eng;
        if (!s->open_valid) {
            s->open_lo = s->open_hi = v;
            s->open_valid = true;
        } else {
            if (v < s->open_lo) s->open_lo = v;
            if (v > s->open_hi) s->open_hi = v;
        }
    }
    hmi_mutex_unlock(&t->lock);
}

size_t hmi_trend_series_count(const hmi_trend_t *t) { return t->n_series; }

int hmi_trend_series_tag(const hmi_trend_t *t, size_t series)
{
    return series < t->n_series ? (int)t->s[series].tag : -1;
}

uint32_t hmi_trend_bucket_ms(const hmi_trend_t *t) { return t->bucket_ms; }

uint32_t hmi_trend_rev(hmi_trend_t *t)
{
    uint32_t r;
    if (!t->ready) return 0;
    hmi_mutex_lock(&t->lock);
    r = t->rev;
    hmi_mutex_unlock(&t->lock);
    return r;
}

size_t hmi_trend_copy(hmi_trend_t *t, size_t series, hmi_trend_bucket_t *out, size_t cap)
{
    if (!t->ready || series >= t->n_series) return 0;
    hmi_mutex_lock(&t->lock);
    const hmi_trend_series_t *s = &t->s[series];
    size_t n = s->count < cap ? s->count : cap;
    size_t oldest = (s->head + HMI_TREND_POINTS - s->count) % HMI_TREND_POINTS;
    size_t start = (oldest + (s->count - n)) % HMI_TREND_POINTS;
    for (size_t i = 0; i < n; i++) out[i] = s->ring[(start + i) % HMI_TREND_POINTS];
    hmi_mutex_unlock(&t->lock);
    return n;
}

size_t hmi_trend_decimate(const hmi_trend_bucket_t *in, size_t n, size_t factor, hmi_trend_bucket_t *out, size_t out_cap)
{
    if (factor == 0) factor = 1;
    size_t runs = (n + factor - 1) / factor;
    size_t keep = runs < out_cap ? runs : out_cap;
    for (size_t r = 0; r < keep; r++) {            /* r = 0 is the newest run */
        size_t end = n - r * factor;
        size_t begin = end > factor ? end - factor : 0;
        hmi_trend_bucket_t m = GAP;
        for (size_t i = begin; i < end; i++) {
            if (!hmi_trend_bucket_valid(&in[i])) continue;
            if (!hmi_trend_bucket_valid(&m)) m = in[i];
            else {
                if (in[i].lo < m.lo) m.lo = in[i].lo;
                if (in[i].hi > m.hi) m.hi = in[i].hi;
            }
        }
        out[keep - 1 - r] = m;
    }
    return keep;
}

bool hmi_trend_range(const hmi_trend_bucket_t *b, size_t n, double *lo, double *hi)
{
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (!hmi_trend_bucket_valid(&b[i])) continue;
        if (!any) {
            *lo = b[i].lo;
            *hi = b[i].hi;
            any = true;
        } else {
            if (b[i].lo < *lo) *lo = b[i].lo;
            if (b[i].hi > *hi) *hi = b[i].hi;
        }
    }
    return any;
}
