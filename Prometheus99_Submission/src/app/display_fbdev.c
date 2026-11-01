/**
 * @file display_fbdev.c
 * @brief The framebuffer display backend of the device program (/dev/fb0, evdev touch).
 *
 * The screen is 800 x 480; on a larger framebuffer the picture is centred. The device file is written with pwrite(),
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
static uint8_t s_draw[P99_W * P99_DRAW_LINES * 2] __attribute__((aligned(4)));

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
    uint32_t row32[P99_W];
    for (int32_t y = a->y1; y <= a->y2 && s_fd >= 0; y++, src += w) {
        off_t off = (off_t)(y + s_yoff) * s_stride + (off_t)(a->x1 + s_xoff) * (s_bpp / 8u);
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
    if (v.xres < P99_W || v.yres < P99_H) {
        fprintf(stderr, "prometheus99: the framebuffer '%s' is %ux%u; the screens need at least %dx%d\n", path,
                (unsigned)v.xres, (unsigned)v.yres, P99_W, P99_H);
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
    s_xoff = v.xoffset + (v.xres - P99_W) / 2u;
    s_yoff = v.yoffset + (v.yres - P99_H) / 2u;
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
    lv_display_t *d = lv_display_create(P99_W, P99_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    if (!none) lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
    return d;
}

bool p99_display_pump(void) { return true; }
