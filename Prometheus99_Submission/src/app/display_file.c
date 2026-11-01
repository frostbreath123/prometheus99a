/**
 * @file display_file.c
 * @brief Measurement build only (make target hmi-bench): a regular file stands in for /dev/fb0.
 *
 * The file is mapped shared and the flush callback copies rows into it, as a framebuffer driver would copy into device
 * memory, so the real executable can be measured (PSS, threads, mappings) on a machine without a framebuffer. The
 * mapping is reported on its own by the bench scripts. The draw buffer is the size of the device backend's. There is no
 * touch input. Never part of a product build.
 */
#include "display.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint8_t *s_fb;
static uint8_t s_draw[P99_W * P99_DRAW_LINES * 2] __attribute__((aligned(4)));

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    size_t w = (size_t)(a->x2 - a->x1 + 1);
    for (int32_t y = a->y1; y <= a->y2; y++, px += w * 2u)
        memcpy(s_fb + ((size_t)y * P99_W + (size_t)a->x1) * 2u, px, w * 2u);
    lv_display_flush_ready(d);
}

lv_display_t *p99_display_create(const char *fb_path, const char *touch_path)
{
    (void)touch_path;
    int fd = open(fb_path, O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, P99_W * P99_H * 2) != 0) {
        fprintf(stderr, "prometheus99: cannot create the file framebuffer '%s'\n", fb_path);
        return NULL;
    }
    s_fb = mmap(NULL, P99_W * P99_H * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (s_fb == MAP_FAILED) {
        fprintf(stderr, "prometheus99: cannot map the file framebuffer '%s'\n", fb_path);
        return NULL;
    }
    lv_display_t *d = lv_display_create(P99_W, P99_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    return d;
}

bool p99_display_pump(void) { return true; }
