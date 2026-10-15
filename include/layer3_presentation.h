/**
 * @file layer3_presentation.h
 * @brief Layer 3: LVGL Embedded Presentation Layer & Pre-Allocated Static Screens
 * 
 * Implements standard LVGL widget rendering, unified input group handling,
 * 30 Hz refresh timer with 8-bit Indexed Color memory optimization (< 500 KB RAM),
 * and pre-allocated static screen memory.
 */

#ifndef LAYER3_PRESENTATION_H
#define LAYER3_PRESENTATION_H

#include "config.h"
#include "layer2_core.h"
#include "lvgl.h"

/* --- High Contrast / Color Palette Theme (8-bit Indexed) --- */
typedef struct {
    lv_color_t bg_color;
    lv_color_t card_bg;
    lv_color_t primary_accent;
    lv_color_t secondary_accent;
    lv_color_t text_primary;
    lv_color_t text_secondary;
    lv_color_t alarm_critical;
    lv_color_t alarm_ok;
    bool is_high_contrast;
} hmi_theme_t;

/* --- Function Declarations --- */
void layer3_presentation_init(void);

/* 30 Hz UI Timer Tick Handler (LVGL timer_handler, polls input, reports software alive) */
void layer3_ui_timer_tick_30hz(void);

/* Input Group Processing */
void layer3_inject_input_key(input_key_t key);
void layer3_handle_touch(int16_t x, int16_t y, bool pressed);

/* Theme & Contrast Toggle */
void layer3_toggle_high_contrast_theme(void);
const hmi_theme_t* layer3_get_current_theme(void);

/* Partial Draw Band Rendering (LVGL v8 Standard: 18.75 KB Band Buffer) */
void layer3_render_all_bands(void);

/* Rendering Buffer Access (4-bit Partial Draw Buffer: 18.75 KB) */
const uint8_t* layer3_get_framebuffer(void);

/* 16-Color Palette Table (0x00RRGGBB format for Win32 GDI RGBQUAD) */
const uint32_t* layer3_get_palette(void);

#endif /* LAYER3_PRESENTATION_H */
