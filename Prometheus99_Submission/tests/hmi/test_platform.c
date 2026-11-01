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

static void test_size_rename(void)
{
    SECTION("platform: hmi_fs_size and hmi_fs_rename");
    const char *a = "platform_ren_a.bin", *b = "platform_ren_b.bin";
    remove(a);
    remove(b);
    uint64_t sz = 99;
    CHECK(!hmi_fs_size(a, &sz) && sz == 99); /* missing file: false, untouched */
    CHECK(!hmi_fs_rename(a, b));             /* missing source fails */

    CHECK(hmi_fs_atomic_write(a, "12345", 5));
    CHECK(hmi_fs_size(a, &sz) && sz == 5);
    CHECK(hmi_fs_append_sync(a, "678", 3));
    CHECK(hmi_fs_size(a, &sz) && sz == 8);

    CHECK(hmi_fs_rename(a, b));
    CHECK(!hmi_fs_size(a, &sz)); /* source is gone */
    CHECK(hmi_fs_size(b, &sz) && sz == 8);

    CHECK(hmi_fs_atomic_write(a, "zz", 2)); /* rename replaces an existing target */
    CHECK(hmi_fs_rename(a, b));
    CHECK(hmi_fs_size(b, &sz) && sz == 2);
    char buf[8];
    size_t len = 0;
    CHECK(hmi_fs_read_all(b, buf, sizeof(buf), &len) == HMI_FS_READ_OK && len == 2 && memcmp(buf, "zz", 2) == 0);
    remove(b);
}

static void test_read_tail(void)
{
    SECTION("platform: hmi_fs_read_tail");
    const char *path = "platform_tail_test.bin";
    remove(path);
    char buf[16];
    size_t len = 77;
    bool trunc = true;
    CHECK(hmi_fs_read_tail(path, buf, sizeof(buf), &len, &trunc) == HMI_FS_READ_ABSENT && len == 77 && trunc);

    CHECK(hmi_fs_atomic_write(path, "0123456789", 10));
    CHECK(hmi_fs_read_tail(path, buf, sizeof(buf), &len, &trunc) == HMI_FS_READ_OK); /* shorter than the buffer: all of it */
    CHECK(len == 10 && !trunc && memcmp(buf, "0123456789", 10) == 0);
    CHECK(hmi_fs_read_tail(path, buf, 10, &len, &trunc) == HMI_FS_READ_OK && len == 10 && !trunc); /* exactly the size */
    CHECK(hmi_fs_read_tail(path, buf, 4, &len, &trunc) == HMI_FS_READ_OK);                         /* only the end */
    CHECK(len == 4 && trunc && memcmp(buf, "6789", 4) == 0);
    CHECK(hmi_fs_read_tail(path, buf, 0, &len, &trunc) == HMI_FS_READ_OK && len == 0 && trunc);
    CHECK(hmi_fs_read_tail(path, buf, 4, NULL, NULL) == HMI_FS_READ_OK); /* both outputs are optional */

    CHECK(hmi_fs_atomic_write(path, "", 0));
    CHECK(hmi_fs_read_tail(path, buf, sizeof(buf), &len, &trunc) == HMI_FS_READ_OK && len == 0 && !trunc);

    static char big[100000]; /* a file larger than the buffer, with a marker at the end */
    memset(big, 'a', sizeof(big));
    memcpy(big + sizeof(big) - 8, "THE-END\n", 8);
    CHECK(hmi_fs_atomic_write(path, big, sizeof(big)));
    char small[64];
    CHECK(hmi_fs_read_tail(path, small, sizeof(small), &len, &trunc) == HMI_FS_READ_OK);
    CHECK(len == 64 && trunc && memcmp(small + 56, "THE-END\n", 8) == 0);
    remove(path);
}

static void test_wall(void)
{
    SECTION("platform: hmi_wall_ms");
    uint64_t t1 = 0, t2 = 0;
    bool s = true;
    CHECK(hmi_wall_ms(&t1, &s));
    CHECK(t1 > 1735689600000ull); /* later than 2025-01-01 UTC on any machine that can run this test */
    CHECK(t1 < 4102444800000ull); /* earlier than 2100-01-01 UTC */
#ifdef _WIN32
    CHECK(!s); /* the development host never claims a synchronised clock */
#endif
    CHECK(hmi_wall_ms(&t2, &s));
    CHECK(t2 + 5000u >= t1); /* wall time may step, but not by seconds within one test */
}

static void signal_later(void *arg)
{
    hmi_sleep_ms(60);
    hmi_event_signal((hmi_event_t *)arg);
}

static void test_event(void)
{
    SECTION("platform: hmi_event");
    hmi_event_t e;
    CHECK(hmi_event_init(&e));
    uint64_t t0 = hmi_now_ms();
    CHECK(!hmi_event_wait(&e, 40)); /* nothing signalled: times out */
    CHECK(hmi_now_ms() - t0 >= 30u);

    hmi_event_signal(&e);
    CHECK(hmi_event_wait(&e, 0)); /* a signal made before the wait is not lost */
    CHECK(!hmi_event_wait(&e, 0)); /* and it is consumed */

    hmi_event_signal(&e);
    hmi_event_signal(&e);
    CHECK(hmi_event_wait(&e, 0));
    CHECK(!hmi_event_wait(&e, 0)); /* signals do not accumulate */

    hmi_thread_t th;
    CHECK(hmi_thread_start(&th, signal_later, &e));
    t0 = hmi_now_ms();
    CHECK(hmi_event_wait(&e, 5000)); /* woken early by another thread */
    CHECK(hmi_now_ms() - t0 < 3000u);
    CHECK(hmi_thread_join(&th, 5000));
    hmi_event_destroy(&e);
}

int main(void)
{
    test_event();
    test_read_tail();
    test_size_rename();
    test_wall();
    test_mode();
    test_read_all();
    test_random();
    return test_finish();
}
