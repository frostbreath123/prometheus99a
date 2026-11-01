#include "ui_priv.h"

/* The look of every widget, in one place and in the 16-colour palette of ui_priv.h. LVGL's default theme is
 * not compiled in (LV_USE_THEME_DEFAULT 0): it carries styles for every widget the program does not use, and
 * rounded corners, shadows, gradients and press animations the program does not want. Only three kinds of
 * object are styled here: plain objects (panels), buttons and charts. Labels take their colour and font from
 * the screen. Square corners, solid fills and one-pixel borders only, so the screen needs no anti-aliased edges
 * except where a chart line runs at a slant. */

static lv_theme_t s_theme;
static lv_style_t s_panel, s_btn, s_btn_pressed, s_btn_disabled, s_chart;
static bool s_styles_ready;

static void styles_init(void)
{
    if (s_styles_ready) return;
    s_styles_ready = true;

    lv_style_init(&s_panel);
    lv_style_set_bg_opa(&s_panel, LV_OPA_COVER);
    lv_style_set_bg_color(&s_panel, C_PANEL);
    lv_style_set_border_width(&s_panel, 0);
    lv_style_set_radius(&s_panel, 0);
    lv_style_set_pad_all(&s_panel, 0);
    lv_style_set_text_color(&s_panel, C_TEXT);

    lv_style_init(&s_btn);
    lv_style_set_bg_opa(&s_btn, LV_OPA_COVER);
    lv_style_set_bg_color(&s_btn, C_LINE);
    lv_style_set_border_width(&s_btn, 1);
    lv_style_set_border_color(&s_btn, C_MUTED);
    lv_style_set_radius(&s_btn, 0);
    lv_style_set_pad_all(&s_btn, 0);
    lv_style_set_text_color(&s_btn, C_TEXT);

    lv_style_init(&s_btn_pressed);
    lv_style_set_bg_color(&s_btn_pressed, C_ACCENT);

    lv_style_init(&s_btn_disabled);
    lv_style_set_bg_color(&s_btn_disabled, C_PANEL);
    lv_style_set_border_color(&s_btn_disabled, C_LINE);
    lv_style_set_text_color(&s_btn_disabled, C_DIM);

    lv_style_init(&s_chart);
    lv_style_set_bg_opa(&s_chart, LV_OPA_COVER);
    lv_style_set_bg_color(&s_chart, C_PANEL);
    lv_style_set_border_width(&s_chart, 0);
    lv_style_set_radius(&s_chart, 0);
    lv_style_set_pad_all(&s_chart, 0);
    lv_style_set_line_color(&s_chart, C_LINE);
    lv_style_set_line_width(&s_chart, 1);
}

static void theme_apply(lv_theme_t *th, lv_obj_t *obj)
{
    (void)th;
    if (lv_obj_get_parent(obj) == NULL) return;   /* the screen is styled by hmi_ui.c */
    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, &s_btn, 0);
        lv_obj_add_style(obj, &s_btn_pressed, LV_STATE_PRESSED);
        lv_obj_add_style(obj, &s_btn_disabled, LV_STATE_DISABLED);
#if LV_USE_CHART
    } else if (lv_obj_check_type(obj, &lv_chart_class)) {
        lv_obj_add_style(obj, &s_chart, 0);
#endif
    } else if (lv_obj_check_type(obj, &lv_obj_class)) {
        lv_obj_add_style(obj, &s_panel, 0);
    }
}

void ui_theme_install(void)
{
    styles_init();
    lv_display_t *d = lv_display_get_default();
    s_theme.disp = d;
    s_theme.font_small = LV_FONT_DEFAULT;
    s_theme.font_normal = LV_FONT_DEFAULT;
    s_theme.font_large = LV_FONT_DEFAULT;
    s_theme.apply_cb = theme_apply;
    lv_display_set_theme(d, &s_theme);
}
