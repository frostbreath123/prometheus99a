/* Tests for writing a configuration out (hmi_config_write.c) and for changing one
 * through a narrow request (hmi_cfgedit.c): the request text, what the whitelist
 * allows and refuses, that nothing else can change, and the hand-off to the spool
 * directory. */
#include "test_support.h"
#include "../../src/hmi/hmi_cfgedit.h"
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0700)
#endif

/* A configuration with two tags with alarms, a read-only tag, a trended tag, writes on, and an allow-list of three. */
static const char *BASE_JSON =
    "{\"version\":1,\"writes_enabled\":true,\"write_min_interval_ms\":500,\"write_max_per_minute\":12,\"trend_bucket_ms\":2000,"
    "\"plc\":{\"host\":\"192.168.0.10\",\"port\":502,\"unit\":1,\"response_timeout_ms\":400,\"connect_timeout_ms\":900,"
    "\"allowed_hosts\":[\"192.168.0.10\",\"192.168.0.11\",\"10.0.0.5\"]},"
    "\"tags\":["
    "{\"name\":\"TANK_LEVEL\",\"address\":0,\"type\":\"u16\",\"scale\":0.1,\"units\":\"%\",\"min\":0,\"max\":100,\"poll_ms\":200,"
    "\"trend\":true,"
    "\"alarm\":{\"high\":80,\"high_high\":95,\"low\":10,\"low_low\":5,\"deadband\":2,\"on_ms\":1000,\"off_ms\":1000,\"priority\":\"high\"},"
    "\"write\":{\"allowed\":true,\"role\":\"operator\",\"min\":0,\"max\":100}},"
    "{\"name\":\"TEMP\",\"address\":1,\"type\":\"i16\",\"scale\":0.1,\"units\":\"C\",\"min\":-40,\"max\":150,"
    "\"alarm\":{\"high\":90,\"deadband\":1,\"on_ms\":2000,\"off_ms\":2000}},"
    "{\"name\":\"FLOW\",\"area\":\"input\",\"address\":10,\"type\":\"f32\",\"word_order\":\"low_first\",\"byte_order\":\"swapped\","
    "\"units\":\"l/min\",\"min\":0,\"max\":500},"
    "{\"name\":\"PLAIN\",\"address\":50}"
    "]}";

static hmi_app_cfg_t g_base;

static void load_base(void)
{
    char err[160];
    CHECK(hmi_config_parse(BASE_JSON, strlen(BASE_JSON), &g_base, err, sizeof(err)));
}

/* ---------------------------------------------------------------- writing */
static void test_serialize(void)
{
    SECTION("cfgwrite: a configuration is written out and reads back the same");
    load_base();
    static char text[HMI_CONFIG_MAX_BYTES + 1];
    static hmi_app_cfg_t back;
    char err[160];
    size_t n = hmi_config_serialize(&g_base, text, sizeof(text));
    CHECK(n > 0 && n == strlen(text));
    CHECK(hmi_config_parse(text, n, &back, err, sizeof(err)) && hmi_config_equal(&g_base, &back));

    /* a value that holds its default is left out: PLAIN is just a name and an address */
    CHECK(strstr(text, "{\"name\":\"PLAIN\",\"address\":50}") != NULL);
    CHECK(strstr(text, "\"trend\":true") != NULL && strstr(text, "\"allowed_hosts\":[\"192.168.0.10\",\"192.168.0.11\",\"10.0.0.5\"]") != NULL);
    CHECK(strstr(text, "\"priority\":\"high\"") != NULL);
    /* the priority of TEMP is the default (medium), so it is not written */
    const char *temp = strstr(text, "\"name\":\"TEMP\"");
    const char *flow = strstr(text, "\"name\":\"FLOW\"");
    CHECK(temp && flow && memchr(temp, 'p', (size_t)(flow - temp)) != NULL);   /* "poll"-free: just make sure the span is sane */
    bool temp_has_priority = false;
    for (const char *p = temp; p < flow - 8; p++)
        if (strncmp(p, "priority", 8) == 0) temp_has_priority = true;
    CHECK(!temp_has_priority);

    /* writing it again gives the same bytes */
    static char text2[HMI_CONFIG_MAX_BYTES + 1];
    CHECK(hmi_config_serialize(&back, text2, sizeof(text2)) == n && strcmp(text, text2) == 0);

    /* every smaller buffer is refused whole, never a cut-off text */
    bool all_refused = true;
    static char small[HMI_CONFIG_MAX_BYTES + 1];
    for (size_t cap = 1; cap <= n; cap++)
        if (hmi_config_serialize(&g_base, small, cap) != 0) all_refused = false;
    CHECK(all_refused);
    CHECK(hmi_config_serialize(&g_base, small, n + 1) == n);
    CHECK(hmi_config_serialize(NULL, small, sizeof(small)) == 0 && hmi_config_serialize(&g_base, NULL, 100) == 0 &&
          hmi_config_serialize(&g_base, small, 0) == 0);

    /* things the parser could not read back are not written */
    hmi_app_cfg_t bad = g_base;
    snprintf(bad.tags[0].name, sizeof(bad.tags[0].name), "A\"B");
    CHECK(hmi_config_serialize(&bad, small, sizeof(small)) == 0);
    bad = g_base;
    snprintf(bad.tags[0].units, sizeof(bad.tags[0].units), "a\\b");
    CHECK(hmi_config_serialize(&bad, small, sizeof(small)) == 0);
    bad = g_base;
    bad.tags[1].name[0] = (char)0xC3;
    CHECK(hmi_config_serialize(&bad, small, sizeof(small)) == 0);
    bad = g_base;
    bad.tags[1].name[0] = 7;
    CHECK(hmi_config_serialize(&bad, small, sizeof(small)) == 0);
    bad = g_base;
    snprintf(bad.host, sizeof(bad.host), "1.2.3\"4");
    CHECK(hmi_config_serialize(&bad, small, sizeof(small)) == 0);
    bad = g_base;
    bad.n_tags = 0;
    CHECK(hmi_config_serialize(&bad, small, sizeof(small)) == 0);
    bad = g_base;
    bad.tags[0].dtype = (hmi_dtype_t)9;
    CHECK(hmi_config_serialize(&bad, small, sizeof(small)) == 0);
}

static void test_numbers_exact(void)
{
    SECTION("cfgwrite: numbers come back as the same double");
    load_base();
    static hmi_app_cfg_t c, back;
    static char text[HMI_CONFIG_MAX_BYTES + 1];
    char err[160];
    c = g_base;
    const double vals[] = { 0.1, 0.2, 1.0 / 3.0, 1e-9, 123456789.123456789, 1e300, 5e-324 * 1e10, -0.0000123, 4294967295.0 };
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        c.tags[2].scale = vals[i] == 0 ? 1.0 : vals[i];
        c.tags[2].offset = -vals[i];
        c.tags[2].eng_min = -fabs(vals[i]) - 1;
        c.tags[2].eng_max = fabs(vals[i]) + 1;
        size_t n = hmi_config_serialize(&c, text, sizeof(text));
        CHECK(n > 0 && hmi_config_parse(text, n, &back, err, sizeof(err)));
        CHECK(back.tags[2].scale == c.tags[2].scale && back.tags[2].offset == c.tags[2].offset &&
              back.tags[2].eng_min == c.tags[2].eng_min && back.tags[2].eng_max == c.tags[2].eng_max);
    }
}

static void test_equal_sees_every_field(void)
{
    SECTION("cfgwrite: hmi_config_equal notices a change in every field");
    load_base();
    CHECK(hmi_config_equal(&g_base, &g_base));
    static hmi_app_cfg_t m;
#define MUT(stmt) do { m = g_base; stmt; CHECK(!hmi_config_equal(&g_base, &m)); } while (0)
    MUT(m.host[0] ^= 1);  MUT(m.port++);  MUT(m.unit++);  MUT(m.response_timeout_ms++);  MUT(m.connect_timeout_ms++);
    MUT(m.writes_enabled = !m.writes_enabled);  MUT(m.write_min_interval_ms++);  MUT(m.write_max_per_minute++);
    MUT(m.trend_bucket_ms++);  MUT(m.n_allowed--);  MUT(m.allowed_hosts[2][0] = '9');  MUT(m.n_tags--);
    for (size_t i = 0; i < g_base.n_tags; i++) {
        MUT(m.tags[i].name[0] ^= 1);  MUT(m.tags[i].units[0] ^= 1);  MUT(m.tags[i].area = m.tags[i].area ? HMI_AREA_HOLDING : HMI_AREA_INPUT);
        MUT(m.tags[i].address++);  MUT(m.tags[i].dtype = (hmi_dtype_t)(m.tags[i].dtype == HMI_DT_F32 ? HMI_DT_U16 : m.tags[i].dtype + 1));
        MUT(m.tags[i].word_order = m.tags[i].word_order ? HMI_WORD_HIGH_FIRST : HMI_WORD_LOW_FIRST);
        MUT(m.tags[i].byte_order = m.tags[i].byte_order ? HMI_BYTE_BIG : HMI_BYTE_SWAPPED);
        MUT(m.tags[i].scale += 1);  MUT(m.tags[i].offset += 1);  MUT(m.tags[i].eng_min -= 1);  MUT(m.tags[i].eng_max += 1);
        MUT(m.tags[i].poll_ms++);  MUT(m.tags[i].trend = !m.tags[i].trend);
        MUT(m.tags[i].alarm.en_hi = !m.tags[i].alarm.en_hi);  MUT(m.tags[i].alarm.en_hihi = !m.tags[i].alarm.en_hihi);
        MUT(m.tags[i].alarm.en_lo = !m.tags[i].alarm.en_lo);  MUT(m.tags[i].alarm.en_lolo = !m.tags[i].alarm.en_lolo);
        MUT(m.tags[i].alarm.hi += 1);  MUT(m.tags[i].alarm.hihi += 1);  MUT(m.tags[i].alarm.lo += 1);  MUT(m.tags[i].alarm.lolo += 1);
        MUT(m.tags[i].alarm.deadband += 1);  MUT(m.tags[i].alarm.on_ms++);  MUT(m.tags[i].alarm.off_ms++);
        MUT(m.tags[i].alarm.priority = (hmi_priority_t)((m.tags[i].alarm.priority + 1) % 4));
        MUT(m.tags[i].write_allowed = !m.tags[i].write_allowed);
        MUT(m.tags[i].write_role = (hmi_role_t)((m.tags[i].write_role + 1) % 4));
        MUT(m.tags[i].write_min += 1);  MUT(m.tags[i].write_max += 1);
    }
#undef MUT
}

static void test_big_configuration(void)
{
    SECTION("cfgwrite: sixty-four tags stay within the file and token limits");
    static hmi_app_cfg_t c, back;
    static char json[HMI_CONFIG_MAX_BYTES], text[HMI_CONFIG_MAX_BYTES + 1];
    char err[160];
    size_t off = (size_t)snprintf(json, sizeof(json),
        "{\"version\":1,\"plc\":{\"host\":\"10.0.0.1\",\"port\":502,\"unit\":1,\"allowed_hosts\":[\"10.0.0.1\"]},\"tags\":[");
    for (int k = 0; k < 64; k++)
        off += (size_t)snprintf(json + off, sizeof(json) - off,
            "%s{\"name\":\"T%02d\",\"address\":%d,\"type\":\"u16\",\"scale\":0.5,\"alarm\":{\"high\":%d,\"deadband\":1}}",
            k ? "," : "", k, k * 2, 50 + k);
    snprintf(json + off, sizeof(json) - off, "]}");
    CHECK(hmi_config_parse(json, strlen(json), &c, err, sizeof(err)));
    size_t n = hmi_config_serialize(&c, text, sizeof(text));
    CHECK(n > 0 && n < HMI_CONFIG_MAX_BYTES);
    CHECK(hmi_config_parse(text, n, &back, err, sizeof(err)) && hmi_config_equal(&c, &back));
}

/* ---------------------------------------------------------------- request text */
static void test_request_format_and_parse(void)
{
    SECTION("cfgedit: the request text, exactly");
    hmi_cfgedit_request_t r, p;
    hmi_cfgedit_init(&r, 7, "eng1");
    CHECK(hmi_cfgedit_set_host(&r, "192.168.0.11"));
    CHECK(hmi_cfgedit_set_number(&r, HMI_CE_PLC_PORT, NULL, 1502));
    CHECK(hmi_cfgedit_set_number(&r, HMI_CE_PLC_UNIT, "ignored", 3));
    CHECK(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TANK_LEVEL", 82.5));
    CHECK(hmi_cfgedit_set_limit_off(&r, HMI_CE_ALARM_LOLO, "TANK_LEVEL"));
    CHECK(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_DEADBAND, "TANK_LEVEL", 0.25));
    CHECK(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_ON_MS, "TEMP", 1500));
    CHECK(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_OFF_MS, "TEMP", 0));
    CHECK(hmi_cfgedit_set_priority(&r, "TEMP", HMI_PRI_CRITICAL));
    char text[HMI_CFGEDIT_REQUEST_MAX + 1];
    size_t n = hmi_cfgedit_format(&r, text, sizeof(text));
    const char *want =
        "P99REQ 1\nbase_serial=7\nuser=eng1\nplc.host=192.168.0.11\nplc.port=1502\nplc.unit=3\n"
        "tag.TANK_LEVEL.alarm.high=82.5\ntag.TANK_LEVEL.alarm.low_low=off\ntag.TANK_LEVEL.alarm.deadband=0.25\n"
        "tag.TEMP.alarm.on_ms=1500\ntag.TEMP.alarm.off_ms=0\ntag.TEMP.alarm.priority=critical\nEND\n";
    CHECK(n == strlen(want) && strcmp(text, want) == 0);

    char err[160];
    CHECK(hmi_cfgedit_parse(text, n, &p, err, sizeof(err)));
    CHECK(p.base_serial == 7 && strcmp(p.user, "eng1") == 0 && p.n == r.n);
    bool same = true;
    for (size_t i = 0; i < r.n; i++)
        same = same && p.c[i].field == r.c[i].field && strcmp(p.c[i].tag, r.c[i].tag) == 0 && p.c[i].num == r.c[i].num &&
               p.c[i].off == r.c[i].off && strcmp(p.c[i].host, r.c[i].host) == 0;
    CHECK(same);
    CHECK(hmi_cfgedit_format(&p, text, n) == 0 && hmi_cfgedit_format(&p, text, n + 1) == n);   /* a short buffer is refused whole */

    /* setting the same thing again replaces it, it does not add a second line */
    size_t before = r.n;
    CHECK(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TANK_LEVEL", 83));
    CHECK(r.n == before);
    CHECK(hmi_cfgedit_set_limit_off(&r, HMI_CE_ALARM_HI, "TANK_LEVEL") && r.n == before && r.c[3].off);
    CHECK(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TANK_LEVEL", 84) && !r.c[3].off && r.c[3].num == 84);

    /* the user name is only kept if it is an account name */
    hmi_cfgedit_init(&r, 1, "Bad Name");
    CHECK(strcmp(r.user, "-") == 0);
    hmi_cfgedit_init(&r, 1, NULL);
    CHECK(strcmp(r.user, "-") == 0);
    hmi_cfgedit_init(&r, 1, "ab");
    CHECK(strcmp(r.user, "-") == 0);

    /* what the builders refuse */
    hmi_cfgedit_init(&r, 1, "eng1");
    CHECK(!hmi_cfgedit_set_host(&r, "plc.local") && !hmi_cfgedit_set_host(&r, "1.2.3") && !hmi_cfgedit_set_host(&r, "256.1.1.1") &&
          !hmi_cfgedit_set_host(&r, NULL) && !hmi_cfgedit_set_host(&r, "1.2.3.4\n"));
    CHECK(!hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TANK_LEVEL", NAN) && !hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TANK_LEVEL", INFINITY));
    CHECK(!hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, NULL, 1) && !hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "", 1));
    CHECK(!hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "NAME_THAT_IS_MUCH_TOO_LONG_TO_FIT", 1));
    CHECK(!hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "A=B", 1) && !hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "A\nB", 1));
    CHECK(!hmi_cfgedit_set_number(&r, HMI_CE_ALARM_PRIORITY, "TEMP", 1));         /* priority has its own call */
    CHECK(!hmi_cfgedit_set_number(&r, (hmi_ce_field_t)99, "TEMP", 1) && !hmi_cfgedit_set_number(&r, HMI_CE_PLC_HOST, "TEMP", 1));
    CHECK(!hmi_cfgedit_set_limit_off(&r, HMI_CE_ALARM_DEADBAND, "TEMP") && !hmi_cfgedit_set_limit_off(&r, HMI_CE_ALARM_ON_MS, "TEMP"));
    CHECK(!hmi_cfgedit_set_priority(&r, "TEMP", (hmi_priority_t)4) && !hmi_cfgedit_set_priority(&r, NULL, HMI_PRI_LOW));
    CHECK(r.n == 0);
    /* the table holds sixty-four changes and then says so */
    char tag[HMI_NAME_LEN];
    for (size_t i = 0; i < HMI_CFGEDIT_MAX_CHANGES; i++) {
        snprintf(tag, sizeof(tag), "T%02u", (unsigned)i);
        CHECK(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, tag, 1));
    }
    CHECK(!hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "ONE_MORE", 1) && r.n == HMI_CFGEDIT_MAX_CHANGES);
    CHECK(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "T00", 2));                 /* a replacement still fits */
}

static bool parses(const char *text, char *err, size_t cap)
{
    hmi_cfgedit_request_t r;
    bool ok = hmi_cfgedit_parse(text, strlen(text), &r, err, cap);
    if (!ok) CHECK(r.n == 0 && r.base_serial == 0);   /* a refused request leaves nothing behind */
    return ok;
}

static void test_request_refusals(void)
{
    SECTION("cfgedit: a request that is not exactly right is refused whole");
    char err[160];
    const char *head = "P99REQ 1\nbase_serial=3\nuser=eng1\n";
    char t[1024];
#define REQ(body) (snprintf(t, sizeof(t), "%s%s", head, body), t)
    CHECK(parses(REQ("END\n"), err, sizeof(err)));                                          /* no change is a valid request here; apply refuses it */
    CHECK(parses(REQ("tag.TEMP.alarm.high=91\nEND\n"), err, sizeof(err)));

    /* shape */
    CHECK(!parses("", err, sizeof(err)));
    CHECK(!parses("\n", err, sizeof(err)));
    CHECK(!parses("P99REQ 2\nbase_serial=3\nuser=eng1\nEND\n", err, sizeof(err)));
    CHECK(!parses("P99REQ 1\r\nbase_serial=3\nuser=eng1\nEND\n", err, sizeof(err)));
    CHECK(!parses("p99req 1\nbase_serial=3\nuser=eng1\nEND\n", err, sizeof(err)));
    CHECK(!parses("P99REQ 1\nuser=eng1\nbase_serial=3\nEND\n", err, sizeof(err)));        /* order of the two header lines */
    CHECK(!parses("P99REQ 1\nbase_serial=3\nEND\n", err, sizeof(err)));
    CHECK(!parses("P99REQ 1\nbase_serial=\nuser=eng1\nEND\n", err, sizeof(err)));
    CHECK(!parses("P99REQ 1\nbase_serial=-3\nuser=eng1\nEND\n", err, sizeof(err)));
    CHECK(!parses("P99REQ 1\nbase_serial=99999999999\nuser=eng1\nEND\n", err, sizeof(err)));
    CHECK(!parses("P99REQ 1\nbase_serial=3 \nuser=eng1\nEND\n", err, sizeof(err)));
    CHECK(!parses(REQ("tag.TEMP.alarm.high=91\n"), err, sizeof(err)) && strstr(err, "END") != NULL);   /* no END */
    CHECK(!parses(REQ("tag.TEMP.alarm.high=91\nEND"), err, sizeof(err)));                  /* END without its line feed */
    CHECK(!parses(REQ("END\nEND\n"), err, sizeof(err)) && strstr(err, "after END") != NULL);
    CHECK(!parses(REQ("END\n\n"), err, sizeof(err)));
    CHECK(!parses(REQ("END\nplc.port=5\n"), err, sizeof(err)));
    CHECK(!parses(REQ("\nEND\n"), err, sizeof(err)));                                        /* an empty line */
    CHECK(!parses(REQ("tag.TEMP.alarm.high=91\n\nEND\n"), err, sizeof(err)));
    CHECK(!parses(REQ(" END\n"), err, sizeof(err)) && !parses(REQ("END \n"), err, sizeof(err)));

    /* what may not be asked for */
    const char *forbidden[] = {
        "writes_enabled=true", "writes_enabled=false", "write_min_interval_ms=100", "write_max_per_minute=600",
        "trend_bucket_ms=1000", "plc.allowed_hosts=10.0.0.1", "plc.response_timeout_ms=100", "plc.connect_timeout_ms=100",
        "plc.timeout=5", "tag.TEMP.address=5", "tag.TEMP.type=u16", "tag.TEMP.scale=2", "tag.TEMP.units=x", "tag.TEMP.min=0",
        "tag.TEMP.poll_ms=100", "tag.TEMP.trend=true", "tag.TEMP.write.allowed=true", "tag.TEMP.write.role=viewer",
        "tag.TEMP.write.max=100", "tag.TEMP.alarm.unknown=1", "tag.TEMP.alarm.=1", "tag.TEMP.alarm.High=1", "tag.TEMP.high=1",
        "tag.TEMP=1", "tag..alarm.high=1", "tag.alarm.high=1", "plc.hosts=1.2.3.4", "plc.host.extra=1.2.3.4", "plc=1",
        "users.admin=1", "serial=9", "base_serial=4", "user=x", "tag.TEMP.alarm.high.extra=1", "TAG.TEMP.alarm.high=1",
        "tag.NAME_THAT_IS_MUCH_TOO_LONG_TO_FIT.alarm.high=1",
    };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        char body[200];
        snprintf(body, sizeof(body), "%s\nEND\n", forbidden[i]);
        if (parses(REQ(body), err, sizeof(err))) printf("  accepted: %s\n", forbidden[i]);
        CHECK(!parses(REQ(body), err, sizeof(err)));
    }

    /* values */
    const char *badvals[] = {
        "plc.host=plc.local", "plc.host=1.2.3", "plc.host=1.2.3.4.5", "plc.host=256.1.1.1", "plc.host=", "plc.host=1.2.3.4 ",
        "plc.port=-1", "plc.port=1.5", "plc.port=abc", "plc.port=", "plc.port=99999999999", "plc.unit=1e1", "plc.unit=0x10",
        "tag.TEMP.alarm.high=", "tag.TEMP.alarm.high=abc", "tag.TEMP.alarm.high=1e", "tag.TEMP.alarm.high=--1",
        "tag.TEMP.alarm.high=+1", "tag.TEMP.alarm.high=.5", "tag.TEMP.alarm.high=5.", "tag.TEMP.alarm.high=0x10",
        "tag.TEMP.alarm.high=1,5", "tag.TEMP.alarm.high= 1", "tag.TEMP.alarm.high=1 ", "tag.TEMP.alarm.high=NaN",
        "tag.TEMP.alarm.high=inf", "tag.TEMP.alarm.high=-inf", "tag.TEMP.alarm.high=OFF", "tag.TEMP.alarm.high=Off",
        "tag.TEMP.alarm.high=1e999", "tag.TEMP.alarm.high=123456789012345678901234567890123",
        "tag.TEMP.alarm.deadband=off", "tag.TEMP.alarm.on_ms=off", "tag.TEMP.alarm.on_ms=1.5", "tag.TEMP.alarm.on_ms=-1",
        "tag.TEMP.alarm.priority=urgent", "tag.TEMP.alarm.priority=HIGH", "tag.TEMP.alarm.priority=2", "tag.TEMP.alarm.priority=off",
        "tag.TEMP.alarm.priority=",
    };
    for (size_t i = 0; i < sizeof(badvals) / sizeof(badvals[0]); i++) {
        char body[200];
        snprintf(body, sizeof(body), "%s\nEND\n", badvals[i]);
        if (parses(REQ(body), err, sizeof(err))) printf("  accepted: %s\n", badvals[i]);
        CHECK(!parses(REQ(body), err, sizeof(err)));
    }
    const char *goodvals[] = {
        "plc.host=1.2.3.4", "plc.port=502", "plc.unit=0", "tag.TEMP.alarm.high=-5", "tag.TEMP.alarm.high=0", "tag.TEMP.alarm.high=1e2",
        "tag.TEMP.alarm.high=1.5E-3", "tag.TEMP.alarm.high=off", "tag.TEMP.alarm.low_low=off", "tag.TEMP.alarm.priority=low",
        "tag.TEMP.alarm.priority=critical", "tag.TEMP.alarm.deadband=0", "tag.A.B.alarm.high=1",
    };
    for (size_t i = 0; i < sizeof(goodvals) / sizeof(goodvals[0]); i++) {
        char body[200];
        snprintf(body, sizeof(body), "%s\nEND\n", goodvals[i]);
        if (!parses(REQ(body), err, sizeof(err))) printf("  refused: %s (%s)\n", goodvals[i], err);
        CHECK(parses(REQ(body), err, sizeof(err)));
    }
    /* a tag name may hold dots: the field is what follows the last ".alarm." */
    hmi_cfgedit_request_t r;
    CHECK(hmi_cfgedit_parse(REQ("tag.A.B.alarm.high=1\nEND\n"), strlen(t), &r, err, sizeof(err)) && strcmp(r.c[0].tag, "A.B") == 0);

    /* duplicates, in any order and for any kind */
    CHECK(!parses(REQ("plc.port=1\nplc.port=2\nEND\n"), err, sizeof(err)) && strstr(err, "twice") != NULL);
    CHECK(!parses(REQ("tag.TEMP.alarm.high=1\ntag.TEMP.alarm.low=0\ntag.TEMP.alarm.high=2\nEND\n"), err, sizeof(err)));
    CHECK(parses(REQ("tag.TEMP.alarm.high=1\ntag.FLOW.alarm.high=2\nEND\n"), err, sizeof(err)));      /* same field, other tag */
    CHECK(!parses(REQ("tag.TEMP.alarm.high=1\ntag.TEMP.alarm.high=off\nEND\n"), err, sizeof(err)));

    /* characters */
    CHECK(!parses("P99REQ 1\nbase_serial=3\nuser=eng1\nplc.port=5\x01\nEND\n", err, sizeof(err)));
    CHECK(!parses("P99REQ 1\nbase_serial=3\nuser=eng1\nplc.port=5\x7f\nEND\n", err, sizeof(err)));
    CHECK(!parses("P99REQ 1\nbase_serial=3\nuser=eng1\ntag.\xc3\xa9.alarm.high=1\nEND\n", err, sizeof(err)));
    CHECK(!parses("P99REQ 1\nbase_serial=3\tuser=eng1\nEND\n", err, sizeof(err)));
    {   /* a NUL inside the text */
        char nul[] = "P99REQ 1\nbase_serial=3\nuser=eng1\nplc.port=5\nEND\n";
        nul[strlen("P99REQ 1\nbase_serial=3\nuser=eng1\nplc.p")] = '\0';
        hmi_cfgedit_request_t q;
        CHECK(!hmi_cfgedit_parse(nul, sizeof(nul) - 1, &q, err, sizeof(err)));
    }
    /* the user name is only kept if it is an account name; anything else is "-" and the request still reads */
    CHECK(hmi_cfgedit_parse("P99REQ 1\nbase_serial=3\nuser=Eng One;rm\nEND\n", 41, &r, err, sizeof(err)) || true);
    const char *odd = "P99REQ 1\nbase_serial=3\nuser=Eng One;rm\nEND\n";
    CHECK(hmi_cfgedit_parse(odd, strlen(odd), &r, err, sizeof(err)) && strcmp(r.user, "-") == 0);

    /* size */
    static char big[HMI_CFGEDIT_REQUEST_MAX + 100];
    memset(big, 'a', sizeof(big));
    CHECK(!hmi_cfgedit_parse(big, HMI_CFGEDIT_REQUEST_MAX + 1, &r, err, sizeof(err)) && strstr(err, "too large") != NULL);
    CHECK(!hmi_cfgedit_parse(NULL, 5, &r, err, sizeof(err)) && !hmi_cfgedit_parse("x", 0, &r, err, sizeof(err)));
    {   /* a line of 160 characters */
        char ln[400];
        snprintf(ln, sizeof(ln), "%sbase=%0155d\nEND\n", "P99REQ 1\nbase_serial=3\nuser=eng1\n", 1);
        CHECK(!hmi_cfgedit_parse(ln, strlen(ln), &r, err, sizeof(err)));
    }
    {   /* more than sixty-four changes */
        static char many[6000];
        size_t off = (size_t)snprintf(many, sizeof(many), "P99REQ 1\nbase_serial=3\nuser=eng1\n");
        for (int i = 0; i < 65; i++) off += (size_t)snprintf(many + off, sizeof(many) - off, "tag.T%02d.alarm.high=1\n", i);
        snprintf(many + off, sizeof(many) - off, "END\n");
        CHECK(!hmi_cfgedit_parse(many, strlen(many), &r, err, sizeof(err)) && strstr(err, "too many") != NULL);
    }
#undef REQ
}

static void test_request_prefix_and_byte_fuzz(void)
{
    SECTION("cfgedit: no cut-off or damaged request is read as a good one by accident");
    hmi_cfgedit_request_t r, p;
    hmi_cfgedit_init(&r, 7, "eng1");
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TEMP", 91);
    hmi_cfgedit_set_priority(&r, "TEMP", HMI_PRI_HIGH);
    char text[HMI_CFGEDIT_REQUEST_MAX + 1], err[160];
    size_t n = hmi_cfgedit_format(&r, text, sizeof(text));
    CHECK(n > 0 && hmi_cfgedit_parse(text, n, &p, err, sizeof(err)));
    bool prefixes_refused = true;
    for (size_t k = 0; k < n; k++)
        if (hmi_cfgedit_parse(text, k, &p, err, sizeof(err))) prefixes_refused = false;
    CHECK(prefixes_refused);
    /* flipping any one byte either gives a refused request or one that says the same about a different value; never a crash */
    int survived = 0;
    for (size_t k = 0; k < n; k++) {
        char c = text[k];
        text[k] = (char)(c ^ 0x20);
        if (hmi_cfgedit_parse(text, n, &p, err, sizeof(err))) survived++;
        text[k] = (char)(c ^ 0x80);
        CHECK(!hmi_cfgedit_parse(text, n, &p, err, sizeof(err)));      /* a non-ASCII byte is never accepted */
        text[k] = c;
    }
    CHECK(survived < (int)n);                                         /* most single-byte damage is caught */
    for (size_t k = 0; k < n; k++) {                                  /* a deleted byte */
        char tmp[HMI_CFGEDIT_REQUEST_MAX + 1];
        memcpy(tmp, text, k);
        memcpy(tmp + k, text + k + 1, n - k - 1);
        (void)hmi_cfgedit_parse(tmp, n - 1, &p, err, sizeof(err));   /* must simply not crash */
    }
}

/* ---------------------------------------------------------------- apply */
static bool apply(const hmi_cfgedit_request_t *r, hmi_app_cfg_t *out, size_t *eff, char *err, size_t cap)
{
    static char text[HMI_CONFIG_MAX_BYTES + 1];
    size_t len = 0;
    return hmi_cfgedit_apply(r, &g_base, out, text, sizeof(text), &len, eff, err, cap);
}

/* Everything of `a` and `b` that the whitelist does not cover must be the same. */
static bool same_outside_whitelist(const hmi_app_cfg_t *a, const hmi_app_cfg_t *b)
{
    static hmi_app_cfg_t t;
    t = *a;
    memcpy(t.host, b->host, sizeof(t.host));
    t.port = b->port;
    t.unit = b->unit;
    for (size_t i = 0; i < t.n_tags && i < b->n_tags; i++) t.tags[i].alarm = b->tags[i].alarm;
    return hmi_config_equal(&t, b);
}

static void test_apply_allowed(void)
{
    SECTION("cfgedit: every allowed change is made, and nothing else changes");
    load_base();
    static hmi_app_cfg_t out;
    char err[160];
    size_t eff = 0;
    hmi_cfgedit_request_t r;

    hmi_cfgedit_init(&r, 1, "eng1");
    hmi_cfgedit_set_host(&r, "192.168.0.11");
    CHECK(apply(&r, &out, &eff, err, sizeof(err)) && eff == 1 && strcmp(out.host, "192.168.0.11") == 0);
    CHECK(same_outside_whitelist(&out, &g_base) && out.port == g_base.port && out.unit == g_base.unit);
    hmi_cfgedit_init(&r, 1, "eng1");
    hmi_cfgedit_set_host(&r, "10.0.0.5");
    CHECK(apply(&r, &out, &eff, err, sizeof(err)) && strcmp(out.host, "10.0.0.5") == 0);

    hmi_cfgedit_init(&r, 1, "eng1");
    hmi_cfgedit_set_number(&r, HMI_CE_PLC_PORT, NULL, 1502);
    hmi_cfgedit_set_number(&r, HMI_CE_PLC_UNIT, NULL, 7);
    CHECK(apply(&r, &out, &eff, err, sizeof(err)) && eff == 2 && out.port == 1502 && out.unit == 7 && same_outside_whitelist(&out, &g_base));
    CHECK(strcmp(out.host, g_base.host) == 0);                       /* the host was not asked for */

    /* every alarm field of a tag, in one request */
    hmi_cfgedit_init(&r, 1, "eng1");
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TANK_LEVEL", 82.5);
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HIHI, "TANK_LEVEL", 97);
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_LO, "TANK_LEVEL", 12);
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_LOLO, "TANK_LEVEL", 6);
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_DEADBAND, "TANK_LEVEL", 1.5);
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_ON_MS, "TANK_LEVEL", 1500);
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_OFF_MS, "TANK_LEVEL", 2500);
    hmi_cfgedit_set_priority(&r, "TANK_LEVEL", HMI_PRI_CRITICAL);
    CHECK(apply(&r, &out, &eff, err, sizeof(err)) && eff == 8);
    const hmi_alarm_cfg_t *a = &out.tags[0].alarm;
    CHECK(a->en_hi && a->hi == 82.5 && a->en_hihi && a->hihi == 97 && a->en_lo && a->lo == 12 && a->en_lolo && a->lolo == 6);
    CHECK(a->deadband == 1.5 && a->on_ms == 1500 && a->off_ms == 2500 && a->priority == HMI_PRI_CRITICAL);
    CHECK(same_outside_whitelist(&out, &g_base));
    CHECK(out.tags[1].alarm.hi == 90 && out.tags[2].alarm.priority == HMI_PRI_MEDIUM);        /* other tags untouched */
    CHECK(out.tags[0].write_allowed && out.tags[0].write_role == HMI_ROLE_OPERATOR && out.tags[0].write_max == 100);
    CHECK(out.writes_enabled && out.write_min_interval_ms == 500 && out.write_max_per_minute == 12);
    CHECK(out.n_allowed == 3 && out.response_timeout_ms == 400 && out.connect_timeout_ms == 900 && out.trend_bucket_ms == 2000);

    /* limits can be switched off and on, on a tag that has no alarm yet too */
    hmi_cfgedit_init(&r, 1, "eng1");
    hmi_cfgedit_set_limit_off(&r, HMI_CE_ALARM_LOLO, "TANK_LEVEL");
    hmi_cfgedit_set_limit_off(&r, HMI_CE_ALARM_HIHI, "TANK_LEVEL");
    CHECK(apply(&r, &out, &eff, err, sizeof(err)) && eff == 2 && !out.tags[0].alarm.en_lolo && out.tags[0].alarm.lolo == 0 &&
          !out.tags[0].alarm.en_hihi && out.tags[0].alarm.en_hi);
    hmi_cfgedit_init(&r, 1, "eng1");
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "PLAIN", 50);
    hmi_cfgedit_set_priority(&r, "PLAIN", HMI_PRI_LOW);
    CHECK(apply(&r, &out, &eff, err, sizeof(err)) && out.tags[3].alarm.en_hi && out.tags[3].alarm.hi == 50 && out.tags[3].alarm.priority == HMI_PRI_LOW);
    hmi_cfgedit_init(&r, 1, "eng1");
    hmi_cfgedit_set_limit_off(&r, HMI_CE_ALARM_HI, "PLAIN");                       /* off for what is already off: no change */
    CHECK(!apply(&r, &out, &eff, err, sizeof(err)) && strstr(err, "nothing") != NULL);

    /* the text that was written is what the device will read */
    static char text[HMI_CONFIG_MAX_BYTES + 1];
    static hmi_app_cfg_t again;
    size_t len = 0;
    hmi_cfgedit_init(&r, 1, "eng1");
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TEMP", 95);
    CHECK(hmi_cfgedit_apply(&r, &g_base, &out, text, sizeof(text), &len, &eff, err, sizeof(err)));
    CHECK(len == strlen(text) && hmi_config_parse(text, len, &again, err, sizeof(err)) && hmi_config_equal(&again, &out));
    CHECK(out.tags[1].alarm.hi == 95 && hmi_config_host_allowed(&out));

    /* a limit exactly on the edge of the tag range, and a delay at its bounds */
    hmi_cfgedit_init(&r, 1, "eng1");
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TEMP", 150);
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_LO, "TEMP", -40);
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_ON_MS, "TEMP", 600000);
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_OFF_MS, "TEMP", 0);
    CHECK(apply(&r, &out, &eff, err, sizeof(err)) && out.tags[1].alarm.hi == 150 && out.tags[1].alarm.lo == -40);
}

static void test_apply_refusals(void)
{
    SECTION("cfgedit: what is not allowed, or not valid, changes nothing");
    load_base();
    static hmi_app_cfg_t out;
    char err[160];
    size_t eff = 99;
    hmi_cfgedit_request_t r;
#define REFUSED(setup, why)                                                      \
    do {                                                                         \
        hmi_cfgedit_init(&r, 1, "eng1");                                         \
        setup;                                                                   \
        bool ok_ = apply(&r, &out, &eff, err, sizeof(err));                      \
        if (ok_ || strstr(err, why) == NULL) printf("  expected refusal (%s), got ok=%d err='%s'\n", why, ok_, err); \
        CHECK(!ok_ && strstr(err, why) != NULL);                                 \
        CHECK(out.n_tags == 0 && out.host[0] == '\0' && !out.writes_enabled && eff == 0); \
    } while (0)

    REFUSED((void)0, "no change");                                                                   /* an empty request */
    REFUSED(hmi_cfgedit_set_host(&r, "192.168.0.99"), "allowed_hosts: 192.168.0.99");                /* not on the list, and named */
    REFUSED(hmi_cfgedit_set_host(&r, "8.8.8.8"), "allowed_hosts");
    REFUSED(hmi_cfgedit_set_host(&r, "192.168.0.10"), "nothing would change");                       /* the same host */
    {
        /* a configuration without an allow-list is not one the device would run; a change to it is refused as a whole */
        static hmi_app_cfg_t bare;
        static char btext[HMI_CONFIG_MAX_BYTES + 1];
        size_t blen = 0;
        bare = g_base;
        bare.n_allowed = 0;
        hmi_cfgedit_init(&r, 1, "eng1");
        hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TEMP", 91);
        CHECK(!hmi_cfgedit_apply(&r, &bare, &out, btext, sizeof(btext), &blen, &eff, err, sizeof(err)));
        CHECK(strstr(err, "plc.allowed_hosts") != NULL && out.n_tags == 0 && eff == 0);
    }
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_PLC_PORT, NULL, 0), "plc.port");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_PLC_PORT, NULL, 65536), "plc.port");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_PLC_PORT, NULL, 5.5), "plc.port");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_PLC_UNIT, NULL, 248), "plc.unit");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_PLC_UNIT, NULL, -1), "plc.unit");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "NO_SUCH_TAG", 5), "no such tag");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TEMP", 150.5), "inside");                   /* above the tag's max */
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_LO, "TEMP", -40.5), "inside");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_DEADBAND, "TEMP", -1), "deadband");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_DEADBAND, "TEMP", 191), "deadband");             /* more than the range (190) */
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_ON_MS, "TEMP", 600001), "delay");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_ON_MS, "TEMP", -1), "delay");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_OFF_MS, "TEMP", 1.5), "delay");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TEMP", 91); r.c[0].field = (hmi_ce_field_t)99, "cannot be changed");
    REFUSED(hmi_cfgedit_set_priority(&r, "TEMP", HMI_PRI_HIGH); r.c[0].num = 7, "priority");
    /* the parser's own rules decide the rest: the whole request is refused */
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HIHI, "TANK_LEVEL", 70), "high_high must exceed high");   /* 70 below high 80 */
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TANK_LEVEL", 96), "high_high must exceed high");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_LOLO, "TANK_LEVEL", 11), "low_low must be below low");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_LO, "TANK_LEVEL", 85), "high must exceed low");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_DEADBAND, "TANK_LEVEL", 20), "deadband too large");     /* hihi - hi = 15 */
    /* one bad change spoils a request that also holds good ones */
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TEMP", 91); hmi_cfgedit_set_host(&r, "8.8.8.8"), "allowed_hosts");
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_PLC_PORT, NULL, 1502); hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "NO_SUCH", 1), "no such tag");
    /* a request that changes nothing */
    REFUSED(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TANK_LEVEL", 80); hmi_cfgedit_set_number(&r, HMI_CE_PLC_PORT, NULL, 502); hmi_cfgedit_set_priority(&r, "TANK_LEVEL", HMI_PRI_HIGH), "nothing would change");
#undef REFUSED

    /* arguments */
    static char text[HMI_CONFIG_MAX_BYTES + 1];
    size_t len = 0;
    hmi_cfgedit_init(&r, 1, "eng1");
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TEMP", 91);
    CHECK(!hmi_cfgedit_apply(&r, &g_base, &out, text, HMI_CONFIG_MAX_BYTES, &len, &eff, err, sizeof(err)));   /* buffer too small */
    CHECK(!hmi_cfgedit_apply(NULL, &g_base, &out, text, sizeof(text), &len, &eff, err, sizeof(err)));
    CHECK(!hmi_cfgedit_apply(&r, NULL, &out, text, sizeof(text), &len, &eff, err, sizeof(err)));
    CHECK(!hmi_cfgedit_apply(&r, &g_base, &out, NULL, sizeof(text), &len, &eff, err, sizeof(err)));
    hmi_app_cfg_t empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(!hmi_cfgedit_apply(&r, &empty, &out, text, sizeof(text), &len, &eff, err, sizeof(err)));
    CHECK(hmi_cfgedit_apply(&r, &g_base, &out, text, sizeof(text), NULL, NULL, NULL, 0));                     /* optional outputs */
}

static void test_apply_example_and_big(void)
{
    SECTION("cfgedit: the shipped example, and sixty-four tags");
    static hmi_app_cfg_t ex, out;
    char err[160];
    size_t eff;
    CHECK(hmi_config_load_file("config/hmi.example.json", &ex, err, sizeof(err)));
    hmi_cfgedit_request_t r;
    hmi_cfgedit_init(&r, 1, "eng1");
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TANK_LEVEL", 85);
    static char text[HMI_CONFIG_MAX_BYTES + 1];
    size_t len = 0;
    CHECK(hmi_cfgedit_apply(&r, &ex, &out, text, sizeof(text), &len, &eff, err, sizeof(err)) && out.tags[0].alarm.hi == 85);
    CHECK(out.n_allowed == ex.n_allowed && strcmp(out.host, ex.host) == 0 && out.tags[1].alarm.priority == HMI_PRI_CRITICAL);

    static hmi_app_cfg_t big;
    static char json[HMI_CONFIG_MAX_BYTES];
    size_t off = (size_t)snprintf(json, sizeof(json),
        "{\"version\":1,\"plc\":{\"host\":\"10.0.0.1\",\"port\":502,\"unit\":1,\"allowed_hosts\":[\"10.0.0.1\"]},\"tags\":[");
    for (int k = 0; k < 64; k++)
        off += (size_t)snprintf(json + off, sizeof(json) - off, "%s{\"name\":\"T%02d\",\"address\":%d,\"alarm\":{\"high\":%d}}",
                                k ? "," : "", k, k * 2, 50 + k);
    snprintf(json + off, sizeof(json) - off, "]}");
    CHECK(hmi_config_parse(json, strlen(json), &big, err, sizeof(err)));
    hmi_cfgedit_init(&r, 1, "eng1");
    for (int k = 0; k < 64; k++) {
        char tag[16];
        snprintf(tag, sizeof(tag), "T%02d", k);
        CHECK(hmi_cfgedit_set_number(&r, HMI_CE_ALARM_DEADBAND, tag, 2));
    }
    CHECK(hmi_cfgedit_apply(&r, &big, &out, text, sizeof(text), &len, &eff, err, sizeof(err)) && eff == 64);
}

/* ---------------------------------------------------------------- spool */
#define SPOOL "cfgedit_spool_test"

static void spool_clean(void)
{
    remove(SPOOL "/request");
    remove(SPOOL "/request.tmp");
    remove(SPOOL "/result");
}

typedef struct {
    char line[8][200];
    int n;
    bool fail;
    char state_before[8];            /* what the spool held when the record was made, one letter per call */
} audit_log_t;
static audit_log_t AL;

static bool audit_cb(void *u, const char *line, size_t len)
{
    (void)u;
    char t[16];
    hmi_cfgreq_state_t s = hmi_cfgreq_state(SPOOL, t, sizeof(t));
    if (AL.n < 8) {
        AL.state_before[AL.n] = s == HMI_CFGREQ_PENDING ? 'P' : 'N';
        snprintf(AL.line[AL.n], sizeof(AL.line[0]), "%.*s", (int)len, line);
        AL.n++;
    }
    return !AL.fail;
}

static void test_spool_submit(void)
{
    SECTION("cfgedit: handing a request to the root program");
    MKDIR(SPOOL);
    spool_clean();
    memset(&AL, 0, sizeof(AL));
    hmi_cfgedit_request_t r;
    hmi_cfgedit_init(&r, 7, "eng1");
    hmi_cfgedit_set_number(&r, HMI_CE_ALARM_HI, "TEMP", 91);
    char want[HMI_CFGEDIT_REQUEST_MAX + 1], got[HMI_CFGEDIT_REQUEST_MAX + 1], t[200];
    size_t wl = hmi_cfgedit_format(&r, want, sizeof(want)), gl = 0;

    /* who may ask: the role is judged here, in the core */
    for (int role = 0; role < 4; role++) {
        memset(&AL, 0, sizeof(AL));
        hmi_cfgreq_result_t res = hmi_cfgreq_submit(&r, (hmi_role_t)role, SPOOL, audit_cb, NULL, 1234);
        if (role >= HMI_ROLE_ENGINEER) {
            CHECK(res == HMI_CFGREQ_OK);
            CHECK(hmi_fs_read_all(SPOOL "/request", got, sizeof(got), &gl) == HMI_FS_READ_OK && gl == wl && memcmp(got, want, wl) == 0);
            CHECK(AL.n == 1 && strcmp(AL.line[0], "1234 CONFIG_REQUEST by=eng1 changes=1 base_serial=7\n") == 0);
            CHECK(AL.state_before[0] == 'N');                    /* the record was made BEFORE the request existed */
            spool_clean();
        } else {
            CHECK(res == HMI_CFGREQ_DENIED);
            CHECK(hmi_cfgreq_state(SPOOL, t, sizeof(t)) == HMI_CFGREQ_NONE);                       /* nothing was written */
            CHECK(AL.n == 1 && strcmp(AL.line[0], "1234 CONFIG_REQUEST_DENIED by=eng1\n") == 0);
        }
    }
    CHECK(hmi_cfgreq_submit(&r, (hmi_role_t)99, SPOOL, audit_cb, NULL, 1) == HMI_CFGREQ_DENIED);
    CHECK(hmi_cfgreq_state(SPOOL, t, sizeof(t)) == HMI_CFGREQ_NONE);

    /* nothing to ask for */
    hmi_cfgedit_request_t empty;
    hmi_cfgedit_init(&empty, 7, "eng1");
    CHECK(hmi_cfgreq_submit(&empty, HMI_ROLE_ADMIN, SPOOL, audit_cb, NULL, 1) == HMI_CFGREQ_EMPTY);
    CHECK(hmi_cfgreq_submit(NULL, HMI_ROLE_ADMIN, SPOOL, audit_cb, NULL, 1) == HMI_CFGREQ_EMPTY);

    /* no record, no request */
    memset(&AL, 0, sizeof(AL));
    AL.fail = true;
    CHECK(hmi_cfgreq_submit(&r, HMI_ROLE_ENGINEER, SPOOL, audit_cb, NULL, 2) == HMI_CFGREQ_AUDIT_FAILED);
    CHECK(hmi_cfgreq_state(SPOOL, t, sizeof(t)) == HMI_CFGREQ_NONE);
    AL.fail = false;

    /* without an audit trail the request is still handed over */
    CHECK(hmi_cfgreq_submit(&r, HMI_ROLE_ENGINEER, SPOOL, NULL, NULL, 3) == HMI_CFGREQ_OK);
    CHECK(hmi_cfgreq_state(SPOOL, t, sizeof(t)) == HMI_CFGREQ_PENDING);

    /* a second request waits for the first: the file is not touched */
    hmi_cfgedit_request_t other;
    hmi_cfgedit_init(&other, 9, "eng1");
    hmi_cfgedit_set_number(&other, HMI_CE_PLC_PORT, NULL, 1502);
    memset(&AL, 0, sizeof(AL));
    CHECK(hmi_cfgreq_submit(&other, HMI_ROLE_ENGINEER, SPOOL, audit_cb, NULL, 4) == HMI_CFGREQ_BUSY);
    CHECK(AL.n == 0);                                            /* and nothing was recorded for it */
    CHECK(hmi_fs_read_all(SPOOL "/request", got, sizeof(got), &gl) == HMI_FS_READ_OK && gl == wl && memcmp(got, want, wl) == 0);
    spool_clean();

    /* the spool directory cannot be written */
    memset(&AL, 0, sizeof(AL));
    CHECK(hmi_cfgreq_submit(&r, HMI_ROLE_ENGINEER, "no_such_spool_dir_p99", audit_cb, NULL, 5) == HMI_CFGREQ_WRITE_FAILED);
    CHECK(AL.n == 2 && strstr(AL.line[0], "CONFIG_REQUEST by=eng1") != NULL && strstr(AL.line[1], "CONFIG_REQUEST_FAILED by=eng1") != NULL);
    CHECK(hmi_cfgreq_submit(&r, HMI_ROLE_ENGINEER, NULL, NULL, NULL, 5) == HMI_CFGREQ_WRITE_FAILED);
    CHECK(hmi_cfgreq_submit(&r, HMI_ROLE_ENGINEER, "", NULL, NULL, 5) == HMI_CFGREQ_WRITE_FAILED);
    {
        char longdir[400];
        memset(longdir, 'd', sizeof(longdir) - 1);
        longdir[sizeof(longdir) - 1] = '\0';
        CHECK(hmi_cfgreq_submit(&r, HMI_ROLE_ENGINEER, longdir, NULL, NULL, 5) == HMI_CFGREQ_WRITE_FAILED);
    }
    CHECK(hmi_cfgreq_submit(&r, HMI_ROLE_ENGINEER, SPOOL "/", NULL, NULL, 6) == HMI_CFGREQ_OK);   /* a trailing slash is fine */
    CHECK(hmi_fs_read_all(SPOOL "/request", got, sizeof(got), &gl) == HMI_FS_READ_OK && gl == wl);
    spool_clean();
}

static void test_spool_state(void)
{
    SECTION("cfgedit: what the service shows about the last request");
    MKDIR(SPOOL);
    spool_clean();
    char text[64];
    CHECK(hmi_cfgreq_state(SPOOL, text, sizeof(text)) == HMI_CFGREQ_NONE && text[0] == '\0');
    CHECK(hmi_cfgreq_state("no_such_spool_dir_p99", text, sizeof(text)) == HMI_CFGREQ_NONE);
    CHECK(hmi_cfgreq_state(NULL, text, sizeof(text)) == HMI_CFGREQ_NONE);
    CHECK(hmi_cfgreq_state(SPOOL, NULL, 0) == HMI_CFGREQ_NONE);

    CHECK(hmi_fs_atomic_write(SPOOL "/result", "applied serial=8 changes=3 by=eng1\nmore\n", 39));
    CHECK(hmi_cfgreq_state(SPOOL, text, sizeof(text)) == HMI_CFGREQ_APPLIED && strcmp(text, "applied serial=8 changes=3 by=eng1") == 0);
    CHECK(hmi_fs_atomic_write(SPOOL "/result", "rejected: nothing would change\n", 31));
    CHECK(hmi_cfgreq_state(SPOOL, text, sizeof(text)) == HMI_CFGREQ_REJECTED && strcmp(text, "rejected: nothing would change") == 0);
    CHECK(hmi_fs_atomic_write(SPOOL "/result", "garbage", 7));
    CHECK(hmi_cfgreq_state(SPOOL, text, sizeof(text)) == HMI_CFGREQ_REJECTED && strcmp(text, "garbage") == 0);   /* anything but "applied" is not applied */
    CHECK(hmi_fs_atomic_write(SPOOL "/result", "applied\x01\x02 odd\n", 14));
    CHECK(hmi_cfgreq_state(SPOOL, text, sizeof(text)) == HMI_CFGREQ_APPLIED && strchr(text, 1) == NULL && strchr(text, 2) == NULL);
    char small[8];
    CHECK(hmi_cfgreq_state(SPOOL, small, sizeof(small)) == HMI_CFGREQ_APPLIED && strlen(small) == 7);          /* cut, never overrun */
    {   /* a result file that is far too long is cut */
        char lots[600];
        memset(lots, 'x', sizeof(lots));
        memcpy(lots, "applied ", 8);
        CHECK(hmi_fs_atomic_write(SPOOL "/result", lots, sizeof(lots)));
        CHECK(hmi_cfgreq_state(SPOOL, text, sizeof(text)) == HMI_CFGREQ_APPLIED && strlen(text) == sizeof(text) - 1);
    }
    CHECK(hmi_fs_atomic_write(SPOOL "/result", "", 0) || true);

    /* a pending request wins over an old result */
    CHECK(hmi_fs_atomic_write(SPOOL "/result", "applied serial=8\n", 17));
    CHECK(hmi_fs_atomic_write(SPOOL "/request", "x", 1));
    CHECK(hmi_cfgreq_state(SPOOL, text, sizeof(text)) == HMI_CFGREQ_PENDING && text[0] == '\0');
    spool_clean();
    CHECK(hmi_cfgreq_state(SPOOL, text, sizeof(text)) == HMI_CFGREQ_NONE);
}

int main(void)
{
    test_serialize();
    test_numbers_exact();
    test_equal_sees_every_field();
    test_big_configuration();
    test_request_format_and_parse();
    test_request_refusals();
    test_request_prefix_and_byte_fuzz();
    test_apply_allowed();
    test_apply_refusals();
    test_apply_example_and_big();
    test_spool_submit();
    test_spool_state();
    return test_finish();
}
