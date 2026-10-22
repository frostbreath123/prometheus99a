/* Verifier of the kill-loop test: reads the files left in DIR after a process
 * was killed (or stopped) and checks what must hold whatever the moment of the
 * kill was. Exit status 0 when everything holds, 1 otherwise, with the reason.
 *
 *   usage: killloop_verify DIR MAX_TORN
 *
 * MAX_TORN is how many unfinished records each of the audit file and the
 * journal may contain in total (at most one per kill so far).
 *
 * Checked:
 *  - the audit file, oldest generation first: every line is a complete record
 *    of a known form with a wall-clock stamp, except for at most MAX_TORN lines
 *    that are the unfinished end of a run (they are followed by a BOOT or a
 *    LOG_ROTATED line, or end a file);
 *  - for every write id of a run (between two BOOT lines): RESERVED once,
 *    SENT once and only after RESERVED, one final record at most and only after
 *    the record it depends on;
 *  - the alarm journal: every line parses (CRC and fields), at most MAX_TORN do
 *    not, and the sequence numbers increase;
 *  - the alarm state file is absent or valid for the configuration;
 *  - the writes the mock PLC received in the last run (plc.log): each has a
 *    SENT record in the audit file. This is audit before send, through a kill.
 */
#include "test_support.h"
#include "../../src/hmi/hmi_auditfmt.h"

#define MAX_FILE (2 * 1024 * 1024)
#define MAX_IDS 100000

static int fail(const char *why, const char *detail)
{
    printf("VERIFY FAILED: %s%s%s\n", why, detail ? ": " : "", detail ? detail : "");
    return 1;
}

static char *slurp_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = (char *)malloc(MAX_FILE);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    *len = fread(buf, 1, MAX_FILE - 1, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

static bool starts_digits_space(const char *s, size_t len, size_t *after)
{
    size_t i = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') i++;
    if (i == 0 || i >= len || s[i] != ' ') return false;
    *after = i + 1;
    return true;
}

static bool has(const char *line, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    if (n > len) return false;
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(line + i, needle, n) == 0) return true;
    }
    return false;
}

static bool ends_with(const char *line, size_t len, const char *tail)
{
    size_t n = strlen(tail);
    return len >= n && memcmp(line + len - n, tail, n) == 0;
}

static bool event_is(const char *rest, size_t rlen, const char *name)
{
    size_t n = strlen(name);
    return rlen > n && memcmp(rest, name, n) == 0 && rest[n] == ' ';
}

typedef enum { E_NONE, E_BOOT, E_ROTATED, E_ACK, E_RESERVED, E_SENT, E_FINAL, E_BAD } audit_kind_t;

/* Classifies one complete audit line. Returns E_BAD for anything not exactly of a known form. */
static audit_kind_t classify(const char *line, size_t len, uint32_t *id, uint32_t *raw)
{
    size_t a;
    *id = 0;
    *raw = 0;
    if (!starts_digits_space(line, len, &a)) return E_BAD;
    const char *rest = line + a;
    size_t rlen = len - a;
    if (event_is(rest, rlen, "LOG_ROTATED")) return has(rest, rlen, " generations=") ? E_ROTATED : E_BAD;
    if (!(ends_with(line, len, " sync=ok") || ends_with(line, len, " sync=no")) || !has(rest, rlen, " wall=")) return E_BAD;
    if (event_is(rest, rlen, "BOOT")) return E_BOOT;
    if (event_is(rest, rlen, "ALARM_ACK")) {
        return (has(rest, rlen, " result=GRANTED ") || has(rest, rlen, " result=DENIED ")) && has(rest, rlen, " user=killer ")
                   ? E_ACK : E_BAD;
    }
    static const char *const finals[] = { "CONFIRMED", "REJECTED", "EXPIRED", "UNKNOWN", "VERIFY_FAILED", "REJECTED_REVALIDATION" };
    audit_kind_t k = E_BAD;
    if (event_is(rest, rlen, "RESERVED")) k = E_RESERVED;
    else if (event_is(rest, rlen, "SENT")) k = E_SENT;
    else {
        for (size_t i = 0; i < sizeof(finals) / sizeof(finals[0]); i++) if (event_is(rest, rlen, finals[i])) k = E_FINAL;
    }
    if (k == E_BAD) return E_BAD;
    const char *p = strstr(rest, " id=");
    const char *r = strstr(rest, " raw=0x");
    if (!p || !r || !has(rest, rlen, " user=killer ") || !has(rest, rlen, " old=") || !has(rest, rlen, " oldq=")) return E_BAD;
    *id = (uint32_t)strtoul(p + 4, NULL, 10);
    *raw = (uint32_t)strtoul(r + 7, NULL, 16);
    return *id ? k : E_BAD;
}

typedef struct {
    uint8_t reserved, sent, final_;
} idstate_t;

static uint32_t g_sent_raw[200000];
static size_t g_n_sent_raw;

int main(int argc, char **argv)
{
    if (argc != 3) return fail("usage", "killloop_verify DIR MAX_TORN");
    const char *dir = argv[1];
    int max_torn = atoi(argv[2]);
    char path[400];

    /* ---- audit file, oldest generation first ---- */
    static idstate_t ids[MAX_IDS];
    memset(ids, 0, sizeof(ids));
    bool in_run = false; /* a BOOT line has been seen, so the ids below belong to a run we saw from its start */
    int audit_lines = 0, audit_bad = 0, files_seen = 0, n_boot = 0, n_rot = 0, n_sent = 0;
    bool prev_bad = false;
    for (int gen = 8; gen >= 0; gen--) {
        if (gen == 0) snprintf(path, sizeof(path), "%s/audit.log", dir);
        else snprintf(path, sizeof(path), "%s/audit.log.%d", dir, gen);
        size_t len = 0;
        char *buf = slurp_file(path, &len);
        if (!buf) continue;
        files_seen++;
        size_t pos = 0;
        while (pos < len) {
            size_t s = pos;
            while (pos < len && buf[pos] != '\n') pos++;
            bool complete = pos < len;
            size_t ll = pos - s;
            pos++;
            uint32_t id, raw;
            audit_kind_t k = ll ? classify(buf + s, ll, &id, &raw) : E_BAD;
            if (!complete) k = E_BAD; /* no line break: unfinished */
            audit_lines++;
            if (k == E_BAD) {
                audit_bad++;
                prev_bad = true;
                continue;
            }
            if (prev_bad && k != E_BOOT && k != E_ROTATED) {
                /* an unfinished line must be followed by a new run or a new file */
                free(buf);
                return fail("audit file: damaged line is followed by an ordinary record", NULL);
            }
            prev_bad = false;
            if (k == E_BOOT) {
                memset(ids, 0, sizeof(ids));
                in_run = true;
                n_boot++;
            } else if (k == E_ROTATED) {
                n_rot++;
            } else if (k == E_RESERVED || k == E_SENT || k == E_FINAL) {
                if (id >= MAX_IDS) {
                    free(buf);
                    return fail("audit file: write id out of range", NULL);
                }
                idstate_t *st = &ids[id];
                if (in_run) {
                    if (k == E_RESERVED && st->reserved) { free(buf); return fail("audit file: RESERVED twice for one id", NULL); }
                    if (k == E_SENT) {
                        if (!st->reserved) { free(buf); return fail("audit file: SENT without RESERVED", NULL); }
                        if (st->sent) { free(buf); return fail("audit file: SENT twice for one id (transmitted twice)", NULL); }
                        if (st->final_) { free(buf); return fail("audit file: SENT after the final record", NULL); }
                    }
                    if (k == E_FINAL) {
                        const char *rest = buf + s + 1;
                        while (*rest != ' ') rest++;
                        rest++;
                        bool needs_sent = event_is(rest, 40, "CONFIRMED") || event_is(rest, 40, "UNKNOWN") ||
                                          event_is(rest, 40, "VERIFY_FAILED");
                        if (!st->reserved) { free(buf); return fail("audit file: final record without RESERVED", NULL); }
                        if (needs_sent && !st->sent) { free(buf); return fail("audit file: CONFIRMED/UNKNOWN/VERIFY_FAILED without SENT", NULL); }
                        if (st->final_) { free(buf); return fail("audit file: two final records for one id", NULL); }
                    }
                    if (k == E_RESERVED) st->reserved = 1;
                    if (k == E_SENT) st->sent = 1;
                    if (k == E_FINAL) st->final_ = 1;
                }
                if (k == E_SENT) {
                    n_sent++;
                    if (g_n_sent_raw < sizeof(g_sent_raw) / sizeof(g_sent_raw[0])) g_sent_raw[g_n_sent_raw++] = raw;
                }
            }
        }
        if (prev_bad && gen == 0) { /* the live file ends in an unfinished line: normal after a kill */ }
        free(buf);
    }
    if (audit_bad > max_torn) {
        char d[80];
        snprintf(d, sizeof(d), "%d damaged lines, at most %d expected", audit_bad, max_torn);
        return fail("audit file", d);
    }

    /* ---- alarm journal ---- */
    static hmi_app_cfg_t cfg;
    char err[128];
    if (!parse(GOOD_CFG, &cfg, err)) return fail("test configuration", err);
    const char *names[HMI_MAX_TAGS];
    for (size_t i = 0; i < cfg.n_tags; i++) names[i] = cfg.tags[i].name;
    int j_lines = 0, j_bad = 0;
    uint32_t last_seq = 0;
    for (int gen = 8; gen >= 0; gen--) {
        if (gen == 0) snprintf(path, sizeof(path), "%s/alarms.bin.journal", dir);
        else snprintf(path, sizeof(path), "%s/alarms.bin.journal.%d", dir, gen);
        size_t len = 0;
        char *buf = slurp_file(path, &len);
        if (!buf) continue;
        size_t pos = 0;
        while (pos < len) {
            size_t s = pos;
            while (pos < len && buf[pos] != '\n') pos++;
            bool complete = pos < len;
            size_t ll = pos - s;
            pos++;
            hmi_alarm_log_t e;
            j_lines++;
            if (!complete || !hmi_audit_parse_journal(buf + s, ll, names, cfg.n_tags, &e)) {
                j_bad++;
                continue;
            }
            if (e.seq <= last_seq) {
                free(buf);
                char d[80];
                snprintf(d, sizeof(d), "seq %u after %u", (unsigned)e.seq, (unsigned)last_seq);
                return fail("journal: sequence numbers do not increase", d);
            }
            last_seq = e.seq;
        }
        free(buf);
    }
    if (j_bad > max_torn) {
        char d[80];
        snprintf(d, sizeof(d), "%d damaged lines, at most %d expected", j_bad, max_torn);
        return fail("journal", d);
    }

    /* ---- alarm state ---- */
    const char *state_txt = "absent";
    snprintf(path, sizeof(path), "%s/alarms.bin", dir);
    {
        uint8_t sb[HMI_ALARM_STATE_MAX];
        size_t sl = 0;
        hmi_fs_read_t rr = hmi_fs_read_all(path, sb, sizeof(sb), &sl);
        if (rr == HMI_FS_READ_OK) {
            hmi_tagdb_t *db = (hmi_tagdb_t *)calloc(1, sizeof(*db));
            hmi_alarm_t *al = (hmi_alarm_t *)calloc(1, sizeof(*al));
            bool ok = db && al && hmi_tagdb_init(db, cfg.tags, cfg.n_tags, 0) && hmi_alarm_init(al, db) &&
                      hmi_alarm_state_apply(al, sb, sl);
            if (al) hmi_alarm_destroy(al);
            if (db) hmi_tagdb_destroy(db);
            free(al);
            free(db);
            if (!ok) return fail("alarm state file exists but is not valid", NULL);
            state_txt = "valid";
        } else if (rr != HMI_FS_READ_ABSENT) {
            return fail("alarm state file cannot be read", NULL);
        }
    }

    /* ---- every write the PLC received has a SENT record ---- */
    int plc_writes = 0;
    snprintf(path, sizeof(path), "%s/plc.log", dir);
    {
        size_t len = 0;
        char *buf = slurp_file(path, &len);
        if (buf) {
            size_t pos = 0;
            while (pos < len) {
                size_t s = pos;
                while (pos < len && buf[pos] != '\n') pos++;
                if (pos >= len) break; /* an unfinished line was never "received" */
                buf[pos] = '\0';
                unsigned addr = 0, n = 0, v0 = 0, v1 = 0;
                if (sscanf(buf + s, "W addr=%u n=%u v0=%u v1=%u", &addr, &n, &v0, &v1) == 4) {
                    plc_writes++;
                    uint32_t key = (v1 << 16) | v0;
                    bool found = false;
                    for (size_t i = 0; i < g_n_sent_raw; i++) {
                        if (g_sent_raw[i] == key) {
                            g_sent_raw[i] = 0xFFFFFFFFu; /* each SENT record covers one transmission */
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        free(buf);
                        char d[80];
                        snprintf(d, sizeof(d), "write of %u reached the PLC without a SENT record", key);
                        return fail("audit before send", d);
                    }
                }
                pos++;
            }
            free(buf);
        }
    }

    printf("VERIFY ok audit_lines=%d audit_damaged=%d runs=%d rotations=%d sent=%d journal_lines=%d journal_damaged=%d "
           "plc_writes=%d state=%s files=%d\n",
           audit_lines, audit_bad, n_boot, n_rot, n_sent, j_lines, j_bad, plc_writes, state_txt, files_seen);
    return 0;
}
