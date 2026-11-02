/* Tests for hmi_auth (named users, PIN, lockout, idle logout) and the "users" part of the configuration.
 * Part of the HMI core host tests; shared fixtures are in test_support.h. */
#include "test_support.h"
#include "../../src/hmi/hmi_auth.h"
#include "../../src/hmi/hmi_auditchain.h"

static const uint8_t CFG_KEY[32] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 9, 8, 7, 6, 5, 4,
                                     3, 2, 1, 0, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 9, 8 };

static void hexs(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", b[i]);
}

/* A user record as the configuration holds it. */
static void make_user(hmi_user_cfg_t *u, const char *name, hmi_role_t role, uint8_t salt_byte, const char *pin,
                      const uint8_t *pin_key)
{
    memset(u, 0, sizeof(*u));
    snprintf(u->name, sizeof(u->name), "%s", name);
    u->role = role;
    memset(u->salt, salt_byte, sizeof(u->salt));
    hmi_auth_hash(pin_key, u->salt, pin, u->pin_hash);
}

static void test_pbkdf2_vectors(void)
{
    SECTION("auth: PBKDF2-HMAC-SHA-512 matches published vectors (checked against Python hashlib)");
    struct { const char *pw, *salt; uint32_t c; size_t len; const char *hex; } v[] = {
        { "password", "salt", 1, 64, "867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252c02d470a285a0501bad999bfe943c08f050235d7d68b1da55e63f73b60a57fce" },
        { "password", "salt", 4096, 64, "d197b1b33db0143e018b12f3d1d1479e6cdebdcc97c5c0f87f6902e072f457b5143f30602641b3d55cd335988cb36b84376060ecd532e039b742a239434af2d5" },
        { "passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 64, "8c0511f4c6e597c6ac6315d8f0362e225f3c501495ba23b868c005174dc4ee71115b59f9e60cd9532fa33e0f75aefe30225c583a186cd82bd4daea9724a3d3b8" },
        { "password", "salt", 2, 80, "e1d9c16aa681708a45f5c7c4e215ceb66e011a2e9f0040713f18aefdb866d53cf76cab2868a39b9f7840edce4fef5a82be67335c77a6068e04112754f27ccf4e473e311ad827b68945f4e2dddb204c78" },
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        uint8_t out[80];
        char hex[161];
        hmi_auth_pbkdf2_sha512((const uint8_t *)v[i].pw, strlen(v[i].pw), (const uint8_t *)v[i].salt, strlen(v[i].salt),
                               v[i].c, out, v[i].len);
        hexs(out, v[i].len, hex);
        CHECK(strcmp(hex, v[i].hex) == 0);
    }
}

static void test_hash(void)
{
    SECTION("auth: the PIN hash depends on the key, the salt and the PIN, and takes measurable time");
    uint8_t k1[32], k2[32], salt[HMI_SALT_LEN], salt2[HMI_SALT_LEN], a[32], b[32];
    hmi_auth_derive_key(CFG_KEY, sizeof(CFG_KEY), k1);
    memcpy(k2, k1, 32);
    k2[0] ^= 1;
    memset(salt, 0x11, sizeof(salt));
    memset(salt2, 0x12, sizeof(salt2));
    uint64_t t0 = hmi_now_ms();
    hmi_auth_hash(k1, salt, "1234", a);
    uint64_t dt = hmi_now_ms() - t0;
    printf("  one PIN check (PBKDF2-HMAC-SHA-512, %u iterations): %u ms on this machine\n", (unsigned)HMI_PIN_ITERATIONS,
           (unsigned)dt);
    hmi_auth_hash(k1, salt, "1234", b);
    CHECK(memcmp(a, b, 32) == 0);                        /* deterministic */
    hmi_auth_hash(k2, salt, "1234", b);
    CHECK(memcmp(a, b, 32) != 0);                        /* another key */
    hmi_auth_hash(k1, salt2, "1234", b);
    CHECK(memcmp(a, b, 32) != 0);                        /* another salt */
    hmi_auth_hash(k1, salt, "1235", b);
    CHECK(memcmp(a, b, 32) != 0);                        /* another PIN */
    uint8_t audit_key[32];
    hmi_chain_derive_key(CFG_KEY, sizeof(CFG_KEY), audit_key);
    CHECK(memcmp(audit_key, k1, 32) != 0);               /* the two derived keys differ */

    CHECK(hmi_auth_pin_format_ok("1234") && hmi_auth_pin_format_ok("12345678"));
    CHECK(!hmi_auth_pin_format_ok("123") && !hmi_auth_pin_format_ok("123456789"));
    CHECK(!hmi_auth_pin_format_ok("12a4") && !hmi_auth_pin_format_ok("") && !hmi_auth_pin_format_ok("12 34"));
}

static void test_login_lock_idle(void)
{
    SECTION("auth: login, five wrong PINs lock, the lock doubles, a success resets, idle logout after 5 min");
    uint8_t pk[32];
    hmi_auth_derive_key(CFG_KEY, sizeof(CFG_KEY), pk);
    static hmi_user_cfg_t users[2];
    make_user(&users[0], "op1", HMI_ROLE_OPERATOR, 0x21, "2468", pk);
    make_user(&users[1], "eng1", HMI_ROLE_ENGINEER, 0x22, "13579", pk);
    hmi_auth_t a;
    hmi_auth_init(&a, users, 2, pk);
    CHECK(hmi_auth_enabled(&a));
    CHECK(hmi_auth_role(&a) == HMI_ROLE_VIEWER && hmi_auth_user(&a) == NULL); /* nobody logged in: viewer */

    uint64_t t = 1000000;
    CHECK(hmi_auth_login(&a, 0, "2468", t) == HMI_AUTH_OK);
    CHECK(hmi_auth_role(&a) == HMI_ROLE_OPERATOR && strcmp(hmi_auth_user(&a), "op1") == 0);
    hmi_auth_logout(&a);
    CHECK(hmi_auth_role(&a) == HMI_ROLE_VIEWER && hmi_auth_user(&a) == NULL);
    CHECK(hmi_auth_login(&a, 1, "2468", t) == HMI_AUTH_WRONG_PIN);   /* another user's PIN */
    CHECK(hmi_auth_login(&a, 5, "2468", t) == HMI_AUTH_NO_USER);
    CHECK(hmi_auth_login(&a, 0, "12", t) == HMI_AUTH_BAD_FORMAT);   /* refused without a check, not counted */
    CHECK(a.fails == 1);

    /* four more wrong PINs: the fifth locks for 60 s */
    for (int i = 0; i < 3; i++) CHECK(hmi_auth_login(&a, 0, "0000", t) == HMI_AUTH_WRONG_PIN);
    CHECK(hmi_auth_locked_for(&a, t) == 0);
    CHECK(hmi_auth_login(&a, 0, "0000", t) == HMI_AUTH_WRONG_PIN);
    CHECK(hmi_auth_locked_for(&a, t) == HMI_AUTH_LOCK_MS);
    CHECK(hmi_auth_login(&a, 0, "2468", t + 1000) == HMI_AUTH_LOCKED); /* even the right PIN is not checked */
    CHECK(hmi_auth_role(&a) == HMI_ROLE_VIEWER);
    t += HMI_AUTH_LOCK_MS;
    CHECK(hmi_auth_locked_for(&a, t) == 0);
    /* five more wrong PINs: 120 s */
    for (int i = 0; i < 5; i++) CHECK(hmi_auth_login(&a, 1, "1111", t) == HMI_AUTH_WRONG_PIN);
    CHECK(hmi_auth_locked_for(&a, t) == 2 * HMI_AUTH_LOCK_MS);
    /* the doubling stops at the maximum */
    for (int round = 0; round < 6; round++) {
        t += HMI_AUTH_LOCK_MAX_MS;
        for (int i = 0; i < 5; i++) hmi_auth_login(&a, 1, "1111", t);
    }
    CHECK(hmi_auth_locked_for(&a, t) == HMI_AUTH_LOCK_MAX_MS);
    t += HMI_AUTH_LOCK_MAX_MS;
    CHECK(hmi_auth_login(&a, 1, "13579", t) == HMI_AUTH_OK);         /* a success ends it all */
    CHECK(a.fails == 0 && a.locks == 0 && hmi_auth_role(&a) == HMI_ROLE_ENGINEER);

    /* idle logout: input keeps the session, 5 minutes without input end it, once */
    hmi_auth_input(&a, t + HMI_AUTH_IDLE_MS - 1000);
    CHECK(!hmi_auth_idle_check(&a, t + HMI_AUTH_IDLE_MS));
    CHECK(hmi_auth_idle_check(&a, t + 2 * HMI_AUTH_IDLE_MS));
    CHECK(hmi_auth_role(&a) == HMI_ROLE_VIEWER);
    CHECK(!hmi_auth_idle_check(&a, t + 3 * HMI_AUTH_IDLE_MS));

    /* a lock while someone is logged in logs them out */
    CHECK(hmi_auth_login(&a, 0, "2468", t) == HMI_AUTH_OK);
    for (int i = 0; i < 5; i++) hmi_auth_login(&a, 1, "9999", t);
    CHECK(hmi_auth_role(&a) == HMI_ROLE_VIEWER);
    hmi_auth_destroy(&a);
    CHECK(!hmi_auth_enabled(&a));

    /* disabled: no users, or no key */
    hmi_auth_init(&a, users, 0, pk);
    CHECK(!hmi_auth_enabled(&a) && hmi_auth_login(&a, 0, "2468", t) == HMI_AUTH_DISABLED);
    hmi_auth_init(&a, users, 2, NULL);
    CHECK(!hmi_auth_enabled(&a) && hmi_auth_login(&a, 0, "2468", t) == HMI_AUTH_DISABLED);
}

/* GOOD_CFG with a "users" member inserted before the final brace. */
static void with_users(char *out, size_t cap, const char *users_json)
{
    size_t n = strlen(GOOD_CFG);
    snprintf(out, cap, "%.*s,%s}", (int)(n - 1), GOOD_CFG, users_json);
}

static void test_config_users(void)
{
    SECTION("auth: users in the configuration are parsed strictly and written back unchanged");
    uint8_t pk[32];
    hmi_auth_derive_key(CFG_KEY, sizeof(CFG_KEY), pk);
    hmi_user_cfg_t u;
    make_user(&u, "op1", HMI_ROLE_OPERATOR, 0x31, "2468", pk);
    char salt[33], hash[65], users[512], text[4096], err[128];
    hexs(u.salt, 16, salt);
    hexs(u.pin_hash, 32, hash);
    static hmi_app_cfg_t c, c2;

    snprintf(users, sizeof(users), "\"users\":[{\"name\":\"op1\",\"role\":\"operator\",\"salt\":\"%s\",\"pin_hash\":\"%s\"}]",
             salt, hash);
    with_users(text, sizeof(text), users);
    CHECK(parse(text, &c, err));
    CHECK(c.n_users == 1 && strcmp(c.users[0].name, "op1") == 0 && c.users[0].role == HMI_ROLE_OPERATOR);
    CHECK(memcmp(c.users[0].salt, u.salt, 16) == 0 && memcmp(c.users[0].pin_hash, u.pin_hash, 32) == 0);
    /* the parsed record logs in */
    hmi_auth_t a;
    hmi_auth_init(&a, c.users, c.n_users, pk);
    CHECK(hmi_auth_login(&a, 0, "2468", 1) == HMI_AUTH_OK);

    /* round trip: written out and read back identically (the on-device editor rewrites the configuration) */
    static char out[HMI_CONFIG_MAX_BYTES + 1];
    size_t n = hmi_config_serialize(&c, out, sizeof(out));
    CHECK(n > 0 && hmi_config_parse(out, n, &c2, err, sizeof(err)) && hmi_config_equal(&c, &c2));
    CHECK(strstr(out, "\"users\":[") != NULL);
    CHECK(parse(GOOD_CFG, &c, err) && c.n_users == 0);  /* no users: none */
    n = hmi_config_serialize(&c, out, sizeof(out));
    CHECK(n > 0 && strstr(out, "users") == NULL);        /* and none written */

    /* rejections */
    with_users(text, sizeof(text), "\"users\":[]");                 /* empty */
    CHECK(!parse(text, &c, err));
    with_users(text, sizeof(text), "\"users\":{}");                 /* not an array */
    CHECK(!parse(text, &c, err));
    char short_salt[33], long_salt[40], bad_hash[65];
    snprintf(short_salt, sizeof(short_salt), "%.30s", salt);
    snprintf(long_salt, sizeof(long_salt), "%sAA", salt);
    snprintf(bad_hash, sizeof(bad_hash), "%.62sXY", hash);
    struct { const char *name, *role, *salt, *hash, *extra; } bad[] = {
        { "op1", "operator", salt, NULL, "" },              /* no hash */
        { "op 1", "operator", salt, hash, "" },             /* space in the name */
        { "op1", "boss", salt, hash, "" },                  /* unknown role */
        { "op1", "operator", short_salt, hash, "" },        /* short salt */
        { "op1", "operator", long_salt, hash, "" },         /* long salt */
        { "op1", "operator", salt, bad_hash, "" },          /* not hex */
        { "op1", "operator", salt, hash, ",\"pin\":\"1\"" },/* unknown key */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (bad[i].hash)
            snprintf(users, sizeof(users), "\"users\":[{\"name\":\"%s\",\"role\":\"%s\",\"salt\":\"%s\",\"pin_hash\":\"%s\"%s}]",
                     bad[i].name, bad[i].role, bad[i].salt, bad[i].hash, bad[i].extra);
        else
            snprintf(users, sizeof(users), "\"users\":[{\"name\":\"%s\",\"role\":\"%s\",\"salt\":\"%s\"}]", bad[i].name,
                     bad[i].role, bad[i].salt);
        with_users(text, sizeof(text), users);
        CHECK(!parse(text, &c, err));
    }
    /* a duplicate name */
    snprintf(users, sizeof(users),
             "\"users\":[{\"name\":\"op1\",\"role\":\"operator\",\"salt\":\"%s\",\"pin_hash\":\"%s\"},"
             "{\"name\":\"op1\",\"role\":\"admin\",\"salt\":\"%s\",\"pin_hash\":\"%s\"}]",
             salt, hash, salt, hash);
    with_users(text, sizeof(text), users);
    CHECK(!parse(text, &c, err) && strstr(err, "duplicate user") != NULL);
    /* nine users: one too many */
    static char big[4096];
    size_t off = (size_t)snprintf(big, sizeof(big), "\"users\":[");
    for (int i = 0; i < HMI_MAX_USERS + 1; i++)
        off += (size_t)snprintf(big + off, sizeof(big) - off,
                                "%s{\"name\":\"u%d\",\"role\":\"viewer\",\"salt\":\"%s\",\"pin_hash\":\"%s\"}",
                                i ? "," : "", i, salt, hash);
    snprintf(big + off, sizeof(big) - off, "]");
    static char text2[8192];
    size_t gl = strlen(GOOD_CFG);
    snprintf(text2, sizeof(text2), "%.*s,%s}", (int)(gl - 1), GOOD_CFG, big);
    CHECK(!parse(text2, &c, err) && strstr(err, "user count") != NULL);
}

int main(void)
{
    test_pbkdf2_vectors();
    test_hash();
    test_login_lock_idle();
    test_config_users();
    return test_finish();
}
