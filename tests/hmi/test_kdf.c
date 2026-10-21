/* Tests for hmi_kdf: Argon2id known answer (RFC 9106), parameter bounds,
 * determinism, constant-time compare and wipe. */
#include "test_support.h"
#include "../../src/hmi/hmi_kdf.h"

static void fill(uint8_t *p, size_t n, uint8_t v) { memset(p, v, n); }

static void test_rfc9106(void)
{
    SECTION("kdf: Argon2id known answer, RFC 9106 section 5.3");
    /* t=3, m=32 KiB, p=4, tag length 32, password 32 x 0x01, salt 16 x 0x02,
     * secret 8 x 0x03, associated data 12 x 0x04. */
    uint8_t pw[32], salt[16], secret[8], ad[12], tag[32];
    fill(pw, sizeof(pw), 1);
    fill(salt, sizeof(salt), 2);
    fill(secret, sizeof(secret), 3);
    fill(ad, sizeof(ad), 4);
    static const uint8_t expect[32] = { 0x0d, 0x64, 0x0d, 0xf5, 0x8d, 0x78, 0x76, 0x6c, 0x08, 0xc0, 0x37,
                                        0xa3, 0x4a, 0x8b, 0x53, 0xc9, 0xd0, 0x1e, 0xf0, 0x45, 0x2d, 0x75,
                                        0xb6, 0x5e, 0xb5, 0x25, 0x20, 0xe9, 0x6b, 0x01, 0xe6, 0x59 };
    hmi_kdf_params_t p = { 3, 32, 4 };
    CHECK(hmi_kdf_hash_ex(&p, pw, sizeof(pw), salt, sizeof(salt), secret, sizeof(secret), ad, sizeof(ad), tag,
                          sizeof(tag)));
    CHECK(memcmp(tag, expect, 32) == 0);
}

static void test_params(void)
{
    SECTION("kdf: parameter bounds");
    hmi_kdf_params_t d = hmi_kdf_default_params();
    CHECK(d.t == 3 && d.m_kib == 8192 && d.p == 1);
    CHECK(hmi_kdf_params_valid(&d));
    CHECK(!hmi_kdf_params_valid(NULL));
    hmi_kdf_params_t p = d;
    p.t = 0;
    CHECK(!hmi_kdf_params_valid(&p));
    p.t = 17;
    CHECK(!hmi_kdf_params_valid(&p));
    p = d;
    p.p = 0;
    CHECK(!hmi_kdf_params_valid(&p));
    p.p = 5;
    CHECK(!hmi_kdf_params_valid(&p));
    p = d;
    p.m_kib = 7;
    CHECK(!hmi_kdf_params_valid(&p));
    p.m_kib = 65537;
    CHECK(!hmi_kdf_params_valid(&p));
    p.m_kib = 65536;
    CHECK(hmi_kdf_params_valid(&p));
    p = d;
    p.p = 4;
    p.m_kib = 31; /* below 8 * p */
    CHECK(!hmi_kdf_params_valid(&p));
    p.m_kib = 32;
    CHECK(hmi_kdf_params_valid(&p));

    uint8_t salt[HMI_KDF_SALT_LEN], out[HMI_KDF_HASH_LEN];
    fill(salt, sizeof(salt), 9);
    fill(out, sizeof(out), 0xAA);
    p = d;
    p.m_kib = 0xFFFFFFFFu; /* hostile value from a corrupted file */
    CHECK(!hmi_kdf_hash(&p, "pw", 2, salt, out));
    CHECK(out[0] == 0xAA && out[31] == 0xAA); /* untouched on failure */
    CHECK(!hmi_kdf_hash(NULL, "pw", 2, salt, out));
    CHECK(!hmi_kdf_hash(&d, "pw", 2, NULL, out));
}

static void test_behaviour(void)
{
    SECTION("kdf: determinism and separation");
    hmi_kdf_params_t p = { 1, 64, 1 }; /* cheap, test only */
    uint8_t s1[HMI_KDF_SALT_LEN], s2[HMI_KDF_SALT_LEN];
    fill(s1, sizeof(s1), 1);
    fill(s2, sizeof(s2), 1);
    s2[15] = 2;
    uint8_t a[32], b[32], c[32], d[32], e[32];
    CHECK(hmi_kdf_hash(&p, "correct horse", 13, s1, a));
    CHECK(hmi_kdf_hash(&p, "correct horse", 13, s1, b));
    CHECK(hmi_kdf_equal(a, b));
    CHECK(hmi_kdf_hash(&p, "correct horsf", 13, s1, c)); /* one character */
    CHECK(!hmi_kdf_equal(a, c));
    CHECK(hmi_kdf_hash(&p, "correct horse", 13, s2, d)); /* one salt bit */
    CHECK(!hmi_kdf_equal(a, d));
    hmi_kdf_params_t p2 = p;
    p2.t = 2;
    CHECK(hmi_kdf_hash(&p2, "correct horse", 13, s1, e)); /* cost is part of the hash */
    CHECK(!hmi_kdf_equal(a, e));
    CHECK(hmi_kdf_hash(&p, "", 0, s1, e)); /* empty input is hashed, policy rejects it elsewhere */
    CHECK(!hmi_kdf_equal(a, e));

    /* A single differing byte at each end of the hash is detected. */
    memcpy(c, a, 32);
    c[0] ^= 1;
    CHECK(!hmi_kdf_equal(a, c));
    memcpy(c, a, 32);
    c[31] ^= 0x80;
    CHECK(!hmi_kdf_equal(a, c));
}

static void test_wipe(void)
{
    SECTION("kdf: wipe");
    uint8_t buf[16];
    fill(buf, sizeof(buf), 0x5A);
    hmi_wipe(buf, sizeof(buf));
    int zero = 1;
    for (size_t i = 0; i < sizeof(buf); i++) zero &= (buf[i] == 0);
    CHECK(zero);
    hmi_wipe(NULL, 5); /* must not crash */
    hmi_wipe(buf, 0);
}

int main(void)
{
    test_rfc9106();
    test_params();
    test_behaviour();
    test_wipe();
    return test_finish();
}
