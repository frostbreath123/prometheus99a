/* Unit tests for the pure display view-model (src/ui/hmi_view.c).
 * No LVGL, no runtime threads, no clock: every case is a function call. */
#include "../../src/ui/hmi_view.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { g_fail++; printf("  FAIL line %d: '%s' != '%s'\n", __LINE__, _a, _b); } } while (0)
#define SECTION(n) printf("%s\n", n)

static hmi_tag_cfg_t make_cfg(double scale, const char *units)
{
    hmi_tag_cfg_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "TANK");
    snprintf(c.units, sizeof(c.units), "%s", units);
    c.scale = scale;
    c.write_min = 0;
    c.write_max = 100;
    return c;
}

static hmi_tag_value_t make_val(hmi_quality_t q, hmi_reason_t r, double eng)
{
    hmi_tag_value_t v;
    memset(&v, 0, sizeof(v));
    v.quality = q;
    v.reason = r;
    v.eng = eng;
    return v;
}

static void test_decimals(void)
{
    SECTION("view: decimals follow the tag scale");
    CHECK(hmi_view_decimals(1.0) == 0);
    CHECK(hmi_view_decimals(0.1) == 1);
    CHECK(hmi_view_decimals(0.01) == 2);
    CHECK(hmi_view_decimals(0.001) == 3);
    CHECK(hmi_view_decimals(0.0001) == 3); /* capped */
    CHECK(hmi_view_decimals(0.5) == 1);
}

static void test_value_and_quality(void)
{
    SECTION("view: value and quality cells");
    char b[64];
    hmi_tag_cfg_t c = make_cfg(0.1, "%");
    hmi_tag_value_t good = make_val(HMI_Q_GOOD, HMI_REASON_NONE, 70.0);
    hmi_view_value_text(&c, &good, b, sizeof(b));
    CHECK_STR(b, "70.0 %");
    hmi_tag_value_t unc = make_val(HMI_Q_UNCERTAIN, HMI_REASON_OUT_OF_RANGE, 91.5);
    hmi_view_value_text(&c, &unc, b, sizeof(b));
    CHECK_STR(b, "91.5 %");           /* uncertain still shows the number */
    hmi_tag_value_t bad = make_val(HMI_Q_BAD, HMI_REASON_COMM_LOSS, 70.0);
    hmi_view_value_text(&c, &bad, b, sizeof(b));
    CHECK_STR(b, "---");
    hmi_tag_value_t una = make_val(HMI_Q_UNAVAILABLE, HMI_REASON_NONE, 0.0);
    hmi_view_value_text(&c, &una, b, sizeof(b));
    CHECK_STR(b, "---");
    hmi_tag_cfg_t cnt = make_cfg(1.0, "");
    hmi_tag_value_t n = make_val(HMI_Q_GOOD, HMI_REASON_NONE, 12345.0);
    hmi_view_value_text(&cnt, &n, b, sizeof(b));
    CHECK_STR(b, "12345 ");           /* empty units keep the format stable */

    CHECK(hmi_view_value_tone(HMI_Q_GOOD) == HMI_TONE_TEXT);
    CHECK(hmi_view_value_tone(HMI_Q_UNCERTAIN) == HMI_TONE_MUTED);
    CHECK(hmi_view_value_tone(HMI_Q_BAD) == HMI_TONE_MUTED);

    CHECK_STR(hmi_view_quality_text(&good), "GOOD");
    CHECK_STR(hmi_view_quality_text(&unc), "UNCERTAIN RANGE");
    hmi_tag_value_t unc2 = make_val(HMI_Q_UNCERTAIN, HMI_REASON_STALE, 0);
    CHECK_STR(hmi_view_quality_text(&unc2), "UNCERTAIN");
    CHECK_STR(hmi_view_quality_text(&bad), "BAD COMM");
    hmi_tag_value_t b1 = make_val(HMI_Q_BAD, HMI_REASON_EXCEPTION, 0);
    hmi_tag_value_t b2 = make_val(HMI_Q_BAD, HMI_REASON_STALE, 0);
    hmi_tag_value_t b3 = make_val(HMI_Q_BAD, HMI_REASON_PROTOCOL, 0);
    hmi_tag_value_t b4 = make_val(HMI_Q_BAD, HMI_REASON_NONE, 0);
    CHECK_STR(hmi_view_quality_text(&b1), "BAD PLC-EXC");
    CHECK_STR(hmi_view_quality_text(&b2), "BAD STALE");
    CHECK_STR(hmi_view_quality_text(&b3), "BAD PROTO");
    CHECK_STR(hmi_view_quality_text(&b4), "BAD");
    CHECK_STR(hmi_view_quality_text(&una), "NO DATA");

    CHECK(hmi_view_quality_tone(HMI_Q_GOOD) == HMI_TONE_GOOD);
    CHECK(hmi_view_quality_tone(HMI_Q_UNCERTAIN) == HMI_TONE_WARN);
    CHECK(hmi_view_quality_tone(HMI_Q_BAD) == HMI_TONE_BAD);
    CHECK(hmi_view_quality_tone(HMI_Q_UNAVAILABLE) == HMI_TONE_MUTED);
}

static void test_alarm_cells(void)
{
    SECTION("view: alarm cell text and tone");
    char b[40];
    hmi_alarm_point_t p;
    memset(&p, 0, sizeof(p));
    p.state = HMI_AL_NORMAL;
    hmi_view_alarm_text(&p, b, sizeof(b));
    CHECK_STR(b, "");
    CHECK(hmi_view_alarm_tone(&p) == HMI_TONE_MUTED);

    p.state = HMI_AL_ACTIVE_UNACK;
    p.kind = HMI_ALK_HI;
    hmi_view_alarm_text(&p, b, sizeof(b));
    CHECK_STR(b, "HI UNACK");
    CHECK(hmi_view_alarm_tone(&p) == HMI_TONE_BAD);

    p.kind = HMI_ALK_LOLO;
    p.state = HMI_AL_ACTIVE_ACK;
    hmi_view_alarm_text(&p, b, sizeof(b));
    CHECK_STR(b, "LOLO ACK");
    CHECK(hmi_view_alarm_tone(&p) == HMI_TONE_WARN);

    p.kind = HMI_ALK_COMMS;
    p.state = HMI_AL_ACTIVE_UNACK;
    hmi_view_alarm_text(&p, b, sizeof(b));
    CHECK_STR(b, "COMMS UNACK");

    p.state = HMI_AL_RTN_UNACK;
    hmi_view_alarm_text(&p, b, sizeof(b));
    CHECK_STR(b, "RTN UNACK");
    CHECK(hmi_view_alarm_tone(&p) == HMI_TONE_WARN);

    p.state = HMI_AL_ACTIVE_UNACK;
    p.kind = HMI_ALK_HIHI;
    p.data_suppressed = true;
    hmi_view_alarm_text(&p, b, sizeof(b));
    CHECK_STR(b, "HIHI UNACK (no data)");

    /* a tiny buffer must truncate, never overflow */
    char small[6];
    hmi_view_alarm_text(&p, small, sizeof(small));
    CHECK(strlen(small) < sizeof(small));
}

static void test_alarm_row(void)
{
    SECTION("view: one alarm row, ready to draw");
    const char *names[] = { "TANK_LEVEL", "TEMP" };
    hmi_alarm_row_t a;
    hmi_view_alarm_row_t v;
    memset(&a, 0, sizeof(a));
    a.point = 1;
    a.priority = HMI_PRI_CRITICAL;
    a.pt.state = HMI_AL_ACTIVE_UNACK;
    a.pt.kind = HMI_ALK_HI;
    a.pt.since_ms = 1000;
    hmi_view_alarm_row(&a, names, 2, 4000, &v);
    CHECK_STR(v.priority, "CRIT");
    CHECK(v.priority_tone == hmi_view_priority_tone(HMI_PRI_CRITICAL));
    CHECK_STR(v.name, "TEMP");
    CHECK_STR(v.state, "HI UNACK");
    CHECK(v.state_tone == hmi_view_alarm_tone(&a.pt));
    CHECK_STR(v.age, "3s");
    CHECK(v.waiting);

    a.pt.state = HMI_AL_ACTIVE_ACK;                      /* acknowledged: no longer waiting */
    hmi_view_alarm_row(&a, names, 2, 4000, &v);
    CHECK(!v.waiting);
    a.pt.state = HMI_AL_RTN_UNACK;                       /* back to normal but not yet acknowledged: waiting */
    hmi_view_alarm_row(&a, names, 2, 4000, &v);
    CHECK(v.waiting);

    a.point = 2;                                         /* beyond the tags: the communications alarm */
    hmi_view_alarm_row(&a, names, 2, 4000, &v);
    CHECK_STR(v.name, "PLC LINK");
    a.pt.since_ms = 9000;                                /* a clock that is behind the alarm gives 0s, never a huge age */
    hmi_view_alarm_row(&a, names, 2, 4000, &v);
    CHECK_STR(v.age, "0s");
}

static void test_link_and_summary(void)
{
    SECTION("view: link indicator and alarm summary");
    char b[40];
    hmi_view_link_text(HMI_LINK_HEALTHY, b, sizeof(b));
    CHECK_STR(b, "PLC: HEALTHY");
    hmi_view_link_text(HMI_LINK_NO_RESPONSE, b, sizeof(b));
    CHECK_STR(b, "PLC: NO RESPONSE");
    hmi_view_link_text(HMI_LINK_DOWN, b, sizeof(b));
    CHECK_STR(b, "PLC: DOWN");
    hmi_view_link_text(HMI_LINK_DEGRADED, b, sizeof(b));
    CHECK_STR(b, "PLC: DEGRADED");
    CHECK(hmi_view_link_tone(HMI_LINK_HEALTHY) == HMI_TONE_GOOD);
    CHECK(hmi_view_link_tone(HMI_LINK_DEGRADED) == HMI_TONE_WARN);
    CHECK(hmi_view_link_tone(HMI_LINK_NO_RESPONSE) == HMI_TONE_BAD);
    CHECK(hmi_view_link_tone(HMI_LINK_DOWN) == HMI_TONE_BAD);

    hmi_view_alarm_summary_text(0, b, sizeof(b));
    CHECK_STR(b, "ALARMS: none unacked");
    hmi_view_alarm_summary_text(3, b, sizeof(b));
    CHECK_STR(b, "ALARMS: 3 UNACK");
    CHECK(hmi_view_alarm_summary_tone(0) == HMI_TONE_MUTED);
    CHECK(hmi_view_alarm_summary_tone(1) == HMI_TONE_BAD);
}

static void test_setpoint(void)
{
    SECTION("view: setpoint stepping, clamping and text");
    hmi_tag_cfg_t c = make_cfg(0.1, "%");
    c.write_min = 0;
    c.write_max = 100;
    CHECK(fabs(hmi_view_setpoint_step(&c, 50.0, +1) - 51.0) < 1e-9);   /* 1% of span */
    CHECK(fabs(hmi_view_setpoint_step(&c, 50.0, -1) - 49.0) < 1e-9);
    CHECK(fabs(hmi_view_setpoint_step(&c, 100.0, +1) - 100.0) < 1e-9); /* clamp high */
    CHECK(fabs(hmi_view_setpoint_step(&c, 0.0, -1) - 0.0) < 1e-9);     /* clamp low */
    CHECK(fabs(hmi_view_setpoint_step(&c, 99.5, +1) - 100.0) < 1e-9);

    hmi_tag_cfg_t narrow = make_cfg(0.1, "C");
    narrow.write_min = 20.0;
    narrow.write_max = 20.5;                                            /* span/100 < 0.1 */
    CHECK(fabs(hmi_view_setpoint_step(&narrow, 20.0, +1) - 20.1) < 1e-9); /* floor = one display unit */

    char b[64];
    hmi_view_setpoint_text(&c, 50.0, b, sizeof(b));
    CHECK_STR(b, "SETPOINT TANK: 50.0 %");
}

static void test_write_wording(void)
{
    SECTION("view: write outcome wording and tone");
    CHECK_STR(hmi_view_write_state_text(HMI_WR_CONFIRMED), "CONFIRMED");
    CHECK_STR(hmi_view_write_state_text(HMI_WR_EXPIRED), "EXPIRED (not sent)");
    CHECK_STR(hmi_view_write_state_text(HMI_WR_UNKNOWN), "UNKNOWN (sent, no confirmation)");
    CHECK_STR(hmi_view_write_state_text(HMI_WR_VERIFY_FAILED), "VERIFY FAILED");
    CHECK_STR(hmi_view_write_state_text(HMI_WR_FREE), "-");
    CHECK_STR(hmi_view_submit_text(HMI_WRS_ERR_ROLE), "role not permitted");
    CHECK_STR(hmi_view_submit_text(HMI_WRS_ERR_LINK_DOWN), "PLC link down");
    CHECK_STR(hmi_view_submit_text(HMI_WRS_ERR_AUDIT), "audit unavailable");

    hmi_write_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.id = 7;
    rec.state = HMI_WR_SENT;
    char b[80];
    hmi_view_write_status_text(&rec, b, sizeof(b));
    CHECK_STR(b, "WRITE #7: SENT");
    hmi_view_submit_refused_text(HMI_WRS_ERR_RANGE, b, sizeof(b));
    CHECK_STR(b, "SEND REFUSED: value out of range");

    CHECK(hmi_view_write_status_tone(HMI_WR_CONFIRMED) == HMI_TONE_GOOD);
    CHECK(hmi_view_write_status_tone(HMI_WR_PENDING) == HMI_TONE_WARN);
    CHECK(hmi_view_write_status_tone(HMI_WR_SENT) == HMI_TONE_WARN);
    CHECK(hmi_view_write_status_tone(HMI_WR_RESERVED) == HMI_TONE_WARN);
    CHECK(hmi_view_write_status_tone(HMI_WR_UNKNOWN) == HMI_TONE_BAD);
    CHECK(hmi_view_write_status_tone(HMI_WR_VERIFY_FAILED) == HMI_TONE_BAD);
    CHECK(hmi_view_write_status_tone(HMI_WR_REJECTED) == HMI_TONE_BAD);
    CHECK(hmi_view_write_status_tone(HMI_WR_EXPIRED) == HMI_TONE_BAD);
}

static void test_ack_wording(void)
{
    SECTION("view: acknowledgement outcome wording");
    CHECK_STR(hmi_view_ack_result_text(HMI_ACK_OK), "ACKNOWLEDGED");
    CHECK_STR(hmi_view_ack_result_text(HMI_ACK_NOTHING), "NOTHING TO ACKNOWLEDGE");
    CHECK_STR(hmi_view_ack_result_text(HMI_ACK_DENIED), "ACK DENIED: role not permitted");
    CHECK_STR(hmi_view_ack_result_text(HMI_ACK_AUDIT_FAILED), "ACK REFUSED: audit unavailable");
    CHECK(hmi_view_ack_result_tone(HMI_ACK_OK) == HMI_TONE_GOOD);
    CHECK(hmi_view_ack_result_tone(HMI_ACK_NOTHING) == HMI_TONE_MUTED);
    CHECK(hmi_view_ack_result_tone(HMI_ACK_DENIED) == HMI_TONE_BAD);
    CHECK(hmi_view_ack_result_tone(HMI_ACK_AUDIT_FAILED) == HMI_TONE_BAD);
    CHECK_STR(hmi_view_ack_result_text(HMI_ACK_PENDING), "ACK PENDING: recording");
    CHECK(hmi_view_ack_result_tone(HMI_ACK_PENDING) == HMI_TONE_WARN);
    CHECK_STR(hmi_view_write_state_text(HMI_WR_RESERVED), "RECORDING (not sent yet)");
    {
        char sb[64];
        hmi_view_storage_fault_text(sb, sizeof(sb));
        CHECK_STR(sb, "STORAGE FAULT: records not saved");
        hmi_view_storage_fault_text(sb, 8); /* truncates, stays terminated */
        CHECK(strlen(sb) == 7);
    }
}

static void test_heartbeat(void)
{
    SECTION("view: heartbeat mark");
    char b[16];
    hmi_view_heartbeat_text(0, b, sizeof(b));
    CHECK_STR(b, "HB 00");
    hmi_view_heartbeat_text(7, b, sizeof(b));
    CHECK_STR(b, "HB 07");
    hmi_view_heartbeat_text(1234, b, sizeof(b));
    CHECK_STR(b, "HB 34");        /* seconds modulo 100 */
    hmi_view_heartbeat_text(100, b, sizeof(b));
    CHECK_STR(b, "HB 00");
    char small[4];
    hmi_view_heartbeat_text(55, small, sizeof(small));
    CHECK(strlen(small) < sizeof(small));
}

static void test_journal(void)
{
    SECTION("view: alarm journal text");
    const char *cfg[2] = { "TANK_LEVEL", "TEMP" };
    char b[480];

    hmi_view_journal_text(NULL, 0, cfg, 2, b, sizeof(b));
    CHECK_STR(b, "no alarm events");

    hmi_alarm_log_t j[3];
    memset(j, 0, sizeof(j));
    j[0].point = 0; j[0].event = HMI_ALE_RAISED;   /* printed last */
    j[1].point = 1; j[1].event = HMI_ALE_ACKED;
    j[2].point = 2; j[2].event = HMI_ALE_RAISED;   /* point == n_tags: comms alarm, printed first */
    hmi_view_journal_text(j, 3, cfg, 2, b, sizeof(b));
    CHECK_STR(b, "PLC LINK RAISED\nTEMP ACKED\nTANK_LEVEL RAISED\n");

    j[0].event = 99;                               /* unknown event id prints empty word */
    hmi_view_journal_text(j, 1, cfg, 2, b, sizeof(b));
    CHECK_STR(b, "TANK_LEVEL \n");

    char small[40];                                /* truncation: stops before overflow */
    hmi_view_journal_text(j, 3, cfg, 2, small, sizeof(small));
    CHECK(strlen(small) < sizeof(small));
}

static void test_role_wording(void)
{
    SECTION("claimed role wording");
    char b[160];
    hmi_view_role_label(HMI_ROLE_VIEWER, b, sizeof(b));
    CHECK_STR(b, "VIEWER");
    hmi_view_role_label(HMI_ROLE_OPERATOR, b, sizeof(b));
    CHECK_STR(b, "OPERATOR");
    hmi_view_role_label(HMI_ROLE_ENGINEER, b, sizeof(b));
    CHECK_STR(b, "ENGINEER");
    hmi_view_role_label(HMI_ROLE_ADMIN, b, sizeof(b));
    CHECK_STR(b, "ADMIN");
    char small[6];
    hmi_view_role_label(HMI_ROLE_ENGINEER, small, sizeof(small));
    CHECK(strlen(small) == 5);                                   /* cut and terminated */
    hmi_view_role_label((hmi_role_t)9, b, sizeof(b));
    CHECK_STR(b, "INVALID");

    hmi_view_claimed_role_text(HMI_ROLE_VIEWER, b, sizeof(b));
    CHECK_STR(b, "Role: VIEWER (read only)");
    hmi_view_claimed_role_text(HMI_ROLE_OPERATOR, b, sizeof(b));
    CHECK(strstr(b, "operator") != NULL && strstr(b, "NOT authenticated") != NULL);
    hmi_view_claimed_role_text(HMI_ROLE_ADMIN, small, sizeof(small));
    CHECK(strlen(small) == 5);
}

static void test_priority_wording(void)
{
    printf("view: alarm priority wording\n");
    CHECK_STR(hmi_view_priority_text(HMI_PRI_LOW), "LOW");
    CHECK_STR(hmi_view_priority_text(HMI_PRI_MEDIUM), "MED");
    CHECK_STR(hmi_view_priority_text(HMI_PRI_HIGH), "HIGH");
    CHECK_STR(hmi_view_priority_text(HMI_PRI_CRITICAL), "CRIT");
    CHECK_STR(hmi_view_priority_text((hmi_priority_t)9), "?");
    CHECK(hmi_view_priority_tone(HMI_PRI_CRITICAL) == HMI_TONE_BAD);
    CHECK(hmi_view_priority_tone(HMI_PRI_HIGH) == HMI_TONE_WARN);
    CHECK(hmi_view_priority_tone(HMI_PRI_MEDIUM) == HMI_TONE_TEXT);
    CHECK(hmi_view_priority_tone(HMI_PRI_LOW) == HMI_TONE_MUTED);
}

static void test_list_wording(void)
{
    printf("view: alarm list wording\n");
    char b[24];
    const char *names[2] = { "LEVEL", "TEMP" };
    CHECK_STR(hmi_view_alarm_point_name(names, 2, 0), "LEVEL");
    CHECK_STR(hmi_view_alarm_point_name(names, 2, 1), "TEMP");
    CHECK_STR(hmi_view_alarm_point_name(names, 2, 2), "PLC LINK");   /* the comms alarm follows the tags */
    CHECK_STR(hmi_view_alarm_point_name(names, 2, 99), "PLC LINK");

    static const struct { uint64_t ms; const char *txt; } age[] = {
        { 0, "0s" }, { 999, "0s" }, { 1000, "1s" }, { 59999, "59s" }, { 60000, "1m00s" },
        { 61000, "1m01s" }, { 3599999, "59m59s" }, { 3600000, "1h00m" }, { 3660000, "1h01m" },
        { 359999999ULL, "99h59m" }, { 360000000ULL, ">99h" }, { 999999999999ULL, ">99h" },
    };
    for (size_t i = 0; i < sizeof(age) / sizeof(age[0]); i++) {
        hmi_view_age_text(age[i].ms, b, sizeof(b));
        if (strcmp(b, age[i].txt) != 0) printf("  age %llu -> '%s', expected '%s'\n", (unsigned long long)age[i].ms, b, age[i].txt);
        CHECK_STR(b, age[i].txt);
    }
    hmi_view_age_text(3660000, b, 4);                    /* a short buffer is cut, never overrun */
    CHECK(strlen(b) == 3);

    hmi_view_page_text(0, 1, b, sizeof(b)); CHECK_STR(b, "1/1");
    hmi_view_page_text(2, 7, b, sizeof(b)); CHECK_STR(b, "3/7");
    hmi_view_page_text(0, 0, b, sizeof(b)); CHECK_STR(b, "0/0");
    CHECK_STR(hmi_view_no_alarms_text(), "no active alarms");
    CHECK_STR(hmi_view_ack_changed_text(), "ALARM CHANGED: check the list");

    hmi_view_axis_text(50.04, b, sizeof(b)); CHECK_STR(b, "50.04");
    hmi_view_axis_text(0, b, sizeof(b)); CHECK_STR(b, "0");
    hmi_view_axis_text(-12.5, b, sizeof(b)); CHECK_STR(b, "-12.5");
    hmi_view_axis_text(1234.56, b, sizeof(b)); CHECK_STR(b, "1235");
    hmi_view_axis_text(0.0001235, b, sizeof(b)); CHECK_STR(b, "0.0001235");
    hmi_view_axis_text(12345678, b, sizeof(b)); CHECK_STR(b, "1.235e+07");

    char s[96];
    hmi_view_trend_status_text("LEVEL", "%", 1, true, 49.8, 50.34, "10m00s", s, sizeof(s));
    CHECK_STR(s, "LEVEL  min 49.8  max 50.3 %  (last 10m00s)");
    hmi_view_trend_status_text("LEVEL", "", 0, false, 0, 0, "20m00s", s, sizeof(s));
    CHECK_STR(s, "LEVEL: no good data in the last 20m00s");
    hmi_view_trend_status_text("T", "C", 9, true, 1.5, 2.5, "1h00m", s, sizeof(s));        /* decimals are limited to 0..3 */
    CHECK_STR(s, "T  min 1.500  max 2.500 C  (last 1h00m)");
    hmi_view_trend_status_text("T", "C", -2, true, 1.4, 2.6, "1h00m", s, sizeof(s));   /* 0 places; values away from a .5 so the result does not depend on the C library */
    CHECK_STR(s, "T  min 1  max 3 C  (last 1h00m)");
    hmi_view_trend_status_text("LONG_NAME_LONG_NAME", "units", 1, true, 1, 2, "1m00s", s, 20);   /* cut, never overrun */
    CHECK(strlen(s) == 19);
    CHECK(strstr(hmi_view_no_trend_text(), "\"trend\": true") != NULL);
}

static void test_config_wording(void)
{
    printf("view: configuration change wording\n");
    char b[160];
    for (int r = 0; r <= (int)HMI_CFGREQ_TOO_LARGE; r++) {
        hmi_view_cfgreq_text((hmi_cfgreq_result_t)r, b, sizeof(b));
        CHECK(b[0] != '\0' && strstr(b, "unknown") == NULL);
        CHECK((hmi_view_cfgreq_tone((hmi_cfgreq_result_t)r) == HMI_TONE_GOOD) == (r == (int)HMI_CFGREQ_OK));   /* a failure is never good */
    }
    hmi_view_cfgreq_text((hmi_cfgreq_result_t)99, b, sizeof(b)); CHECK_STR(b, "Request: unknown result");
    hmi_view_cfgreq_text(HMI_CFGREQ_DENIED, b, sizeof(b)); CHECK(strstr(b, "this role cannot change settings") != NULL);
    CHECK(hmi_view_cfgreq_tone(HMI_CFGREQ_BUSY) == HMI_TONE_WARN && hmi_view_cfgreq_tone(HMI_CFGREQ_AUDIT_FAILED) == HMI_TONE_BAD);
    hmi_view_cfgreq_text(HMI_CFGREQ_WRITE_FAILED, b, 10); CHECK(strlen(b) == 9);                       /* cut, never overrun */

    hmi_view_cfgstate_text(HMI_CFGREQ_NONE, "x", b, sizeof(b)); CHECK_STR(b, "");
    hmi_view_cfgstate_text(HMI_CFGREQ_PENDING, "", b, sizeof(b)); CHECK_STR(b, "Waiting for the configuration helper");
    hmi_view_cfgstate_text(HMI_CFGREQ_APPLIED, "applied serial=8 changes=3 by=eng1", b, sizeof(b));
    CHECK_STR(b, "applied serial=8 changes=3 by=eng1. The service restarts to use it");
    hmi_view_cfgstate_text(HMI_CFGREQ_APPLIED, NULL, b, sizeof(b)); CHECK_STR(b, "applied. The service restarts to use it");
    hmi_view_cfgstate_text(HMI_CFGREQ_REJECTED, "rejected: stale request", b, sizeof(b)); CHECK_STR(b, "rejected: stale request");
    hmi_view_cfgstate_text(HMI_CFGREQ_REJECTED, "", b, sizeof(b)); CHECK_STR(b, "rejected");
    CHECK(hmi_view_cfgstate_tone(HMI_CFGREQ_APPLIED) == HMI_TONE_GOOD && hmi_view_cfgstate_tone(HMI_CFGREQ_REJECTED) == HMI_TONE_BAD);
    CHECK(hmi_view_cfgstate_tone(HMI_CFGREQ_PENDING) == HMI_TONE_TEXT && hmi_view_cfgstate_tone(HMI_CFGREQ_NONE) == HMI_TONE_TEXT);

    hmi_view_cfg_number_text(82, b, sizeof(b)); CHECK_STR(b, "82");
    hmi_view_cfg_number_text(0.5, b, sizeof(b)); CHECK_STR(b, "0.5");
    hmi_view_cfg_number_text(-12.25, b, sizeof(b)); CHECK_STR(b, "-12.25");
    hmi_view_cfg_number_text(1234567.0, b, sizeof(b)); CHECK_STR(b, "1.23457e+06");
    hmi_view_cfg_number_text(0.1 + 0.2, b, sizeof(b)); CHECK_STR(b, "0.3");
    CHECK_STR(hmi_view_priority_caps(HMI_PRI_LOW), "LOW");
    CHECK_STR(hmi_view_priority_caps(HMI_PRI_MEDIUM), "MEDIUM");
    CHECK_STR(hmi_view_priority_caps(HMI_PRI_HIGH), "HIGH");
    CHECK_STR(hmi_view_priority_caps(HMI_PRI_CRITICAL), "CRITICAL");
    CHECK_STR(hmi_view_priority_caps((hmi_priority_t)7), "?");
    CHECK(strstr(hmi_view_config_need_engineer_text(), "engineer") != NULL);
    CHECK(strstr(hmi_view_config_unavailable_text(), "not set up") != NULL);
}

static void test_settings_wording(void)
{
    printf("view: settings wording\n");
    char b[96];
    hmi_view_plc_text("192.168.0.10", 502, 1, b, sizeof(b)); CHECK_STR(b, "PLC 192.168.0.10:502  unit 1");
    hmi_view_writes_text(true, true, b, sizeof(b)); CHECK_STR(b, "Writes: enabled");
    hmi_view_writes_text(false, false, b, sizeof(b)); CHECK(strstr(b, "configuration") != NULL);
    hmi_view_writes_text(true, false, b, sizeof(b)); CHECK(strstr(b, "audit") != NULL);
    hmi_view_storage_text(false, 12, 0, b, sizeof(b)); CHECK_STR(b, "Storage: ok, 12 records written");
    hmi_view_storage_text(true, 12, 3, b, sizeof(b)); CHECK(strstr(b, "FAULT") != NULL && strstr(b, "3 failed") != NULL);
    hmi_view_clock_text(true, true, b, sizeof(b)); CHECK(strstr(b, "synchronised") != NULL && strstr(b, "NOT") == NULL);
    hmi_view_clock_text(true, false, b, sizeof(b)); CHECK(strstr(b, "NOT synchronised") != NULL);
    hmi_view_clock_text(false, false, b, sizeof(b)); CHECK(strstr(b, "cannot be read") != NULL);
    hmi_view_tags_text(4, 2, 1, b, sizeof(b)); CHECK_STR(b, "Tags: 4   with alarms: 1   trended: 2");
}

int main(void)
{
    test_settings_wording();
    test_config_wording();
    test_list_wording();
    test_priority_wording();
    test_decimals();
    test_value_and_quality();
    test_alarm_cells();
    test_alarm_row();
    test_link_and_summary();
    test_setpoint();
    test_write_wording();
    test_journal();
    test_heartbeat();
    test_ack_wording();
    test_role_wording();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
