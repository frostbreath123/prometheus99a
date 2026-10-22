/* Tests for hmi_trend: which tags are recorded, min and max per bucket, only
 * GOOD data, gaps instead of invented values, a jump in time, the ring wrapping,
 * decimation for a longer window, and a sampler running against a reader. */
#include "test_support.h"
#include "../../src/hmi/hmi_trend.h"
#include <math.h>

#define T0 10000u

static void set(hmi_tag_value_t *v, double eng, hmi_quality_t q)
{
    memset(v, 0, sizeof(*v));
    v->eng = eng;
    v->quality = q;
}

/* Four tags; the ones named in `mask` (bit i = tag i) are trended. */
static hmi_trend_t *new_trend(unsigned mask, uint32_t bucket_ms)
{
    static hmi_tag_cfg_t tags[4];
    memset(tags, 0, sizeof(tags));
    for (int i = 0; i < 4; i++) tags[i].trend = (mask >> i) & 1u;
    hmi_trend_t *t = (hmi_trend_t *)calloc(1, sizeof(*t));
    if (!hmi_trend_init(t, tags, 4, bucket_ms, T0)) {
        free(t);
        return NULL;
    }
    return t;
}

static void free_trend(hmi_trend_t *t)
{
    hmi_trend_destroy(t);
    free(t);
}

static void test_init(void)
{
    SECTION("trend: which tags are recorded and what is refused");
    hmi_tag_cfg_t tags[12];
    memset(tags, 0, sizeof(tags));
    hmi_trend_t *t = (hmi_trend_t *)calloc(1, sizeof(*t));

    CHECK(hmi_trend_init(t, tags, 4, 5000, T0));                 /* none trended is fine */
    CHECK(hmi_trend_series_count(t) == 0 && hmi_trend_series_tag(t, 0) == -1);
    hmi_trend_destroy(t);

    tags[1].trend = tags[3].trend = true;
    CHECK(hmi_trend_init(t, tags, 4, 5000, T0));
    CHECK(hmi_trend_series_count(t) == 2);
    CHECK(hmi_trend_series_tag(t, 0) == 1 && hmi_trend_series_tag(t, 1) == 3 && hmi_trend_series_tag(t, 2) == -1);
    CHECK(hmi_trend_bucket_ms(t) == 5000);
    hmi_trend_destroy(t);

    CHECK(!hmi_trend_init(t, tags, 4, 999, T0));                 /* bucket length bounds */
    CHECK(hmi_trend_init(t, tags, 4, 1000, T0));
    hmi_trend_destroy(t);
    CHECK(hmi_trend_init(t, tags, 4, 60000, T0));
    hmi_trend_destroy(t);
    CHECK(!hmi_trend_init(t, tags, 4, 60001, T0));
    CHECK(!hmi_trend_init(t, tags, 4, 0, T0));
    CHECK(!hmi_trend_init(t, NULL, 4, 5000, T0));

    for (int i = 0; i < 12; i++) tags[i].trend = true;
    CHECK(!hmi_trend_init(t, tags, 9, 5000, T0));                /* more than eight */
    CHECK(hmi_trend_init(t, tags, HMI_TREND_MAX_SERIES, 5000, T0));
    hmi_trend_destroy(t);
    free(t);
}

static void test_min_max_and_boundaries(void)
{
    SECTION("trend: min and max per bucket, the boundary belongs to the next bucket");
    hmi_trend_t *t = new_trend(1u << 2, 1000);
    CHECK(t && hmi_trend_series_count(t) == 1);
    hmi_tag_value_t v[4];
    for (int i = 0; i < 4; i++) set(&v[i], 0, HMI_Q_GOOD);
    hmi_trend_bucket_t b[8];

    uint32_t r0 = hmi_trend_rev(t);
    set(&v[2], 5.0, HMI_Q_GOOD); hmi_trend_step(t, T0 + 0, v, 4);
    set(&v[2], 9.5, HMI_Q_GOOD); hmi_trend_step(t, T0 + 300, v, 4);
    set(&v[2], 2.25, HMI_Q_GOOD); hmi_trend_step(t, T0 + 999, v, 4);
    CHECK(hmi_trend_copy(t, 0, b, 8) == 0);                      /* nothing is closed yet */
    CHECK(hmi_trend_rev(t) == r0);                               /* and nothing changed for a reader */

    set(&v[2], 7.0, HMI_Q_GOOD); hmi_trend_step(t, T0 + 1000, v, 4);   /* exactly the next bucket */
    CHECK(hmi_trend_rev(t) == r0 + 1);
    CHECK(hmi_trend_copy(t, 0, b, 8) == 1);
    CHECK(b[0].lo == 2.25f && b[0].hi == 9.5f);                  /* the 7.0 is not in it */

    set(&v[2], 3.0, HMI_Q_GOOD); hmi_trend_step(t, T0 + 1500, v, 4);
    set(&v[2], 8.0, HMI_Q_GOOD); hmi_trend_step(t, T0 + 2000, v, 4);
    CHECK(hmi_trend_copy(t, 0, b, 8) == 2);
    CHECK(b[1].lo == 3.0f && b[1].hi == 7.0f);                   /* 7.0, 3.0; not 8.0 */
    CHECK(hmi_trend_rev(t) == r0 + 2);

    CHECK(hmi_trend_copy(t, 1, b, 8) == 0);                      /* no such series */
    CHECK(hmi_trend_copy(t, 0, b, 0) == 0);
    free_trend(t);
}

static void test_only_good_data(void)
{
    SECTION("trend: only GOOD data is recorded, everything else is a gap");
    hmi_trend_t *t = new_trend(1u << 0 | 1u << 1, 1000);
    hmi_tag_value_t v[4];
    for (int i = 0; i < 4; i++) set(&v[i], 0, HMI_Q_GOOD);
    hmi_trend_bucket_t b[8];
    static const hmi_quality_t bad[3] = { HMI_Q_UNAVAILABLE, HMI_Q_BAD, HMI_Q_UNCERTAIN };

    for (int k = 0; k < 3; k++) {                                /* bucket k: tag 0 is never GOOD */
        set(&v[0], 123.0, bad[k]);
        set(&v[1], 10.0 + k, HMI_Q_GOOD);
        hmi_trend_step(t, T0 + (uint64_t)k * 1000u + 100u, v, 4);
        hmi_trend_step(t, T0 + (uint64_t)k * 1000u + 600u, v, 4);
    }
    hmi_trend_step(t, T0 + 3000, v, 4);
    CHECK(hmi_trend_copy(t, 0, b, 8) == 3);
    for (int k = 0; k < 3; k++) CHECK(!hmi_trend_bucket_valid(&b[k]));      /* never the 123 */
    CHECK(hmi_trend_copy(t, 1, b, 8) == 3);
    for (int k = 0; k < 3; k++) CHECK(hmi_trend_bucket_valid(&b[k]) && b[k].lo == 10.0f + k && b[k].hi == 10.0f + k);

    /* a bucket with bad data in the middle keeps only the good samples around it */
    set(&v[0], 4.0, HMI_Q_GOOD); hmi_trend_step(t, T0 + 3100, v, 4);
    set(&v[0], 999.0, HMI_Q_BAD); hmi_trend_step(t, T0 + 3300, v, 4);
    set(&v[0], 6.0, HMI_Q_GOOD); hmi_trend_step(t, T0 + 3500, v, 4);
    set(&v[0], -999.0, HMI_Q_UNCERTAIN); hmi_trend_step(t, T0 + 3700, v, 4);
    hmi_trend_step(t, T0 + 4000, v, 4);
    CHECK(hmi_trend_copy(t, 0, b, 8) == 4);
    CHECK(hmi_trend_bucket_valid(&b[3]) && b[3].lo == 4.0f && b[3].hi == 6.0f);

    /* NaN and infinity are not data even when the quality says GOOD */
    set(&v[0], NAN, HMI_Q_GOOD); hmi_trend_step(t, T0 + 4100, v, 4);
    set(&v[0], INFINITY, HMI_Q_GOOD); hmi_trend_step(t, T0 + 4200, v, 4);
    set(&v[0], -INFINITY, HMI_Q_GOOD); hmi_trend_step(t, T0 + 4300, v, 4);
    hmi_trend_step(t, T0 + 5000, v, 4);
    CHECK(hmi_trend_copy(t, 0, b, 8) == 5 && !hmi_trend_bucket_valid(&b[4]));

    free_trend(t);

    /* a value table shorter than the tag index is a gap, not a read past the end */
    t = new_trend(1u << 3, 1000);
    set(&v[3], 5.0, HMI_Q_GOOD);
    hmi_trend_step(t, T0 + 100, v, 2);                           /* only tags 0 and 1 are given */
    hmi_trend_step(t, T0 + 1000, v, 2);
    CHECK(hmi_trend_copy(t, 0, b, 8) == 1 && !hmi_trend_bucket_valid(&b[0]));
    hmi_trend_step(t, T0 + 1100, v, 4);                          /* now tag 3 is given */
    hmi_trend_step(t, T0 + 2000, v, 4);
    CHECK(hmi_trend_copy(t, 0, b, 8) == 2 && hmi_trend_bucket_valid(&b[1]) && b[1].lo == 5.0f);
    free_trend(t);
}

static void test_time_jump_and_alignment(void)
{
    SECTION("trend: a stall in time makes gaps, later buckets stay aligned");
    hmi_trend_t *t = new_trend(1u << 0, 1000);
    hmi_tag_value_t v[4];
    for (int i = 0; i < 4; i++) set(&v[i], 1.0, HMI_Q_GOOD);
    hmi_trend_bucket_t b[HMI_TREND_POINTS];

    hmi_trend_step(t, T0 + 100, v, 4);                           /* bucket 0 holds 1.0 */
    set(&v[0], 2.0, HMI_Q_GOOD);
    hmi_trend_step(t, T0 + 5300, v, 4);                          /* the sampler stalled: now in bucket 5 */
    CHECK(hmi_trend_copy(t, 0, b, HMI_TREND_POINTS) == 5);
    CHECK(hmi_trend_bucket_valid(&b[0]) && b[0].lo == 1.0f);
    for (int k = 1; k < 5; k++) CHECK(!hmi_trend_bucket_valid(&b[k]));
    hmi_trend_step(t, T0 + 5999, v, 4);
    CHECK(hmi_trend_copy(t, 0, b, HMI_TREND_POINTS) == 5);       /* still bucket 5: aligned to T0 */
    hmi_trend_step(t, T0 + 6000, v, 4);
    CHECK(hmi_trend_copy(t, 0, b, HMI_TREND_POINTS) == 6 && b[5].lo == 2.0f && b[5].hi == 2.0f);

    /* a stall longer than the ring: everything old is gone, nothing is invented */
    uint64_t far = T0 + (uint64_t)(HMI_TREND_POINTS + 50) * 1000u + 400u;
    uint32_t rev = hmi_trend_rev(t);
    set(&v[0], 3.0, HMI_Q_GOOD);
    hmi_trend_step(t, far, v, 4);
    CHECK(hmi_trend_rev(t) > rev);
    CHECK(hmi_trend_copy(t, 0, b, HMI_TREND_POINTS) == HMI_TREND_POINTS);
    bool any_value = false;
    for (size_t k = 0; k < HMI_TREND_POINTS; k++) any_value = any_value || hmi_trend_bucket_valid(&b[k]);
    CHECK(!any_value);                                           /* every old value has scrolled out, none was made up */
    /* the open bucket starts at a multiple of the bucket length from T0, whatever the jump was:
     * far is 400 ms into its bucket, so 599 ms later it is still the same one */
    rev = hmi_trend_rev(t);
    hmi_trend_step(t, far + 599, v, 4);
    CHECK(hmi_trend_rev(t) == rev);
    hmi_trend_step(t, far + 600, v, 4);
    CHECK(hmi_trend_rev(t) == rev + 1);
    CHECK(hmi_trend_copy(t, 0, b, HMI_TREND_POINTS) == HMI_TREND_POINTS);
    CHECK(hmi_trend_bucket_valid(&b[HMI_TREND_POINTS - 1]) && b[HMI_TREND_POINTS - 1].lo == 3.0f);

    /* the clock never goes backwards; if a caller's does, nothing breaks */
    hmi_trend_step(t, T0 - 5000, v, 4);
    hmi_trend_step(t, 0, v, 4);
    CHECK(hmi_trend_copy(t, 0, b, HMI_TREND_POINTS) == HMI_TREND_POINTS);
    free_trend(t);
}

static void test_ring_wraps(void)
{
    SECTION("trend: the ring keeps the newest buckets, oldest first");
    hmi_trend_t *t = new_trend(1u << 1, 1000);
    hmi_tag_value_t v[4];
    for (int i = 0; i < 4; i++) set(&v[i], 0, HMI_Q_GOOD);
    const size_t total = HMI_TREND_POINTS + 37;
    for (size_t k = 0; k < total; k++) {
        set(&v[1], (double)k, HMI_Q_GOOD);
        hmi_trend_step(t, T0 + (uint64_t)k * 1000u + 10u, v, 4);
    }
    hmi_trend_step(t, T0 + (uint64_t)total * 1000u, v, 4);       /* closes bucket total - 1 */
    static hmi_trend_bucket_t b[HMI_TREND_POINTS + 10];
    size_t n = hmi_trend_copy(t, 0, b, HMI_TREND_POINTS + 10);
    CHECK(n == HMI_TREND_POINTS);
    bool order = true;
    for (size_t i = 0; i < n; i++) order = order && b[i].lo == (float)(total - n + i) && b[i].hi == b[i].lo;
    CHECK(order);
    n = hmi_trend_copy(t, 0, b, 10);                             /* a small buffer gets the newest ten */
    CHECK(n == 10 && b[0].lo == (float)(total - 10) && b[9].lo == (float)(total - 1));
    free_trend(t);
}

static void test_series_independent(void)
{
    SECTION("trend: series do not affect each other");
    hmi_trend_t *t = new_trend((1u << 0) | (1u << 3), 1000);
    hmi_tag_value_t v[4];
    for (int i = 0; i < 4; i++) set(&v[i], 0, HMI_Q_GOOD);
    set(&v[0], 1.0, HMI_Q_GOOD);
    set(&v[3], 50.0, HMI_Q_BAD);
    hmi_trend_step(t, T0 + 10, v, 4);
    set(&v[0], 2.0, HMI_Q_GOOD);
    set(&v[3], 60.0, HMI_Q_GOOD);
    hmi_trend_step(t, T0 + 500, v, 4);
    hmi_trend_step(t, T0 + 1000, v, 4);
    hmi_trend_bucket_t a[2], c[2];
    CHECK(hmi_trend_copy(t, 0, a, 2) == 1 && a[0].lo == 1.0f && a[0].hi == 2.0f);
    CHECK(hmi_trend_copy(t, 1, c, 2) == 1 && c[0].lo == 60.0f && c[0].hi == 60.0f);   /* the BAD 50 is absent */
    free_trend(t);
}

static hmi_trend_bucket_t bk(float lo, float hi)
{
    hmi_trend_bucket_t b = { lo, hi };
    return b;
}

static void test_decimate(void)
{
    SECTION("trend: decimation keeps the extremes and counts runs from the newest bucket");
    const hmi_trend_bucket_t gap = bk(1, 0);
    hmi_trend_bucket_t in[8] = { bk(5, 6), bk(1, 3), bk(2, 9), gap, bk(4, 4), bk(0, 8), gap, gap };
    hmi_trend_bucket_t out[8];

    CHECK(hmi_trend_decimate(in, 8, 1, out, 8) == 8);            /* factor 1 is a copy */
    CHECK(memcmp(in, out, sizeof(in)) == 0);
    CHECK(hmi_trend_decimate(in, 8, 0, out, 8) == 8);            /* factor 0 counts as 1 */

    CHECK(hmi_trend_decimate(in, 8, 2, out, 8) == 4);
    CHECK(out[0].lo == 1 && out[0].hi == 6);                     /* (5,6) + (1,3) */
    CHECK(out[1].lo == 2 && out[1].hi == 9);                     /* (2,9) + gap: the gap does not count */
    CHECK(out[2].lo == 0 && out[2].hi == 8);                     /* (4,4) + (0,8) */
    CHECK(!hmi_trend_bucket_valid(&out[3]));                     /* gap + gap is a gap */

    /* five buckets, runs of two from the newest: [3,4] [1,2] [0] */
    CHECK(hmi_trend_decimate(in, 5, 2, out, 8) == 3);
    CHECK(out[0].lo == 5 && out[0].hi == 6);                     /* the oldest run holds one bucket */
    CHECK(out[1].lo == 1 && out[1].hi == 9);                     /* (1,3) + (2,9) */
    CHECK(out[2].lo == 4 && out[2].hi == 4);                     /* gap + (4,4) */

    /* a short output keeps the newest runs */
    CHECK(hmi_trend_decimate(in, 8, 2, out, 2) == 2);
    CHECK(out[0].lo == 0 && out[0].hi == 8 && !hmi_trend_bucket_valid(&out[1]));

    CHECK(hmi_trend_decimate(in, 8, 8, out, 8) == 1);            /* everything in one run */
    CHECK(out[0].lo == 0 && out[0].hi == 9);
    CHECK(hmi_trend_decimate(in, 8, 100, out, 8) == 1);
    CHECK(out[0].lo == 0 && out[0].hi == 9);
    CHECK(hmi_trend_decimate(in, 0, 2, out, 8) == 0);
    CHECK(hmi_trend_decimate(in, 8, 2, out, 0) == 0);

    /* a spike is not lost when a long window is squeezed: one narrow peak among 480 flat buckets */
    static hmi_trend_bucket_t flat[HMI_TREND_POINTS], sq[120];
    for (size_t i = 0; i < HMI_TREND_POINTS; i++) flat[i] = bk(10, 11);
    flat[301] = bk(10, 99);
    CHECK(hmi_trend_decimate(flat, HMI_TREND_POINTS, 4, sq, 120) == 120);
    float top = 0;
    for (size_t i = 0; i < 120; i++) if (sq[i].hi > top) top = sq[i].hi;
    CHECK(top == 99.0f);
    CHECK(sq[119].hi == 11.0f && sq[75].hi == 99.0f);            /* run 75 holds buckets 300..303 */

    double lo = 0, hi = 0;
    CHECK(!hmi_trend_range(in + 6, 2, &lo, &hi));                /* only gaps */
    CHECK(hmi_trend_range(in, 8, &lo, &hi) && lo == 0 && hi == 9);
    CHECK(!hmi_trend_range(in, 0, &lo, &hi));
}

/* ---- a sampler against a reader (meant for ThreadSanitizer as well) ---- */
typedef struct {
    hmi_trend_t *t;
    hmi_atomic_flag_t done;       /* an atomic: a plain flag would be a data race of the test itself */
} race_t;

static void sampler(void *arg)
{
    race_t *r = (race_t *)arg;
    hmi_tag_value_t v[4];
    for (int i = 0; i < 4; i++) set(&v[i], 0, HMI_Q_GOOD);
    for (uint64_t k = 0; k < 30000; k++) {
        set(&v[0], (double)(k % 100), (k % 7 == 0) ? HMI_Q_BAD : HMI_Q_GOOD);
        hmi_trend_step(r->t, T0 + k * 97u, v, 4);              /* about 2.9 s of bucket time per 30 buckets */
    }
    hmi_flag_set(&r->done, 1);
}

static void test_sampler_and_reader(void)
{
    SECTION("trend: a sampler and a reader at the same time");
    race_t r;
    r.t = new_trend(1u << 0, 1000);
    hmi_flag_set(&r.done, 0);
    hmi_thread_t th;
    CHECK(hmi_thread_start(&th, sampler, &r));
    static hmi_trend_bucket_t b[HMI_TREND_POINTS];
    bool sane = true;
    size_t last = 0;
    uint32_t last_rev = 0;
    long reads = 0;
    while (!hmi_flag_get(&r.done)) {
        size_t n = hmi_trend_copy(r.t, 0, b, HMI_TREND_POINTS);
        uint32_t rev = hmi_trend_rev(r.t);
        sane = sane && n >= last && n <= HMI_TREND_POINTS && rev >= last_rev;   /* never fewer, never over */
        for (size_t i = 0; i < n; i++)
            sane = sane && (!hmi_trend_bucket_valid(&b[i]) || (b[i].lo >= 0.0f && b[i].hi <= 99.0f));
        last = n;
        last_rev = rev;
        reads++;
    }
    CHECK(hmi_thread_join(&th, 10000));
    CHECK(sane);
    CHECK(reads > 0);
    CHECK(hmi_trend_rev(r.t) > 0);
    free_trend(r.t);
}

int main(void)
{
    test_init();
    test_min_max_and_boundaries();
    test_only_good_data();
    test_time_jump_and_alignment();
    test_ring_wraps();
    test_series_independent();
    test_decimate();
    test_sampler_and_reader();
    return test_finish();
}
