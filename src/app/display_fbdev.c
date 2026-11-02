/**
 * @file display_fbdev.c
 * @brief The framebuffer display backend of the device program (/dev/fb0, evdev touch).
 *
 * The screen is the whole framebuffer (at least 320 x 240; below 640 x 400 only a safe "screen too small" page is
 * shown); the screens lay themselves out from its size. The device file is written with pwrite(),
 * one row of a finished strip at a time, and is not mapped, so the program holds no copy of the screen (the display
 * memory belongs to the kernel and the display driver). Supported: 16 bits per pixel RGB565, and 32 bits per pixel
 * (the channel positions are read from the device). Anything else is refused with a message.
 *
 * "--fb none" runs without a display: the picture is discarded and there is no touch input (for the executable tests
 * and for a controller that is deliberately headless). A device that is not a framebuffer is an error, not "none".
 *
 * Tested only on the emulated display of docs/EMULATION.md (a virtual GPU at 32 bits per pixel). The 16 bits per pixel
 * branch (a plain row copy) has not run on any framebuffer: the emulated GPU offers only 32 bits. Never run on real
 * display hardware.
 */
#include "display.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int s_fd = -1;                                   /* -1: no display ("--fb none") */
static uint32_t s_bpp, s_stride, s_xoff, s_yoff;
static uint32_t s_rs, s_gs, s_bs;                       /* bit positions of red, green and blue at 32 bits per pixel */
static uint8_t s_draw[P99_DRAW_BYTES] __attribute__((aligned(4)));
static int s_w = P99_W, s_h = P99_H;

void p99_display_size(int *w, int *h) { *w = s_w; *h = s_h; }

/* A failed write leaves a stale row on the screen; the next redraw of that row fixes it, so there is nothing to recover. */
static void put_row(const void *row, size_t len, off_t off)
{
    ssize_t n = pwrite(s_fd, row, len, off);
    (void)n;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int32_t w = a->x2 - a->x1 + 1;
    const uint16_t *src = (const uint16_t *)px;
    uint32_t row32[2048];   /* one converted row, on the stack so it adds no static data; the screen is at most 2048 pixels wide */
    for (int32_t y = a->y1; y <= a->y2 && s_fd >= 0; y++, src += w) {
        off_t off = ((off_t)y + (off_t)s_yoff) * (off_t)s_stride + ((off_t)a->x1 + (off_t)s_xoff) * (off_t)(s_bpp / 8u);
        if (s_bpp == 16) {
            put_row(src, (size_t)w * 2u, off);
            continue;
        }
        for (int32_t x = 0; x < w; x++) {
            uint32_t p = src[x], r = (p >> 11) & 31u, g = (p >> 5) & 63u, b = p & 31u;
            row32[x] = (((r << 3) | (r >> 2)) << s_rs) | (((g << 2) | (g >> 4)) << s_gs) | (((b << 3) | (b >> 2)) << s_bs);
        }
        put_row(row32, (size_t)w * 4u, off);
    }
    lv_display_flush_ready(d);
}

static bool open_framebuffer(const char *path)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    s_fd = open(path, O_RDWR);
    if (s_fd < 0 || ioctl(s_fd, FBIOGET_VSCREENINFO, &v) != 0 || ioctl(s_fd, FBIOGET_FSCREENINFO, &f) != 0) {
        fprintf(stderr, "prometheus99: cannot use the framebuffer '%s': %s (use --fb none to run without a display)\n", path,
                strerror(errno));
        return false;
    }
    /* From 320 x 240 up the program runs; below 640 x 400 it shows only the safe "screen too small" page (hmi_ui.c). */
    if (v.xres < 320 || v.yres < 240 || v.xres > 2048) {
        fprintf(stderr, "prometheus99: the framebuffer '%s' is %ux%u; it must be 320x240 up to 2048 pixels wide\n", path,
                (unsigned)v.xres, (unsigned)v.yres);
        return false;
    }
    bool rgb565 = v.bits_per_pixel == 16 && v.red.offset == 11 && v.red.length == 5 && v.green.offset == 5 &&
                  v.green.length == 6 && v.blue.offset == 0 && v.blue.length == 5;
    if (!rgb565 && v.bits_per_pixel != 32) {
        fprintf(stderr, "prometheus99: the framebuffer '%s' is %u bits per pixel (red at %u, %u bits); only 16 bits per pixel "
                        "RGB565 and 32 bits per pixel are supported\n", path, (unsigned)v.bits_per_pixel,
                (unsigned)v.red.offset, (unsigned)v.red.length);
        return false;
    }
    s_bpp = v.bits_per_pixel;
    s_stride = f.line_length;
    s_xoff = v.xoffset;
    s_yoff = v.yoffset;
    s_w = (int)v.xres;
    s_h = (int)v.yres;
    s_rs = v.red.offset;
    s_gs = v.green.offset;
    s_bs = v.blue.offset;
    fprintf(stderr, "prometheus99: framebuffer %s %ux%u, %u bits per pixel%s\n", path, (unsigned)v.xres, (unsigned)v.yres,
            (unsigned)s_bpp, s_bpp == 32 ? " (converted from RGB565 row by row)" : "");
    return true;
}

lv_display_t *p99_display_create(const char *fb_path, const char *touch_path)
{
    bool none = strcmp(fb_path, "none") == 0;
    if (none) fprintf(stderr, "prometheus99: --fb none: running without a display\n");
    else if (!open_framebuffer(fb_path)) return NULL;
    lv_display_t *d = lv_display_create(s_w, s_h);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    if (!none) lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
    return d;
}

bool p99_display_pump(void) { return true; }
bool p99_display_take_resize(void) { return false; }
