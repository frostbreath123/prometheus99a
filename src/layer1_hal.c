/**
 * @file layer1_hal.c
 * @brief Layer 1: Hardware Abstraction Layer & Drivers Implementation
 *
 * Changes from original:
 *   - HW heartbeat moved into ISR path (was in display_flush_cb which runs
 *     on the UI thread, defeating the dual-loop watchdog check)
 *   - Added input event queue (circular buffer) so Win32 WndProc can push
 *     events through HAL instead of bypassing layers directly
 *   - Display flush callback no longer falsely reports hardware alive
 */

#include "../include/layer1_hal.h"
#include "../include/layer0_hardware.h"
#include "../include/layer2_core.h"
#include <stdio.h>

static uint64_t s_last_hw_heartbeat_ms = 0;

/* --- Input Event Queue (Static Circular Buffer) --- */
static input_key_t s_key_queue[INPUT_QUEUE_CAPACITY];
static volatile uint8_t s_key_queue_head = 0;
static volatile uint8_t s_key_queue_tail = 0;

static int16_t s_touch_x = 0;
static int16_t s_touch_y = 0;
static volatile bool s_touch_pending = false;

void layer1_hal_init(void)
{
    s_last_hw_heartbeat_ms = layer0_get_system_time_ms();
    s_key_queue_head = 0;
    s_key_queue_tail = 0;
    s_touch_pending = false;
    printf("[L1] HAL Ready\n");
}

/* Executed by High-Resolution Interrupt / Thread at 1000 Hz */
void layer1_sensor_isr_handler_1000hz(void)
{
    raw_hardware_sensors_t raw;
    layer0_read_raw_sensors(&raw);

    /* Ingest binary telemetry into Layer 2 via lock-free publish */
    layer2_update_state_binary(
        raw.raw_adc_temp,
        raw.raw_adc_pressure,
        raw.raw_adc_rpm,
        raw.raw_adc_bus_v,
        raw.cpu_load_pct,
        raw.ram_load_pct,
        raw.ssd_read_kb_s,
        raw.ssd_write_kb_s
    );

    /* Report hardware alive FROM the ISR thread (fixes watchdog dual-loop).
     * Previously this was called from display_flush_cb which runs on the
     * UI thread — making both HW and SW heartbeats come from the same thread,
     * so the watchdog could never detect a sensor thread crash. */
    layer1_report_hardware_alive();
}

void layer1_report_hardware_alive(void)
{
    s_last_hw_heartbeat_ms = layer0_get_system_time_ms();
}

uint64_t layer1_get_last_hardware_heartbeat_ms(void)
{
    return s_last_hw_heartbeat_ms;
}

static void (*s_display_flush_handler)(const display_area_t *area, const uint8_t *color_p) = NULL;

void layer1_set_display_flush_handler(void (*handler)(const display_area_t *area, const uint8_t *color_p))
{
    s_display_flush_handler = handler;
}

void layer1_display_flush_cb(const display_area_t *area, const uint8_t *color_p)
{
    if (s_display_flush_handler) {
        s_display_flush_handler(area, color_p);
    }
}

/* --- Input Queue: Push API (called from Win32 WndProc on UI thread) --- */

void layer1_queue_key(input_key_t key)
{
    uint8_t next_head = (s_key_queue_head + 1) % INPUT_QUEUE_CAPACITY;
    if (next_head == s_key_queue_tail) {
        return; /* Queue full, drop event */
    }
    s_key_queue[s_key_queue_head] = key;
    s_key_queue_head = next_head;
}

void layer1_queue_touch(int16_t x, int16_t y, bool pressed)
{
    if (pressed) {
        s_touch_x = x;
        s_touch_y = y;
        s_touch_pending = true;
    }
}

/* --- Input Queue: Poll API (called from UI timer tick on UI thread) --- */

bool layer1_poll_button_event(input_key_t *key_out)
{
    if (s_key_queue_head == s_key_queue_tail) {
        return false; /* Queue empty */
    }
    if (key_out) *key_out = s_key_queue[s_key_queue_tail];
    s_key_queue_tail = (s_key_queue_tail + 1) % INPUT_QUEUE_CAPACITY;
    return true;
}

bool layer1_poll_touch_event(int16_t *x, int16_t *y, bool *pressed)
{
    if (!s_touch_pending) {
        if (pressed) *pressed = false;
        return false;
    }
    if (x) *x = s_touch_x;
    if (y) *y = s_touch_y;
    if (pressed) *pressed = true;
    s_touch_pending = false;
    return true;
}
