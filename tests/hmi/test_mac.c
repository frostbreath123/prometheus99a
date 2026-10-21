/* Tests for hmi_mac: HMAC-SHA-512 known answers (RFC 4231) and comparison. */
#include "test_support.h"
#include "../../src/hmi/hmi_mac.h"

static void hex(const char *h, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void test_rfc4231(void)
{
    SECTION("mac: HMAC-SHA-512 known answers, RFC 4231");
    uint8_t out[64], want[64];

    /* test case 1 */
    uint8_t key1[20];
    memset(key1, 0x0b, sizeof(key1));
    hex("87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
        "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854", want, 64);
    hmi_mac(key1, sizeof(key1), (const uint8_t *)"Hi There", 8, out);
    CHECK(memcmp(out, want, 64) == 0);

    /* test case 2 */
    hex("164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
        "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737", want, 64);
    hmi_mac((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, out);
    CHECK(memcmp(out, want, 64) == 0);

    /* test case 3: key 20 x 0xaa, data 50 x 0xdd */
    uint8_t key3[20], data3[50];
    memset(key3, 0xaa, sizeof(key3));
    memset(data3, 0xdd, sizeof(data3));
    hex("fa73b0089d56a284efb0f0756c890be9b1b5dbdd8ee81a3655f83e33b2279d39"
        "bf3e848279a722c806b485a47e67c807b946a337bee8942674278859e13292fb", want, 64);
    hmi_mac(key3, sizeof(key3), data3, sizeof(data3), out);
    CHECK(memcmp(out, want, 64) == 0);

    /* test case 6: a key longer than the block size (131 x 0xaa) */
    uint8_t key6[131];
    memset(key6, 0xaa, sizeof(key6));
    hex("80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f352"
        "6b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598", want, 64);
    hmi_mac(key6, sizeof(key6), (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54, out);
    CHECK(memcmp(out, want, 64) == 0);
}

static void test_incremental(void)
{
    SECTION("mac: incremental equals one call, pieces do not matter");
    uint8_t key[32], msg[300], a[64], b[64];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7 + 1);
    for (int i = 0; i < 300; i++) msg[i] = (uint8_t)(i * 13 + 5);
    hmi_mac(key, sizeof(key), msg, sizeof(msg), a);
    hmi_mac_ctx_t c;
    hmi_mac_init(&c, key, sizeof(key));
    hmi_mac_update(&c, msg, 1);
    hmi_mac_update(&c, msg + 1, 127);
    hmi_mac_update(&c, msg + 128, 172);
    hmi_mac_final(&c, b);
    CHECK(memcmp(a, b, 64) == 0);

    int zero = 1;
    for (size_t i = 0; i < sizeof(c); i++) zero &= (((uint8_t *)&c)[i] == 0);
    CHECK(zero); /* final wipes the context */
}

static void test_equal_and_sensitivity(void)
{
    SECTION("mac: comparison and key/message sensitivity");
    uint8_t key[32], key2[32], msg[40], a[64], b[64];
    memset(key, 1, sizeof(key));
    memcpy(key2, key, sizeof(key));
    key2[31] ^= 1;
    memset(msg, 2, sizeof(msg));
    hmi_mac(key, 32, msg, sizeof(msg), a);
    hmi_mac(key, 32, msg, sizeof(msg), b);
    CHECK(hmi_mac_equal(a, b));
    hmi_mac(key2, 32, msg, sizeof(msg), b);
    CHECK(!hmi_mac_equal(a, b)); /* one key bit */
    msg[39] ^= 0x80;
    hmi_mac(key, 32, msg, sizeof(msg), b);
    CHECK(!hmi_mac_equal(a, b)); /* one message bit */
    msg[39] ^= 0x80;
    hmi_mac(key, 32, msg, sizeof(msg) - 1, b);
    CHECK(!hmi_mac_equal(a, b)); /* truncated message */
    hmi_mac(key, 32, msg, sizeof(msg), b);
    b[0] ^= 1;
    CHECK(!hmi_mac_equal(a, b));
    b[0] ^= 1;
    b[63] ^= 1;
    CHECK(!hmi_mac_equal(a, b)); /* difference only in the last byte */
    hmi_mac(key, 32, (const uint8_t *)"", 0, a); /* empty message is allowed */
    hmi_mac(key, 32, (const uint8_t *)"", 0, b);
    CHECK(hmi_mac_equal(a, b));
}

int main(void)
{
    test_rfc4231();
    test_incremental();
    test_equal_and_sensitivity();
    return test_finish();
}
