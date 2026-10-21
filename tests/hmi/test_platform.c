/* Tests for the platform additions used by authentication: whole-file read
 * with distinct failure outcomes, and OS random bytes. */
#include "test_support.h"
#include <stdio.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

static void test_read_all(void)
{
    SECTION("platform: hmi_fs_read_all");
    const char *path = "platform_read_test.bin";
    remove(path);
    uint8_t buf[16];
    size_t len = 77;
    CHECK(hmi_fs_read_all(path, buf, sizeof(buf), &len) == HMI_FS_READ_ABSENT);
    CHECK(len == 77); /* untouched on failure */

    const uint8_t data[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    CHECK(hmi_fs_atomic_write(path, data, sizeof(data)));
    memset(buf, 0, sizeof(buf));
    CHECK(hmi_fs_read_all(path, buf, sizeof(buf), &len) == HMI_FS_READ_OK);
    CHECK(len == 10 && memcmp(buf, data, 10) == 0);

    CHECK(hmi_fs_read_all(path, buf, 10, &len) == HMI_FS_READ_OK && len == 10); /* exactly cap */
    len = 77;
    CHECK(hmi_fs_read_all(path, buf, 9, &len) == HMI_FS_READ_TOO_BIG && len == 77);
    CHECK(hmi_fs_read_all(path, buf, 0, &len) == HMI_FS_READ_TOO_BIG);

    CHECK(hmi_fs_atomic_write(path, "", 0)); /* empty file is OK, length 0 */
    CHECK(hmi_fs_read_all(path, buf, sizeof(buf), &len) == HMI_FS_READ_OK && len == 0);
    CHECK(hmi_fs_read_all(path, buf, sizeof(buf), NULL) == HMI_FS_READ_OK); /* len is optional */
    remove(path);

    CHECK(hmi_fs_read_all("no_such_dir_p99/x.bin", buf, sizeof(buf), &len) == HMI_FS_READ_ABSENT);
}

static void test_random(void)
{
    SECTION("platform: hmi_random_bytes");
    uint8_t a[32], b[32], z[32];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    memset(z, 0, sizeof(z));
    CHECK(hmi_random_bytes(a, sizeof(a)));
    CHECK(hmi_random_bytes(b, sizeof(b)));
    CHECK(memcmp(a, z, 32) != 0); /* 2^-256 chance of a false failure */
    CHECK(memcmp(a, b, 32) != 0);
    CHECK(hmi_random_bytes(a, 0)); /* zero bytes is a success */
    uint8_t big[70000];            /* larger than one OS call may return */
    memset(big, 0, sizeof(big));
    CHECK(hmi_random_bytes(big, sizeof(big)));
    int nz = 0;
    for (size_t i = 0; i < sizeof(big); i += 97) nz += big[i] != 0;
    CHECK(nz > 600); /* roughly 99.6 percent of 722 samples are non-zero */
}

static void test_mode(void)
{
    SECTION("platform: hmi_fs_mode");
    const char *path = "platform_mode_test.bin";
    uint32_t m = 1;
    remove(path);
    CHECK(!hmi_fs_mode(path, &m) && m == 1);           /* missing file: false, untouched */
    CHECK(hmi_fs_atomic_write(path, "x", 1));
    CHECK(hmi_fs_mode(path, &m) && m == 0600u);          /* files are created private */
#ifndef _WIN32
    CHECK(chmod(path, 0644) == 0);
    CHECK(hmi_fs_mode(path, &m) && m == 0644u);
    CHECK(chmod(path, 0666) == 0);
    CHECK(hmi_fs_mode(path, &m) && m == 0666u);
#endif
    remove(path);
}

int main(void)
{
    test_mode();
    test_read_all();
    test_random();
    return test_finish();
}
