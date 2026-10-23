/* Tests for hmi_cfgsec: signature format, every rejection class, tamper
 * sweeps over every byte, rollback, allow-list, and "nothing is parsed before
 * the MAC succeeds". Files live in the working directory. */
#include "test_support.h"
#include "../../src/hmi/hmi_cfgsec.h"
#include "../../src/hmi/hmi_util.h"
#include <stdio.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#define CFG "cs_cfg.json"
#define SIG "cs_cfg.json.sig"
#define KEY "cs_key.bin"
#define STATE "cs_state.bin"

static uint8_t KEYB[32];
static char GOODTXT[2048];

static void clean(void)
{
    remove(CFG);
    remove(SIG);
    remove(KEY);
    remove(STATE);
    remove(CFG ".tmp");
    remove(SIG ".tmp");
    remove(STATE ".tmp");
}

static void put_file(const char *p, const void *d, size_t n)
{
    if (!hmi_fs_atomic_write(p, d, n)) {
        printf("  cannot write %s\n", p);
        exit(3);
    }
}

/* GOOD_CFG with allowed_hosts inserted into the plc object. */
static void make_good(const char *allowed_member)
{
    const char *anchor = "\"unit\":1,";
    const char *at = strstr(GOOD_CFG, anchor) + strlen(anchor);
    snprintf(GOODTXT, sizeof(GOODTXT), "%.*s%s%s", (int)(at - GOOD_CFG), GOOD_CFG, allowed_member, at);
}

/* Write config, key and a matching signature with the given serial. */
static void setup_signed(const char *text, uint32_t serial)
{
    uint8_t sig[HMI_CFGSEC_SIG_SIZE];
    put_file(KEY, KEYB, sizeof(KEYB));
    put_file(CFG, text, strlen(text));
    hmi_cfgsec_make_sig(KEYB, serial, (const uint8_t *)text, strlen(text), sig);
    put_file(SIG, sig, sizeof(sig));
}

static hmi_cfgsec_paths_t paths(bool with_state)
{
    hmi_cfgsec_paths_t p;
    p.config_path = CFG;
    p.sig_path = NULL;
    p.key_path = KEY;
    p.state_path = with_state ? STATE : NULL;
    return p;
}

static hmi_cfgsec_result_t load(hmi_app_cfg_t *c, hmi_cfgsec_info_t *i, bool with_state)
{
    hmi_cfgsec_paths_t p = paths(with_state);
    return hmi_cfgsec_load(&p, c, i);
}

static bool zeroed(const hmi_app_cfg_t *c)
{
    return c->n_tags == 0 && !c->writes_enabled && c->host[0] == '\0' && c->n_allowed == 0;
}

static size_t read_all(const char *p, uint8_t *b, size_t cap)
{
    size_t n = 0;
    if (hmi_fs_read_all(p, b, cap, &n) != HMI_FS_READ_OK) return 0;
    return n;
}

static void test_blocks(void)
{
    SECTION("cfgsec: signature layout and state file");
    uint8_t sig[HMI_CFGSEC_SIG_SIZE];
    const char *t = "hello";
    hmi_cfgsec_make_sig(KEYB, 7, (const uint8_t *)t, 5, sig);
    CHECK(memcmp(sig, "P99C", 4) == 0 && sig[4] == 1 && sig[8] == 7 && sig[12] == 5);
    uint32_t serial, len, crc;
    CHECK(hmi_cfgsec_parse_sig(sig, sizeof(sig), &serial, &len, &crc) && serial == 7 && len == 5);
    CHECK(crc == hmi_crc32((const uint8_t *)t, 5));
    CHECK(!hmi_cfgsec_parse_sig(sig, sizeof(sig) - 1, NULL, NULL, NULL));
    CHECK(!hmi_cfgsec_parse_sig(NULL, sizeof(sig), NULL, NULL, NULL));
    for (size_t off = 0; off < 24; off++) { /* every header byte that must be zero or fixed */
        uint8_t c[HMI_CFGSEC_SIG_SIZE];
        memcpy(c, sig, sizeof(c));
        c[off] ^= 0x40;
        if (off == 5 || off == 6 || off == 7 || (off >= 20 && off < 24) || off < 5)
            CHECK(!hmi_cfgsec_parse_sig(c, sizeof(c), NULL, NULL, NULL));
        else
            CHECK(hmi_cfgsec_parse_sig(c, sizeof(c), NULL, NULL, NULL)); /* serial, length, crc carry data */
    }
    uint8_t other[HMI_CFGSEC_SIG_SIZE];
    uint8_t key2[32];
    memcpy(key2, KEYB, 32);
    key2[0] ^= 1;
    hmi_cfgsec_make_sig(key2, 7, (const uint8_t *)t, 5, other);
    CHECK(memcmp(sig + 24, other + 24, 64) != 0);                 /* the key is in the MAC */
    hmi_cfgsec_make_sig(KEYB, 8, (const uint8_t *)t, 5, other);
    CHECK(memcmp(sig + 24, other + 24, 64) != 0);                 /* so is the serial */

    remove(STATE);
    uint32_t s = 99;
    bool dmg = true;
    CHECK(!hmi_cfgsec_state_load(STATE, &s, &dmg) && s == 0 && !dmg);   /* absent */
    CHECK(hmi_cfgsec_state_save(STATE, 41));
    CHECK(hmi_cfgsec_state_load(STATE, &s, &dmg) && s == 41 && !dmg);
    uint8_t raw[32];
    size_t n = read_all(STATE, raw, sizeof(raw));
    CHECK(n == 16);
    for (size_t i = 0; i < n; i++) {                              /* any damaged byte is detected */
        uint8_t c[16];
        memcpy(c, raw, 16);
        c[i] ^= 1;
        put_file(STATE, c, 16);
        CHECK(!hmi_cfgsec_state_load(STATE, &s, &dmg) && dmg && s == 0);
    }
    put_file(STATE, raw, 15);
    CHECK(!hmi_cfgsec_state_load(STATE, &s, &dmg) && dmg);
    remove(STATE);
}

static void test_accept(void)
{
    SECTION("cfgsec: a correctly signed configuration is accepted");
    clean();
    make_good("\"allowed_hosts\":[\"10.0.0.9\",\"127.0.0.1\"],");
    setup_signed(GOODTXT, 5);
    hmi_app_cfg_t c;
    hmi_cfgsec_info_t i;
    CHECK(load(&c, &i, true) == HMI_CFGSEC_OK);
    CHECK(i.result == HMI_CFGSEC_OK && i.serial == 5 && i.serial_recorded && !i.state_damaged);
    CHECK(c.n_tags == 2 && c.writes_enabled && strcmp(c.host, "127.0.0.1") == 0 && c.n_allowed == 2);
    uint32_t st;
    CHECK(hmi_cfgsec_state_load(STATE, &st, NULL) && st == 5);

    CHECK(load(&c, &i, true) == HMI_CFGSEC_OK && i.serial_recorded);   /* the same serial again is fine */
    CHECK(load(&c, NULL, false) == HMI_CFGSEC_OK);                      /* info and state are optional */

    hmi_cfgsec_paths_t p = paths(true);
    p.sig_path = "cs_other.sig";                                        /* an explicit signature path */
    uint8_t sig[HMI_CFGSEC_SIG_SIZE];
    hmi_cfgsec_make_sig(KEYB, 5, (const uint8_t *)GOODTXT, strlen(GOODTXT), sig);
    put_file("cs_other.sig", sig, sizeof(sig));
    CHECK(hmi_cfgsec_load(&p, &c, &i) == HMI_CFGSEC_OK);
    remove("cs_other.sig");

    char line[100];
    CHECK(hmi_cfgsec_format_audit(1234, &i, line, sizeof(line)) > 0 && strcmp(line, "1234 CONFIG_ACCEPTED serial=5\n") == 0);
    clean();
}

static void test_files_missing_or_bad(void)
{
    SECTION("cfgsec: missing, damaged and badly protected files");
    hmi_app_cfg_t c;
    hmi_cfgsec_info_t i;
    make_good("\"allowed_hosts\":[\"127.0.0.1\"],");

    clean();
    CHECK(load(&c, &i, false) == HMI_CFGSEC_KEY_MISSING && zeroed(&c));
    put_file(KEY, KEYB, 31);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_KEY_FORMAT);
    uint8_t k33[33];
    memset(k33, 7, sizeof(k33));
    put_file(KEY, k33, sizeof(k33));
    CHECK(load(&c, &i, false) == HMI_CFGSEC_KEY_FORMAT);
    put_file(KEY, KEYB, 32);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_CONFIG_UNREADABLE);
    put_file(CFG, GOODTXT, strlen(GOODTXT));
    CHECK(load(&c, &i, false) == HMI_CFGSEC_SIG_MISSING && strstr(i.message, "not signed"));
    CHECK(zeroed(&c));

    /* too big */
    static char big[HMI_CONFIG_MAX_BYTES + 100];
    memset(big, ' ', sizeof(big));
    put_file(CFG, big, sizeof(big));
    CHECK(load(&c, &i, false) == HMI_CFGSEC_CONFIG_TOO_BIG);

    /* signature format */
    uint8_t sig[HMI_CFGSEC_SIG_SIZE];
    setup_signed(GOODTXT, 1);
    size_t n = read_all(SIG, sig, sizeof(sig));
    CHECK(n == HMI_CFGSEC_SIG_SIZE);
    put_file(SIG, sig, n - 1);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_SIG_FORMAT);
    uint8_t longer[HMI_CFGSEC_SIG_SIZE + 1];
    memcpy(longer, sig, n);
    longer[n] = 0;
    put_file(SIG, longer, n + 1);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_SIG_FORMAT);
    put_file(SIG, "", 0);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_SIG_FORMAT);
    uint8_t bad[HMI_CFGSEC_SIG_SIZE];
    memcpy(bad, sig, n);
    bad[0] = 'X';
    put_file(SIG, bad, n);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_SIG_FORMAT);
    memcpy(bad, sig, n);
    bad[4] = 2;
    put_file(SIG, bad, n);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_SIG_FORMAT);
    put_file(SIG, sig, n);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_OK);                         /* and the good one still works */

#ifndef _WIN32
    /* file modes */
    CHECK(chmod(KEY, 0644) == 0);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_KEY_MODE && zeroed(&c));     /* world readable key */
    CHECK(chmod(KEY, 0620) == 0);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_KEY_MODE);                   /* group writable key */
    CHECK(chmod(KEY, 0640) == 0);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_OK);                         /* root:service 0640 is the intended shape */
    CHECK(chmod(KEY, 0440) == 0);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_OK);
    CHECK(chmod(KEY, 0600) == 0);
    CHECK(chmod(CFG, 0664) == 0);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_CONFIG_MODE && zeroed(&c));  /* group writable configuration */
    CHECK(chmod(CFG, 0646) == 0);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_CONFIG_MODE);
    CHECK(chmod(CFG, 0644) == 0);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_OK);                         /* readable by others is fine */
    CHECK(chmod(SIG, 0666) == 0);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_CONFIG_MODE && strstr(i.message, "signature"));
    CHECK(chmod(SIG, 0600) == 0);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_OK);
    CHECK(i.config_writable_by_service);                                  /* the test owns the files: flagged */
#endif
    clean();
}

static void test_tamper(void)
{
    SECTION("cfgsec: tampering is always rejected, and nothing unsigned is parsed");
    hmi_app_cfg_t c;
    hmi_cfgsec_info_t i;
    make_good("\"allowed_hosts\":[\"127.0.0.1\"],");
    clean();
    setup_signed(GOODTXT, 3);
    size_t len = strlen(GOODTXT);
    uint8_t sig[HMI_CFGSEC_SIG_SIZE];
    size_t sn = read_all(SIG, sig, sizeof(sig));

    /* every byte of the configuration, flipped in turn */
    int rejected = 0, corrupt = 0;
    for (size_t k = 0; k < len; k++) {
        char t[2048];
        memcpy(t, GOODTXT, len);
        t[k] ^= 0x01;
        put_file(CFG, t, len);
        hmi_cfgsec_result_t r = load(&c, &i, false);
        if (r != HMI_CFGSEC_OK && zeroed(&c)) rejected++;
        if (r == HMI_CFGSEC_CONFIG_CORRUPT) corrupt++;
    }
    CHECK(rejected == (int)len);
    CHECK(corrupt == (int)len);                    /* a single flipped bit is caught by the CRC as damage */

    /* an attacker who knows the format re-computes length and CRC but has no key */
    for (size_t k = 0; k < len; k += 7) {
        char t[2048];
        memcpy(t, GOODTXT, len);
        t[k] ^= 0x01;
        uint8_t forged[HMI_CFGSEC_SIG_SIZE];
        memcpy(forged, sig, sn);
        uint32_t crc = hmi_crc32((const uint8_t *)t, len);
        memcpy(forged + 16, &crc, 4); /* little endian host in every supported build */
        put_file(CFG, t, len);
        put_file(SIG, forged, sn);
        CHECK(load(&c, &i, false) == HMI_CFGSEC_MAC_MISMATCH && zeroed(&c));
    }
    put_file(SIG, sig, sn);

    /* the key the device holds is not the key that signed */
    {
        uint8_t other[32];
        memset(other, 0x5A, sizeof(other));
        put_file(CFG, GOODTXT, len);
        put_file(KEY, other, sizeof(other));
        CHECK(load(&c, &i, false) == HMI_CFGSEC_MAC_MISMATCH && zeroed(&c));
        put_file(KEY, KEYB, sizeof(KEYB));
    }

    /* every byte of the signature file, flipped in turn: never accepted */
    int sig_rejected = 0;
    for (size_t k = 0; k < sn; k++) {
        uint8_t b[HMI_CFGSEC_SIG_SIZE];
        memcpy(b, sig, sn);
        b[k] ^= 0x01;
        put_file(SIG, b, sn);
        if (load(&c, &i, false) != HMI_CFGSEC_OK && zeroed(&c)) sig_rejected++;
    }
    CHECK(sig_rejected == (int)sn);
    put_file(SIG, sig, sn);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_OK);

    /* truncation and extension of the configuration */
    put_file(CFG, GOODTXT, len - 1);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_CONFIG_CORRUPT);
    char ext[2100];
    memcpy(ext, GOODTXT, len);
    ext[len] = ' ';
    put_file(CFG, ext, len + 1);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_CONFIG_CORRUPT);
    put_file(CFG, GOODTXT, len);

    /* THE security property: turning the write master switch on in the text
     * is a change like any other, and the device then refuses to run at all.
     * The tampered text has the same length as the signed one and is still
     * valid JSON, so only the MAC can catch it. */
    {
        const char *needle = "\"writes_enabled\":true";
        const char *off_txt = "\"writes_enabled\":false";
        const char *on_txt = "\"writes_enabled\":true ";
        const char *at = strstr(GOODTXT, needle);
        CHECK(at != NULL);
        char t[2048];
        snprintf(t, sizeof(t), "%.*s%s%s", (int)(at - GOODTXT), GOODTXT, off_txt, at + strlen(needle));
        setup_signed(t, 4);
        CHECK(load(&c, &i, false) == HMI_CFGSEC_OK && !c.writes_enabled);
        char *at2 = strstr(t, off_txt);
        CHECK(at2 != NULL && strlen(on_txt) == strlen(off_txt));
        memcpy(at2, on_txt, strlen(on_txt));
        put_file(CFG, t, strlen(t));
        hmi_cfgsec_result_t r = load(&c, &i, false);
        CHECK(r != HMI_CFGSEC_OK && !c.writes_enabled && zeroed(&c));
        /* and it is the CRC that notices an edit made without the key's help */
        CHECK(r == HMI_CFGSEC_CONFIG_CORRUPT);
    }

    /* Nothing unauthenticated reaches the parser: garbage that is signed with the
     * wrong key, and garbage with no signature at all, are reported as
     * signature problems, never as configuration syntax problems. */
    {
        const char *junk = "{ this is not json at all";
        uint8_t other[32], sg[HMI_CFGSEC_SIG_SIZE];
        memset(other, 0x33, sizeof(other));
        put_file(CFG, junk, strlen(junk));
        hmi_cfgsec_make_sig(other, 1, (const uint8_t *)junk, strlen(junk), sg);
        put_file(SIG, sg, sizeof(sg));
        CHECK(load(&c, &i, false) == HMI_CFGSEC_MAC_MISMATCH);
        remove(SIG);
        CHECK(load(&c, &i, false) == HMI_CFGSEC_SIG_MISSING);
    }
    clean();
}

static void test_rollback(void)
{
    SECTION("cfgsec: an older signed configuration cannot be put back");
    hmi_app_cfg_t c;
    hmi_cfgsec_info_t i;
    clean();
    make_good("\"allowed_hosts\":[\"127.0.0.1\"],");
    setup_signed(GOODTXT, 5);
    CHECK(load(&c, &i, true) == HMI_CFGSEC_OK);
    uint32_t st = 0;
    CHECK(hmi_cfgsec_state_load(STATE, &st, NULL) && st == 5);

    setup_signed(GOODTXT, 4);                                   /* genuinely signed, but older */
    CHECK(load(&c, &i, true) == HMI_CFGSEC_ROLLBACK && zeroed(&c));
    CHECK(strstr(i.message, "older") != NULL);
    CHECK(hmi_cfgsec_state_load(STATE, &st, NULL) && st == 5);  /* a refused one changes nothing */
    CHECK(load(&c, &i, false) == HMI_CFGSEC_OK);                /* with rollback protection switched off it loads */

    setup_signed(GOODTXT, 5);
    CHECK(load(&c, &i, true) == HMI_CFGSEC_OK);                 /* equal is fine (a restart) */
    setup_signed(GOODTXT, 9);
    CHECK(load(&c, &i, true) == HMI_CFGSEC_OK && i.serial_recorded);
    CHECK(hmi_cfgsec_state_load(STATE, &st, NULL) && st == 9);
    setup_signed(GOODTXT, 8);
    CHECK(load(&c, &i, true) == HMI_CFGSEC_ROLLBACK);

    /* a damaged state file is reported and treated as absent; a missing one too */
    put_file(STATE, "garbage-garbage!", 16);
    setup_signed(GOODTXT, 2);
    CHECK(load(&c, &i, true) == HMI_CFGSEC_OK && i.state_damaged && i.serial_recorded);
    CHECK(hmi_cfgsec_state_load(STATE, &st, NULL) && st == 2);
    remove(STATE);
    CHECK(load(&c, &i, true) == HMI_CFGSEC_OK && !i.state_damaged);

    /* serial zero and the largest serial are both just numbers */
    setup_signed(GOODTXT, 0);
    remove(STATE);
    CHECK(load(&c, &i, true) == HMI_CFGSEC_OK && i.serial == 0);
    setup_signed(GOODTXT, 0xFFFFFFFFu);
    CHECK(load(&c, &i, true) == HMI_CFGSEC_OK && i.serial == 0xFFFFFFFFu);

    /* an unwritable state path does not block a verified configuration, and says so */
    setup_signed(GOODTXT, 7);
    hmi_cfgsec_paths_t p = paths(true);
    p.state_path = "no_such_dir_p99/state.bin";
    CHECK(hmi_cfgsec_load(&p, &c, &i) == HMI_CFGSEC_OK && !i.serial_recorded);
    clean();
}

static void test_allow_list(void)
{
    SECTION("cfgsec: allow-list and strict parser apply to the signed text");
    hmi_app_cfg_t c;
    hmi_cfgsec_info_t i;
    clean();

    setup_signed(GOOD_CFG, 1);                                    /* signed, but no allowed_hosts */
    CHECK(load(&c, &i, false) == HMI_CFGSEC_HOST_NOT_ALLOWED && zeroed(&c));
    CHECK(strstr(i.message, "needs plc.allowed_hosts") != NULL);

    make_good("\"allowed_hosts\":[\"10.1.1.1\"],");               /* host 127.0.0.1 is not listed */
    setup_signed(GOODTXT, 1);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_HOST_NOT_ALLOWED && zeroed(&c));
    CHECK(strstr(i.message, "not in plc.allowed_hosts") != NULL);

    setup_signed("{\"version\":1}", 1);                            /* signed but fails the schema */
    CHECK(load(&c, &i, false) == HMI_CFGSEC_CONFIG_INVALID && zeroed(&c));
    CHECK(strstr(i.message, "invalid") != NULL);
    setup_signed("{ not json", 1);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_CONFIG_INVALID);

    make_good("\"allowed_hosts\":[\"127.0.0.1\"],");
    setup_signed(GOODTXT, 1);
    CHECK(load(&c, &i, false) == HMI_CFGSEC_OK);

    /* reason codes and the audit line */
    CHECK(strcmp(hmi_cfgsec_reason_code(HMI_CFGSEC_MAC_MISMATCH), "signature_mismatch") == 0);
    CHECK(strcmp(hmi_cfgsec_reason_code(HMI_CFGSEC_ROLLBACK), "rollback") == 0);
    CHECK(strcmp(hmi_cfgsec_reason_code((hmi_cfgsec_result_t)99), "unknown") == 0);
    for (int r = 0; r <= HMI_CFGSEC_HOST_NOT_ALLOWED; r++) CHECK(hmi_cfgsec_reason_code((hmi_cfgsec_result_t)r)[0] != '\0');
    hmi_cfgsec_info_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.result = HMI_CFGSEC_ROLLBACK;
    char line[100];
    CHECK(hmi_cfgsec_format_audit(9, &bad, line, sizeof(line)) > 0 && strcmp(line, "9 CONFIG_REJECTED reason=rollback\n") == 0);
    CHECK(hmi_cfgsec_format_audit(9, &bad, line, 10) == 0);
    CHECK(hmi_cfgsec_format_audit(9, NULL, line, sizeof(line)) == 0);

    /* missing arguments */
    hmi_cfgsec_paths_t p;
    memset(&p, 0, sizeof(p));
    CHECK(hmi_cfgsec_load(&p, &c, &i) != HMI_CFGSEC_OK);
    CHECK(hmi_cfgsec_load(NULL, &c, &i) != HMI_CFGSEC_OK);
    clean();
}

int main(void)
{
    for (int k = 0; k < 32; k++) KEYB[k] = (uint8_t)(k * 11 + 3);
    test_blocks();
    test_accept();
    test_files_missing_or_bad();
    test_tamper();
    test_rollback();
    test_allow_list();
    clean();
    return test_finish();
}
