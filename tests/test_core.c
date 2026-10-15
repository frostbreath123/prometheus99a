/* Layer 1/2 regression tests: alarms, watchdog, journal, queue, races.
 * Build: make test (Windows; uses a stubbed Layer 0 with a fake clock). */
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include "../include/layer2_core.h"
#include "test_hooks.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <process.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define V_ALL (SENSOR_VALID_TEMP | SENSOR_VALID_FAN | SENSOR_VALID_SSD | SENSOR_VALID_CPU | SENSOR_VALID_RAM)

static void reset(void)
{
    test_set_time(1000);
    test_reset_pets();
    layer0_hardware_init();
    layer2_core_init();
}

static void publish(uint32_t temp_mC, uint16_t cpu, uint16_t ram, uint8_t valid)
{
    layer2_update_state_binary(temp_mC, 0, 1500, cpu * 100u, cpu, ram, 0, 0, valid);
}

/* Publish at 10 ms steps for duration_ms of fake time. */
static void run_for(uint32_t duration_ms, uint32_t temp_mC, uint16_t cpu, uint16_t ram, uint8_t valid)
{
    for (uint32_t t = 0; t < duration_ms; t += 10) {
        test_advance_time(10);
        publish(temp_mC, cpu, ram, valid);
    }
}

static shared_state_buffer_t snap(void)
{
    shared_state_buffer_t s;
    layer2_snapshot_state(&s);
    return s;
}

static void wd_step(int hw_alive, int sw_alive, uint32_t ms)
{
    test_advance_time(ms);
    if (hw_alive) layer2_watchdog_report_hardware_alive();
    if (sw_alive) layer2_watchdog_report_software_alive();
    layer2_watchdog_supervisor_tick();
}

static void test_alarm_debounce_and_raise(void)
{
    puts("alarm debounce / raise");
    reset();
    run_for(200, 40000, 10, 30, V_ALL);
    CHECK(snap().alarm_latch_state == ALARM_STATE_CLEARED);
    run_for(300, 56000, 10, 30, V_ALL); /* shorter than debounce */
    CHECK(snap().alarm_latch_state == ALARM_STATE_CLEARED);
    run_for(400, 56000, 10, 30, V_ALL);
    CHECK(snap().alarm_latch_state == ALARM_STATE_ACTIVE);
    CHECK(snap().alarm_severity == ALARM_CRITICAL);
}

static void test_ack_survives_sensor_updates(void)
{
    puts("ack not overwritten by sensor thread");
    reset();
    run_for(700, 56000, 10, 30, V_ALL);
    layer2_fsm_process_event(KEY_ALARM_ACK);
    CHECK(snap().alarm_latch_state == ALARM_STATE_ACKNOWLEDGED);
    run_for(2000, 56000, 10, 30, V_ALL);
    CHECK(snap().alarm_latch_state == ALARM_STATE_ACKNOWLEDGED);
    run_for(100, 40000, 10, 30, V_ALL);
    CHECK(snap().alarm_latch_state == ALARM_STATE_CLEARED);
}

static void test_ack_without_condition_ignored(void)
{
    puts("ack with no alarm is ignored");
    reset();
    run_for(100, 40000, 10, 30, V_ALL);
    layer2_fsm_process_event(KEY_ALARM_ACK);
    CHECK(snap().alarm_latch_state == ALARM_STATE_CLEARED);
    alarm_journal_t j;
    layer2_get_alarm_journal_copy(&j);
    CHECK(j.count == 0);
}

static void test_hysteresis_no_chatter(void)
{
    puts("hysteresis suppresses chatter");
    reset();
    run_for(700, 56000, 10, 30, V_ALL);
    alarm_journal_t j;
    layer2_get_alarm_journal_copy(&j);
    uint8_t before = j.count;
    for (int i = 0; i < 50; i++) { /* oscillate between 54 C and 56 C */
        run_for(20, (i & 1) ? 54000 : 56000, 10, 30, V_ALL);
    }
    layer2_get_alarm_journal_copy(&j);
    CHECK(snap().alarm_severity == ALARM_CRITICAL);
    CHECK(j.count == before);
}

static void test_escalation_reactivates(void)
{
    puts("escalation after ack re-activates");
    reset();
    run_for(700, 49000, 10, 30, V_ALL);
    CHECK(snap().alarm_severity == ALARM_WARNING);
    layer2_fsm_process_event(KEY_ALARM_ACK);
    CHECK(snap().alarm_latch_state == ALARM_STATE_ACKNOWLEDGED);
    run_for(700, 57000, 10, 30, V_ALL);
    CHECK(snap().alarm_severity == ALARM_CRITICAL);
    CHECK(snap().alarm_latch_state == ALARM_STATE_ACTIVE);
}

static void test_invalid_sensor_never_alarms(void)
{
    puts("invalid temp channel cannot raise an alarm");
    reset();
    run_for(1500, 99000, 10, 30, (uint8_t)(V_ALL & ~SENSOR_VALID_TEMP));
    CHECK(snap().alarm_latch_state == ALARM_STATE_CLEARED);
}

static void test_test_alarm_persists(void)
{
    puts("test alarm persists until toggled off");
    reset();
    layer2_fsm_process_event(KEY_TEST_ALARM);
    CHECK(snap().alarm_latch_state == ALARM_STATE_ACTIVE);
    run_for(1000, 30000, 5, 20, V_ALL);
    CHECK(snap().alarm_latch_state == ALARM_STATE_ACTIVE);
    layer2_fsm_process_event(KEY_TEST_ALARM);
    run_for(50, 30000, 5, 20, V_ALL);
    CHECK(snap().alarm_latch_state == ALARM_STATE_CLEARED);
}

static void test_journal_wraps(void)
{
    puts("journal wraps without overflow");
    reset();
    for (int i = 0; i < ALARM_LOG_CAPACITY * 2 + 3; i++) {
        layer2_fsm_process_event(KEY_TEST_ALARM);
    }
    alarm_journal_t j;
    layer2_get_alarm_journal_copy(&j);
    CHECK(j.count == ALARM_LOG_CAPACITY);
    CHECK(j.head < ALARM_LOG_CAPACITY);
}

static void test_watchdog_stall_and_recovery(void)
{
    puts("watchdog: stall detect, failover, recovery");
    reset();
    for (int i = 0; i < 20; i++) wd_step(1, 1, 50);
    watchdog_supervisor_t w;
    layer2_get_watchdog_status_copy(&w);
    CHECK(w.system_healthy);
    CHECK(test_get_pets() > 0);

    for (int i = 0; i < 20; i++) wd_step(0, 1, 50); /* sensor loop stalls */
    layer2_get_watchdog_status_copy(&w);
    CHECK(!w.hw_loop_alive);
    CHECK(!w.system_healthy);
    CHECK(w.watchdog_fault_count == 1); /* counted once, not per tick */
    CHECK(layer2_fsm_get_active_screen() == SCREEN_FAILOVER_STANDBY);
    CHECK(snap().failover_active == 1);

    for (int i = 0; i < 10; i++) wd_step(1, 1, 50);
    layer2_get_watchdog_status_copy(&w);
    CHECK(w.system_healthy);
    CHECK(w.watchdog_fault_count == 1);
}

static void test_ui_stall_detected(void)
{
    puts("watchdog: UI stall detected from sensor side");
    reset();
    for (int i = 0; i < 20; i++) wd_step(1, 0, 50);
    watchdog_supervisor_t w;
    layer2_get_watchdog_status_copy(&w);
    CHECK(!w.sw_loop_alive);
    CHECK(!w.system_healthy);
}

static void test_simulated_fault_self_recovers(void)
{
    puts("simulated watchdog fault is transient");
    reset();
    layer2_fsm_process_event(KEY_SIMULATE_WD_FAULT);
    for (int i = 0; i < 6; i++) wd_step(1, 1, 100);
    watchdog_supervisor_t w;
    layer2_get_watchdog_status_copy(&w);
    CHECK(!w.system_healthy);
    for (int i = 0; i < 20; i++) wd_step(1, 1, 100);
    layer2_get_watchdog_status_copy(&w);
    CHECK(w.system_healthy);
}

static void test_input_queue(void)
{
    puts("input queue capacity and drop counter");
    reset();
    layer1_hal_init();
    for (int i = 0; i < INPUT_QUEUE_CAPACITY + 5; i++) layer1_queue_key(KEY_NEXT);
    int n = 0;
    input_key_t k;
    while (layer1_poll_button_event(&k)) n++;
    CHECK(n == INPUT_QUEUE_CAPACITY - 1);
    CHECK(layer1_get_dropped_key_count() == 6);
}

static void test_fsm_bounds(void)
{
    puts("fsm rejects invalid screen");
    reset();
    CHECK(!layer2_fsm_request_screen_change((screen_id_t)SCREEN_COUNT));
    CHECK(!layer2_fsm_request_screen_change((screen_id_t)99));
    CHECK(layer2_fsm_get_active_screen() == SCREEN_DASHBOARD);
}

static void test_checksum_valid(void)
{
    puts("snapshot checksum valid after updates and events");
    reset();
    shared_state_buffer_t s;
    publish(40000, 10, 30, V_ALL);
    CHECK(layer2_snapshot_state(&s));
    layer2_fsm_process_event(KEY_NEXT);
    CHECK(layer2_snapshot_state(&s));
    CHECK(layer2_consume_dirty_flag());
    CHECK(layer2_snapshot_state(&s));
}

static void test_dirty_only_on_change(void)
{
    puts("dirty flag only when telemetry changes");
    reset();
    publish(40000, 10, 30, V_ALL);
    CHECK(layer2_consume_dirty_flag());
    test_advance_time(1);
    publish(40000, 10, 30, V_ALL);
    CHECK(!layer2_consume_dirty_flag());
    publish(40500, 10, 30, V_ALL);
    CHECK(layer2_consume_dirty_flag());
}

/* ---- concurrency stress: writer + FSM threads vs snapshot reader ---- */
static volatile LONG g_stop = 0;

static unsigned __stdcall writer_thread(void *a)
{
    (void)a;
    uint32_t i = 0;
    while (!InterlockedCompareExchange(&g_stop, 1, 1)) {
        test_advance_time(1);
        layer2_update_state_binary(40000 + (i % 5000), i, i * 3u, i * 7u, (uint16_t)(i % 100),
                                   (uint16_t)(i % 90), i, i, V_ALL);
        layer2_watchdog_report_hardware_alive();
        layer2_watchdog_supervisor_tick();
        i++;
    }
    return 0;
}

static unsigned __stdcall fsm_thread(void *a)
{
    (void)a;
    while (!InterlockedCompareExchange(&g_stop, 1, 1)) {
        layer2_fsm_process_event(KEY_NEXT);
        layer2_fsm_process_event(KEY_ALARM_ACK);
        layer2_fsm_process_event(KEY_TEST_ALARM);
        layer2_watchdog_report_software_alive();
    }
    return 0;
}

static void test_concurrent_snapshot_consistency(void)
{
    puts("concurrent publish/FSM vs snapshot: no torn state");
    reset();
    g_stop = 0;
    HANDLE h[2];
    h[0] = (HANDLE)_beginthreadex(NULL, 0, writer_thread, NULL, 0, NULL);
    h[1] = (HANDLE)_beginthreadex(NULL, 0, fsm_thread, NULL, 0, NULL);
    DWORD end = GetTickCount() + 1500;
    long reads = 0, bad = 0;
    while (GetTickCount() < end) {
        shared_state_buffer_t s;
        if (!layer2_snapshot_state(&s)) bad++;
        reads++;
    }
    InterlockedExchange(&g_stop, 1);
    WaitForMultipleObjects(2, h, TRUE, 5000);
    CloseHandle(h[0]);
    CloseHandle(h[1]);
    printf("  %ld snapshots, bad=%ld\n", reads, bad);
    CHECK(bad == 0);
    CHECK(reads > 1000);
}

int main(void)
{
    test_alarm_debounce_and_raise();
    test_ack_survives_sensor_updates();
    test_ack_without_condition_ignored();
    test_hysteresis_no_chatter();
    test_escalation_reactivates();
    test_invalid_sensor_never_alarms();
    test_test_alarm_persists();
    test_journal_wraps();
    test_watchdog_stall_and_recovery();
    test_ui_stall_detected();
    test_simulated_fault_self_recovers();
    test_input_queue();
    test_fsm_bounds();
    test_checksum_valid();
    test_dirty_only_on_change();
    test_concurrent_snapshot_consistency();
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
