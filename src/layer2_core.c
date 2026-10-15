/**
 * @file layer2_core.c
 * @brief Layer 2: Core State Management, FSM, Alarms & Watchdog Supervisor
 *
 * Concurrency model:
 *   Two threads touch this layer: the sensor thread (publish + watchdog tick)
 *   and the UI thread (snapshot, FSM events, watchdog tick). Everything below
 *   is protected by one recursive mutex (layer0_state_lock). Critical sections
 *   are short, bounded and never allocate, log to console or block.
 *
 * Alarm model (per ISA-18.2 style latching):
 *   level   : NONE / WARNING / CRITICAL computed from valid sensors with
 *             enter/leave hysteresis and a raise debounce.
 *   latch   : CLEARED -> ACTIVE (level raised)
 *             ACTIVE -> ACKNOWLEDGED (operator ack, only while level > NONE)
 *             ACKNOWLEDGED -> ACTIVE (level escalated above the acked level)
 *             ACTIVE / ACKNOWLEDGED -> CLEARED (level returned to NONE)
 *   Invalid sensor channels never raise or hold an alarm.
 *   KEY_TEST_ALARM forces a CRITICAL level until pressed again.
 *
 * Integrity: the shared snapshot carries a CRC-16/CCITT-FALSE. It detects
 * memory corruption; it is NOT a synchronisation mechanism (the mutex is).
 */

#include "../include/layer2_core.h"
#include "../include/layer0_hardware.h"
#include <string.h>

#define STATE_MAGIC 0x50524F4Du /* 'PROM' */

static shared_state_buffer_t s_state;
static watchdog_supervisor_t s_wd;
static telemetry_ring_buffer_t s_trend;
static alarm_journal_t s_journal;

static screen_id_t s_active_screen = SCREEN_DASHBOARD;
static screen_id_t s_previous_screen = SCREEN_DASHBOARD;

static alarm_latch_state_t s_latch = ALARM_STATE_CLEARED;
static alarm_level_t s_level = ALARM_NONE;       /* current hysteresis-filtered level */
static alarm_level_t s_acked_level = ALARM_NONE; /* level the operator acknowledged */
static alarm_level_t s_pending_level = ALARM_NONE;
static uint64_t s_pending_since_ms = 0;
static bool s_test_forced = false;

static uint64_t s_last_trend_push_ms = 0;
static uint64_t s_last_wd_eval_ms = 0;
static uint64_t s_wd_inject_until_ms = 0;
static bool s_wd_fault_latched = false;

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). */
static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Must hold lock. Recompute integrity word after any mutation. */
static void seal_locked(void)
{
    s_state.magic_header = STATE_MAGIC;
    s_state.checksum = crc16((const uint8_t *)&s_state, offsetof(shared_state_buffer_t, checksum));
}

static void mark_dirty_locked(void)
{
    s_state.dirty_flag = 1;
    seal_locked();
}

static void push_log_locked(uint64_t ts, uint8_t severity, uint8_t latch, const char *desc)
{
    alarm_log_entry_t *e = &s_journal.entries[s_journal.head];
    e->timestamp_ms = ts;
    e->severity = severity;
    e->latch_state = latch;
    memset(e->description, 0, sizeof(e->description));
    if (desc) {
        size_t n = strlen(desc);
        if (n >= sizeof(e->description)) n = sizeof(e->description) - 1;
        memcpy(e->description, desc, n);
    }
    /* Oldest entry is overwritten when full (documented ring behaviour). */
    s_journal.head = (uint8_t)((s_journal.head + 1) % ALARM_LOG_CAPACITY);
    if (s_journal.count < ALARM_LOG_CAPACITY) s_journal.count++;
}

static void set_failover_locked(bool on)
{
    s_state.failover_active = on ? 1 : 0;
}

static bool change_screen_locked(screen_id_t new_screen)
{
    if (new_screen >= SCREEN_COUNT) return false;
    if (s_active_screen == new_screen) return true;
    s_previous_screen = s_active_screen;
    s_active_screen = new_screen;
    s_state.active_screen = (uint8_t)new_screen;
    mark_dirty_locked();
    return true;
}

/* Level with hysteresis: thresholds to stay in a level are lower than to enter. */
static alarm_level_t compute_level(alarm_level_t cur, uint8_t valid, uint32_t temp_mC,
                                   uint16_t cpu, uint16_t ram)
{
    bool t = (valid & SENSOR_VALID_TEMP) != 0;
    bool c = (valid & SENSOR_VALID_CPU) != 0;
    bool r = (valid & SENSOR_VALID_RAM) != 0;

    bool crit_enter = (t && temp_mC >= ALARM_CRIT_TEMP_MC_ENTER) || (c && cpu >= ALARM_CRIT_LOAD_ENTER);
    bool crit_stay  = (t && temp_mC >= ALARM_CRIT_TEMP_MC_LEAVE) || (c && cpu >= ALARM_CRIT_LOAD_LEAVE);
    bool warn_enter = (t && temp_mC >= ALARM_WARN_TEMP_MC_ENTER) || (c && cpu >= ALARM_WARN_LOAD_ENTER) ||
                      (r && ram >= ALARM_WARN_RAM_ENTER);
    bool warn_stay  = (t && temp_mC >= ALARM_WARN_TEMP_MC_LEAVE) || (c && cpu >= ALARM_WARN_LOAD_LEAVE) ||
                      (r && ram >= ALARM_WARN_RAM_LEAVE);

    if (crit_enter || (cur == ALARM_CRITICAL && crit_stay)) return ALARM_CRITICAL;
    if (warn_enter || (cur >= ALARM_WARNING && (warn_stay || crit_stay))) return ALARM_WARNING;
    return ALARM_NONE;
}

static const char *level_text(alarm_level_t lvl, bool test)
{
    if (test) return "TEST ALARM";
    if (lvl == ALARM_CRITICAL) return "CRIT: TEMP/LOAD";
    if (lvl == ALARM_WARNING) return "WARN: TEMP/LOAD/RAM";
    return "RETURN TO NORMAL";
}

/* Must hold lock. Advances the latch FSM for the supplied raw (unfiltered) level. */
static void alarm_step_locked(uint64_t now, alarm_level_t raw_level)
{
    if (s_test_forced) raw_level = ALARM_CRITICAL;

    /* Debounce raising/escalating; dropping is immediate (hysteresis guards chatter). */
    if (raw_level > s_level) {
        if (s_pending_level != raw_level) {
            s_pending_level = raw_level;
            s_pending_since_ms = now;
        }
        if ((now - s_pending_since_ms) >= ALARM_DEBOUNCE_MS || s_test_forced) {
            s_level = raw_level;
            s_pending_level = ALARM_NONE;
        }
    } else {
        s_pending_level = ALARM_NONE;
        s_level = raw_level;
    }

    alarm_latch_state_t prev = s_latch;
    if (s_level == ALARM_NONE) {
        if (s_latch != ALARM_STATE_CLEARED) {
            s_latch = ALARM_STATE_CLEARED;
            s_acked_level = ALARM_NONE;
            push_log_locked(now, ALARM_NONE, ALARM_STATE_CLEARED,
                            prev == ALARM_STATE_ACKNOWLEDGED ? "CLEARED AFTER ACK" : "AUTO-CLEARED");
        }
    } else if (s_latch == ALARM_STATE_CLEARED) {
        s_latch = ALARM_STATE_ACTIVE;
        push_log_locked(now, s_level, ALARM_STATE_ACTIVE, level_text(s_level, s_test_forced));
    } else if (s_latch == ALARM_STATE_ACKNOWLEDGED && s_level > s_acked_level) {
        s_latch = ALARM_STATE_ACTIVE;
        push_log_locked(now, s_level, ALARM_STATE_ACTIVE, "ESCALATED");
    }

    if (s_state.alarm_severity != (uint8_t)s_level || s_state.alarm_latch_state != (uint8_t)s_latch) {
        s_state.alarm_severity = (uint8_t)s_level;
        s_state.alarm_latch_state = (uint8_t)s_latch;
        s_state.dirty_flag = 1;
    }
}

void layer2_core_init(void)
{
    uint64_t now = layer0_get_system_time_ms();
    layer0_state_lock();

    memset(&s_state, 0, sizeof(s_state));
    memset(&s_wd, 0, sizeof(s_wd));
    memset(&s_trend, 0, sizeof(s_trend));
    memset(&s_journal, 0, sizeof(s_journal));

    s_state.active_screen = (uint8_t)SCREEN_DASHBOARD;
    s_state.dirty_flag = 1;
    s_state.timestamp_ms = now;

    s_wd.hw_loop_alive = true;
    s_wd.sw_loop_alive = true;
    s_wd.system_healthy = true;
    s_wd.last_hw_ping_ms = now;
    s_wd.last_sw_ping_ms = now;

    s_active_screen = SCREEN_DASHBOARD;
    s_previous_screen = SCREEN_DASHBOARD;
    s_latch = ALARM_STATE_CLEARED;
    s_level = ALARM_NONE;
    s_acked_level = ALARM_NONE;
    s_pending_level = ALARM_NONE;
    s_pending_since_ms = 0;
    s_test_forced = false;
    s_last_trend_push_ms = now;
    s_last_wd_eval_ms = now;
    s_wd_inject_until_ms = 0;
    s_wd_fault_latched = false;

    seal_locked();
    layer0_state_unlock();
}

void layer2_update_state_binary(uint32_t temp_mC, uint32_t press_kPa, uint32_t rpm, uint32_t bus_mv,
                                uint16_t cpu_load_pct, uint16_t ram_load_pct,
                                uint32_t ssd_read_kb_s, uint32_t ssd_write_kb_s,
                                uint8_t valid_flags)
{
    uint64_t now = layer0_get_system_time_ms();
    layer0_state_lock();

    bool changed = s_state.sensor_temp_mC != temp_mC || s_state.sensor_pressure_kPa != press_kPa ||
                   s_state.sensor_rpm != rpm || s_state.sensor_bus_mv != bus_mv ||
                   s_state.ssd_read_kb_s != ssd_read_kb_s || s_state.ssd_write_kb_s != ssd_write_kb_s ||
                   s_state.cpu_load_pct != cpu_load_pct || s_state.ram_load_pct != ram_load_pct ||
                   s_state.sensor_valid_flags != valid_flags;

    s_state.timestamp_ms = now;
    s_state.sensor_temp_mC = temp_mC;
    s_state.sensor_pressure_kPa = press_kPa;
    s_state.sensor_rpm = rpm;
    s_state.sensor_bus_mv = bus_mv;
    s_state.ssd_read_kb_s = ssd_read_kb_s;
    s_state.ssd_write_kb_s = ssd_write_kb_s;
    s_state.cpu_load_pct = cpu_load_pct;
    s_state.ram_load_pct = ram_load_pct;
    s_state.sensor_valid_flags = valid_flags;
    s_state.heartbeat_counter++;
    if (changed) s_state.dirty_flag = 1; /* telemetry unchanged => nothing to redraw */

    alarm_level_t raw = compute_level(s_level, valid_flags, temp_mC, cpu_load_pct, ram_load_pct);
    alarm_step_locked(now, raw);
    seal_locked();

    /* Trend: decimate to one sample per TREND_DECIMATE_MS. */
    if ((now - s_last_trend_push_ms) >= TREND_DECIMATE_MS) {
        s_last_trend_push_ms = now;
        telemetry_sample_t *smp = &s_trend.samples[s_trend.head];
        smp->temp_mC = (valid_flags & SENSOR_VALID_TEMP) ? temp_mC : 0;
        smp->pressure_kPa = press_kPa;
        smp->rpm = rpm;
        smp->bus_mv = bus_mv;
        s_trend.head = (uint16_t)((s_trend.head + 1) % TREND_HISTORY_SAMPLES);
        if (s_trend.count < TREND_HISTORY_SAMPLES) s_trend.count++;
    }

    layer0_state_unlock();
}

bool layer2_snapshot_state(shared_state_buffer_t *out)
{
    if (!out) return false;
    layer0_state_lock();
    *out = s_state;
    layer0_state_unlock();
    uint16_t crc = crc16((const uint8_t *)out, offsetof(shared_state_buffer_t, checksum));
    return out->magic_header == STATE_MAGIC && crc == out->checksum;
}

bool layer2_consume_dirty_flag(void)
{
    bool was;
    layer0_state_lock();
    was = s_state.dirty_flag != 0;
    if (was) {
        s_state.dirty_flag = 0;
        seal_locked();
    }
    layer0_state_unlock();
    return was;
}

screen_id_t layer2_fsm_get_active_screen(void)
{
    screen_id_t s;
    layer0_state_lock();
    s = s_active_screen;
    layer0_state_unlock();
    return s;
}

bool layer2_fsm_request_screen_change(screen_id_t new_screen)
{
    bool ok;
    layer0_state_lock();
    ok = change_screen_locked(new_screen);
    layer0_state_unlock();
    return ok;
}

bool layer2_test_alarm_forced(void)
{
    bool f;
    layer0_state_lock();
    f = s_test_forced;
    layer0_state_unlock();
    return f;
}

static void navigate_locked(input_key_t key)
{
    switch (s_active_screen) {
        case SCREEN_DASHBOARD:
            if (key == KEY_NEXT) change_screen_locked(SCREEN_DIAGNOSTICS);
            else if (key == KEY_PREV) change_screen_locked(SCREEN_FAILOVER_STANDBY);
            else if (key == KEY_SELECT) change_screen_locked(SCREEN_ALARM);
            break;
        case SCREEN_DIAGNOSTICS:
            if (key == KEY_NEXT) change_screen_locked(SCREEN_ALARM);
            else if (key == KEY_PREV || key == KEY_BACK) change_screen_locked(SCREEN_DASHBOARD);
            break;
        case SCREEN_ALARM:
            if (key == KEY_NEXT) change_screen_locked(SCREEN_SETTINGS);
            else if (key == KEY_PREV) change_screen_locked(SCREEN_DIAGNOSTICS);
            else if (key == KEY_BACK) change_screen_locked(SCREEN_DASHBOARD);
            break;
        case SCREEN_SETTINGS:
            if (key == KEY_NEXT) change_screen_locked(SCREEN_FAILOVER_STANDBY);
            else if (key == KEY_PREV) change_screen_locked(SCREEN_ALARM);
            else if (key == KEY_BACK) change_screen_locked(SCREEN_DASHBOARD);
            break;
        case SCREEN_FAILOVER_STANDBY:
            if (key == KEY_NEXT || key == KEY_BACK || key == KEY_SELECT) change_screen_locked(SCREEN_DASHBOARD);
            else if (key == KEY_PREV) change_screen_locked(SCREEN_SETTINGS);
            break;
        default:
            break;
    }
}

void layer2_fsm_process_event(input_key_t key)
{
    if (key == KEY_NONE) return;
    uint64_t now = layer0_get_system_time_ms();

    layer0_state_lock();
    switch (key) {
        case KEY_TOGGLE_FAILOVER:
            if (s_active_screen == SCREEN_FAILOVER_STANDBY) {
                set_failover_locked(!s_state.failover_active);
            } else {
                change_screen_locked(SCREEN_FAILOVER_STANDBY);
                set_failover_locked(true);
            }
            mark_dirty_locked();
            break;

        case KEY_SIMULATE_WD_FAULT:
            /* Injected stall of the hardware loop; supervisor sees it as a real
             * heartbeat timeout and recovers on its own when it expires. */
            s_wd_inject_until_ms = now + (2u * WATCHDOG_MAX_AGE_MS);
            break;

        case KEY_TEST_ALARM:
            s_test_forced = !s_test_forced;
            /* Apply immediately so the operator sees the result without
             * waiting for the next sensor sample. */
            alarm_step_locked(now, compute_level(s_level, s_state.sensor_valid_flags,
                              s_state.sensor_temp_mC, s_state.cpu_load_pct, s_state.ram_load_pct));
            mark_dirty_locked();
            break;

        case KEY_ALARM_ACK:
            /* Ack only while an alarm condition is present and unacknowledged. */
            if (s_latch == ALARM_STATE_ACTIVE && s_level > ALARM_NONE) {
                s_latch = ALARM_STATE_ACKNOWLEDGED;
                s_acked_level = s_level;
                s_state.alarm_latch_state = (uint8_t)s_latch;
                push_log_locked(now, (uint8_t)s_level, ALARM_STATE_ACKNOWLEDGED, "ACKNOWLEDGED");
                mark_dirty_locked();
            }
            break;

        default:
            navigate_locked(key);
            break;
    }
    layer0_state_unlock();
}

void layer2_watchdog_report_software_alive(void)
{
    uint64_t now = layer0_get_system_time_ms();
    layer0_state_lock();
    s_wd.last_sw_ping_ms = now;
    layer0_state_unlock();
}

void layer2_watchdog_report_hardware_alive(void)
{
    uint64_t now = layer0_get_system_time_ms();
    layer0_state_lock();
    s_wd.last_hw_ping_ms = now;
    layer0_state_unlock();
}

/* Called from BOTH the sensor thread and the UI thread. Each side therefore
 * detects a stall of the other; a single thread can never judge its own death. */
void layer2_watchdog_supervisor_tick(void)
{
    uint64_t now = layer0_get_system_time_ms();
    layer0_state_lock();

    if ((now - s_last_wd_eval_ms) < WATCHDOG_CHECK_PERIOD_MS) {
        layer0_state_unlock();
        return;
    }
    s_last_wd_eval_ms = now;

    bool injected = now < s_wd_inject_until_ms;
    s_wd.hw_loop_alive = !injected && ((now - s_wd.last_hw_ping_ms) <= WATCHDOG_MAX_AGE_MS);
    s_wd.sw_loop_alive = ((now - s_wd.last_sw_ping_ms) <= WATCHDOG_MAX_AGE_MS);
    s_wd.system_healthy = s_wd.hw_loop_alive && s_wd.sw_loop_alive;

    if (s_wd.system_healthy) {
        layer0_pet_hardware_watchdog();
        s_wd.total_watchdog_pets++;
        if (s_wd_fault_latched) {
            s_wd_fault_latched = false;
            push_log_locked(now, ALARM_INFO, (uint8_t)s_latch, "WATCHDOG RECOVERED");
            mark_dirty_locked();
        }
    } else if (!s_wd_fault_latched) {
        /* Edge: first detection of the fault. Log once, engage failover once. */
        s_wd_fault_latched = true;
        s_wd.watchdog_fault_count++;
        push_log_locked(now, ALARM_EMERGENCY, (uint8_t)s_latch,
                        !s_wd.hw_loop_alive ? "WD FAULT: SENSOR LOOP" : "WD FAULT: UI LOOP");
        change_screen_locked(SCREEN_FAILOVER_STANDBY);
        set_failover_locked(true);
        mark_dirty_locked();
    }
    layer0_state_unlock();
}

void layer2_get_watchdog_status_copy(watchdog_supervisor_t *out)
{
    if (!out) return;
    layer0_state_lock();
    *out = s_wd;
    layer0_state_unlock();
}

void layer2_get_trend_copy(telemetry_ring_buffer_t *out)
{
    if (!out) return;
    layer0_state_lock();
    *out = s_trend;
    layer0_state_unlock();
}

void layer2_get_alarm_journal_copy(alarm_journal_t *out)
{
    if (!out) return;
    layer0_state_lock();
    *out = s_journal;
    layer0_state_unlock();
}
