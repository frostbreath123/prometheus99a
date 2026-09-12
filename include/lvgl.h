/**
 * @file lvgl.h
 * @brief Lightweight Embedded LVGL v8 API & Architecture Specification
 * @architect Team Doomsday (Abhilash L, Adarsh Abhilash, Nikhil Nuguri)
 *
 * Provides standard LVGL v8 types, widget interfaces, display & indev drivers,
 * partial draw buffering, and 16-bit RGB565 color representation for
 * strict static memory embedded targets (< 1.5 MB RAM footprint).
 */

#ifndef LVGL_H
#define LVGL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LVGL_VERSION_MAJOR 8
#define LVGL_VERSION_MINOR 3
#define LVGL_VERSION_PATCH 0

/* --- Coordinate Types --- */
typedef int16_t lv_coord_t;

typedef struct {
    lv_coord_t x1;
    lv_coord_t y1;
    lv_coord_t x2;
    lv_coord_t y2;
} lv_area_t;

typedef struct {
    lv_coord_t x;
    lv_coord_t y;
} lv_point_t;

/* --- 4-Bit Indexed Color Format (Extreme Embedded Memory: 187.5 KB FB) --- */
#define LV_COLOR_INDEX_BLACK         0
#define LV_COLOR_INDEX_BG_DARK       1
#define LV_COLOR_INDEX_CARD_BG       2
#define LV_COLOR_INDEX_CARD_CONTRAST 3
#define LV_COLOR_INDEX_CYAN          4
#define LV_COLOR_INDEX_BLUE          5
#define LV_COLOR_INDEX_WHITE         6
#define LV_COLOR_INDEX_GREY          7
#define LV_COLOR_INDEX_LIGHT_GREY    8
#define LV_COLOR_INDEX_DARK_GREY     9
#define LV_COLOR_INDEX_RED           10
#define LV_COLOR_INDEX_BRIGHT_RED    11
#define LV_COLOR_INDEX_GREEN         12
#define LV_COLOR_INDEX_BRIGHT_GREEN  13
#define LV_COLOR_INDEX_YELLOW        14
#define LV_COLOR_INDEX_AMBER         15

typedef union {
    struct {
        uint8_t index : 4;
    } ch;
    uint8_t full;
} lv_color_t;

typedef lv_color_t lv_color4_t;
typedef lv_color_t lv_color8_t;
typedef lv_color_t lv_color16_t;

static inline lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b)
{
    lv_color_t c;
    /* Map exact UI colors if matched */
    if (r == 0 && g == 0 && b == 0) { c.full = LV_COLOR_INDEX_BLACK; return c; }
    if (r == 0x0B && g == 0x10 && b == 0x1D) { c.full = LV_COLOR_INDEX_BG_DARK; return c; }
    if (r == 0x16 && g == 0x1F && b == 0x33) { c.full = LV_COLOR_INDEX_CARD_BG; return c; }
    if (r == 0x1A && g == 0x1A && b == 0x1A) { c.full = LV_COLOR_INDEX_CARD_CONTRAST; return c; }
    if (r == 0x00 && g == 0xD2 && b == 0xFF) { c.full = LV_COLOR_INDEX_CYAN; return c; }
    if (r == 0x3B && g == 0x82 && b == 0xF6) { c.full = LV_COLOR_INDEX_BLUE; return c; }
    if (r >= 0xF0 && g >= 0xF0 && b >= 0xF0) { c.full = LV_COLOR_INDEX_WHITE; return c; }
    if (r == 0x94 && g == 0xA3 && b == 0xB8) { c.full = LV_COLOR_INDEX_GREY; return c; }
    if (r == 0xE0 && g == 0xE0 && b == 0xE0) { c.full = LV_COLOR_INDEX_LIGHT_GREY; return c; }
    if (r == 0x22 && g == 0x33 && b == 0x44) { c.full = LV_COLOR_INDEX_DARK_GREY; return c; }
    if (r == 0xEF && g == 0x44 && b == 0x44) { c.full = LV_COLOR_INDEX_RED; return c; }
    if (r == 0xFF && g == 0x00 && b == 0x00) { c.full = LV_COLOR_INDEX_BRIGHT_RED; return c; }
    if (r == 0x10 && g == 0xB9 && b == 0x81) { c.full = LV_COLOR_INDEX_GREEN; return c; }
    if (r == 0x00 && g == 0xFF && b == 0x00) { c.full = LV_COLOR_INDEX_BRIGHT_GREEN; return c; }
    if (r >= 0xF0 && g >= 0xF0 && b < 0x20)   { c.full = LV_COLOR_INDEX_YELLOW; return c; }
    if (r == 0xFB && g == 0xBF && b == 0x24) { c.full = LV_COLOR_INDEX_AMBER; return c; }

    /* Nearest color classification in 16-color space */
    if (r > 200 && g > 200 && b > 200) c.full = LV_COLOR_INDEX_WHITE;
    else if (r > 150 && g < 50 && b < 50) c.full = LV_COLOR_INDEX_RED;
    else if (r < 50 && g > 150 && b < 50) c.full = LV_COLOR_INDEX_GREEN;
    else if (r < 50 && g > 150 && b > 200) c.full = LV_COLOR_INDEX_CYAN;
    else if (r > 180 && g > 180 && b < 50) c.full = LV_COLOR_INDEX_YELLOW;
    else if (r > 100 && g > 100 && b > 100) c.full = LV_COLOR_INDEX_GREY;
    else if (r > 20 || g > 20 || b > 40) c.full = LV_COLOR_INDEX_DARK_GREY;
    else c.full = LV_COLOR_INDEX_BLACK;
    return c;
}

static inline lv_color_t lv_color_hex(uint32_t hex)
{
    uint8_t r = (uint8_t)((hex >> 16) & 0xFF);
    uint8_t g = (uint8_t)((hex >> 8)  & 0xFF);
    uint8_t b = (uint8_t)(hex & 0xFF);
    return lv_color_make(r, g, b);
}

static inline uint8_t lv_color_to8(lv_color_t c)
{
    return (uint8_t)(c.full & 0x0F);
}

static inline uint8_t lv_color_to4(lv_color_t c)
{
    return (uint8_t)(c.full & 0x0F);
}

static inline uint16_t lv_color_to16(lv_color_t c)
{
    return c.full;
}

#define LV_COLOR_MAKE(r, g, b) lv_color_make((r), (g), (b))
#define LV_COLOR_HEX(hex)      lv_color_hex((hex))

/* --- Alignment & State Enumerations --- */
typedef enum {
    LV_ALIGN_DEFAULT = 0,
    LV_ALIGN_TOP_LEFT,
    LV_ALIGN_TOP_MID,
    LV_ALIGN_TOP_RIGHT,
    LV_ALIGN_BOTTOM_LEFT,
    LV_ALIGN_BOTTOM_MID,
    LV_ALIGN_BOTTOM_RIGHT,
    LV_ALIGN_LEFT_MID,
    LV_ALIGN_RIGHT_MID,
    LV_ALIGN_CENTER,
} lv_align_t;

typedef enum {
    LV_ANIM_OFF = 0,
    LV_ANIM_ON
} lv_anim_enable_t;

/* --- Display Draw Buffer (Partial / Full Buffer Architecture) --- */
typedef struct {
    void *buf1;
    void *buf2;
    void *buf_act;
    uint32_t size; /* In pixels */
    lv_area_t area;
} lv_disp_draw_buf_t;

struct _lv_disp_drv_t;

typedef void (*lv_disp_flush_cb_t)(struct _lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p);

typedef struct _lv_disp_drv_t {
    lv_coord_t hor_res;
    lv_coord_t ver_res;
    lv_disp_draw_buf_t *draw_buf;
    lv_disp_flush_cb_t flush_cb;
    void *user_data;
} lv_disp_drv_t;

typedef struct _lv_disp_t {
    lv_disp_drv_t *driver;
} lv_disp_t;

/* --- Input Device Driver (Keypad, Touch Pointer) --- */
typedef enum {
    LV_INDEV_TYPE_NONE = 0,
    LV_INDEV_TYPE_POINTER,
    LV_INDEV_TYPE_KEYPAD,
    LV_INDEV_TYPE_BUTTON,
    LV_INDEV_TYPE_ENCODER
} lv_indev_type_t;

typedef enum {
    LV_INDEV_STATE_RELEASED = 0,
    LV_INDEV_STATE_PRESSED
} lv_indev_state_t;

typedef struct {
    lv_point_t point;
    uint32_t key;
    lv_indev_state_t state;
} lv_indev_data_t;

struct _lv_indev_drv_t;

typedef void (*lv_indev_read_cb_t)(struct _lv_indev_drv_t *drv, lv_indev_data_t *data);

typedef struct _lv_indev_drv_t {
    lv_indev_type_t type;
    lv_indev_read_cb_t read_cb;
    void *user_data;
} lv_indev_drv_t;

typedef struct _lv_indev_t {
    lv_indev_drv_t *driver;
} lv_indev_t;

/* --- Generic LVGL Object Structure (Static Pre-Allocated) --- */
typedef enum {
    LV_OBJ_TYPE_BASE = 0,
    LV_OBJ_TYPE_LABEL,
    LV_OBJ_TYPE_BTN,
    LV_OBJ_TYPE_BAR,
    LV_OBJ_TYPE_CHART
} lv_obj_type_t;

typedef struct _lv_obj_t {
    lv_obj_type_t type;
    lv_area_t coords;
    struct _lv_obj_t *parent;
    bool is_hidden;
    
    /* Widget-specific payload */
    union {
        struct {
            char text[128];
            lv_color_t color;
            uint8_t font_scale;
        } label;
        struct {
            lv_color_t bg_color;
            lv_color_t border_color;
            bool is_pressed;
        } btn;
        struct {
            int32_t cur_val;
            int32_t min_val;
            int32_t max_val;
            lv_color_t fill_color;
            lv_color_t bg_color;
        } bar;
        struct {
            int16_t values[64];
            uint16_t count;
            int32_t min_val;
            int32_t max_val;
            lv_color_t line_color;
        } chart;
    } spec;
} lv_obj_t;

/* --- Core Initialization & Driver Registration API --- */
void lv_init(void);

void lv_disp_draw_buf_init(lv_disp_draw_buf_t *draw_buf, void *buf1, void *buf2, uint32_t size_in_px_cnt);
void lv_disp_drv_init(lv_disp_drv_t *driver);
lv_disp_t *lv_disp_drv_register(lv_disp_drv_t *driver);
void lv_disp_flush_ready(lv_disp_drv_t *disp_drv);

void lv_indev_drv_init(lv_indev_drv_t *driver);
lv_indev_t *lv_indev_drv_register(lv_indev_drv_t *driver);

uint32_t lv_timer_handler(void);
uint32_t lv_task_handler(void); /* LVGL v7/v8 compatibility alias */
void lv_tick_inc(uint32_t tick_period);

/* --- Object Tree & Widget Creation APIs --- */
lv_obj_t *lv_scr_act(void);
lv_obj_t *lv_obj_create(lv_obj_t *parent);
void lv_obj_set_pos(lv_obj_t *obj, lv_coord_t x, lv_coord_t y);
void lv_obj_set_size(lv_obj_t *obj, lv_coord_t w, lv_coord_t h);
void lv_obj_clean(lv_obj_t *obj);

/* Label Widget API */
lv_obj_t *lv_label_create(lv_obj_t *parent);
void lv_label_set_text(lv_obj_t *label, const char *text);
void lv_label_set_text_fmt(lv_obj_t *label, const char *fmt, ...);
void lv_label_set_color(lv_obj_t *label, lv_color_t color);
void lv_label_set_scale(lv_obj_t *label, uint8_t scale);

/* Button Widget API */
lv_obj_t *lv_btn_create(lv_obj_t *parent);
void lv_btn_set_color(lv_obj_t *btn, lv_color_t bg_color, lv_color_t border_color);

/* Progress Bar Widget API */
lv_obj_t *lv_bar_create(lv_obj_t *parent);
void lv_bar_set_value(lv_obj_t *bar, int32_t value, lv_anim_enable_t anim);
void lv_bar_set_range(lv_obj_t *bar, int32_t min, int32_t max);
void lv_bar_set_color(lv_obj_t *bar, lv_color_t fill_color, lv_color_t bg_color);

/* Chart / Trend Graph Widget API */
lv_obj_t *lv_chart_create(lv_obj_t *parent);
void lv_chart_set_point_count(lv_obj_t *chart, uint16_t cnt);
void lv_chart_set_range(lv_obj_t *chart, int32_t min, int32_t max);
void lv_chart_set_next_value(lv_obj_t *chart, lv_coord_t value);
void lv_chart_set_line_color(lv_obj_t *chart, lv_color_t color);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_H */
