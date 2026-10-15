/* Tests for hmi_watchdog: when a liveness ping may be sent. Pure logic. */
#include "test_support.h"
#include "../../src/hmi/hmi_watchdog.h"

static void test_ping_cadence(void)
{
    SECTION("watchdog: pings at the interval while both workers beat");
    hmi_watchdog_t w;
    int32_t b[2] = { 0, 0 };
    hmi_watchdog_init(&w, 500, 3000, 1000, b);
    int pings = 0;
    for (uint64_t t = 1000; t <= 3000; t += 10) {
        b[0]++; b[1]++;
        if (hmi_watchdog_step(&w, t, b)) pings++;
    }
    CHECK(pings == 4);                       /* t=1500,2000,2500,3000 */
    CHECK(!hmi_watchdog_stalled(&w, 0, 3000) && !hmi_watchdog_stalled(&w, 1, 3000));
    CHECK(!hmi_watchdog_step(&w, 3100, b));  /* interval not yet elapsed */
}

static void test_stall_withholds(void)
{
    SECTION("watchdog: a stalled worker withholds the ping, recovery resumes it");
    hmi_watchdog_t w;
    int32_t b[2] = { 0, 0 };
    hmi_watchdog_init(&w, 500, 3000, 0, b);
    uint64_t t = 0;
    int pings_before = 0, pings_stalled = 0, pings_after = 0;
    for (; t <= 2000; t += 10) {            /* both alive */
        b[0]++; b[1]++;
        if (hmi_watchdog_step(&w, t, b)) pings_before++;
    }
    for (; t <= 6000; t += 10) {            /* supervisor thread stuck */
        b[0]++;
        if (hmi_watchdog_step(&w, t, b)) pings_stalled++;
    }
    CHECK(pings_before == 4);
    CHECK(hmi_watchdog_stalled(&w, 1, 6000) && !hmi_watchdog_stalled(&w, 0, 6000));
    /* Stall began at t=2000, limit 3000: pings at 2500..5000 (6) are still
     * allowed, because a worker may legitimately be blocked that long. From
     * t>5000 every ping is withheld. */
    CHECK(pings_stalled == 6);
    int late = 0;
    for (uint64_t u = 6000; u <= 9000; u += 10) {
        b[0]++;
        if (hmi_watchdog_step(&w, u, b)) late++;
    }
    CHECK(late == 0);                        /* still stuck: no ping at all */
    t = 9010;
    for (; t <= 11000; t += 10) {            /* worker comes back */
        b[0]++; b[1]++;
        if (hmi_watchdog_step(&w, t, b)) pings_after++;
    }
    CHECK(pings_after >= 3);
}

static void test_exact_limit(void)
{
    SECTION("watchdog: stall boundary is strict");
    hmi_watchdog_t w;
    int32_t b[2] = { 5, 5 };
    hmi_watchdog_init(&w, 100, 1000, 0, b);
    CHECK(!hmi_watchdog_stalled(&w, 0, 1000));   /* exactly at the limit: not stalled */
    CHECK(hmi_watchdog_stalled(&w, 0, 1001));
    CHECK(hmi_watchdog_stalled(&w, 7, 0));       /* invalid channel fails safe */
    CHECK(!hmi_watchdog_stalled(&w, 0, 0));
}

static void test_counter_wrap_and_clock(void)
{
    SECTION("watchdog: counter wrap and a backwards clock");
    hmi_watchdog_t w;
    int32_t b[2] = { INT32_MAX, INT32_MAX };
    hmi_watchdog_init(&w, 100, 1000, 5000, b);
    b[0] = INT32_MIN;                            /* INT32_MAX + 1 wrapped: still a change */
    b[1] = INT32_MIN;
    CHECK(hmi_watchdog_step(&w, 5100, b));
    CHECK(!hmi_watchdog_step(&w, 4000, b));      /* clock went backwards: no ping, no stall verdict */
    CHECK(!hmi_watchdog_stalled(&w, 0, 4000));
}

static void test_disabled(void)
{
    SECTION("watchdog: interval 0 means disabled");
    hmi_watchdog_t w;
    int32_t b[2] = { 0, 0 };
    hmi_watchdog_init(&w, 0, 1000, 0, b);
    CHECK(!hmi_watchdog_step(&w, 100000, b));
}

static void test_env(void)
{
    SECTION("watchdog: systemd environment parsing");
    CHECK(hmi_watchdog_interval_from_env("10000000", NULL, 42) == 5000);   /* 10 s -> 5 s */
    CHECK(hmi_watchdog_interval_from_env("10000000", "42", 42) == 5000);
    CHECK(hmi_watchdog_interval_from_env("10000000", "41", 42) == 0);      /* other process */
    CHECK(hmi_watchdog_interval_from_env("10000000", "", 42) == 5000);
    CHECK(hmi_watchdog_interval_from_env(NULL, NULL, 42) == 0);
    CHECK(hmi_watchdog_interval_from_env("", NULL, 42) == 0);
    CHECK(hmi_watchdog_interval_from_env("0", NULL, 42) == 0);
    CHECK(hmi_watchdog_interval_from_env("abc", NULL, 42) == 0);
    CHECK(hmi_watchdog_interval_from_env("-5", NULL, 42) == 0);
    CHECK(hmi_watchdog_interval_from_env("1500", NULL, 42) == 1);          /* tiny: clamp to 1 ms */
    CHECK(hmi_watchdog_interval_from_env("99999999999999999999999", NULL, 42) == 0); /* overflow */
    CHECK(hmi_watchdog_interval_from_env("10000000", "x", 42) == 0);
    CHECK(hmi_watchdog_interval_from_env("18446744073709551615", NULL, 42) == 0xFFFFFFFFu); /* clamp */
}

int main(void)
{
    test_ping_cadence();
    test_stall_withholds();
    test_exact_limit();
    test_counter_wrap_and_clock();
    test_disabled();
    test_env();
    return test_finish();
}
