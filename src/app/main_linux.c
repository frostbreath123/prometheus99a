/**
 * @file main_linux.c
 * @brief HMI entry point for Linux (framebuffer + evdev touch).
 *
 * Build target: `make -f Makefile.linux hmi` (needs libmodbus-dev). Linked and
 * launched on WSL2 without a framebuffer only; never run on a real display or
 * a Raspberry Pi. Exits with status 2 if the framebuffer cannot be opened.
 *
 *   prometheus99 --config /etc/prometheus99/hmi.json \
 *                --audit /var/lib/prometheus99/audit.log \
 *                --alarms /var/lib/prometheus99/alarms.bin \
 *                --config-key /etc/prometheus99/config.key \
 *                --config-state /var/lib/prometheus99/config.serial \
 *                [--role viewer|operator|engineer|admin]
 *                [--spool /var/spool/prometheus99] [--fb /dev/fb0] [--touch /dev/input/event0]
 *
 * The configuration is used only if "<config>.sig" verifies against the key
 * (hmi_cfgsec.h, tool p99_sign). A missing, damaged, altered, wrongly keyed or
 * rolled-back configuration, or one whose plc.host is not in plc.allowed_hosts,
 * is refused: the screen shows the reason, no PLC connection is made and writes
 * stay off. --unsigned-config exists only in the test build (-DHMI_TEST_ROLE=1).
 *
 * There is no login: the product does not authenticate anyone. The role is a
 * CLAIMED role given with --role by whoever starts the process (for example the
 * service unit); the default is viewer, which can neither acknowledge alarms nor
 * write. The core still checks the role on every privileged call (hmi_authz.h),
 * and a role above viewer is announced on stderr and in the header.
 *
 * An invalid or missing configuration never falls back to a default PLC: the
 * UI shows the reason, no connection is made and writes stay disabled.
 */
#include "lvgl.h"
#include "../ui/hmi_ui.h"
#include "../hmi/plc_libmodbus.h"
#include "../hmi/hmi_watchdog.h"
#include "../hmi/hmi_cli.h"
#include "../hmi/hmi_cfgsec.h"
#include "../hmi/hmi_auditfmt.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static volatile sig_atomic_t g_stop;
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint32_t tick_cb(void) { return (uint32_t)hmi_now_ms(); }

#ifdef P99_FILE_FB
/* Measurement build only (make target hmi-bench). A regular file stands in for /dev/fb0: it is mapped
 * shared and the flush callback copies rows into it, exactly as the fbdev driver copies into the device
 * memory. The draw buffer has the same size as the driver's (800 x LV_LINUX_FBDEV_BUFFER_SIZE lines x 2 bytes). There is no
 * touch input. It exists so that the real executable can be measured (PSS, threads, mappings) on a
 * machine without a framebuffer. Never part of a product build. */
#include <sys/mman.h>
#include <sys/stat.h>
#define BENCH_W 800
#define BENCH_H 480
#define BENCH_LINES LV_LINUX_FBDEV_BUFFER_SIZE   /* the same number of lines as the real driver (config/lv_conf.h) */
static uint8_t *s_bench_fb;
static uint8_t s_bench_draw[BENCH_W * BENCH_LINES * 2] __attribute__((aligned(4)));

static void bench_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int32_t w = a->x2 - a->x1 + 1;
    for (int32_t y = a->y1; y <= a->y2; y++) {
        memcpy(s_bench_fb + ((size_t)y * BENCH_W + (size_t)a->x1) * 2u, px, (size_t)w * 2u);
        px += (size_t)w * 2u;
    }
    lv_display_flush_ready(d);
}

static lv_display_t *bench_display_create(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, BENCH_W * BENCH_H * 2) != 0) return NULL;
    s_bench_fb = mmap(NULL, BENCH_W * BENCH_H * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (s_bench_fb == MAP_FAILED) return NULL;
    lv_display_t *d = lv_display_create(BENCH_W, BENCH_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, s_bench_draw, NULL, sizeof(s_bench_draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, bench_flush);
    return d;
}
#endif

static hmi_runtime_t g_rt;
static hmi_app_cfg_t g_cfg;

int main(int argc, char **argv)
{
    /* Strict parsing: anything not understood is exit status 2, before any
     * hardware is touched. The role is a CLAIMED role chosen by whoever starts
     * the process, not an authenticated identity (see hmi_cli.h). */
    hmi_cli_t cli;
    char cli_err[160];
    if (!hmi_cli_parse(argc, argv, &cli, cli_err, sizeof(cli_err))) {
        fprintf(stderr, "prometheus99: %s\n", cli_err);
        fprintf(stderr, "usage: prometheus99 [--config FILE] [--audit FILE] [--alarms FILE]\n"
                        "                    [--config-key FILE] [--config-state FILE] [--spool DIR]\n"
                        "                    [--role viewer|operator|engineer|admin] [--fb DEV] [--touch DEV]"
#ifdef HMI_TEST_ROLE
                        " [--unsigned-config]"
#endif
                        "\n");
        return 2;
    }
#ifndef HMI_TEST_ROLE
    if (cli.unsigned_config) {
        fprintf(stderr, "prometheus99: --unsigned-config is not available in this build: the configuration must be "
                        "signed (see p99_sign). (The simulation build 'hmi-sim' accepts it for tests.)\n");
        return 2;
    }
#endif
    const char *cfg_path = cli.config;
    const char *audit_path = cli.audit;
    const char *alarm_path = cli.alarms;
    const char *fb_path = cli.fb;
    const char *touch_path = cli.touch;
    hmi_role_t role = cli.role;
    fprintf(stderr, "prometheus99: role=%s (claimed on the command line, not authenticated)\n", hmi_role_name(role));
    if (role != HMI_ROLE_VIEWER)
        fprintf(stderr, "prometheus99: warning: nothing authenticates this role; anyone who can start the process or edit "
                        "its service unit can choose it\n");
#ifdef HMI_TEST_ROLE
    fprintf(stderr, "prometheus99: TEST BUILD: --unsigned-config is available and nothing is authenticated\n");
#endif

    /* lv_linux_fbdev_set_file() returns void, so probe the device ourselves.
     * Without a display the process must fail fast (non-zero) so a service
     * manager can see it, not keep polling the PLC headless. */
#ifdef P99_FILE_FB
    int fb_probe = open(fb_path, O_RDWR | O_CREAT, 0600);
#else
    int fb_probe = open(fb_path, O_RDWR);
#endif
    if (fb_probe < 0) {
        fprintf(stderr, "prometheus99: cannot open framebuffer '%s': %s\n", fb_path, strerror(errno));
        return 2;
    }
    close(fb_probe);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    char err[200];
    bool cfg_ok;
    char banner[240];
    banner[0] = '\0';
    char cfg_info[96] = "Configuration: not loaded";   /* shown on the settings screen */
    uint32_t cfg_serial = 0;                           /* serial of the signature in use: the base of a change request */
#ifdef HMI_TEST_ROLE
    if (cli.unsigned_config) {
        /* Test build only: no signature, no key, no allow-list. */
        cfg_ok = hmi_config_load_file(cfg_path, &g_cfg, err, sizeof(err));
        if (cfg_ok) snprintf(cfg_info, sizeof(cfg_info), "Configuration: NOT verified (test build)");
        fprintf(stderr, "prometheus99: TEST BUILD: configuration NOT verified (--unsigned-config)\n");
    } else
#endif
    {
        /* The configuration is used only if its signature verifies (hmi_cfgsec.h).
         * Any failure is safe mode: banner on screen, no PLC endpoint, writes off. */
        hmi_cfgsec_paths_t sp;
        sp.config_path = cfg_path;
        sp.sig_path = NULL;
        sp.key_path = cli.config_key;
        sp.state_path = cli.config_state;
        hmi_cfgsec_info_t si;
        hmi_cfgsec_result_t sr = hmi_cfgsec_load(&sp, &g_cfg, &si);
        cfg_ok = (sr == HMI_CFGSEC_OK);
        cfg_serial = si.serial;
        snprintf(err, sizeof(err), "%s", si.message);
        char aline[200];
        size_t alen = audit_path ? hmi_cfgsec_format_audit(hmi_now_ms(), &si, aline, sizeof(aline)) : 0;
        if (alen > 0) {
            hmi_wall_t aw;
            hmi_wall_now(&aw);
            size_t stamped = hmi_audit_stamp_line(aline, strlen(aline), sizeof(aline), &aw);
            hmi_fs_append_sync(audit_path, aline, stamped ? stamped : strlen(aline)); /* best effort: nothing depends on it */
        }
        if (cfg_ok) {
            snprintf(cfg_info, sizeof(cfg_info), "Configuration: signed, serial %u", (unsigned)si.serial);
            fprintf(stderr, "prometheus99: configuration verified (signature serial %u)\n", (unsigned)si.serial);
            if (si.state_damaged)
                fprintf(stderr, "prometheus99: warning: the config serial file '%s' was damaged and was treated as absent\n",
                        cli.config_state);
            if (!si.serial_recorded)
                fprintf(stderr, "prometheus99: warning: the config serial could not be stored in '%s'; rollback protection is not active\n",
                        cli.config_state);
            if (si.config_writable_by_service)
                fprintf(stderr, "prometheus99: warning: the service user can modify the configuration file; a compromised "
                                "service could rewrite it (the signature would still be checked at the next start)\n");
            if (si.key_writable_by_service)
                fprintf(stderr, "prometheus99: warning: the service user can modify the configuration key file\n");
        } else {
            fprintf(stderr, "prometheus99: CONFIGURATION REJECTED (%s): %s\n", hmi_cfgsec_reason_code(sr), si.message);
        }
    }
    if (!cfg_ok) snprintf(banner, sizeof(banner), "CONFIG REJECTED: %s", err);

    plc_libmodbus_t *plc = NULL;
    bool runtime_up = false;
    if (cfg_ok) {
        plc = plc_libmodbus_create(g_cfg.host, g_cfg.port, g_cfg.unit, g_cfg.response_timeout_ms);
        if (plc) {
            hmi_plc_driver_t drv;
            plc_libmodbus_driver(plc, &drv);
            runtime_up = hmi_runtime_start(&g_rt, &g_cfg, &drv, audit_path, alarm_path);
        }
        if (!runtime_up) snprintf(banner, sizeof(banner), "RUNTIME START FAILED");
    }

    lv_init();
    lv_tick_set_cb(tick_cb);
#ifdef P99_FILE_FB
    if (!bench_display_create(fb_path)) {
        fprintf(stderr, "prometheus99: cannot create the file framebuffer '%s'\n", fb_path);
        return 2;
    }
    (void)touch_path;
#else
    lv_display_t *disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, fb_path);
    /* Only RGB565 is compiled into LVGL's drawing code (config/lv_conf.h, LV_DRAW_SW_SUPPORT_*): a framebuffer of
     * another depth would draw nothing, so refuse to start if the driver gives another one. (Not tested on a real
     * framebuffer: the check reads the format the LVGL driver chose from the device's reported depth.) */
    if (lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565) {
        fprintf(stderr, "prometheus99: the framebuffer '%s' is not 16 bits per pixel; set the display to 16 bits "
                        "(on a Raspberry Pi: framebuffer_depth=16 in config.txt, or fbset -depth 16)\n", fb_path);
        return 2;
    }
    lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
    (void)touch;
#endif

    hmi_ui_enable_heartbeat(true);
    hmi_ui_init(runtime_up ? &g_rt : NULL, role, banner[0] ? banner : NULL);
    hmi_ui_set_config_info(cfg_info);
    /* Changing settings from the screen needs a verified configuration, a running runtime (it writes the
     * audit record) and a spool directory for the root helper p99_cfg. Without all three the view stays off. */
#ifdef P99_LITE
    if (cli.spool) fprintf(stderr, "prometheus99: note: --spool is ignored: the lite profile has no CONFIG view\n");
#else
    if (cli.spool && cfg_ok && runtime_up) hmi_ui_set_config_edit(cli.spool, cfg_serial);
#endif

    /* Supervisor liveness. Pings (systemd WATCHDOG=1) are sent from this
     * loop only while the comms and supervisor threads are also advancing, so
     * a stuck thread of any kind ends in a supervisor restart. Without a
     * supervisor (NOTIFY_SOCKET / WATCHDOG_USEC unset) this is a no-op. */
    hmi_watchdog_t wd;
    int32_t beats[HMI_WD_CHANNELS] = { 0, 0 };
    uint32_t wd_interval = hmi_watchdog_interval_from_env(getenv("WATCHDOG_USEC"), getenv("WATCHDOG_PID"),
                                                          (uint32_t)getpid());
    /* Without a runtime there are no worker threads to watch. */
    uint32_t stall_ms = runtime_up ? hmi_runtime_worst_stall_ms(&g_rt) : 0xFFFFFFFFu;
    hmi_watchdog_init(&wd, wd_interval, stall_ms, hmi_now_ms(), beats);
    bool ready_sent = false;

    while (!g_stop) {
        uint32_t wait = lv_timer_handler();
        if (!ready_sent) { /* the first frame has been built and flushed */
            hmi_notify("READY=1");
            ready_sent = true;
        }
        if (runtime_up) {
            hmi_runtime_beats_t b = hmi_runtime_beats(&g_rt);
            beats[0] = b.comms;
            beats[1] = b.supervisor;
        }
        if (hmi_watchdog_step(&wd, hmi_now_ms(), beats)) hmi_notify("WATCHDOG=1");
        if (wait > 33u) wait = 33u;
        if (wait < 5u) wait = 5u;
        hmi_sleep_ms(wait);
    }

    hmi_notify("STOPPING=1");
    if (runtime_up) hmi_runtime_stop(&g_rt);
    if (plc) plc_libmodbus_destroy(plc);
    lv_deinit();
    return 0;
}
