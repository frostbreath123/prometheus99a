/* Tests for hmi_config: parse, strict rejection, atomic save, shipped example.
 * Part of the HMI core host tests; shared fixtures are in test_support.h. */
#include "test_support.h"

static void expect_reject(const char *json, const char *why)
{
    static hmi_app_cfg_t c;
    char err[128];
    bool ok = parse(json, &c, err);
    if (ok) printf("  expected reject: %s\n", why);
    CHECK(!ok);
    CHECK(!c.writes_enabled && c.n_tags == 0 && c.host[0] == '\0'); /* nothing leaks from a failed parse */
}

static char *replace(const char *src, const char *a, const char *b)
{
    static char out[8][4096];
    static int k;
    char *o = out[k++ & 7];
    const char *p = strstr(src, a);
    if (!p) { strcpy(o, src); return o; }
    size_t head = (size_t)(p - src);
    memcpy(o, src, head);
    strcpy(o + head, b);
    strcpy(o + head + strlen(b), p + strlen(a));
    return o;
}

static void test_config(void)
{
    SECTION("config: valid parse and strict rejection");
    static hmi_app_cfg_t c;
    char err[128];
    CHECK(parse(GOOD_CFG, &c, err));
    CHECK(c.n_tags == 2 && c.port == 5020 && c.unit == 1 && strcmp(c.host, "127.0.0.1") == 0);
    CHECK(c.response_timeout_ms == 300 && c.connect_timeout_ms == 1000 && c.writes_enabled);
    CHECK(c.tags[0].write_allowed && c.tags[0].write_role == HMI_ROLE_OPERATOR && c.tags[0].alarm.en_hi);
    CHECK(c.tags[1].area == HMI_AREA_INPUT && c.tags[1].dtype == HMI_DT_F32 && c.tags[1].word_order == HMI_WORD_LOW_FIRST);

    expect_reject("", "empty");
    expect_reject("{", "truncated");
    expect_reject("[]", "not an object");
    expect_reject(replace(GOOD_CFG, "\"version\":1", "\"version\":2"), "wrong version");
    expect_reject(replace(GOOD_CFG, "\"version\":1,", ""), "no version");
    expect_reject(replace(GOOD_CFG, "127.0.0.1", "plc.example.com"), "hostname (no DNS)");
    expect_reject(replace(GOOD_CFG, "127.0.0.1", "127.0.0.256"), "octet range");
    expect_reject(replace(GOOD_CFG, "127.0.0.1", "127.0.0"), "short ipv4");
    expect_reject(replace(GOOD_CFG, "\"port\":5020", "\"port\":0"), "port 0");
    expect_reject(replace(GOOD_CFG, "\"port\":5020", "\"port\":70000"), "port range");
    expect_reject(replace(GOOD_CFG, "\"unit\":1", "\"unit\":300"), "unit range");
    expect_reject(replace(GOOD_CFG, "\"response_timeout_ms\":300", "\"response_timeout_ms\":1"), "timeout too small");
    expect_reject(replace(GOOD_CFG, "\"writes_enabled\":true", "\"writes_enabled\":\"yes\""), "bool type");
    expect_reject(replace(GOOD_CFG, "\"writes_enabled\":true,", "\"writes_enabled\":true,\"surprise\":1,"), "unknown key");
    expect_reject(replace(GOOD_CFG, "\"unit\":1", "\"unit\":1,\"unit\":2"), "duplicate key");
    /* A repeated master switch must never be "last one wins": false then true
     * would silently enable writes. */
    expect_reject(replace(GOOD_CFG, "\"writes_enabled\":true,",
                          "\"writes_enabled\":false,\"writes_enabled\":true,"), "duplicate writes_enabled false,true");
    expect_reject(replace(GOOD_CFG, "\"writes_enabled\":true,",
                          "\"writes_enabled\":true,\"writes_enabled\":false,"), "duplicate writes_enabled true,false");
    expect_reject(replace(GOOD_CFG, "\"scale\":0.1", "\"scale\":0.1,\"colour\":\"red\""), "unknown tag key");
    expect_reject(replace(GOOD_CFG, "\"deadband\":2", "\"deadband\":2,\"hyst\":1"), "unknown alarm key");
    expect_reject(replace(GOOD_CFG, "\"allowed\":true", "\"allowed\":true,\"force\":true"), "unknown write key");
    expect_reject(replace(GOOD_CFG, "\"unit\":1", "\"unit\":1,\"tls\":true"), "unknown plc key");
    expect_reject(replace(GOOD_CFG, "\"address\":0", "\"address\":-1"), "negative address");
    expect_reject(replace(GOOD_CFG, "\"type\":\"f32\"", "\"type\":\"f64\""), "unknown type");
    expect_reject(replace(GOOD_CFG, "\"name\":\"TEMP\"", "\"name\":\"LVL\""), "duplicate tag name");
    expect_reject(replace(GOOD_CFG, "\"address\":10,\"type\":\"f32\"", "\"address\":65535,\"type\":\"f32\""), "address overflow");
    expect_reject(replace(GOOD_CFG, "\"role\":\"operator\"", "\"role\":\"root\""), "unknown role");
    expect_reject(replace(GOOD_CFG, "\"role\":\"operator\"", "\"role\":\"viewer\""), "viewer cannot write");
    expect_reject(replace(GOOD_CFG, "\"min\":0,\"max\":100}}", "\"min\":0,\"max\":900}}"), "write range outside eng range");
    expect_reject(replace(GOOD_CFG, "\"role\":\"operator\",", ""), "writable without role");
    expect_reject(replace(GOOD_CFG, "\"high\":80", "\"high\":99"), "high above high_high");
    expect_reject(replace(GOOD_CFG, "\"deadband\":2", "\"deadband\":50"), "deadband bridges limits");
    expect_reject(replace(GOOD_CFG, "\"scale\":0.1", "\"scale\":0"), "zero scale");
    expect_reject(replace(GOOD_CFG, "\"poll_ms\":200", "\"poll_ms\":5"), "poll too fast");
    expect_reject(replace(GOOD_CFG, "\"area\":\"input\"", "\"area\":\"coil\""), "unknown area");
    {
        /* trailing garbage after the root object */
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s ,{}", GOOD_CFG);
        expect_reject(buf, "trailing data");
    }
    {
        /* writable input register */
        expect_reject(replace(GOOD_CFG,
            "{\"name\":\"TEMP\",\"area\":\"input\",\"address\":10,\"type\":\"f32\",\"word_order\":\"low_first\"}",
            "{\"name\":\"TEMP\",\"area\":\"input\",\"address\":10,\"type\":\"u16\","
            "\"write\":{\"allowed\":true,\"role\":\"admin\",\"min\":0,\"max\":1}}"),
            "writable input register");
    }
    {
        static char big[HMI_CONFIG_MAX_BYTES + 10];
        memset(big, ' ', sizeof(big) - 1);
        memcpy(big, GOOD_CFG, strlen(GOOD_CFG));
        big[sizeof(big) - 1] = '\0';
        static hmi_app_cfg_t cc;
        CHECK(!hmi_config_parse(big, strlen(big), &cc, err, sizeof(err)));
    }
}

static void test_config_files(void)
{
    SECTION("config: file load, atomic save, invalid never replaces good");
    static hmi_app_cfg_t c;
    char err[128];
    const char *path = "cfg_test.json";
    remove(path);
    CHECK(!hmi_config_load_file(path, &c, err, sizeof(err))); /* missing -> no defaults */
    CHECK(c.n_tags == 0 && !c.writes_enabled && c.host[0] == '\0');
    CHECK(hmi_config_save_text(path, GOOD_CFG, strlen(GOOD_CFG), err, sizeof(err)));
    CHECK(hmi_config_load_file(path, &c, err, sizeof(err)) && c.n_tags == 2);
    const char *bad = "{\"version\":1}";
    CHECK(!hmi_config_save_text(path, bad, strlen(bad), err, sizeof(err)));
    CHECK(hmi_config_load_file(path, &c, err, sizeof(err)) && c.n_tags == 2); /* old good file intact */
    /* truncated file on disk is rejected */
    FILE *fp = fopen(path, "wb");
    fwrite(GOOD_CFG, 1, strlen(GOOD_CFG) / 2, fp);
    fclose(fp);
    CHECK(!hmi_config_load_file(path, &c, err, sizeof(err)));
    CHECK(c.n_tags == 0 && !c.writes_enabled);
    remove(path);
}

static void test_example_config(void)
{
    SECTION("config: shipped example file is valid and writes are off");
    static hmi_app_cfg_t c;
    char err[128];
    bool ok = hmi_config_load_file("config/hmi.example.json", &c, err, sizeof(err));
    if (!ok) printf("  example config: %s\n", err);
    CHECK(ok);
    CHECK(!c.writes_enabled); /* shipped default must not enable writes */
    CHECK(c.n_tags == 4 && c.tags[2].dtype == HMI_DT_U32 && c.tags[3].dtype == HMI_DT_F32);
}

/* GOOD_CFG with an "allowed_hosts" member inserted into the plc object. */
static void with_allowed(const char *member, char *out, size_t cap)
{
    const char *anchor = "\"unit\":1,";
    const char *at = strstr(GOOD_CFG, anchor) + strlen(anchor);
    snprintf(out, cap, "%.*s%s%s", (int)(at - GOOD_CFG), GOOD_CFG, member, at);
}

static void test_allowed_hosts(void)
{
    SECTION("config: plc.allowed_hosts");
    char json[2048], err[128];
    hmi_app_cfg_t c;

    CHECK(parse(GOOD_CFG, &c, err) && c.n_allowed == 0);               /* optional for the parser */
    CHECK(!hmi_config_host_allowed(&c));                                /* but an empty list allows nothing */

    with_allowed("\"allowed_hosts\":[\"127.0.0.1\"],", json, sizeof(json));
    CHECK(parse(json, &c, err) && c.n_allowed == 1 && strcmp(c.allowed_hosts[0], "127.0.0.1") == 0);
    CHECK(hmi_config_host_allowed(&c));

    with_allowed("\"allowed_hosts\":[\"10.0.0.5\",\"127.0.0.1\",\"192.168.1.20\"],", json, sizeof(json));
    CHECK(parse(json, &c, err) && c.n_allowed == 3 && hmi_config_host_allowed(&c)); /* host is the second entry */

    with_allowed("\"allowed_hosts\":[\"10.0.0.5\"],", json, sizeof(json));
    CHECK(parse(json, &c, err) && c.n_allowed == 1);
    CHECK(!hmi_config_host_allowed(&c));                                /* host 127.0.0.1 is not on the list */
    CHECK(!hmi_config_host_allowed(NULL));

    /* a prefix or look-alike is not a match */
    with_allowed("\"allowed_hosts\":[\"127.0.0.10\",\"27.0.0.1\"],", json, sizeof(json));
    CHECK(parse(json, &c, err) && !hmi_config_host_allowed(&c));

    /* eight entries are the limit */
    with_allowed("\"allowed_hosts\":[\"10.0.0.1\",\"10.0.0.2\",\"10.0.0.3\",\"10.0.0.4\","
                 "\"10.0.0.5\",\"10.0.0.6\",\"10.0.0.7\",\"127.0.0.1\"],", json, sizeof(json));
    CHECK(parse(json, &c, err) && c.n_allowed == 8 && hmi_config_host_allowed(&c));
    with_allowed("\"allowed_hosts\":[\"10.0.0.1\",\"10.0.0.2\",\"10.0.0.3\",\"10.0.0.4\","
                 "\"10.0.0.5\",\"10.0.0.6\",\"10.0.0.7\",\"10.0.0.8\",\"127.0.0.1\"],", json, sizeof(json));
    CHECK(!parse(json, &c, err) && strstr(err, "allowed_hosts count") && c.n_allowed == 0);

    const char *bad[] = {
        "\"allowed_hosts\":[],",                                        /* empty */
        "\"allowed_hosts\":\"127.0.0.1\",",                             /* not an array */
        "\"allowed_hosts\":[127],",                                     /* not a string */
        "\"allowed_hosts\":[\"plc.local\"],",                           /* DNS name */
        "\"allowed_hosts\":[\"127.0.0\"],",
        "\"allowed_hosts\":[\"127.0.0.256\"],",
        "\"allowed_hosts\":[\"127.0.0.1\",\"127.0.0.1\"],",             /* duplicate */
        "\"allowed_hosts\":[\"\"],",
        "\"allowed_hosts\":[\"127.0.0.1\"],\"allowed_hosts\":[\"127.0.0.1\"],", /* repeated key */
        "\"allowed_hosts\":[[\"127.0.0.1\"]],",
        "\"allowed_hosts\":null,",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        with_allowed(bad[i], json, sizeof(json));
        bool ok = parse(json, &c, err);
        if (ok) printf("  accepted: %s\n", bad[i]);
        CHECK(!ok);
        CHECK(c.n_allowed == 0 && c.host[0] == '\0');                   /* a rejection leaves nothing behind */
    }
}

int main(void)
{
    test_allowed_hosts();
    test_config();
    test_config_files();
    test_example_config();
    return test_finish();
}
