/**
 * @file layer1_hal.h
 * @brief Layer 1: Hardware Abstraction Layer & Drivers
 * 
 * Provides debounced button/touch drivers, 1000 Hz high-speed Sensor ISR,
 * Display Flush Callback, and an input event queue for proper layer isolation.
 *
 * Changes from original:
 *   - Added input event queue API (layer1_queue_key / layer1_queue_touch)
 *     so that main.c routes all input through Layer 1 instead of bypassing it
 */

#ifndef LAYER1_HAL_H
#define LAYER1_HAL_H

#include "config.h"

/* --- Display Flush Buffer Structure (LVGL Compatible) --- */
typedef struct {
    int16_t x1;
    int16_t y1;
    int16_t x2;
    int16_t y2;
    const uint8_t *pixel_color_p;
} display_area_t;

/* --- Function Declarations --- */
void layer1_hal_init(void);

/* Sensor ISR triggered at 1000 Hz */
void layer1_sensor_isr_handler_1000hz(void);

/* Input Queue: Push events from Win32 WndProc into HAL queue */
void layer1_queue_key(input_key_t key);
void layer1_queue_touch(int16_t x, int16_t y, bool pressed);

/* Input Queue: Poll events from HAL queue (consumed by Layer 2/3) */
bool layer1_poll_button_event(input_key_t *key_out);
bool layer1_poll_touch_event(int16_t *x, int16_t *y, bool *pressed);

/* Display Flush Callback (called by LVGL rendering engine) */
void layer1_display_flush_cb(const display_area_t *area, const uint8_t *color_p);
void layer1_set_display_flush_handler(void (*handler)(const display_area_t *area, const uint8_t *color_p));

/* Heartbeat reporter from hardware side */
void layer1_report_hardware_alive(void);
uint64_t layer1_get_last_hardware_heartbeat_ms(void);

#endif /* LAYER1_HAL_H */
