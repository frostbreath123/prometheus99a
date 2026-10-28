/**
 * @file display_sdl.c
 * @brief Desktop window backend (make target hmi-sdl): the real program in an SDL2 window instead of /dev/fb0.
 *
 * For showing the Linux build on a machine with a window system (for example WSLg). A development and demonstration
 * backend: the device build never contains it. A mouse click stands in for a touch. A copy of the frame is kept so that
 * SIGUSR1 saves exactly what the window shows (a PPM file named by $P99_SHOT, default /tmp/p99_shot.ppm).
 */
#include "display.h"
#include <SDL2/SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static uint16_t s_shadow[P99_W * P99_H];
static uint8_t s_draw[P99_W * P99_DRAW_LINES * 2] __attribute__((aligned(4)));
static volatile sig_atomic_t s_shot_requested;
static bool s_dirty;
static int s_mx, s_my;
static bool s_down;
static bool s_press_pending;   /* a press seen since the last read: reported once even if the button was released before LVGL read it */

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int32_t w = a->x2 - a->x1 + 1;
    for (int32_t y = a->y1; y <= a->y2; y++)
        memcpy(&s_shadow[(size_t)y * P99_W + (size_t)a->x1], px + (size_t)(y - a->y1) * (size_t)w * 2u, (size_t)w * 2u);
    SDL_Rect r = { a->x1, a->y1, w, a->y2 - a->y1 + 1 };
    SDL_UpdateTexture(s_tex, &r, px, w * 2);
    s_dirty = true;
    lv_display_flush_ready(d);
}

static void pointer_cb(lv_indev_t *i, lv_indev_data_t *data)
{
    (void)i;
    data->point.x = s_mx;
    data->point.y = s_my;
    data->state = (s_down || s_press_pending) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    s_press_pending = false;
}

static void on_usr1(int sig)
{
    (void)sig;
    s_shot_requested = 1;
}

static void save_shot(void)
{
    const char *path = getenv("P99_SHOT");
    FILE *f = fopen(path ? path : "/tmp/p99_shot.ppm", "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", P99_W, P99_H);
    for (int i = 0; i < P99_W * P99_H; i++) {
        uint16_t p = s_shadow[i];
        uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 31) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

lv_display_t *p99_display_create(const char *fb_path, const char *touch_path)
{
    (void)fb_path;
    (void)touch_path;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "prometheus99: SDL_Init: %s\n", SDL_GetError());
        return NULL;
    }
    s_win = SDL_CreateWindow("Prometheus99 HMI (Linux, SDL2 window)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             P99_W, P99_H, SDL_WINDOW_SHOWN);
    s_ren = s_win ? SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE) : NULL;
    s_tex = s_ren ? SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, P99_W, P99_H) : NULL;
    if (!s_tex) {
        fprintf(stderr, "prometheus99: cannot create the window: %s\n", SDL_GetError());
        return NULL;
    }
    lv_display_t *d = lv_display_create(P99_W, P99_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    lv_indev_t *p = lv_indev_create();
    lv_indev_set_type(p, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(p, pointer_cb);
    signal(SIGUSR1, on_usr1);
    return d;
}

bool p99_display_pump(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) return false;
        if (e.type == SDL_MOUSEMOTION) { s_mx = e.motion.x; s_my = e.motion.y; }
        if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) && e.button.button == SDL_BUTTON_LEFT) {
            s_mx = e.button.x;
            s_my = e.button.y;
            s_down = e.type == SDL_MOUSEBUTTONDOWN;
            if (s_down) s_press_pending = true;
        }
    }
    if (s_dirty) {
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        SDL_RenderPresent(s_ren);
        s_dirty = false;
    }
    if (s_shot_requested) { s_shot_requested = 0; save_shot(); }
    return true;
}
