/**
 * @file hmi_trend.h
 * @brief Trend history: a fixed ring of time buckets per trended tag, each
 *        holding the smallest and largest GOOD value seen in it.
 *
 * Only data with quality GOOD is ever recorded. A bucket with no GOOD sample
 * (the link was down, the tag was stale or out of range, or the sampler did not
 * run) is a gap and is drawn as a gap, never as a value. Keeping the minimum and
 * the maximum of each bucket means a short spike is not lost when a long window
 * is shown at a coarse scale (hmi_trend_decimate).
 *
 * Memory is static (no allocation after init) and the history is kept in RAM
 * only: it starts empty at every start and is never written to storage.
 *
 * Threads: one sampler calls hmi_trend_step() (the supervisor thread), readers
 * call the copy functions from any thread. The module has one lock that is held
 * only for the copy of a few kilobytes at most; it is never held while another
 * lock is taken or a file or the display is touched, and it is never nested with
 * another lock. The caller takes the tag snapshot before calling hmi_trend_step.
 */
#ifndef HMI_TREND_H
#define HMI_TREND_H

#include "hmi_tags.h"

#define HMI_TREND_POINTS            480u     /* closed buckets kept per series */
#define HMI_TREND_BUCKET_DEFAULT_MS 5000u
#define HMI_TREND_BUCKET_MIN_MS     1000u
#define HMI_TREND_BUCKET_MAX_MS     60000u

/* A bucket holds data when lo <= hi. A gap has lo > hi (see hmi_trend_bucket_valid). */
typedef struct {
    float lo, hi;
} hmi_trend_bucket_t;

static inline bool hmi_trend_bucket_valid(const hmi_trend_bucket_t *b) { return b->lo <= b->hi; }

typedef struct {
    hmi_trend_bucket_t ring[HMI_TREND_POINTS];
    size_t head;            /* index the next closed bucket is written to */
    size_t count;           /* closed buckets held, at most HMI_TREND_POINTS */
    double open_lo, open_hi;
    bool open_valid;        /* a GOOD sample has been seen in the open bucket */
    uint8_t tag;            /* index of the tag in the tag table */
} hmi_trend_series_t;

typedef struct {
    hmi_trend_series_t s[HMI_TREND_MAX_SERIES];
    size_t n_series;
    uint32_t bucket_ms;
    uint64_t t0_ms;         /* start of bucket number 0 */
    uint64_t open_id;       /* number of the bucket that is open now */
    uint32_t rev;           /* grows each time a bucket is closed */
    hmi_mutex_t lock;
    bool ready;
} hmi_trend_t;

/* The tags whose configuration says trend are recorded, in table order. Fails if
 * there are more than HMI_TREND_MAX_SERIES, bucket_ms is outside
 * [HMI_TREND_BUCKET_MIN_MS, HMI_TREND_BUCKET_MAX_MS], or the lock cannot be made.
 * No trended tag is valid: the module then has no series. */
bool hmi_trend_init(hmi_trend_t *t, const hmi_tag_cfg_t *tags, size_t n_tags, uint32_t bucket_ms, uint64_t now_ms);
void hmi_trend_destroy(hmi_trend_t *t);

/* Record the current values (vals[i] is tag i) and close every bucket whose time
 * has passed. Call it from one thread only; a gap in time (the caller did not
 * run for several buckets) closes the missed buckets as gaps. */
void hmi_trend_step(hmi_trend_t *t, uint64_t now_ms, const hmi_tag_value_t *vals, size_t n_vals);

size_t hmi_trend_series_count(const hmi_trend_t *t);
int hmi_trend_series_tag(const hmi_trend_t *t, size_t series);   /* tag index, or -1 */
uint32_t hmi_trend_bucket_ms(const hmi_trend_t *t);
/* Changes when a bucket has been closed since the previous call. */
uint32_t hmi_trend_rev(hmi_trend_t *t);

/* Copy the newest closed buckets of a series, oldest first, at most cap of them.
 * Returns how many were copied. */
size_t hmi_trend_copy(hmi_trend_t *t, size_t series, hmi_trend_bucket_t *out, size_t cap);

/* Merge each run of `factor` buckets into one: the smallest lo and the largest hi
 * of the runs that hold data, a gap only when every bucket of the run is a gap.
 * The runs are counted from the newest bucket backwards, so the newest bucket is
 * always the last of the last run and the picture does not shift when the window
 * grows by a bucket; the oldest run may be shorter. Output is oldest first.
 * Returns the number written, at most out_cap (the newest runs are kept). factor 0
 * is treated as 1. */
size_t hmi_trend_decimate(const hmi_trend_bucket_t *in, size_t n, size_t factor, hmi_trend_bucket_t *out, size_t out_cap);

/* Smallest lo and largest hi over the buckets that hold data. False if none does. */
bool hmi_trend_range(const hmi_trend_bucket_t *b, size_t n, double *lo, double *hi);

#endif /* HMI_TREND_H */
