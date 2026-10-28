/* Tests for hmi_boot: the start-up decision (use the signed configuration, or safe mode with a banner), tested through
 * its one interface with real files and no process, display or PLC. Files live in the working directory. */
#include "test_support.h"
#include "../../src/hmi/hmi_boot.h"
#include "../../src/hmi/hmi_cfgsec.h"
#include "../../src/hmi/hmi_util.h"
#include <stdio.h>

#define CFG "bt_cfg.json"
#define SIG "bt_cfg.json.sig"
#define KEY "bt_key.bin"
#define STATE "bt_state.bin"
#define AUDIT "bt_audit.log"

static uint8_t KEYB[32];
static char TEXT[2048];

static void clean(void)
{
    remove(CFG);
    remove(SIG);
    remove(KEY);
    remove(STATE);
    remove(AUDIT);
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

/* Config, key and a matching signature with the given serial; plc.host 127.0.0.1 is on the allow-list. */
static void setup_signed(uint32_t serial)
{
    const char *anchor = "\"unit\":1,";
    const char *at = strstr(GOOD_CFG, anchor) + strlen(anchor);
    snprintf(TEXT, sizeof(TEXT), "%.*s\"allowed_hosts\":[\"127.0.0.1\"],%s", (int)(at - GOOD_CFG), GOOD_CFG, at);
    uint8_t sig[HMI_CFGSEC_SIG_SIZE];
    put_file(KEY, KEYB, sizeof(KEYB));
    put_file(CFG, TEXT, strlen(TEXT));
    hmi_cfgsec_make_sig(KEYB, serial, (const uint8_t *)TEXT, strlen(TEXT), sig);
    put_file(SIG, sig, sizeof(sig));
}

static hmi_cli_t cli_for(bool with_audit)
{
    hmi_cli_t c;
    memset(&c, 0, sizeof(c));
    c.config = CFG;
    c.config_key = KEY;
    c.config_state = STATE;
    c.audit = with_audit ? AUDIT : NULL;
    c.role = HMI_ROLE_VIEWER;
    return c;
}

static bool audit_has(const char *text)
{
    char buf[1024];
    size_t n = 0;
    if (hmi_fs_read_all(AUDIT, buf, sizeof(buf) - 1u, &n) != HMI_FS_READ_OK) return false;
    buf[n] = '\0';
    return strstr(buf, text) != NULL;
}

static bool safe_mode(const hmi_app_cfg_t *cfg, const hmi_boot_t *b)
{
    return !b->ok && b->serial == 0 && strncmp(b->banner, "CONFIG REJECTED: ", 17) == 0 && cfg->n_tags == 0 &&
           cfg->host[0] == '\0' && !cfg->writes_enabled;
}

static void test_accepted(void)
{
    SECTION("boot: a signed configuration is used");
    clean();
    setup_signed(3);
    hmi_cli_t cli = cli_for(true);
    hmi_app_cfg_t cfg;
    hmi_boot_t b;
    hmi_boot_config(&cli, &cfg, &b);
    CHECK(b.ok && b.serial == 3 && b.banner[0] == '\0');
    CHECK(strcmp(b.info, "Configuration: signed, serial 3") == 0);
    CHECK(cfg.n_tags == 2 && strcmp(cfg.host, "127.0.0.1") == 0);
    CHECK(audit_has("CONFIG_ACCEPTED serial=3"));
    clean();
}

static void test_rejections(void)
{
    SECTION("boot: every failure is safe mode with a banner and a record");
    hmi_app_cfg_t cfg;
    hmi_boot_t b;
    hmi_cli_t cli = cli_for(true);

    clean();
    setup_signed(1);
    remove(SIG);
    hmi_boot_config(&cli, &cfg, &b);
    CHECK(safe_mode(&cfg, &b) && audit_has("CONFIG_REJECTED"));
    CHECK(strcmp(b.info, "Configuration: not loaded") == 0);

    clean();
    setup_signed(1);
    TEXT[10] ^= 1;
    put_file(CFG, TEXT, strlen(TEXT));                 /* one flipped bit after signing */
    hmi_boot_config(&cli, &cfg, &b);
    CHECK(safe_mode(&cfg, &b) && audit_has("CONFIG_REJECTED"));

    clean();
    setup_signed(4);
    hmi_boot_config(&cli, &cfg, &b);                   /* serial 4 is now the highest this device has accepted */
    CHECK(b.ok && b.serial == 4);
    setup_signed(2);                                   /* an older signed configuration put back */
    hmi_boot_config(&cli, &cfg, &b);
    CHECK(safe_mode(&cfg, &b) && audit_has("reason=rollback"));

    clean();
    setup_signed(1);
    remove(KEY);
    hmi_boot_config(&cli, &cfg, &b);
    CHECK(safe_mode(&cfg, &b) && audit_has("reason=key_missing"));

    clean();
    hmi_boot_config(&cli, &cfg, &b);                   /* nothing exists: never a default PLC */
    CHECK(safe_mode(&cfg, &b));
    clean();
}

static void test_no_audit_file(void)
{
    SECTION("boot: without an audit file the decision is the same and nothing is written");
    clean();
    setup_signed(1);
    hmi_cli_t cli = cli_for(false);
    hmi_app_cfg_t cfg;
    hmi_boot_t b;
    hmi_boot_config(&cli, &cfg, &b);
    CHECK(b.ok && b.serial == 1);
    char buf[16];
    size_t n = 0;
    CHECK(hmi_fs_read_all(AUDIT, buf, sizeof(buf), &n) != HMI_FS_READ_OK);
    clean();
}

int main(void)
{
    for (int i = 0; i < 32; i++) KEYB[i] = (uint8_t)(0xA0 + i);
    test_accepted();
    test_rejections();
    test_no_audit_file();
    return test_finish();
}
