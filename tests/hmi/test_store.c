/* Tests for the storage worker: ordering, completion, bounded queue, fault
 * flag, rotation with generations, coalescing of replaced files, torn writes
 * and the worker thread with drain on stop. File operations are wrapped so a
 * test can make them fail. */
#include "test_support.h"
#include "../../src/hmi/hmi_store.h"
#include <stdio.h>

#define AUD "store_t_audit.log"
#define JRN "store_t_journal.log"
#define STA "store_t_state.bin"

/* ---- injectable file operations ---- */
static int f_append_fail;   /* fail the next N appends */
static int f_append_torn;   /* the next N appends write half the data and fail */
static int f_rename_fail;   /* fail the next N renames */
static int f_replace_fail;  /* fail the next N replaces */
static int n_replace, n_append, n_rename;

static bool io_append(const char *p, const void *d, size_t n)
{
    n_append++;
    if (f_append_fail > 0) { f_append_fail--; return false; }
    if (f_append_torn > 0) {
        f_append_torn--;
        hmi_fs_append_sync(p, d, n / 2);
        return false;
    }
    return hmi_fs_append_sync(p, d, n);
}
static bool io_replace(const char *p, const void *d, size_t n)
{
    n_replace++;
    if (f_replace_fail > 0) { f_replace_fail--; return false; }
    return hmi_fs_atomic_write(p, d, n);
}
static bool io_rename(const char *a, const char *b)
{
    n_rename++;
    if (f_rename_fail > 0) { f_rename_fail--; return false; }
    return hmi_fs_rename(a, b);
}
static const hmi_store_io_t k_io = { io_append, io_replace, hmi_fs_size, io_rename };

static void reset_inject(void)
{
    f_append_fail = f_append_torn = f_rename_fail = f_replace_fail = 0;
    n_replace = n_append = n_rename = 0;
}

static void clean(const char *p)
{
    char n[128];
    remove(p);
    snprintf(n, sizeof(n), "%s.tmp", p);
    remove(n);
    for (int i = 1; i <= 9; i++) {
        snprintf(n, sizeof(n), "%s.%d", p, i);
        remove(n);
    }
}
static void clean_all(void)
{
    clean(AUD);
    clean(JRN);
    clean(STA);
}

static size_t slurp(const char *p, char *buf, size_t cap)
{
    size_t len = 0;
    if (hmi_fs_read_all(p, buf, cap - 1, &len) != HMI_FS_READ_OK) return (size_t)-1;
    buf[len] = '\0';
    return len;
}
static bool exists(const char *p)
{
    uint64_t sz;
    return hmi_fs_size(p, &sz);
}

static hmi_store_cfg_t base_cfg(void)
{
    hmi_store_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.path[0] = AUD;
    c.kind[0] = HMI_STORE_APPEND;
    c.marker[0] = true;
    c.path[1] = JRN;
    c.kind[1] = HMI_STORE_APPEND;
    c.path[2] = STA;
    c.kind[2] = HMI_STORE_REPLACE;
    c.io = &k_io;
    return c;
}

static bool put(hmi_store_t *s, unsigned t, const char *text, uint32_t *tk)
{
    return hmi_store_enqueue(s, t, text, strlen(text), NULL, NULL, 0, tk);
}

static void test_init(void)
{
    SECTION("store: configuration is validated");
    hmi_store_t *s = (hmi_store_t *)calloc(1, sizeof(*s));
    hmi_store_cfg_t c;

    memset(&c, 0, sizeof(c));
    CHECK(!hmi_store_init(s, &c)); /* no target */
    CHECK(!hmi_store_init(s, NULL));

    c = base_cfg();
    c.kind[0] = (hmi_store_kind_t)0; /* a used target must have a kind */
    CHECK(!hmi_store_init(s, &c));

    c = base_cfg();
    c.max_bytes = 100;
    c.generations = 0;
    CHECK(!hmi_store_init(s, &c)); /* a cap needs at least one generation */
    c.generations = 10;
    CHECK(!hmi_store_init(s, &c));
    c.generations = 9;
    CHECK(hmi_store_init(s, &c));
    hmi_store_destroy(s);

    c = base_cfg();
    char longp[300];
    memset(longp, 'a', sizeof(longp) - 1);
    longp[sizeof(longp) - 1] = '\0';
    c.path[0] = longp;
    CHECK(!hmi_store_init(s, &c)); /* no room for the generation suffix */

    c = base_cfg();
    c.max_bytes = 0; /* no cap: generations are not needed */
    CHECK(hmi_store_init(s, &c));
    hmi_store_destroy(s);
    free(s);
}

static int g_cb_n, g_cb_ok, g_cb_fail;
static uint32_t g_cb_arg[8];
static void on_done(void *user, uint32_t arg, bool ok)
{
    (void)user;
    if (g_cb_n < 8) g_cb_arg[g_cb_n] = arg;
    g_cb_n++;
    if (ok) g_cb_ok++; else g_cb_fail++;
}

static void test_append_order(void)
{
    SECTION("store: records are written in order, tickets and callbacks report the outcome");
    clean_all();
    reset_inject();
    hmi_store_t *s = (hmi_store_t *)calloc(1, sizeof(*s));
    hmi_store_cfg_t c = base_cfg();
    CHECK(hmi_store_init(s, &c));

    uint32_t t1 = 0, t2 = 0, t3 = 0;
    g_cb_n = g_cb_ok = g_cb_fail = 0;
    CHECK(hmi_store_enqueue(s, 0, "one\n", 4, on_done, NULL, 11, &t1));
    CHECK(put(s, 0, "two\n", &t2));
    CHECK(hmi_store_enqueue(s, 1, "jrn\n", 4, on_done, NULL, 33, &t3));
    CHECK(t1 != 0 && t2 == t1 + 1 && t3 == t2 + 1);
    CHECK(hmi_store_ticket(s, t1) == HMI_STORE_PENDING);
    CHECK(hmi_store_ticket(s, 0) == HMI_STORE_UNKNOWN);
    CHECK(hmi_store_ticket(s, t3 + 1) == HMI_STORE_UNKNOWN);

    CHECK(hmi_store_run_once(s));
    CHECK(hmi_store_ticket(s, t1) == HMI_STORE_DONE);
    CHECK(hmi_store_ticket(s, t2) == HMI_STORE_PENDING); /* strictly one at a time, in order */
    CHECK(g_cb_n == 1 && g_cb_arg[0] == 11 && g_cb_ok == 1);
    CHECK(hmi_store_run_once(s));
    CHECK(hmi_store_run_once(s));
    CHECK(!hmi_store_run_once(s)); /* queue is empty */
    CHECK(g_cb_n == 2 && g_cb_arg[1] == 33); /* t2 had no callback */

    char buf[128];
    CHECK(slurp(AUD, buf, sizeof(buf)) == 8 && strcmp(buf, "one\ntwo\n") == 0);
    CHECK(slurp(JRN, buf, sizeof(buf)) == 4 && strcmp(buf, "jrn\n") == 0);
    CHECK(hmi_store_flush(s, 0)); /* nothing queued, nothing in flight */

    hmi_store_stats_t st;
    hmi_store_stats(s, &st);
    CHECK(st.enqueued == 3 && st.written == 3 && st.failed == 0 && st.refused == 0 && !st.fault);
    CHECK(st.max_depth == 3);
    CHECK(!hmi_store_fault(s));
    hmi_store_destroy(s);
    free(s);
    clean_all();
}

static void test_refusals(void)
{
    SECTION("store: a request that cannot be queued is refused and changes nothing");
    clean_all();
    reset_inject();
    hmi_store_t *s = (hmi_store_t *)calloc(1, sizeof(*s));
    hmi_store_cfg_t c = base_cfg();
    c.path[1] = NULL; /* target 1 unused */
    CHECK(hmi_store_init(s, &c));

    uint32_t tk = 99;
    CHECK(!hmi_store_enqueue(s, 1, "x", 1, NULL, NULL, 0, &tk)); /* unused target */
    CHECK(!hmi_store_enqueue(s, HMI_STORE_TARGETS, "x", 1, NULL, NULL, 0, &tk));
    CHECK(!hmi_store_enqueue(s, 0, "x", 0, NULL, NULL, 0, &tk)); /* empty */
    CHECK(!hmi_store_enqueue(s, 0, NULL, 1, NULL, NULL, 0, &tk));
    char big[HMI_STORE_REC_MAX + 1];
    memset(big, 'b', sizeof(big));
    CHECK(!hmi_store_enqueue(s, 0, big, sizeof(big), NULL, NULL, 0, &tk)); /* too long */
    CHECK(hmi_store_enqueue(s, 0, big, HMI_STORE_REC_MAX, NULL, NULL, 0, &tk)); /* exactly the limit */
    CHECK(hmi_store_run_once(s));
    CHECK(hmi_store_ticket(s, tk) == HMI_STORE_DONE);
    uint64_t sz = 0;
    CHECK(hmi_fs_size(AUD, &sz) && sz == HMI_STORE_REC_MAX);

    /* fill the queue */
    uint32_t first = 0, last = 0;
    for (int i = 0; i < HMI_STORE_QUEUE_LEN; i++) CHECK(hmi_store_enqueue(s, 0, "r\n", 2, NULL, NULL, 0, i ? &last : &first));
    CHECK(!hmi_store_enqueue(s, 0, "r\n", 2, NULL, NULL, 0, &tk)); /* full: refused, nothing dropped */
    hmi_store_stats_t st;
    hmi_store_stats(s, &st);
    CHECK(st.refused >= 1 && st.max_depth == HMI_STORE_QUEUE_LEN);
    int n = 0;
    while (hmi_store_run_once(s)) n++;
    CHECK(n == HMI_STORE_QUEUE_LEN);
    CHECK(hmi_store_ticket(s, first) == HMI_STORE_DONE && hmi_store_ticket(s, last) == HMI_STORE_DONE);
    CHECK(hmi_fs_size(AUD, &sz) && sz == HMI_STORE_REC_MAX + 2u * HMI_STORE_QUEUE_LEN);
    /* a ticket that is no longer remembered is "unknown", never "done" */
    for (int i = 0; i < HMI_STORE_RESULTS + 5; i++) {
        CHECK(hmi_store_enqueue(s, 0, "z\n", 2, NULL, NULL, 0, &tk));
        hmi_store_run_once(s);
    }
    CHECK(hmi_store_ticket(s, first) == HMI_STORE_UNKNOWN);
    CHECK(hmi_store_ticket(s, tk) == HMI_STORE_DONE);
    hmi_store_destroy(s);
    free(s);
    clean_all();
}

static void test_fault(void)
{
    SECTION("store: a failed write sets the fault flag, the next success clears it");
    clean_all();
    reset_inject();
    hmi_store_t *s = (hmi_store_t *)calloc(1, sizeof(*s));
    hmi_store_cfg_t c = base_cfg();
    CHECK(hmi_store_init(s, &c));
    uint32_t a, b, d;
    g_cb_n = g_cb_ok = g_cb_fail = 0;

    f_append_fail = 1;
    CHECK(hmi_store_enqueue(s, 0, "lost\n", 5, on_done, NULL, 1, &a));
    hmi_store_run_once(s);
    CHECK(hmi_store_ticket(s, a) == HMI_STORE_FAILED);
    CHECK(hmi_store_fault(s));
    CHECK(g_cb_fail == 1 && g_cb_ok == 0); /* the callback learns of the failure */
    CHECK(!exists(AUD));                   /* nothing half-written */

    CHECK(hmi_store_enqueue(s, 0, "kept\n", 5, on_done, NULL, 2, &b));
    hmi_store_run_once(s);
    CHECK(hmi_store_ticket(s, b) == HMI_STORE_DONE);
    CHECK(!hmi_store_fault(s)); /* cleared by the success */

    f_replace_fail = 1;
    CHECK(hmi_store_enqueue(s, 2, "state", 5, NULL, NULL, 0, &d));
    hmi_store_run_once(s);
    CHECK(hmi_store_ticket(s, d) == HMI_STORE_FAILED && hmi_store_fault(s));
    hmi_store_stats_t st;
    hmi_store_stats(s, &st);
    CHECK(st.failed == 2 && st.written == 1 && st.fault); /* the count is never reset */
    char buf[64];
    CHECK(slurp(AUD, buf, sizeof(buf)) == 5 && strcmp(buf, "kept\n") == 0);
    hmi_store_destroy(s);
    free(s);
    clean_all();
}

static void test_rotation(void)
{
    SECTION("store: rotation keeps a bounded set of generations and marks the new file");
    clean_all();
    reset_inject();
    hmi_store_t *s = (hmi_store_t *)calloc(1, sizeof(*s));
    hmi_store_cfg_t c = base_cfg();
    c.max_bytes = 100;
    c.generations = 3;
    CHECK(hmi_store_init(s, &c));

    char rec[16];
    uint32_t tk;
    for (int i = 0; i < 40; i++) { /* 40 records of 10 bytes: 400 bytes, cap 100 */
        snprintf(rec, sizeof(rec), "rec%06d\n", i);
        CHECK(put(s, 0, rec, &tk));
        hmi_store_run_once(s);
        CHECK(hmi_store_ticket(s, tk) == HMI_STORE_DONE);
    }
    CHECK(exists(AUD) && exists(AUD ".1") && exists(AUD ".2") && exists(AUD ".3"));
    CHECK(!exists(AUD ".4")); /* never more than the configured generations */
    uint64_t sz;
    CHECK(hmi_fs_size(AUD, &sz) && sz <= 100u);
    CHECK(hmi_fs_size(AUD ".1", &sz) && sz <= 100u);
    CHECK(hmi_fs_size(AUD ".3", &sz) && sz <= 100u);

    char buf[256];
    CHECK(slurp(AUD, buf, sizeof(buf)) != (size_t)-1);
    CHECK(strstr(buf, "LOG_ROTATED generations=3") != NULL); /* marker at the start of the new file */
    CHECK(strstr(buf, "rec000039") != NULL);                  /* newest record is in the live file */
    CHECK(slurp(AUD ".1", buf, sizeof(buf)) != (size_t)-1 && strstr(buf, "LOG_ROTATED") != NULL);
    hmi_store_stats_t st;
    hmi_store_stats(s, &st);
    CHECK(st.rotations >= 4);

    /* the generations are in order: the highest record numbers are the newest files */
    char b0[256], b1[256], b3[256];
    slurp(AUD, b0, sizeof(b0));
    slurp(AUD ".1", b1, sizeof(b1));
    slurp(AUD ".3", b3, sizeof(b3));
    const char *r0 = strstr(b0, "rec"), *r1 = strstr(b1, "rec"), *r3 = strstr(b3, "rec");
    CHECK(r0 && r1 && r3 && strcmp(r0, r1) > 0 && strcmp(r1, r3) > 0);

    /* the journal target has no marker and a separate size */
    CHECK(put(s, 1, "j\n", &tk));
    hmi_store_run_once(s);
    CHECK(slurp(JRN, buf, sizeof(buf)) == 2 && strcmp(buf, "j\n") == 0);
    hmi_store_destroy(s);
    free(s);
    clean_all();
}

static void test_rotation_failure(void)
{
    SECTION("store: a failed rotation refuses the record and recovers later");
    clean_all();
    reset_inject();
    hmi_store_t *s = (hmi_store_t *)calloc(1, sizeof(*s));
    hmi_store_cfg_t c = base_cfg();
    c.max_bytes = 30;
    c.generations = 2;
    CHECK(hmi_store_init(s, &c));
    uint32_t tk;
    CHECK(put(s, 0, "0123456789012345678\n", &tk)); /* 20 bytes */
    hmi_store_run_once(s);
    CHECK(hmi_store_ticket(s, tk) == HMI_STORE_DONE);

    f_rename_fail = 1;
    CHECK(put(s, 0, "abcdefghij\n", &tk)); /* 20 + 11 > 30: rotation needed, rename fails */
    hmi_store_run_once(s);
    CHECK(hmi_store_ticket(s, tk) == HMI_STORE_FAILED && hmi_store_fault(s));
    char buf[256];
    CHECK(slurp(AUD, buf, sizeof(buf)) == 20 && !exists(AUD ".1")); /* old data untouched, record not written */

    CHECK(put(s, 0, "abcdefghij\n", &tk)); /* same record again: rotation now works */
    hmi_store_run_once(s);
    CHECK(hmi_store_ticket(s, tk) == HMI_STORE_DONE && !hmi_store_fault(s));
    CHECK(exists(AUD ".1"));
    CHECK(slurp(AUD ".1", buf, sizeof(buf)) == 20);
    CHECK(slurp(AUD, buf, sizeof(buf)) != (size_t)-1 && strstr(buf, "abcdefghij") != NULL);
    hmi_store_destroy(s);
    free(s);
    clean_all();
}

static void test_torn(void)
{
    SECTION("store: a torn append is reported as failed and the size is measured again");
    clean_all();
    reset_inject();
    hmi_store_t *s = (hmi_store_t *)calloc(1, sizeof(*s));
    hmi_store_cfg_t c = base_cfg();
    c.max_bytes = 50;
    c.generations = 2;
    CHECK(hmi_store_init(s, &c));
    uint32_t tk;
    CHECK(put(s, 0, "aaaaaaaaaaaaaaaaaaaa\n", &tk)); /* 21 */
    hmi_store_run_once(s);
    f_append_torn = 1;
    CHECK(put(s, 0, "bbbbbbbbbbbbbbbbbbbb\n", &tk)); /* torn: half of it lands */
    hmi_store_run_once(s);
    CHECK(hmi_store_ticket(s, tk) == HMI_STORE_FAILED && hmi_store_fault(s));
    uint64_t sz;
    CHECK(hmi_fs_size(AUD, &sz) && sz == 21u + 10u);
    /* The cached size was dropped, so the next record sees 31 bytes (and a torn
     * last line, which is finished with a line break: 32 bytes) and rotates
     * correctly instead of trusting a stale 21. */
    CHECK(put(s, 0, "cccccccccccccccccccc\n", &tk)); /* 32 + 21 > 50 */
    hmi_store_run_once(s);
    CHECK(hmi_store_ticket(s, tk) == HMI_STORE_DONE);
    CHECK(exists(AUD ".1") && hmi_fs_size(AUD ".1", &sz) && sz == 32u);
    char buf[128];
    CHECK(slurp(AUD ".1", buf, sizeof(buf)) == 32 && strcmp(buf, "aaaaaaaaaaaaaaaaaaaa\nbbbbbbbbbb\n") == 0);
    hmi_store_destroy(s);
    free(s);
    clean_all();
}

static void test_torn_tail_at_start(void)
{
    SECTION("store: an unfinished last line is closed before the first record of a new run");
    clean_all();
    reset_inject();
    char buf[128];

    /* a file that ends in the middle of a record */
    CHECK(hmi_fs_atomic_write(AUD, "old record\nhalf a rec", strlen("old record\nhalf a rec")));
    hmi_store_t *s = (hmi_store_t *)calloc(1, sizeof(*s));
    hmi_store_cfg_t c = base_cfg();
    CHECK(hmi_store_init(s, &c));
    uint32_t tk;
    CHECK(put(s, 0, "new record\n", &tk));
    hmi_store_run_once(s);
    CHECK(hmi_store_ticket(s, tk) == HMI_STORE_DONE);
    CHECK(put(s, 0, "second\n", &tk));
    hmi_store_run_once(s);
    CHECK(slurp(AUD, buf, sizeof(buf)) != (size_t)-1);
    CHECK(strcmp(buf, "old record\nhalf a rec\nnew record\nsecond\n") == 0); /* one break, not one per record */    hmi_store_destroy(s);

    /* a complete file gets nothing extra, an empty or missing one neither */
    CHECK(hmi_fs_atomic_write(AUD, "complete\n", 9));
    CHECK(hmi_store_init(s, &c));
    CHECK(put(s, 0, "next\n", &tk));
    hmi_store_run_once(s);
    CHECK(slurp(AUD, buf, sizeof(buf)) != (size_t)-1 && strcmp(buf, "complete\nnext\n") == 0);
    hmi_store_destroy(s);
    clean(AUD);
    CHECK(hmi_store_init(s, &c));
    CHECK(put(s, 0, "first\n", &tk));
    hmi_store_run_once(s);
    CHECK(slurp(AUD, buf, sizeof(buf)) != (size_t)-1 && strcmp(buf, "first\n") == 0);
    hmi_store_destroy(s);

    /* the journal target behaves the same */
    CHECK(hmi_fs_atomic_write(JRN, "x", 1));
    CHECK(hmi_store_init(s, &c));
    CHECK(put(s, 1, "line\n", &tk));
    hmi_store_run_once(s);
    CHECK(slurp(JRN, buf, sizeof(buf)) != (size_t)-1 && strcmp(buf, "x\nline\n") == 0);

    /* if the line break itself cannot be written, the record is not either */
    hmi_store_destroy(s);
    CHECK(hmi_fs_atomic_write(AUD, "torn", 4));
    CHECK(hmi_store_init(s, &c));
    f_append_fail = 1; /* the first append is the line break */
    CHECK(put(s, 0, "after\n", &tk));
    hmi_store_run_once(s);
    CHECK(hmi_store_ticket(s, tk) == HMI_STORE_FAILED && hmi_store_fault(s));
    CHECK(slurp(AUD, buf, sizeof(buf)) == 4 && strcmp(buf, "torn") == 0);
    CHECK(put(s, 0, "again\n", &tk)); /* tried again from the start */
    hmi_store_run_once(s);
    CHECK(hmi_store_ticket(s, tk) == HMI_STORE_DONE && !hmi_store_fault(s));
    CHECK(slurp(AUD, buf, sizeof(buf)) != (size_t)-1 && strcmp(buf, "torn\nagain\n") == 0);
    hmi_store_destroy(s);
    free(s);
    clean_all();
}

static void test_replace_coalesce(void)
{
    SECTION("store: a queued state file is replaced by a newer request, not written twice");
    clean_all();
    reset_inject();
    hmi_store_t *s = (hmi_store_t *)calloc(1, sizeof(*s));
    hmi_store_cfg_t c = base_cfg();
    CHECK(hmi_store_init(s, &c));
    uint32_t a, b, d, z;
    g_cb_n = g_cb_ok = g_cb_fail = 0;
    CHECK(hmi_store_enqueue(s, 2, "state-1", 7, on_done, NULL, 1, &a));
    CHECK(put(s, 0, "audit\n", &z));
    CHECK(hmi_store_enqueue(s, 2, "state-2", 7, on_done, NULL, 2, &b));
    CHECK(hmi_store_enqueue(s, 2, "state-3-longer", 14, on_done, NULL, 3, &d));
    CHECK(a == b && b == d); /* one request, one ticket */
    int n = 0;
    while (hmi_store_run_once(s)) n++;
    CHECK(n == 2 && n_replace == 1); /* the audit append and one state write */
    char buf[64];
    CHECK(slurp(STA, buf, sizeof(buf)) == 14 && strcmp(buf, "state-3-longer") == 0);
    CHECK(g_cb_n == 1 && g_cb_arg[0] == 3); /* the newest caller is the one told */

    CHECK(hmi_store_enqueue(s, 2, "state-4", 7, NULL, NULL, 0, &a)); /* nothing queued now: a new request */
    CHECK(a != d);
    hmi_store_run_once(s);
    CHECK(slurp(STA, buf, sizeof(buf)) == 7 && strcmp(buf, "state-4") == 0);
    hmi_store_destroy(s);
    free(s);
    clean_all();
}

static void test_worker(void)
{
    SECTION("store: the worker thread writes in order, waits work, stop drains the queue");
    clean_all();
    reset_inject();
    hmi_store_t *s = (hmi_store_t *)calloc(1, sizeof(*s));
    hmi_store_cfg_t c = base_cfg();
    CHECK(hmi_store_init(s, &c));
    CHECK(hmi_store_start(s));
    CHECK(!hmi_store_start(s)); /* once */

    uint32_t tk;
    CHECK(put(s, 0, "w1\n", &tk));
    CHECK(hmi_store_wait(s, tk, 5000) == HMI_STORE_DONE);
    CHECK(hmi_store_wait(s, tk, 0) == HMI_STORE_DONE); /* already done */
    CHECK(hmi_store_wait(s, 424242, 10) == HMI_STORE_UNKNOWN);

    for (int i = 0; i < 40; i++) {
        char r[16];
        snprintf(r, sizeof(r), "n%03d\n", i);
        while (!put(s, 0, r, &tk)) hmi_sleep_ms(1); /* full: wait for the worker to drain */
    }
    CHECK(hmi_store_flush(s, 5000));
    char buf[512];
    CHECK(slurp(AUD, buf, sizeof(buf)) != (size_t)-1);
    CHECK(strncmp(buf, "w1\nn000\nn001\n", 13) == 0);
    CHECK(strstr(buf, "n039\n") != NULL);

    for (int i = 0; i < 20; i++) {
        char r[16];
        snprintf(r, sizeof(r), "d%03d\n", i);
        CHECK(put(s, 1, r, &tk));
    }
    CHECK(hmi_store_stop(s, 5000)); /* stop lets the worker finish what is queued */
    CHECK(!put(s, 1, "late\n", &tk)); /* and refuses anything new */
    CHECK(slurp(JRN, buf, sizeof(buf)) == 100u && strstr(buf, "d019\n") != NULL && strstr(buf, "late") == NULL);
    CHECK(hmi_store_stop(s, 100)); /* a second stop is harmless */
    hmi_store_destroy(s);
    free(s);
    clean_all();
}

typedef struct {
    hmi_store_t *s;
    int n;
} feeder_t;
static void feeder(void *arg)
{
    feeder_t *f = (feeder_t *)arg;
    for (int i = 0; i < f->n; i++) {
        char r[16];
        snprintf(r, sizeof(r), "t%05d\n", i);
        while (!hmi_store_enqueue(f->s, 1, r, 7, NULL, NULL, 0, NULL)) hmi_sleep_ms(1);
    }
}

static void test_threads(void)
{
    SECTION("store: two producers and the worker run concurrently without loss or reordering");
    clean_all();
    reset_inject();
    hmi_store_t *s = (hmi_store_t *)calloc(1, sizeof(*s));
    hmi_store_cfg_t c = base_cfg();
    CHECK(hmi_store_init(s, &c));
    CHECK(hmi_store_start(s));
    feeder_t f1 = { s, 300 };
    hmi_thread_t t1;
    CHECK(hmi_thread_start(&t1, feeder, &f1));
    for (int i = 0; i < 300; i++) { /* this thread feeds the audit target meanwhile */
        char r[16];
        snprintf(r, sizeof(r), "a%05d\n", i);
        while (!put(s, 0, r, NULL)) hmi_sleep_ms(1);
    }
    CHECK(hmi_thread_join(&t1, 20000));
    CHECK(hmi_store_flush(s, 20000));
    CHECK(hmi_store_stop(s, 5000));
    static char big[4096];
    CHECK(slurp(AUD, big, sizeof(big)) == 2100u);
    CHECK(slurp(JRN, big, sizeof(big)) == 2100u);
    /* per target, order is the order of submission */
    bool ordered = true;
    for (int i = 0; i < 300 && ordered; i++) {
        char want[16];
        snprintf(want, sizeof(want), "t%05d\n", i);
        ordered = memcmp(big + 7 * i, want, 7) == 0;
    }
    CHECK(ordered);
    hmi_store_stats_t st;
    hmi_store_stats(s, &st);
    CHECK(st.written == 600 && st.failed == 0);
    hmi_store_destroy(s);
    free(s);
    clean_all();
}

int main(void)
{
    test_init();
    test_append_order();
    test_refusals();
    test_fault();
    test_rotation();
    test_rotation_failure();
    test_torn();
    test_torn_tail_at_start();
    test_replace_coalesce();
    test_worker();
    test_threads();
    return test_finish();
}
