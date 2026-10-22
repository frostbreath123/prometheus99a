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

static void test_login_wording(void)
{
    SECTION("login and session wording");
    char b[160];
    hmi_view_role_label(HMI_ROLE_VIEWER, "", b, sizeof(b));
    CHECK_STR(b, "VIEWER");
    hmi_view_role_label(HMI_ROLE_VIEWER, "someone", b, sizeof(b)); /* a viewer has no user */
    CHECK_STR(b, "VIEWER");
    hmi_view_role_label(HMI_ROLE_OPERATOR, "op1", b, sizeof(b));
    CHECK_STR(b, "OPERATOR  op1");
    hmi_view_role_label(HMI_ROLE_ADMIN, "admin", b, sizeof(b));
    CHECK_STR(b, "ADMIN  admin");
    hmi_view_role_label(HMI_ROLE_ENGINEER, NULL, b, sizeof(b));
    CHECK_STR(b, "ENGINEER");
    char small[12];
    hmi_view_role_label(HMI_ROLE_ENGINEER, "abcdefghijklmnop", small, sizeof(small));
    CHECK(strlen(small) == 11);                                  /* cut and terminated */
    hmi_view_role_label((hmi_role_t)9, "x", b, sizeof(b));
    CHECK_STR(b, "INVALID  x");
    CHECK(hmi_view_role_tone(HMI_ROLE_VIEWER) == HMI_TONE_MUTED);
    CHECK(hmi_view_role_tone(HMI_ROLE_OPERATOR) == HMI_TONE_GOOD);

    hmi_view_wait_text(0, b, sizeof(b));
    CHECK_STR(b, "0:00");
    hmi_view_wait_text(1, b, sizeof(b));
    CHECK_STR(b, "0:01");                                        /* rounded up, never 0:00 while waiting */
    hmi_view_wait_text(1000, b, sizeof(b));
    CHECK_STR(b, "0:01");
    hmi_view_wait_text(1001, b, sizeof(b));
    CHECK_STR(b, "0:02");
    hmi_view_wait_text(30000, b, sizeof(b));
    CHECK_STR(b, "0:30");
    hmi_view_wait_text(61000, b, sizeof(b));
    CHECK_STR(b, "1:01");
    hmi_view_wait_text(900000, b, sizeof(b));
    CHECK_STR(b, "15:00");

    hmi_view_login_text(HMI_LOGIN_OK, 0, "op1", b, sizeof(b));
    CHECK_STR(b, "Logged in as op1");
    hmi_view_login_text(HMI_LOGIN_BAD, 1000, NULL, b, sizeof(b));
    CHECK_STR(b, "Wrong user name or password");
    hmi_view_login_text(HMI_LOGIN_BAD, 30000, NULL, b, sizeof(b));
    CHECK_STR(b, "Wrong user name or password. Locked, try again in 0:30");
    hmi_view_login_text(HMI_LOGIN_LOCKED, 29001, NULL, b, sizeof(b));
    CHECK_STR(b, "Too many failed attempts. Try again in 0:30");
    hmi_view_login_text(HMI_LOGIN_WAIT, 400, NULL, b, sizeof(b));
    CHECK_STR(b, "Please wait 0:01 before trying again");
    hmi_view_login_text(HMI_LOGIN_PENDING, 0, NULL, b, sizeof(b));
    CHECK_STR(b, "Checking, please wait");
    hmi_view_login_text(HMI_LOGIN_BUSY, 0, NULL, b, sizeof(b));
    CHECK_STR(b, "Checking, please wait");
    hmi_view_login_text(HMI_LOGIN_NO_STORE, 0, NULL, b, sizeof(b));
    CHECK(strstr(b, "Create the administrator") != NULL);
    hmi_view_login_text(HMI_LOGIN_STORE_INVALID, 0, NULL, b, sizeof(b));
    CHECK(strstr(b, "damaged") && strstr(b, "disabled"));
    hmi_view_login_text(HMI_LOGIN_SESSION_ACTIVE, 0, NULL, b, sizeof(b));
    CHECK_STR(b, "Already logged in");
    hmi_view_login_text(HMI_LOGIN_AUDIT_FAILED, 0, NULL, b, sizeof(b));
    CHECK(strstr(b, "audit log") != NULL);
    hmi_view_login_text(HMI_LOGIN_INPUT, 0, NULL, b, sizeof(b));
    CHECK(strstr(b, "too long") != NULL);
    hmi_view_login_text(HMI_LOGIN_ERROR, 0, NULL, b, sizeof(b));
    CHECK(strstr(b, "Internal error") != NULL);
    hmi_view_login_text((hmi_login_result_t)99, 0, NULL, b, sizeof(b));
    CHECK(strstr(b, "unknown") != NULL);
    for (int r = 0; r <= HMI_LOGIN_ERROR; r++) {                 /* every outcome has some wording */
        hmi_view_login_text((hmi_login_result_t)r, 5000, "u", b, sizeof(b));
        CHECK(b[0] != '\0');
    }
    hmi_view_login_text(HMI_LOGIN_BAD, 30000, NULL, small, sizeof(small)); /* truncation is safe */
    CHECK(strlen(small) < sizeof(small));

    CHECK(hmi_view_login_tone(HMI_LOGIN_OK) == HMI_TONE_GOOD);
    CHECK(hmi_view_login_tone(HMI_LOGIN_BAD) == HMI_TONE_BAD);
    CHECK(hmi_view_login_tone(HMI_LOGIN_LOCKED) == HMI_TONE_WARN);
    CHECK(hmi_view_login_tone(HMI_LOGIN_WAIT) == HMI_TONE_WARN);
    CHECK(hmi_view_login_tone(HMI_LOGIN_PENDING) == HMI_TONE_TEXT);
    CHECK(hmi_view_login_tone(HMI_LOGIN_AUDIT_FAILED) == HMI_TONE_BAD);

    hmi_view_setup_text(HMI_SETUP_OK, NULL, b, sizeof(b));
    CHECK(strstr(b, "Administrator created") != NULL);
    hmi_view_setup_text(HMI_SETUP_PASSWORD, "password is too short (8 characters at least)", b, sizeof(b));
    CHECK_STR(b, "password is too short (8 characters at least)");
    hmi_view_setup_text(HMI_SETUP_PASSWORD, "", b, sizeof(b));
    CHECK_STR(b, "Password rejected");
    hmi_view_setup_text(HMI_SETUP_NAME, NULL, b, sizeof(b));
    CHECK(strstr(b, "3 to 24") != NULL);
    hmi_view_setup_text(HMI_SETUP_NOT_FIRST_BOOT, NULL, b, sizeof(b));
    CHECK(strstr(b, "already exists") != NULL);
    hmi_view_setup_text(HMI_SETUP_AUDIT_FAILED, NULL, b, sizeof(b));
    CHECK(strstr(b, "audit log") != NULL);
    hmi_view_setup_text(HMI_SETUP_SAVE_FAILED, NULL, b, sizeof(b));
    CHECK(strstr(b, "cannot be saved") != NULL);
    for (int r = 0; r <= HMI_SETUP_ERROR; r++) {
        hmi_view_setup_text((hmi_setup_result_t)r, "x", b, sizeof(b));
        CHECK(b[0] != '\0');
    }
    hmi_view_setup_text((hmi_setup_result_t)99, NULL, b, sizeof(b));
    CHECK(strstr(b, "unknown") != NULL);
    CHECK(hmi_view_setup_tone(HMI_SETUP_OK) == HMI_TONE_GOOD);
    CHECK(hmi_view_setup_tone(HMI_SETUP_PENDING) == HMI_TONE_TEXT);
    CHECK(hmi_view_setup_tone(HMI_SETUP_NAME) == HMI_TONE_BAD);
    CHECK(strstr(hmi_view_setup_mismatch_text(), "do not match") != NULL);

    hmi_view_test_role_banner(HMI_ROLE_OPERATOR, b, sizeof(b));
    CHECK_STR(b, "TEST ROLE operator - NOT AUTHENTICATED");
}

int main(void)
{
    test_decimals();
    test_value_and_quality();
    test_alarm_cells();
    test_link_and_summary();
    test_setpoint();
    test_write_wording();
    test_journal();
    test_heartbeat();
    test_ack_wording();
    test_login_wording();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
