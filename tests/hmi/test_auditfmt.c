/* Tests for the audit line format and the wall-clock stamp: known dates
 * (epoch, leap day, 2038, the example from the documentation), exact lines,
 * truncation of long alarm lists, and every too-small buffer. */
#include "test_support.h"
#include "../../src/hmi/hmi_auditfmt.h"

static hmi_wall_t wall(uint64_t ms, bool synced)
{
    hmi_wall_t w;
    w.unix_ms = ms;
    w.valid = true;
    w.synced = synced;
    return w;
}

static void test_wall_format(void)
{
    SECTION("auditfmt: wall-clock stamp");
    char b[64];
    hmi_wall_t w = wall(0, false);
    CHECK(hmi_wall_format(&w, b, sizeof(b)) > 0 && strcmp(b, "wall=1970-01-01T00:00:00.000Z sync=no") == 0);
    w = wall(1709164800123ull, true); /* 2024-02-29: a leap day */
    CHECK(hmi_wall_format(&w, b, sizeof(b)) > 0 && strcmp(b, "wall=2024-02-29T00:00:00.123Z sync=ok") == 0);
    w = wall(951782400000ull, true); /* 2000-02-29: divisible by 400, so a leap year */
    CHECK(hmi_wall_format(&w, b, sizeof(b)) > 0 && strcmp(b, "wall=2000-02-29T00:00:00.000Z sync=ok") == 0);
    w = wall(951868800000ull, true); /* the day after it */
    CHECK(hmi_wall_format(&w, b, sizeof(b)) > 0 && strcmp(b, "wall=2000-03-01T00:00:00.000Z sync=ok") == 0);
    w = wall(2147483648000ull, true); /* the 32-bit seconds limit */
    CHECK(hmi_wall_format(&w, b, sizeof(b)) > 0 && strcmp(b, "wall=2038-01-19T03:14:08.000Z sync=ok") == 0);
    w = wall(1792664103120ull, true);
    CHECK(hmi_wall_format(&w, b, sizeof(b)) > 0 && strcmp(b, "wall=2026-10-22T10:15:03.120Z sync=ok") == 0);
    w = wall(1798761599999ull, false); /* last millisecond of 2026 */
    CHECK(hmi_wall_format(&w, b, sizeof(b)) > 0 && strcmp(b, "wall=2026-12-31T23:59:59.999Z sync=no") == 0);
    w = wall(4102444800000ull, true); /* 2100-01-01 */
    CHECK(hmi_wall_format(&w, b, sizeof(b)) > 0 && strcmp(b, "wall=2100-01-01T00:00:00.000Z sync=ok") == 0);

    w.valid = false; /* an unreadable clock is never presented as a time, and never as synchronised */
    w.synced = true;
    CHECK(hmi_wall_format(&w, b, sizeof(b)) > 0 && strcmp(b, "wall=- sync=no") == 0);

    w = wall(1792664103120ull, true);
    char small[8];
    CHECK(hmi_wall_format(&w, small, sizeof(small)) == 0);
    CHECK(strlen("wall=2026-10-22T10:15:03.120Z sync=ok") + 1 <= HMI_WALL_STAMP_MAX);

    hmi_wall_t now;
    hmi_wall_now(&now);
    CHECK(now.valid && now.unix_ms > 1735689600000ull);
#ifdef _WIN32
    CHECK(!now.synced);
#endif
}

static hmi_write_rec_t make_rec(void)
{
    hmi_write_rec_t r;
    memset(&r, 0, sizeof(r));
    r.id = 7;
    r.tag = 2;
    r.role = HMI_ROLE_OPERATOR;
    snprintf(r.actor, sizeof(r.actor), "op1");
    r.eng = 42.5;
    r.raw = 0x1A9;
    r.t_submit_ms = 12345;
    r.old_eng = 40.0;
    r.old_quality = HMI_Q_GOOD;
    return r;
}

static void test_write_line(void)
{
    SECTION("auditfmt: PLC write record");
    char b[320];
    hmi_write_rec_t r = make_rec();
    hmi_wall_t w = wall(1792664103120ull, true);
    size_t n = hmi_audit_format_write(b, sizeof(b), &r, "SENT", &w);
    CHECK(n == strlen(b) && n > 0);
    CHECK(strcmp(b, "12345 SENT id=7 tag=2 role=1 eng=42.5 raw=0x000001A9 rb=0x00000000 exc=0 user=op1 old=40 oldq=GOOD "
                    "wall=2026-10-22T10:15:03.120Z sync=ok\n") == 0);

    /* a previous value that is not GOOD is not recorded as a number */
    r.old_quality = HMI_Q_BAD;
    r.old_eng = 98765.0;
    n = hmi_audit_format_write(b, sizeof(b), &r, "RESERVED", &w);
    CHECK(n > 0 && strstr(b, " old=- oldq=BAD ") != NULL && strstr(b, "98765") == NULL);
    r.old_quality = HMI_Q_UNAVAILABLE;
    CHECK(hmi_audit_format_write(b, sizeof(b), &r, "RESERVED", &w) > 0 && strstr(b, " old=- oldq=UNAVAILABLE ") != NULL);
    r.old_quality = HMI_Q_UNCERTAIN;
    CHECK(hmi_audit_format_write(b, sizeof(b), &r, "RESERVED", &w) > 0 && strstr(b, " old=- oldq=UNCERTAIN ") != NULL);

    w = wall(0, false);
    CHECK(hmi_audit_format_write(b, sizeof(b), &r, "CONFIRMED", &w) > 0 && strstr(b, "sync=no\n") != NULL);
    w.valid = false;
    CHECK(hmi_audit_format_write(b, sizeof(b), &r, "CONFIRMED", &w) > 0 && strstr(b, " wall=- sync=no\n") != NULL);

    /* every shorter buffer is refused and nothing is written past the end */
    w = wall(1792664103120ull, true);
    hmi_audit_format_write(b, sizeof(b), &r, "SENT", &w);
    size_t full = strlen(b);
    bool all_refused = true, guard_ok = true;
    for (size_t cap = 1; cap <= full; cap++) {
        char g[400];
        memset(g, 0x5A, sizeof(g));
        size_t k = hmi_audit_format_write(g, cap, &r, "SENT", &w);
        if (k != 0) all_refused = false;
        for (size_t i = cap; i < sizeof(g); i++) if (g[i] != 0x5A) guard_ok = false;
    }
    CHECK(all_refused && guard_ok);
    CHECK(hmi_audit_format_write(b, full + 1, &r, "SENT", &w) == full); /* exactly enough room */
}

static void test_ack_line(void)
{
    SECTION("auditfmt: acknowledgement record");
    char b[704];
    const char *names[2] = { "LVL", "FLOW" };
    hmi_ack_audit_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.t_ms = 500;
    rec.role = HMI_ROLE_OPERATOR;
    snprintf(rec.actor, sizeof(rec.actor), "op1");
    rec.all = true;
    rec.result = HMI_ACK_OK;
    rec.n_points = 2;
    rec.points[0] = 0;
    rec.points[1] = 2; /* equal to the number of names: the PLC link alarm */
    hmi_wall_t w = wall(1792664103120ull, false);
    size_t n = hmi_audit_format_ack(b, sizeof(b), &rec, names, 2, &w);
    CHECK(n == strlen(b));
    CHECK(strcmp(b, "500 ALARM_ACK role=operator action=ACK_ALL result=GRANTED n=2 points=LVL,PLC_LINK user=op1 "
                    "wall=2026-10-22T10:15:03.120Z sync=no\n") == 0);

    rec.all = false;
    rec.result = HMI_ACK_DENIED;
    rec.role = HMI_ROLE_VIEWER;
    rec.n_points = 0; /* an invalid point names nothing */
    CHECK(hmi_audit_format_ack(b, sizeof(b), &rec, names, 2, &w) > 0 &&
          strstr(b, "action=ACK result=DENIED n=0 points= user=op1 wall=") != NULL);

    /* a list too long for the line is cut, says how many were left out, and keeps the user and the stamp */
    rec.all = true;
    rec.result = HMI_ACK_OK;
    rec.role = HMI_ROLE_ADMIN;
    snprintf(rec.actor, sizeof(rec.actor), "abcdefghijklmnopqrstuvwx");
    static char long_names[HMI_MAX_TAGS][HMI_NAME_LEN];
    const char *lp[HMI_MAX_TAGS];
    for (int i = 0; i < HMI_MAX_TAGS; i++) {
        snprintf(long_names[i], HMI_NAME_LEN, "TAG_WITH_A_LONG_NAME_%02d", i);
        lp[i] = long_names[i];
    }
    rec.n_points = HMI_MAX_TAGS + 1;
    for (size_t i = 0; i < rec.n_points; i++) rec.points[i] = (uint8_t)i;
    n = hmi_audit_format_ack(b, sizeof(b), &rec, lp, HMI_MAX_TAGS, &w);
    CHECK(n > 0 && n == strlen(b) && n < sizeof(b));
    CHECK(strstr(b, ",+") != NULL && strstr(b, "_more user=abcdefghijklmnopqrstuvwx wall=2026-10-22T10:15:03.120Z sync=no\n") != NULL);
    CHECK(b[n - 1] == '\n' && strchr(b, '\n') == &b[n - 1]); /* one line */

    /* a short buffer is refused rather than written without the user */
    bool refused_or_complete = true, guard_ok = true;
    for (size_t cap = 1; cap < 200; cap++) {
        char g[300];
        memset(g, 0x5A, sizeof(g));
        rec.n_points = 2;
        size_t k = hmi_audit_format_ack(g, cap, &rec, names, 2, &w);
        if (k != 0 && strstr(g, " user=") == NULL) refused_or_complete = false;
        for (size_t i = cap; i < sizeof(g); i++) if (g[i] != 0x5A) guard_ok = false;
    }
    CHECK(refused_or_complete && guard_ok);
}

static void test_boot_line(void)
{
    SECTION("auditfmt: boot marker");
    char b[96];
    hmi_wall_t w = wall(1792664103120ull, true);
    CHECK(hmi_audit_format_boot(b, sizeof(b), 1000, &w) > 0);
    CHECK(strcmp(b, "1000 BOOT wall=2026-10-22T10:15:03.120Z sync=ok\n") == 0);
    w.valid = false;
    CHECK(hmi_audit_format_boot(b, sizeof(b), 0, &w) > 0 && strcmp(b, "0 BOOT wall=- sync=no\n") == 0);
    CHECK(hmi_audit_format_boot(b, 10, 1000, &w) == 0);
}

static hmi_alarm_log_t make_event(void)
{
    hmi_alarm_log_t e;
    memset(&e, 0, sizeof(e));
    e.seq = 5;
    e.ts_ms = 1500;
    e.point = 1;
    e.kind = HMI_ALK_HI;
    e.event = HMI_ALE_RAISED;
    e.state_after = HMI_AL_ACTIVE_UNACK;
    e.value = 91.5;
    return e;
}

static void test_journal_line(void)
{
    SECTION("auditfmt: alarm journal line, strict read-back");
    const char *names[2] = { "LVL", "TEMP" };
    char b[400];
    hmi_wall_t w = wall(1792664103120ull, true);
    hmi_alarm_log_t e = make_event(), o;
    size_t n = hmi_audit_format_journal(b, sizeof(b), &e, names, 2, &w);
    CHECK(n > 0 && n == strlen(b) && b[n - 1] == '\n');
    const char *prefix = "1500 ALARM_EVENT seq=5 point=TEMP event=RAISED kind=HI state=ACTIVE_UNACK value=91.5 "
                         "wall=2026-10-22T10:15:03.120Z sync=ok crc=";
    CHECK(strncmp(b, prefix, strlen(prefix)) == 0);
    CHECK(n == strlen(prefix) + 8 + 1); /* eight hex digits and the newline */

    memset(&o, 0, sizeof(o));
    CHECK(hmi_audit_parse_journal(b, n - 1, names, 2, &o));
    CHECK(o.seq == 5 && o.ts_ms == 1500 && o.point == 1 && o.kind == HMI_ALK_HI && o.event == HMI_ALE_RAISED);
    CHECK(o.state_after == HMI_AL_ACTIVE_UNACK && o.value == 91.5);

    /* the PLC link alarm, every event and state name, and a value that is not round */
    e.point = 2; /* equal to the number of names */
    e.kind = HMI_ALK_COMMS;
    e.value = 0.0;
    n = hmi_audit_format_journal(b, sizeof(b), &e, names, 2, &w);
    CHECK(n > 0 && strstr(b, " point=PLC_LINK ") != NULL);
    CHECK(hmi_audit_parse_journal(b, n - 1, names, 2, &o) && o.point == 2 && o.kind == HMI_ALK_COMMS);
    bool all_names = true;
    for (int ev = HMI_ALE_RAISED; ev <= HMI_ALE_CLEARED; ev++) {
        for (int st = HMI_AL_NORMAL; st <= HMI_AL_RTN_UNACK; st++) {
            for (int kd = HMI_ALK_NONE; kd <= HMI_ALK_COMMS; kd++) {
                e = make_event();
                e.event = (uint8_t)ev;
                e.state_after = (uint8_t)st;
                e.kind = (uint8_t)kd;
                e.value = -0.1234567;
                n = hmi_audit_format_journal(b, sizeof(b), &e, names, 2, &w);
                if (n == 0 || !hmi_audit_parse_journal(b, n - 1, names, 2, &o) || o.event != ev || o.state_after != st ||
                    o.kind != kd || fabs(o.value - e.value) > 1e-9)
                    all_names = false;
            }
        }
    }
    CHECK(all_names);

    /* values that cannot be a journal entry are not formatted */
    e = make_event();
    e.event = 0;
    CHECK(hmi_audit_format_journal(b, sizeof(b), &e, names, 2, &w) == 0);
    e.event = 99;
    CHECK(hmi_audit_format_journal(b, sizeof(b), &e, names, 2, &w) == 0);
    e = make_event();
    e.kind = 77;
    CHECK(hmi_audit_format_journal(b, sizeof(b), &e, names, 2, &w) == 0);
    e = make_event();
    e.state_after = 9;
    CHECK(hmi_audit_format_journal(b, sizeof(b), &e, names, 2, &w) == 0);

    /* a too small buffer is refused, and nothing is written past it */
    e = make_event();
    n = hmi_audit_format_journal(b, sizeof(b), &e, names, 2, &w);
    bool refused = true, guard_ok = true;
    for (size_t cap = 1; cap < n; cap++) {
        char g[400];
        memset(g, 0x5A, sizeof(g));
        if (hmi_audit_format_journal(g, cap, &e, names, 2, &w) != 0) refused = false;
        for (size_t i = cap; i < sizeof(g); i++) if (g[i] != 0x5A) guard_ok = false;
    }
    CHECK(refused && guard_ok);
    CHECK(hmi_audit_format_journal(b, n + 1, &e, names, 2, &w) == n);

    /* damage: any single flipped bit, and any truncation, makes the line unreadable */
    n = hmi_audit_format_journal(b, sizeof(b), &e, names, 2, &w);
    size_t len = n - 1;
    bool all_rejected = true;
    for (size_t i = 0; i < len; i++) {
        for (int bit = 0; bit < 8; bit++) {
            char c[400];
            memcpy(c, b, len);
            c[i] = (char)(c[i] ^ (1 << bit));
            if (hmi_audit_parse_journal(c, len, names, 2, &o)) all_rejected = false;
        }
    }
    CHECK(all_rejected);
    bool all_short_rejected = true;
    for (size_t k = 0; k < len; k++) {
        if (hmi_audit_parse_journal(b, k, names, 2, &o)) all_short_rejected = false;
    }
    CHECK(all_short_rejected);
    CHECK(!hmi_audit_parse_journal(b, len + 1, names, 2, &o)); /* the newline is not part of the line */

    /* a correct CRC does not make a wrong line acceptable */
    const char *bad[] = {
        "1500 ALARM_EVENT seq=5 point=NOSUCH event=RAISED kind=HI state=ACTIVE_UNACK value=91.5 wall=- sync=no",
        "1500 ALARM_EVENT seq=0 point=LVL event=RAISED kind=HI state=ACTIVE_UNACK value=91.5 wall=- sync=no",
        "1500 ALARM_EVENT seq=5 point=LVL event=BOGUS kind=HI state=ACTIVE_UNACK value=91.5 wall=- sync=no",
        "1500 ALARM_EVENT seq=5 point=LVL event=RAISED kind=XX state=ACTIVE_UNACK value=91.5 wall=- sync=no",
        "1500 ALARM_EVENT seq=5 point=LVL event=RAISED kind=HI state=BOGUS value=91.5 wall=- sync=no",
        "1500 ALARM_EVENT seq=5 point=LVL event=RAISED kind=HI state=ACTIVE_UNACK value=abc wall=- sync=no",
        "1500 ALARM_EVENT seq=5 point=LVL event=RAISED kind=HI state=ACTIVE_UNACK value=91.5x wall=- sync=no",
        "15x0 ALARM_EVENT seq=5 point=LVL event=RAISED kind=HI state=ACTIVE_UNACK value=91.5 wall=- sync=no",
        "1500 ALARM_EVENT seq=5 point=LVL event=RAISED kind=HI state=ACTIVE_UNACK value=91.5 wall=- sync=no extra=1",
        "1500 ALARM_EVENT seq=5 point=LVL event=RAISED kind=HI state=ACTIVE_UNACK value=91.5 wall=-",
        "1500 ALARM_EVENT  seq=5 point=LVL event=RAISED kind=HI state=ACTIVE_UNACK value=91.5 wall=- sync=no",
        "1500 ALARM_EVENT seq=99999999999 point=LVL event=RAISED kind=HI state=ACTIVE_UNACK value=91.5 wall=- sync=no",
        "1500 OTHER seq=5 point=LVL event=RAISED kind=HI state=ACTIVE_UNACK value=91.5 wall=- sync=no",
        "1500 ALARM_EVENT seq=5 point=LVL event=RAISED kind=HI state=ACTIVE_UNACK value=91.5 stamp=- sync=no",
    };
    bool none_accepted = true;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char t[512];
        /* compute the correct CRC for the text by formatting nothing: reuse the module's own CRC via a good line is not
         * possible, so build the line with a CRC found by trying: the parser reports only true or false, hence we sign
         * the text with the same polynomial here. */
        uint32_t crc = 0xFFFFFFFFu;
        for (const char *p = bad[i]; *p; p++) {
            crc ^= (uint8_t)*p;
            for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
        snprintf(t, sizeof(t), "%s crc=%08X", bad[i], (unsigned)~crc);
        if (hmi_audit_parse_journal(t, strlen(t), names, 2, &o)) none_accepted = false;
    }
    CHECK(none_accepted);
    /* the first of them is accepted when the point is a known name: proves the helper above signs correctly */
    {
        const char *good = "1500 ALARM_EVENT seq=5 point=LVL event=RAISED kind=HI state=ACTIVE_UNACK value=91.5 wall=- sync=no";
        char t[512];
        uint32_t crc = 0xFFFFFFFFu;
        for (const char *p = good; *p; p++) {
            crc ^= (uint8_t)*p;
            for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
        snprintf(t, sizeof(t), "%s crc=%08X", good, (unsigned)~crc);
        CHECK(hmi_audit_parse_journal(t, strlen(t), names, 2, &o) && o.point == 0);
    }
    CHECK(!hmi_audit_parse_journal("", 0, names, 2, &o));
    CHECK(!hmi_audit_parse_journal("garbage garbage garbage garbage crc=00000000", 43, names, 2, &o));
}

static void test_stamp_line(void)
{
    SECTION("auditfmt: stamping a line formatted elsewhere");
    char b[128];
    hmi_wall_t w = wall(1792664103120ull, true);
    snprintf(b, sizeof(b), "900 LOGIN_OK user=admin role=admin\n");
    size_t len = strlen(b);
    size_t n = hmi_audit_stamp_line(b, len, sizeof(b), &w);
    CHECK(n == strlen(b) && n > len);
    CHECK(strcmp(b, "900 LOGIN_OK user=admin role=admin wall=2026-10-22T10:15:03.120Z sync=ok\n") == 0);

    snprintf(b, sizeof(b), "no newline");
    CHECK(hmi_audit_stamp_line(b, strlen(b), sizeof(b), &w) == 0 && strcmp(b, "no newline") == 0);
    CHECK(hmi_audit_stamp_line(b, 0, sizeof(b), &w) == 0);

    /* not enough room: refused and unchanged, for every capacity below the need */
    snprintf(b, sizeof(b), "x\n");
    size_t need = 0;
    {
        char t[128];
        snprintf(t, sizeof(t), "x\n");
        need = hmi_audit_stamp_line(t, 2, sizeof(t), &w) + 1u; /* the result plus its terminator */
    }
    bool ok = true;
    for (size_t cap = 3; cap < need; cap++) {
        char t[128] = "x\n";
        if (hmi_audit_stamp_line(t, 2, cap, &w) != 0 || strcmp(t, "x\n") != 0) ok = false;
    }
    CHECK(ok);
    char t[128] = "x\n";
    CHECK(hmi_audit_stamp_line(t, 2, need, &w) == need - 1u);
}

int main(void)
{
    test_journal_line();
    test_stamp_line();
    test_wall_format();
    test_write_line();
    test_ack_line();
    test_boot_line();
    return test_finish();
}
