/* Tests for hmi_auditchain (the keyed hash chain of the audit file) and its use by the storage worker.
 * Part of the HMI core host tests; shared fixtures are in test_support.h. */
#include "test_support.h"
#include "../../src/hmi/hmi_auditchain.h"
#include "../../src/hmi/hmi_store.h"

static const uint8_t CFG_KEY[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                     17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };

/* Builds a chained text of n records "<i> EVENT n=<i> wall=- sync=no" into buf, as the writers do. */
static size_t build(const uint8_t *key, char *buf, size_t cap, int n)
{
    size_t len = 0;
    char prev[HMI_CHAIN_HEX + 1] = "00000000000000000000000000000000";
    for (int i = 1; i <= n; i++) {
        char rec[128];
        int m = snprintf(rec, sizeof(rec), "%d EVENT n=%d wall=- sync=no\n", 1000 + i, i);
        size_t s = hmi_chain_seal(key, prev, rec, (size_t)m, buf + len, cap - len);
        CHECK(s == (size_t)m + HMI_CHAIN_SEAL_LEN);
        hmi_chain_last_seal(buf, len + s, prev);
        len += s;
    }
    return len;
}

static hmi_chain_report_t check_text(const uint8_t *key, const char *text, size_t len)
{
    hmi_chain_report_t r;
    memset(&r, 0, sizeof(r));
    char prev[HMI_CHAIN_HEX + 1] = "";
    hmi_chain_verify_text(key, text, len, prev, &r);
    return r;
}

/* Offset of the start of line k (1-based) in text. */
static size_t line_at(const char *text, size_t len, int k)
{
    size_t pos = 0;
    for (int i = 1; i < k && pos < len; i++) {
        while (pos < len && text[pos] != '\n') pos++;
        pos++;
    }
    return pos;
}

static size_t line_len(const char *text, size_t len, size_t at)
{
    size_t e = at;
    while (e < len && text[e] != '\n') e++;
    return e - at + 1;
}

static void test_chain_basics(void)
{
    SECTION("auditchain: key derivation, seal format, a clean chain verifies");
    uint8_t k1[32], k2[32];
    hmi_chain_derive_key(CFG_KEY, sizeof(CFG_KEY), k1);
    hmi_chain_derive_key(CFG_KEY, sizeof(CFG_KEY), k2);
    CHECK(memcmp(k1, k2, 32) == 0);                      /* deterministic */
    CHECK(memcmp(k1, CFG_KEY, 32) != 0);                 /* not the configuration key itself */

    char rec[64] = "12 BOOT wall=- sync=no\n", out[128];
    CHECK(hmi_chain_seal(k1, "00000000000000000000000000000000", rec, strlen(rec), out, sizeof(out)) == strlen(rec) + 37);
    size_t body;
    CHECK(hmi_chain_has_seal(out, strlen(rec) + 36, &body) && body == strlen(rec) - 1);
    CHECK(strncmp(out, "12 BOOT wall=- sync=no mac=", 27) == 0);
    CHECK(hmi_chain_seal(k1, "00000000000000000000000000000000", "no newline", 10, out, sizeof(out)) == 0);
    CHECK(hmi_chain_seal(k1, "00000000000000000000000000000000", rec, strlen(rec), out, strlen(rec) + 36) == 0); /* no room */
    CHECK(!hmi_chain_has_seal("x mac=0123456789ABCDEF0123456789abcdef", 38, NULL)); /* upper case is not a seal */
    CHECK(!hmi_chain_has_seal("x mac=0123", 10, NULL));

    char text[4096];
    size_t len = build(k1, text, sizeof(text), 6);
    hmi_chain_report_t r = check_text(k1, text, len);
    CHECK(r.lines == 6 && r.verified == 6 && r.failed == 0 && r.unsealed == 0 && r.first_bad_line == 0);
    r = check_text(k2, text, len);
    CHECK(r.verified == 6);
    uint8_t other[32];
    memcpy(other, k1, 32);
    other[0] ^= 1;
    r = check_text(other, text, len);
    CHECK(r.failed == 6 && r.verified == 0);             /* the wrong key fails every line */
}

static void test_chain_tamper(void)
{
    SECTION("auditchain: edits, deletions, insertions, reordering and removed seals are found");
    uint8_t k[32];
    hmi_chain_derive_key(CFG_KEY, sizeof(CFG_KEY), k);
    char text[4096], t[4096];
    size_t len = build(k, text, sizeof(text), 6);
    hmi_chain_report_t r;

    /* edit one character of line 3 */
    memcpy(t, text, len);
    size_t a3 = line_at(t, len, 3);
    t[a3 + 2] = (t[a3 + 2] == '9') ? '8' : '9';
    r = check_text(k, t, len);
    CHECK(r.failed == 1 && r.first_bad_line == 3 && r.verified == 5);

    /* delete line 3: line 4 was sealed after it */
    size_t l3 = line_len(text, len, a3);
    memcpy(t, text, a3);
    memcpy(t + a3, text + a3 + l3, len - a3 - l3);
    r = check_text(k, t, len - l3);
    CHECK(r.lines == 5 && r.failed == 1 && r.first_bad_line == 3);

    /* insert an unsealed line after line 2: it is reported, the chain continues around it */
    size_t a3b = line_at(text, len, 3);
    const char *ins = "1999 EVENT n=99 wall=- sync=no\n";
    size_t il = strlen(ins);
    memcpy(t, text, a3b);
    memcpy(t + a3b, ins, il);
    memcpy(t + a3b + il, text + a3b, len - a3b);
    r = check_text(k, t, len + il);
    CHECK(r.unsealed == 1 && r.first_bad_line == 3 && r.verified == 6);

    /* insert a line with a made-up seal: it fails, and so does the line after it */
    const char *forged = "1999 EVENT n=99 wall=- sync=no mac=0123456789abcdef0123456789abcdef\n";
    il = strlen(forged);
    memcpy(t, text, a3b);
    memcpy(t + a3b, forged, il);
    memcpy(t + a3b + il, text + a3b, len - a3b);
    r = check_text(k, t, len + il);
    CHECK(r.failed == 2 && r.verified == 5);

    /* swap lines 2 and 3 */
    size_t a2 = line_at(text, len, 2), l2 = line_len(text, len, a2);
    l3 = line_len(text, len, a3b);
    memcpy(t, text, len);
    memcpy(t + a2, text + a3b, l3);
    memcpy(t + a2 + l3, text + a2, l2);
    r = check_text(k, t, len);
    CHECK(r.failed >= 2);

    /* remove the seal of line 4: line 4 is unsealed and line 5 fails */
    size_t a4 = line_at(text, len, 4), l4 = line_len(text, len, a4);
    memcpy(t, text, a4 + l4 - 1 - HMI_CHAIN_SEAL_LEN);
    t[a4 + l4 - 1 - HMI_CHAIN_SEAL_LEN] = '\n';
    size_t cut = a4 + l4 - HMI_CHAIN_SEAL_LEN;
    memcpy(t + cut, text + a4 + l4, len - a4 - l4);
    r = check_text(k, t, len - HMI_CHAIN_SEAL_LEN);
    CHECK(r.unsealed == 1 && r.failed == 1 && r.first_bad_line == 4);

    /* the documented limit: lines cut off the end are not detectable */
    size_t a6 = line_at(text, len, 6);
    r = check_text(k, text, a6);
    CHECK(r.lines == 5 && r.verified == 5 && r.failed == 0);
}

static void test_chain_torn_line(void)
{
    SECTION("auditchain: a torn line fails alone and the chain continues after it");
    uint8_t k[32];
    hmi_chain_derive_key(CFG_KEY, sizeof(CFG_KEY), k);
    char text[4096];
    size_t len = build(k, text, sizeof(text), 3);
    /* a power cut left half a record; the store finishes it with a newline, then appends the next sealed record */
    const char *torn = "1004 EVENT n=4 wall=- sy\n";
    memcpy(text + len, torn, strlen(torn));
    len += strlen(torn);
    char prev[HMI_CHAIN_HEX + 1];
    CHECK(hmi_chain_last_seal(text, len, prev));          /* the torn line has no seal: the one before it is used */
    char rec[64] = "1005 EVENT n=5 wall=- sync=no\n";
    len += hmi_chain_seal(k, prev, rec, strlen(rec), text + len, sizeof(text) - len);
    hmi_chain_report_t r = check_text(k, text, len);
    CHECK(r.lines == 5 && r.verified == 4 && r.unsealed == 1 && r.failed == 0 && r.first_bad_line == 4);
    /* an unfinished last line is not counted */
    memcpy(text + len, "1006 EV", 7);
    r = check_text(k, text, len + 7);
    CHECK(r.lines == 5);
}

/* ---- the storage worker writes a chained audit file ---- */
static int file_text(const char *path, char *buf, size_t cap, size_t *len)
{
    *len = 0;
    return hmi_fs_read_all(path, buf, cap, len) == HMI_FS_READ_OK;
}

static hmi_chain_report_t check_files(const uint8_t *key, const char *const *paths, int n)
{
    static char buf[1 << 16];
    hmi_chain_report_t r;
    memset(&r, 0, sizeof(r));
    char prev[HMI_CHAIN_HEX + 1] = "";
    for (int i = 0; i < n; i++) {
        size_t len;
        if (!file_text(paths[i], buf, sizeof(buf), &len)) continue;
        hmi_chain_verify_text(key, buf, len, prev, &r);
    }
    return r;
}

static void store_open(hmi_store_t *s, const char *path, const uint8_t *key, uint64_t max_bytes)
{
    hmi_store_cfg_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.path[0] = path;
    sc.kind[0] = HMI_STORE_APPEND;
    sc.marker[0] = true;
    sc.max_bytes = max_bytes;
    sc.generations = 3;
    sc.chain_key[0] = key;
    CHECK(hmi_store_init(s, &sc));
}

static void put(hmi_store_t *s, int i)
{
    char rec[96];
    int n = snprintf(rec, sizeof(rec), "%d WRITE_SENT id=%d tag=0 wall=- sync=no\n", 5000 + i, i);
    CHECK(hmi_store_enqueue(s, 0, rec, (size_t)n, NULL, NULL, 0, NULL));
    while (hmi_store_run_once(s)) {}
}

static void test_store_chain(void)
{
    SECTION("auditchain: the storage worker seals records across rotation, restart and an outside writer");
    const char *path = "p99_chain_test.log";
    char p1[64], p2[64], p3[64];
    snprintf(p1, sizeof(p1), "%s.1", path);
    snprintf(p2, sizeof(p2), "%s.2", path);
    snprintf(p3, sizeof(p3), "%s.3", path);
    remove(path); remove(p1); remove(p2); remove(p3);
    uint8_t k[32];
    hmi_chain_derive_key(CFG_KEY, sizeof(CFG_KEY), k);

    static hmi_store_t s;
    store_open(&s, path, k, 1200);  /* about 12 records per file: several rotations */
    for (int i = 0; i < 30; i++) put(&s, i);
    hmi_store_stats_t st;
    hmi_store_stats(&s, &st);
    CHECK(st.rotations >= 2 && st.failed == 0);
    hmi_store_destroy(&s);

    const char *seq[] = { p3, p2, p1, path };
    hmi_chain_report_t r = check_files(k, seq, 4);
    CHECK(r.lines > 30 && r.failed == 0 && r.unsealed == 0); /* 30 records plus the LOG_ROTATED markers */
    const char *live[] = { path };
    r = check_files(k, live, 1);
    CHECK(r.lines > 0 && r.failed == 0 && r.unsealed == 0);  /* the live file alone, from its prev= */

    /* restart: a new store continues the chain from the file */
    store_open(&s, path, k, 1200);
    put(&s, 100);
    /* an outside writer (the start-up record, p99_cfg) seals after the file's last seal */
    char prev[HMI_CHAIN_HEX + 1], line[160];
    hmi_chain_prev_for(path, prev);
    const char *rec = "6000 CONFIG_CHANGED serial=2 from_serial=1 changes=1 by=- wall=- sync=no\n";
    size_t n = hmi_chain_seal(k, prev, rec, strlen(rec), line, sizeof(line));
    CHECK(n > 0 && hmi_fs_append_sync(path, line, n));
    put(&s, 101);
    hmi_store_destroy(&s);
    r = check_files(k, seq, 4);
    CHECK(r.failed == 0 && r.unsealed == 0);

    /* tampering with a rotated file is found */
    static char buf[1 << 16];
    size_t len;
    CHECK(file_text(p1, buf, sizeof(buf), &len) && len > 40);
    buf[3] = (buf[3] == '7') ? '6' : '7';
    CHECK(hmi_fs_atomic_write(p1, buf, len));
    r = check_files(k, seq, 4);
    CHECK(r.failed >= 1);

    /* without a key the store writes exactly as before: no seal */
    remove(path); remove(p1); remove(p2); remove(p3);
    store_open(&s, path, NULL, 0);
    put(&s, 1);
    hmi_store_destroy(&s);
    CHECK(file_text(path, buf, sizeof(buf), &len) && len > 0 && !hmi_chain_has_seal(buf, len - 1, NULL));
    remove(path);
}

int main(void)
{
    test_chain_basics();
    test_chain_tamper();
    test_chain_torn_line();
    test_store_chain();
    return test_finish();
}
