/**
 * @file config.h
 * @brief Prometheus99 HMI Runtime System Configuration
 * @architect Team Doomsday (Abhilash L, Adarsh Abhilash, Nikhil Nuguri)
 * 
 * Target: Pure C99 Standard
 * Graphics Engine: LVGL Presentation Layer (4-bit Indexed Static Architecture)
 * Architecture: 4-Layer Static Memory HMI Engine
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* --- Display Specifications --- */
#define DISPLAY_WIDTH          800
#define DISPLAY_HEIGHT         480
#define DISPLAY_COLOR_DEPTH    4      /* 4-bit Indexed: 16-color industrial CLUT */
#define DISPLAY_ROW_BYTES      (DISPLAY_WIDTH / 2) /* 400 bytes per scanline (DWORD-aligned) */
#define DISPLAY_BAND_HEIGHT    48     /* 1/10th Partial Draw Band Buffer (Embedded LVGL standard) */
#define DISPLAY_BAND_COUNT     (DISPLAY_HEIGHT / DISPLAY_BAND_HEIGHT) /* 10 bands */
#define DISPLAY_BUF_SIZE       (DISPLAY_ROW_BYTES * DISPLAY_BAND_HEIGHT) /* 19,200 bytes (18.75 KB RAM!) */

/* --- Timing Specifications --- */
#define SENSOR_ISR_FREQ_HZ     1000   /* Layer 1: Sensor & Fieldbus ISR (1000 Hz / 1 ms tick) */
#define UI_TIMER_FREQ_HZ       30     /* Layer 3: UI Presentation Timer (30 Hz / ~33 ms frame) */
#define UI_FRAME_PERIOD_MS     (1000 / UI_TIMER_FREQ_HZ)

/* --- Watchdog Supervisor Specifications --- */
#define WATCHDOG_MAX_AGE_MS    500    /* Maximum allowed heartbeat silence before failover */
#define WATCHDOG_CHECK_PERIOD_MS 100  /* Supervisor dual-loop validation cycle */

/* --- Telemetry History Ring Buffer --- */
#define TREND_HISTORY_SAMPLES  60     /* 60 data points for trend graph (~60 seconds at 1 sample/sec) */
#define TREND_DECIMATE_MS      1000   /* Push 1 sample per second (decimate from 1000 Hz) */

/* --- Alarm Journal --- */
#define ALARM_LOG_CAPACITY     16     /* Circular log of last 16 alarm events */

/* --- Input Queue Capacity --- */
#define INPUT_QUEUE_CAPACITY   8      /* Max queued input events from HAL to core */

/* --- Pre-Allocated Screen Identifiers --- */
typedef enum {
    SCREEN_DASHBOARD = 0,
    SCREEN_DIAGNOSTICS,
    SCREEN_ALARM,
    SCREEN_SETTINGS,
    SCREEN_FAILOVER_STANDBY,
    SCREEN_COUNT
} screen_id_t;

/* --- Unified Input Action Keys --- */
typedef enum {
    KEY_NONE = 0,
    KEY_PREV,
    KEY_NEXT,
    KEY_SELECT,
    KEY_BACK,
    KEY_ALARM_ACK,
    KEY_TOGGLE_FAILOVER,
    KEY_TOGGLE_CONTRAST,
    KEY_TEST_ALARM,
    KEY_SIMULATE_WD_FAULT
} input_key_t;

/* --- Alarm Severity Levels --- */
typedef enum {
    ALARM_NONE = 0,
    ALARM_INFO,
    ALARM_WARNING,
    ALARM_CRITICAL,
    ALARM_EMERGENCY
} alarm_level_t;

/* --- Alarm Latching State Machine ---
 * Fixes the broken alarm acknowledgment logic where the ISR would
 * immediately overwrite a user's ACK within 1 ms. The latching FSM:
 *   CLEARED  -> ACTIVE        : sensor threshold exceeded
 *   ACTIVE   -> ACKNOWLEDGED  : user presses ACK while alarm active
 *   ACKNOWLEDGED -> CLEARED   : sensor returns to normal range
 *   ACKNOWLEDGED -> ACKNOWLEDGED : sensor still over threshold (stays latched)
 */
typedef enum {
    ALARM_STATE_CLEARED = 0,   /* No alarm condition */
    ALARM_STATE_ACTIVE,        /* Alarm condition detected, not yet acknowledged */
    ALARM_STATE_ACKNOWLEDGED   /* Alarm acknowledged by operator, awaiting clearance */
} alarm_latch_state_t;

#endif /* CONFIG_H */
