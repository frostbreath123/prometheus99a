/**
 * @file layer2_core.c
 * @brief Layer 2: Core State Management, Binary Serialization, FSM & Watchdog Implementation
 *
 * Changes from original:
 *   - Lock-free ping-pong buffer: ISR writes to inactive slot, atomically
 *     flips write index. UI reads from latest completed slot. Eliminates
 *     torn reads of 64-bit timestamp and multi-field telemetry.
 *   - Alarm latching state machine: prevents ISR from overwriting user ACK.
 *     CLEARED -> ACTIVE (threshold exceeded), ACTIVE -> ACKNOWLEDGED (user ACK),
 *     ACKNOWLEDGED -> CLEARED (threshold returns to normal).
 *   - Telemetry ring buffer: decimates 1000 Hz to 1 sample/sec, stores last
 *     60 samples for trend graphs (~960 bytes static).
 *   - Alarm journal: logs alarm events with timestamps (768 bytes static).
 *   - Struct alignment: no more #pragma pack(1), natural alignment.
 *   - Fixed checksum: computed over correct payload.
 */

#include "../include/layer2_core.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include <stdio.h>
#include <string.h>

/* --- Lock-Free Ping-Pong Buffer (Zero Mutex, Zero Latency) ---
 * The ISR thread writes to buffers[write_idx], then flips write_idx.
 * The UI thread reads from buffers[write_idx] (latest complete frame).
 * volatile ensures compiler doesn't reorder/cache the index. */
static shared_state_buffer_t s_ping_pong[2];
static volatile uint32_t s_write_idx = 0;

/* Convenience macro for the buffer the ISR is currently writing to */
#define WRITE_BUF  (s_ping_pong[s_write_idx])
#define READ_IDX   (s_write_idx)  /* UI reads from latest completed write */

static watchdog_supervisor_t s_watchdog_supervisor;

/* Static FSM state */
static screen_id_t s_active_screen = SCREEN_DASHBOARD;
static screen_id_t s_previous_screen = SCREEN_DASHBOARD;

/* Alarm latching state (persists across ISR cycles) */
static alarm_latch_state_t s_alarm_latch = ALARM_STATE_CLEARED;

/* Telemetry trend ring buffer */
static telemetry_ring_buffer_t s_trend_buffer;
static uint64_t s_last_trend_push_ms = 0;

/* Alarm journal */
static alarm_journal_t s_alarm_journal;

/* Binary checksum calculation for data integrity */
static uint16_t compute_binary_checksum(const uint8_t *data, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint16_t)(sum + data[i]);
    }
    return sum;
}

/* Push an alarm event to the journal */
static void push_alarm_log(uint64_t timestamp, uint8_t severity, uint8_t latch, const char *desc)
{
    alarm_log_entry_t *entry = &s_alarm_journal.entries[s_alarm_journal.head];
    entry->timestamp_ms = timestamp;
    entry->severity = severity;
    entry->latch_state = latch;
    memset(entry->description, 0, sizeof(entry->description));
    if (desc) {
        size_t len = strlen(desc);
        if (len >= sizeof(entry->description)) len = sizeof(entry->description) - 1;
        memcpy(entry->description, desc, len);
    }
    s_alarm_journal.head = (s_alarm_journal.head + 1) % ALARM_LOG_CAPACITY;
    if (s_alarm_journal.count < ALARM_LOG_CAPACITY) {
        s_alarm_journal.count++;
    }
}

void layer2_core_init(void)
{
    memset(s_ping_pong, 0, sizeof(s_ping_pong));
    memset(&s_watchdog_supervisor, 0, sizeof(s_watchdog_supervisor));
    memset(&s_trend_buffer, 0, sizeof(s_trend_buffer));
    memset(&s_alarm_journal, 0, sizeof(s_alarm_journal));

    s_write_idx = 0;

    /* Initialize both ping-pong slots */
    for (int i = 0; i < 2; i++) {
        s_ping_pong[i].magic_header = 0x50524F4D; /* 'PROM' */
        s_ping_pong[i].active_screen = (uint8_t)SCREEN_DASHBOARD;
        s_ping_pong[i].dirty_flag = 1;
        s_ping_pong[i].heartbeat_counter = 0;
        s_ping_pong[i].alarm_latch_state = (uint8_t)ALARM_STATE_CLEARED;
    }

    s_watchdog_supervisor.hw_loop_alive = true;
    s_watchdog_supervisor.sw_loop_alive = true;
    s_watchdog_supervisor.system_healthy = true;
    s_watchdog_supervisor.last_hw_ping_ms = layer0_get_system_time_ms();
    s_watchdog_supervisor.last_sw_ping_ms = layer0_get_system_time_ms();

    s_active_screen = SCREEN_DASHBOARD;
    s_previous_screen = SCREEN_DASHBOARD;
    s_alarm_latch = ALARM_STATE_CLEARED;
    s_last_trend_push_ms = layer0_get_system_time_ms();

    printf("[L2] Core Ready\n");
}

void layer2_update_state_binary(uint32_t temp_mC, uint32_t press_kPa, uint32_t rpm, uint32_t bus_mv,
                                uint16_t cpu_load_pct, uint16_t ram_load_pct,
                                uint32_t ssd_read_kb_s, uint32_t ssd_write_kb_s)
{
    uint64_t now = layer0_get_system_time_ms();

    /* Write to the INACTIVE buffer slot (the one UI is NOT reading) */
    uint32_t next_idx = 1 - s_write_idx;
    shared_state_buffer_t *buf = &s_ping_pong[next_idx];

    /* Copy forward persistent fields from current active buffer */
    buf->magic_header = 0x50524F4D;
    buf->timestamp_ms = now;
    buf->sensor_temp_mC = temp_mC;
    buf->sensor_pressure_kPa = press_kPa;
    buf->sensor_rpm = rpm;
    buf->sensor_bus_mv = bus_mv;
    buf->ssd_read_kb_s = ssd_read_kb_s;
    buf->ssd_write_kb_s = ssd_write_kb_s;
    buf->cpu_load_pct = cpu_load_pct;
    buf->ram_load_pct = ram_load_pct;
    buf->heartbeat_counter = s_ping_pong[s_write_idx].heartbeat_counter + 1;
    buf->dirty_flag = 1;
    buf->active_screen = (uint8_t)s_active_screen;
    buf->failover_active = s_ping_pong[s_write_idx].failover_active;

    /* --- Alarm Latching State Machine ---
     * Calibrated for Live Host Metrics:
     * Critical Trip: CPU Temp >= 55 C OR CPU Load >= 80%
     * Warning:       CPU Temp >= 48 C OR CPU Load >= 65% OR System RAM >= 90% */
    bool over_threshold = (temp_mC >= 55000 || cpu_load_pct >= 80);
    bool warn_threshold = (temp_mC >= 48000 || cpu_load_pct >= 65 || ram_load_pct >= 90);

    switch (s_alarm_latch) {
        case ALARM_STATE_CLEARED:
            if (over_threshold) {
                s_alarm_latch = ALARM_STATE_ACTIVE;
                buf->alarm_severity = (uint8_t)ALARM_CRITICAL;
                push_alarm_log(now, ALARM_CRITICAL, ALARM_STATE_ACTIVE, "TRIP: CPU >= 55C / 80%");
            } else if (warn_threshold) {
                s_alarm_latch = ALARM_STATE_ACTIVE;
                buf->alarm_severity = (uint8_t)ALARM_WARNING;
                push_alarm_log(now, ALARM_WARNING, ALARM_STATE_ACTIVE, "WARN: HIGH CPU LOAD");
            } else {
                buf->alarm_severity = (uint8_t)ALARM_NONE;
            }
            break;

        case ALARM_STATE_ACTIVE:
            /* Stay active — severity may escalate but latch stays ACTIVE */
            if (over_threshold) {
                buf->alarm_severity = (uint8_t)ALARM_CRITICAL;
            } else if (warn_threshold) {
                buf->alarm_severity = (uint8_t)ALARM_WARNING;
            } else {
                /* Condition cleared before user acknowledged — auto-clear */
                s_alarm_latch = ALARM_STATE_CLEARED;
                buf->alarm_severity = (uint8_t)ALARM_NONE;
                push_alarm_log(now, ALARM_NONE, ALARM_STATE_CLEARED, "AUTO-CLEARED");
            }
            break;

        case ALARM_STATE_ACKNOWLEDGED:
            /* User has acknowledged. Stay acknowledged until condition clears. */
            if (over_threshold) {
                buf->alarm_severity = (uint8_t)ALARM_CRITICAL;
                /* Stay ACKNOWLEDGED — do NOT re-activate */
            } else if (warn_threshold) {
                buf->alarm_severity = (uint8_t)ALARM_WARNING;
            } else {
                /* Condition cleared AND was acknowledged — fully clear */
                s_alarm_latch = ALARM_STATE_CLEARED;
                buf->alarm_severity = (uint8_t)ALARM_NONE;
                push_alarm_log(now, ALARM_NONE, ALARM_STATE_CLEARED, "CLEARED AFTER ACK");
            }
            break;
    }
    buf->alarm_latch_state = (uint8_t)s_alarm_latch;

    /* Compute checksum over payload (excluding checksum field itself) */
    buf->checksum = 0;
    size_t payload_len = offsetof(shared_state_buffer_t, checksum);
    buf->checksum = compute_binary_checksum((const uint8_t*)buf, payload_len);

    /* Atomically flip the write index — UI thread now sees this buffer */
    s_write_idx = next_idx;

    /* --- Telemetry Trend Ring Buffer (Decimate to 1 sample/sec) --- */
    if ((now - s_last_trend_push_ms) >= TREND_DECIMATE_MS) {
        s_last_trend_push_ms = now;
        telemetry_sample_t *sample = &s_trend_buffer.samples[s_trend_buffer.head];
        sample->temp_mC = temp_mC;
        sample->pressure_kPa = press_kPa;
        sample->rpm = rpm;
        sample->bus_mv = bus_mv;
        s_trend_buffer.head = (s_trend_buffer.head + 1) % TREND_HISTORY_SAMPLES;
        if (s_trend_buffer.count < TREND_HISTORY_SAMPLES) {
            s_trend_buffer.count++;
        }
    }
}

void layer2_snapshot_state(shared_state_buffer_t *out)
{
    if (!out) return;
    /* Read from the latest completed buffer slot.
     * Since s_write_idx is updated atomically (single uint32_t write),
     * and the ISR always writes to the OTHER slot, this read is safe. */
    *out = s_ping_pong[s_write_idx];
}

bool layer2_consume_dirty_flag(void)
{
    shared_state_buffer_t *buf = &s_ping_pong[s_write_idx];
    if (buf->dirty_flag) {
        buf->dirty_flag = 0;
        return true;
    }
    return false;
}

screen_id_t layer2_fsm_get_active_screen(void)
{
    return s_active_screen;
}

bool layer2_fsm_request_screen_change(screen_id_t new_screen)
{
    if (new_screen >= SCREEN_COUNT) {
        return false;
    }

    if (s_active_screen == new_screen) {
        return true;
    }

    s_previous_screen = s_active_screen;
    s_active_screen = new_screen;

    /* Update both ping-pong buffers to ensure consistency */
    s_ping_pong[0].active_screen = (uint8_t)new_screen;
    s_ping_pong[1].active_screen = (uint8_t)new_screen;
    s_ping_pong[0].dirty_flag = 1;
    s_ping_pong[1].dirty_flag = 1;

    printf("[FSM] %d->%d\n", s_previous_screen, s_active_screen);
    return true;
}

void layer2_fsm_process_event(input_key_t key_event)
{
    if (key_event == KEY_NONE) return;

    /* Global key bindings */
    if (key_event == KEY_TOGGLE_FAILOVER) {
        if (s_active_screen == SCREEN_FAILOVER_STANDBY) {
            uint8_t fo = !s_ping_pong[0].failover_active;
            s_ping_pong[0].failover_active = fo;
            s_ping_pong[1].failover_active = fo;
        } else {
            layer2_fsm_request_screen_change(SCREEN_FAILOVER_STANDBY);
            s_ping_pong[0].failover_active = 1;
            s_ping_pong[1].failover_active = 1;
        }
        return;
    }

    if (key_event == KEY_SIMULATE_WD_FAULT) {
        s_watchdog_supervisor.last_hw_ping_ms = 0;
        s_watchdog_supervisor.system_healthy = false;
        s_watchdog_supervisor.hw_loop_alive = false;
        s_watchdog_supervisor.watchdog_fault_count++;
        layer2_fsm_request_screen_change(SCREEN_FAILOVER_STANDBY);
        s_ping_pong[0].failover_active = 1;
        s_ping_pong[1].failover_active = 1;
        return;
    }

    if (key_event == KEY_TEST_ALARM) {
        if (s_alarm_latch == ALARM_STATE_CLEARED) {
            s_alarm_latch = ALARM_STATE_ACTIVE;
            s_ping_pong[0].alarm_latch_state = (uint8_t)ALARM_STATE_ACTIVE;
            s_ping_pong[1].alarm_latch_state = (uint8_t)ALARM_STATE_ACTIVE;
            s_ping_pong[0].alarm_severity = (uint8_t)ALARM_CRITICAL;
            s_ping_pong[1].alarm_severity = (uint8_t)ALARM_CRITICAL;
            push_alarm_log(layer0_get_system_time_ms(), ALARM_CRITICAL, ALARM_STATE_ACTIVE, "TEST TRIP ACTIVE");
        } else {
            s_alarm_latch = ALARM_STATE_CLEARED;
            s_ping_pong[0].alarm_latch_state = (uint8_t)ALARM_STATE_CLEARED;
            s_ping_pong[1].alarm_latch_state = (uint8_t)ALARM_STATE_CLEARED;
            s_ping_pong[0].alarm_severity = (uint8_t)ALARM_NONE;
            s_ping_pong[1].alarm_severity = (uint8_t)ALARM_NONE;
            push_alarm_log(layer0_get_system_time_ms(), ALARM_NONE, ALARM_STATE_CLEARED, "TEST CLEARED");
        }
        return;
    }

    if (key_event == KEY_ALARM_ACK) {
        /* Alarm Latching FSM: ACTIVE -> ACKNOWLEDGED */
        if (s_alarm_latch == ALARM_STATE_ACTIVE) {
            s_alarm_latch = ALARM_STATE_ACKNOWLEDGED;
            s_ping_pong[0].alarm_latch_state = (uint8_t)ALARM_STATE_ACKNOWLEDGED;
            s_ping_pong[1].alarm_latch_state = (uint8_t)ALARM_STATE_ACKNOWLEDGED;
            push_alarm_log(layer0_get_system_time_ms(), 
                          s_ping_pong[s_write_idx].alarm_severity,
                          ALARM_STATE_ACKNOWLEDGED, "ACKNOWLEDGED");
            printf("[FSM] Alarm Acked\n");
        }
        return;
    }

    /* Screen-specific state navigation */
    switch (s_active_screen) {
        case SCREEN_DASHBOARD:
            if (key_event == KEY_NEXT) {
                layer2_fsm_request_screen_change(SCREEN_DIAGNOSTICS);
            } else if (key_event == KEY_PREV) {
                layer2_fsm_request_screen_change(SCREEN_FAILOVER_STANDBY);
            } else if (key_event == KEY_SELECT) {
                layer2_fsm_request_screen_change(SCREEN_ALARM);
            }
            break;

        case SCREEN_DIAGNOSTICS:
            if (key_event == KEY_NEXT) {
                layer2_fsm_request_screen_change(SCREEN_ALARM);
            } else if (key_event == KEY_PREV) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
            } else if (key_event == KEY_BACK) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
            }
            break;

        case SCREEN_ALARM:
            if (key_event == KEY_NEXT) {
                layer2_fsm_request_screen_change(SCREEN_SETTINGS);
            } else if (key_event == KEY_PREV) {
                layer2_fsm_request_screen_change(SCREEN_DIAGNOSTICS);
            } else if (key_event == KEY_BACK) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
            }
            break;

        case SCREEN_SETTINGS:
            if (key_event == KEY_NEXT) {
                layer2_fsm_request_screen_change(SCREEN_FAILOVER_STANDBY);
            } else if (key_event == KEY_PREV) {
                layer2_fsm_request_screen_change(SCREEN_ALARM);
            } else if (key_event == KEY_BACK) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
            }
            break;

        case SCREEN_FAILOVER_STANDBY:
            if (key_event == KEY_NEXT) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
            } else if (key_event == KEY_PREV) {
                layer2_fsm_request_screen_change(SCREEN_SETTINGS);
            } else if (key_event == KEY_BACK || key_event == KEY_SELECT) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
            }
            break;

        default:
            break;
    }
}

void layer2_watchdog_report_software_alive(void)
{
    s_watchdog_supervisor.last_sw_ping_ms = layer0_get_system_time_ms();
}

void layer2_watchdog_supervisor_tick(void)
{
    uint64_t now = layer0_get_system_time_ms();
    uint64_t last_hw = layer1_get_last_hardware_heartbeat_ms();
    uint64_t last_sw = s_watchdog_supervisor.last_sw_ping_ms;

    s_watchdog_supervisor.last_hw_ping_ms = last_hw;

    /* Evaluate Hardware & Software Heartbeat Age */
    s_watchdog_supervisor.hw_loop_alive = ((now - last_hw) <= WATCHDOG_MAX_AGE_MS);
    s_watchdog_supervisor.sw_loop_alive = ((now - last_sw) <= WATCHDOG_MAX_AGE_MS);

    /* Dual-Loop AND Check: BOTH loops must be alive! */
    s_watchdog_supervisor.system_healthy = 
        (s_watchdog_supervisor.hw_loop_alive && s_watchdog_supervisor.sw_loop_alive);

    if (s_watchdog_supervisor.system_healthy) {
        /* Pet physical hardware watchdog */
        layer0_pet_hardware_watchdog();
        s_watchdog_supervisor.total_watchdog_pets++;
    } else {
        /* Watchdog Fault Detected! */
        printf("[WD] Fail HW:%d SW:%d\n",
               s_watchdog_supervisor.hw_loop_alive, s_watchdog_supervisor.sw_loop_alive);
        
        /* Auto-engage Hot Standby Failover FSM mode */
        if (s_active_screen != SCREEN_FAILOVER_STANDBY) {
            layer2_fsm_request_screen_change(SCREEN_FAILOVER_STANDBY);
            s_ping_pong[0].failover_active = 1;
            s_ping_pong[1].failover_active = 1;
        }
    }
}

const watchdog_supervisor_t* layer2_get_watchdog_status(void)
{
    return &s_watchdog_supervisor;
}

const telemetry_ring_buffer_t* layer2_get_trend_buffer(void)
{
    return &s_trend_buffer;
}

const alarm_journal_t* layer2_get_alarm_journal(void)
{
    return &s_alarm_journal;
}
