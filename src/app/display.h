/**
 * @file display.h
 * @brief The display backend of the Linux program: one of display_fbdev.c, display_file.c or display_sdl.c is linked.
 *
 * LVGL always draws RGB565 into a strip buffer (the only pixel format compiled into config/lv_conf.h). A backend turns
 * each finished strip into something a screen shows, and (where it has one) feeds the touch input to LVGL.
 *   display_fbdev.c  the device: /dev/fb0 at 16 or 32 bits per pixel, evdev touch; "--fb none" discards the picture
 *   display_file.c   measurement build (make hmi-bench): a regular file stands in for the framebuffer
 *   display_sdl.c    desktop window (make hmi-sdl): an SDL2 window, the mouse stands in for a touch
 */
#ifndef P99_DISPLAY_H
#define P99_DISPLAY_H

#include "lvgl.h"
#include <stdbool.h>

#define P99_W 800
#define P99_H 480
#define P99_DRAW_LINES 20   /* height of one strip in lines: 20 lines of 800 pixels at 2 bytes is 32 KB */

/* Creates the LVGL display (and the touch input, if the backend has one). On failure it prints the reason and returns
 * NULL; the program then exits with status 2 before it touches the configuration or the PLC. */
lv_display_t *p99_display_create(const char *fb_path, const char *touch_path);

/* Called once per loop turn, before lv_timer_handler(): window events, showing the finished frame. Returns false when
 * the user closed the window. The framebuffer backends have nothing to do and return true. */
bool p99_display_pump(void);

#endif
