/* Tests for hmi_users: names, password policy, strict file format, load
 * outcomes (absent versus invalid), editing rules. Uses a cheap KDF cost. */
#include "test_support.h"
#include "../../src/hmi/hmi_users.h"
#include <stdio.h>

static const hmi_kdf_params_t CHEAP = { 1, 64, 1 };

static hmi_user_t mk(const char *name, hmi_role_t role, const char *pw)
{
    hmi_user_t u;
    const char *why = "";
    hmi_users_err_t e = hmi_user_make(&u, name, role, pw, &CHEAP, &why);
    if (e != HMI_USERS_OK) {
        printf("  setup failure: %s (%s)\n", hmi_users_err_text(e), why);
        exit(3);
    }
    return u;
}

static hmi_users_t two_users(void)
{
    hmi_users_t s;
    memset(&s, 0, sizeof(s));
    hmi_user_t a = mk("admin", HMI_ROLE_ADMIN, "admin-password-1");
    hmi_user_t o = mk("op.one", HMI_ROLE_OPERATOR, "operator-pass-1");
    CHECK(hmi_users_add(&s, &a) == HMI_USERS_OK);
    CHECK(hmi_users_add(&s, &o) == HMI_USERS_OK);
    return s;
}

/* Rewrite the trailer so a field-level check, not the CRC, is what rejects. */
static void fix_crc(uint8_t *b, size_t n)
{
    uint32_t c = hmi_crc32(b, n - 4);
    b[n - 4] = (uint8_t)c;
    b[n - 3] = (uint8_t)(c >> 8);
    b[n - 2] = (uint8_t)(c >> 16);
    b[n - 1] = (uint8_t)(c >> 24);
}

static void test_crc_names_policy(void)
{
    SECTION("users: crc32, names, password policy");
    CHECK(hmi_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u); /* standard check value */
    CHECK(hmi_user_name_valid("abc"));
    CHECK(hmi_user_name_valid("op.one_2-x"));
    CHECK(hmi_user_name_valid("abcdefghijklmnopqrstuvwx")); /* 24 */
    CHECK(!hmi_user_name_valid("abcdefghijklmnopqrstuvwxy")); /* 25 */
    CHECK(!hmi_user_name_valid("ab"));
    CHECK(!hmi_user_name_valid(""));
    CHECK(!hmi_user_name_valid(NULL));
    CHECK(!hmi_user_name_valid("Admin"));      /* upper case */
    CHECK(!hmi_user_name_valid("a b c"));      /* space */
    CHECK(!hmi_user_name_valid("abc\n"));      /* newline would split an audit line */
    CHECK(!hmi_user_name_valid("abc=def"));    /* audit field separator */
    CHECK(!hmi_user_name_valid("\xC3\xA9tienne")); /* non ASCII */

    const char *why = NULL;
    CHECK(hmi_password_policy("admin", "12345678", &why) && why[0] == '\0');
    CHECK(hmi_password_policy("admin", "a password with spaces ~!", NULL));
    CHECK(!hmi_password_policy("admin", "1234567", &why) && strstr(why, "short"));
    CHECK(!hmi_password_policy("admin", "", &why));
    CHECK(!hmi_password_policy("admin", NULL, &why));
    CHECK(!hmi_password_policy("longusername", "longusername", &why) && strstr(why, "differ"));
    CHECK(!hmi_password_policy("admin", "pass\tword1", &why) && strstr(why, "printable"));
    CHECK(!hmi_password_policy("admin", "pass\x7F" "word1", &why));
    CHECK(!hmi_password_policy("admin", "pa\xC3\xA9sword1", &why));
    char big[80];
    memset(big, 'x', sizeof(big));
    big[64] = '\0';
    CHECK(hmi_password_policy("admin", big, &why));  /* 64 is allowed */
    big[64] = 'x';
    big[65] = '\0';
    CHECK(!hmi_password_policy("admin", big, &why) && strstr(why, "long"));
}

static void test_format(void)
{
    SECTION("users: serialize and strict parse");
    hmi_users_t s = two_users();
    uint8_t buf[HMI_USERS_FILE_MAX];
    size_t n = hmi_users_serialize(&s, buf, sizeof(buf));
    CHECK(n == 8 + 96 * 2 + 4);
    CHECK(memcmp(buf, "P99U", 4) == 0 && buf[4] == 1 && buf[5] == 2);
    CHECK(hmi_users_serialize(&s, buf, n - 1) == 0); /* too small */

    hmi_users_t back;
    char err[120];
    CHECK(hmi_users_parse(buf, n, &back, err, sizeof(err)));
    CHECK(back.n == 2 && strcmp(back.u[0].name, "admin") == 0 && back.u[0].role == HMI_ROLE_ADMIN);
    CHECK(strcmp(back.u[1].name, "op.one") == 0 && back.u[1].role == HMI_ROLE_OPERATOR);
    CHECK(memcmp(back.u[0].hash, s.u[0].hash, 32) == 0 && memcmp(back.u[1].salt, s.u[1].salt, 16) == 0);
    CHECK(back.u[1].kdf.t == 1 && back.u[1].kdf.m_kib == 64 && back.u[1].kdf.p == 1);

    /* Truncation and extension. */
    CHECK(!hmi_users_parse(buf, n - 1, &back, err, sizeof(err)));
    CHECK(!hmi_users_parse(buf, 0, &back, err, sizeof(err)));
    CHECK(!hmi_users_parse(NULL, n, &back, err, sizeof(err)));
    uint8_t longer[HMI_USERS_FILE_MAX + 8];
    memcpy(longer, buf, n);
    memset(longer + n, 0, 8);
    CHECK(!hmi_users_parse(longer, n + 1, &back, err, sizeof(err)));
    CHECK(back.n == 0); /* output cleared on failure */

    /* Every single-bit flip is rejected: the CRC catches what a field check
     * would not (the hash and salt bytes carry no structure). */
    int rejected = 0, total = 0;
    for (size_t i = 0; i < n; i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t c[HMI_USERS_FILE_MAX];
            memcpy(c, buf, n);
            c[i] ^= (uint8_t)(1 << bit);
            total++;
            if (!hmi_users_parse(c, n, &back, NULL, 0)) rejected++;
        }
    }
    CHECK(rejected == total);

    /* Field checks with a recomputed CRC, so the CRC is not what rejects. */
    struct { const char *label; size_t off; uint8_t val; const char *expect; } mut[] = {
        { "magic", 0, 'X', "magic" },
        { "version", 4, 2, "version" },
        { "reserved header", 6, 1, "reserved" },
        { "count zero", 5, 0, "count" },
        { "count 17", 5, 17, "count" },
        { "name field garbage after NUL", 8 + 31, 'z', "name field" },
        { "name upper case", 8, 'A', "invalid user name" },
        { "record reserved", 8 + 34, 1, "reserved" },
        { "role viewer", 8 + 32, 0, "role" },
        { "role 4", 8 + 32, 4, "role" },
        { "kdf t zero", 8 + 36, 0, "KDF" },
        { "kdf huge memory", 8 + 40 + 3, 0xFF, "KDF" },
        { "kdf lanes 9", 8 + 44, 9, "KDF" },
    };
    for (size_t k = 0; k < sizeof(mut) / sizeof(mut[0]); k++) {
        uint8_t c[HMI_USERS_FILE_MAX];
        memcpy(c, buf, n);
        c[mut[k].off] = mut[k].val;
        size_t len = n; /* count mutations keep the old length: the count check comes first */
        fix_crc(c, len);
        err[0] = '\0';
        bool ok = hmi_users_parse(c, len, &back, err, sizeof(err));
        if (ok || !strstr(err, mut[k].expect)) {
            printf("  mutation '%s': ok=%d err='%s'\n", mut[k].label, ok, err);
            CHECK(false);
        } else {
            CHECK(true);
        }
    }

    /* Duplicate name. */
    {
        uint8_t c[HMI_USERS_FILE_MAX];
        memcpy(c, buf, n);
        memcpy(c + 8 + 96, c + 8, 32); /* second record takes the first name */
        fix_crc(c, n);
        CHECK(!hmi_users_parse(c, n, &back, err, sizeof(err)) && strstr(err, "duplicate"));
    }
    /* No administrator: demote the only admin in the byte image. */
    {
        uint8_t c[HMI_USERS_FILE_MAX];
        memcpy(c, buf, n);
        c[8 + 32] = HMI_ROLE_OPERATOR;
        fix_crc(c, n);
        CHECK(!hmi_users_parse(c, n, &back, err, sizeof(err)) && strstr(err, "administrator"));
    }
    /* Serialising an invalid store fails instead of writing it. */
    hmi_users_t bad = s;
    bad.u[0].role = HMI_ROLE_VIEWER;
    CHECK(hmi_users_serialize(&bad, buf, sizeof(buf)) == 0);
    bad = s;
    strcpy(bad.u[0].name, "Bad Name");
    CHECK(hmi_users_serialize(&bad, buf, sizeof(buf)) == 0);
    bad = s;
    bad.n = 0;
    CHECK(hmi_users_serialize(&bad, buf, sizeof(buf)) == 0);
}

static void test_file(void)
{
    SECTION("users: load outcomes and save");
    const char *path = "users_test.bin";
    remove(path);
    hmi_users_t s = two_users(), got;
    char err[120];
    CHECK(hmi_users_load(path, &got, err, sizeof(err)) == HMI_USERS_LOAD_ABSENT);

    CHECK(hmi_users_save(path, &s));
    CHECK(hmi_users_load(path, &got, err, sizeof(err)) == HMI_USERS_LOAD_OK && got.n == 2);

    /* A file with bad content is INVALID, never ABSENT: absent alone enables first-boot creation. */
    uint8_t raw[HMI_USERS_FILE_MAX];
    size_t n = hmi_users_serialize(&s, raw, sizeof(raw));
    raw[20] ^= 0xFF;
    CHECK(hmi_fs_atomic_write(path, raw, n));
    CHECK(hmi_users_load(path, &got, err, sizeof(err)) == HMI_USERS_LOAD_INVALID && got.n == 0);
    CHECK(hmi_fs_atomic_write(path, "", 0));
    CHECK(hmi_users_load(path, &got, err, sizeof(err)) == HMI_USERS_LOAD_INVALID); /* empty file */
    uint8_t huge[HMI_USERS_FILE_MAX + 100];
    memset(huge, 0, sizeof(huge));
    CHECK(hmi_fs_atomic_write(path, huge, sizeof(huge)));
    CHECK(hmi_users_load(path, &got, err, sizeof(err)) == HMI_USERS_LOAD_INVALID && strstr(err, "larger"));

    /* The old file survives a rejected save (no administrator). */
    CHECK(hmi_users_save(path, &s));
    hmi_users_t noadmin = s;
    noadmin.u[0].role = HMI_ROLE_OPERATOR;
    CHECK(!hmi_users_save(path, &noadmin));
    CHECK(hmi_users_load(path, &got, err, sizeof(err)) == HMI_USERS_LOAD_OK && got.u[0].role == HMI_ROLE_ADMIN);
    remove(path);
}

static void test_editing(void)
{
    SECTION("users: add, remove, replace and make");
    hmi_user_t u;
    const char *why = "";
    CHECK(hmi_user_make(&u, "Bad", HMI_ROLE_ADMIN, "longenough1", &CHEAP, &why) == HMI_USERS_ERR_NAME);
    CHECK(hmi_user_make(&u, "good", HMI_ROLE_VIEWER, "longenough1", &CHEAP, &why) == HMI_USERS_ERR_ROLE);
    CHECK(hmi_user_make(&u, "good", (hmi_role_t)9, "longenough1", &CHEAP, &why) == HMI_USERS_ERR_ROLE);
    CHECK(hmi_user_make(&u, "good", HMI_ROLE_ADMIN, "short", &CHEAP, &why) == HMI_USERS_ERR_PASSWORD && why[0]);
    hmi_kdf_params_t badp = { 0, 0, 0 };
    CHECK(hmi_user_make(&u, "good", HMI_ROLE_ADMIN, "longenough1", &badp, &why) == HMI_USERS_ERR_INTERNAL);
    CHECK(hmi_user_make(&u, "good", HMI_ROLE_ADMIN, "longenough1", &CHEAP, &why) == HMI_USERS_OK);
    hmi_user_t u2;
    CHECK(hmi_user_make(&u2, "good", HMI_ROLE_ADMIN, "longenough1", &CHEAP, &why) == HMI_USERS_OK);
    CHECK(memcmp(u.salt, u2.salt, 16) != 0);   /* fresh salt each time */
    CHECK(memcmp(u.hash, u2.hash, 32) != 0);   /* so equal passwords hash differently */

    /* The stored hash really is the hash of the password. */
    uint8_t h[32];
    CHECK(hmi_kdf_hash(&u.kdf, "longenough1", 11, u.salt, h) && hmi_kdf_equal(h, u.hash));
    CHECK(hmi_kdf_hash(&u.kdf, "longenough2", 11, u.salt, h) && !hmi_kdf_equal(h, u.hash));

    hmi_users_t s = two_users();
    hmi_user_t dup = mk("admin", HMI_ROLE_ADMIN, "another-pass-1");
    CHECK(hmi_users_add(&s, &dup) == HMI_USERS_ERR_EXISTS);
    CHECK(hmi_users_find(&s, "op.one") == 1 && hmi_users_find(&s, "nobody") < 0 && hmi_users_find(&s, NULL) < 0);

    /* Capacity. */
    hmi_users_t full;
    memset(&full, 0, sizeof(full));
    for (int i = 0; i < HMI_USERS_MAX; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "user%02d", i);
        hmi_user_t x = mk(nm, i == 0 ? HMI_ROLE_ADMIN : HMI_ROLE_OPERATOR, "password-0123");
        CHECK(hmi_users_add(&full, &x) == HMI_USERS_OK);
    }
    hmi_user_t extra = mk("extra", HMI_ROLE_OPERATOR, "password-0123");
    CHECK(hmi_users_add(&full, &extra) == HMI_USERS_ERR_FULL);
    uint8_t buf[HMI_USERS_FILE_MAX];
    CHECK(hmi_users_serialize(&full, buf, sizeof(buf)) == HMI_USERS_FILE_MAX);
    hmi_users_t back;
    CHECK(hmi_users_parse(buf, HMI_USERS_FILE_MAX, &back, NULL, 0) && back.n == HMI_USERS_MAX);

    /* Removing and replacing never leaves the store without an administrator. */
    CHECK(hmi_users_remove(&s, "nobody") == HMI_USERS_ERR_NOT_FOUND);
    CHECK(hmi_users_remove(&s, "admin") == HMI_USERS_ERR_LAST_ADMIN && s.n == 2);
    hmi_user_t demote = s.u[0];
    demote.role = HMI_ROLE_ENGINEER;
    CHECK(hmi_users_replace(&s, &demote) == HMI_USERS_ERR_LAST_ADMIN && s.u[0].role == HMI_ROLE_ADMIN);
    hmi_user_t second = mk("admin2", HMI_ROLE_ADMIN, "second-admin-1");
    CHECK(hmi_users_add(&s, &second) == HMI_USERS_OK);
    CHECK(hmi_users_replace(&s, &demote) == HMI_USERS_OK && s.u[0].role == HMI_ROLE_ENGINEER);
    CHECK(hmi_users_remove(&s, "admin") == HMI_USERS_OK && s.n == 2 && strcmp(s.u[0].name, "op.one") == 0);
    hmi_user_t ghost = mk("ghost", HMI_ROLE_OPERATOR, "ghost-pass-12");
    CHECK(hmi_users_replace(&s, &ghost) == HMI_USERS_ERR_NOT_FOUND);
    CHECK(hmi_users_remove(&s, "admin2") == HMI_USERS_ERR_LAST_ADMIN);
}

int main(void)
{
    test_crc_names_policy();
    test_format();
    test_file();
    test_editing();
    return test_finish();
}
