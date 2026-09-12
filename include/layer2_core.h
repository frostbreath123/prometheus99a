/**
 * @file layer2_core.h
 * @brief Layer 2: Core State Management, Serialization, FSM & Watchdog Supervisor
 * 
 * Changes from original:
 *   - Removed #pragma pack(push, 1) — fields reordered by descending size
 *     for natural alignment (prevents ARM BusFault on Cortex-M0/M0+)
 *   - Added alarm_latch_state to shared state for latching alarm FSM
 *   - Added lock-free ping-pong buffer (state_exchange_t) for safe
 *     ISR thread -> UI thread data exchange without mutexes
 *   - Added telemetry_ring_buffer_t for 60-sample trend history (~720 bytes)
 *   - Added alarm_journal_t for ISA-18.2 compliant alarm logging (~768 bytes)
 */

#ifndef LAYER2_CORE_H
#define LAYER2_CORE_H

#include "config.h"

/* --- Naturally Aligned Shared State Buffer Structure ---
 * Fields ordered by descending size to guarantee natural alignment
 * on all architectures (ARM Cortex-M, x86, RISC-V) without pragma pack.
 * This enables hardware atomic instructions (LDREX/STREX on ARM) and
 * prevents unaligned access faults on strict-alignment platforms. */
typedef struct {
    /* 8-byte aligned fields */
    uint64_t timestamp_ms;        /* High-resolution system timestamp */

    /* 4-byte aligned fields */
    uint32_t magic_header;        /* 0x50524F4D ('PROM') */
    uint32_t sensor_temp_mC;      /* Live CPU Temperature in milli-Celsius */
    uint32_t sensor_pressure_kPa; /* Live SSD Throughput in KB/s */
    uint32_t sensor_rpm;          /* Live Cooling Fan RPM */
    uint32_t sensor_bus_mv;       /* Bus Voltage in mV */
    uint32_t heartbeat_counter;   /* Telemetry frame counter */
    uint32_t ssd_read_kb_s;       /* Live SSD Read Throughput in KB/s */
    uint32_t ssd_write_kb_s;      /* Live SSD Write Throughput in KB/s */

    /* 2-byte aligned fields */
    uint16_t cpu_load_pct;        /* Live Host CPU utilization (0 - 100%) */
    uint16_t ram_load_pct;        /* Live Host RAM utilization (0 - 100%) */
    uint16_t checksum;            /* 16-bit payload checksum */

    /* 1-byte aligned fields */
    uint8_t  alarm_severity;      /* Active alarm level (alarm_level_t) */
    uint8_t  alarm_latch_state;   /* Alarm latching FSM (alarm_latch_state_t) */
    uint8_t  active_screen;       /* Currently active screen ID (screen_id_t) */
    uint8_t  dirty_flag;          /* 1 = New unread telemetry data available */
    uint8_t  failover_active;     /* 1 = Hot Standby Failover engaged */
    uint8_t  _pad;                /* Explicit padding for 4-byte alignment */
} shared_state_buffer_t;

/* --- Watchdog Supervisor Status --- */
typedef struct {
    uint64_t last_hw_ping_ms;
    uint64_t last_sw_ping_ms;
    uint32_t total_watchdog_pets;
    uint32_t watchdog_fault_count;
    bool hw_loop_alive;
    bool sw_loop_alive;
    bool system_healthy;
} watchdog_supervisor_t;

/* --- Telemetry Ring Buffer (Static, 60 samples) --- */
typedef struct {
    uint32_t temp_mC;
    uint32_t pressure_kPa;
    uint32_t rpm;
    uint32_t bus_mv;
} telemetry_sample_t;

typedef struct {
    telemetry_sample_t samples[TREND_HISTORY_SAMPLES]; /* 60 * 16 = 960 bytes */
    uint16_t head;
    uint16_t count;
} telemetry_ring_buffer_t;

/* --- Alarm Journal (Static, 16 entries) --- */
typedef struct {
    uint64_t timestamp_ms;
    uint8_t  severity;         /* alarm_level_t */
    uint8_t  latch_state;      /* alarm_latch_state_t */
    char     description[30];  /* Short description */
} alarm_log_entry_t;

typedef struct {
    alarm_log_entry_t entries[ALARM_LOG_CAPACITY]; /* 16 * 48 = 768 bytes */
    uint8_t head;
    uint8_t count;
} alarm_journal_t;

/* --- Core Function Declarations --- */
void layer2_core_init(void);

/* Shared State Management (Lock-Free ISR -> UI Exchange) */
void layer2_update_state_binary(uint32_t temp_mC, uint32_t press_kPa, uint32_t rpm, uint32_t bus_mv,
                                uint16_t cpu_load_pct, uint16_t ram_load_pct,
                                uint32_t ssd_read_kb_s, uint32_t ssd_write_kb_s);
void layer2_snapshot_state(shared_state_buffer_t *out);
bool layer2_consume_dirty_flag(void);

/* FSM Controller */
screen_id_t layer2_fsm_get_active_screen(void);
bool layer2_fsm_request_screen_change(screen_id_t new_screen);
void layer2_fsm_process_event(input_key_t key_event);

/* Watchdog Supervisor (Dual-Loop AND Check) */
void layer2_watchdog_report_software_alive(void);
void layer2_watchdog_supervisor_tick(void);
const watchdog_supervisor_t* layer2_get_watchdog_status(void);

/* Telemetry History & Alarm Journal */
const telemetry_ring_buffer_t* layer2_get_trend_buffer(void);
const alarm_journal_t* layer2_get_alarm_journal(void);

#endif /* LAYER2_CORE_H */
