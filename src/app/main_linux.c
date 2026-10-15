/**
 * @file main_linux.c
 * @brief Production HMI entry point for Linux (framebuffer + evdev touch).
 *
 * Build target: `make -f Makefile.linux hmi` (needs libmodbus-dev). Linked and
 * launched on WSL2 without a framebuffer only; never run on a real display or
 * a Raspberry Pi. Exits with status 2 if the framebuffer cannot be opened.
 *
 *   prometheus99 --config /etc/prometheus99/hmi.json \
 *                --audit /var/lib/prometheus99/audit.log \
 *                --alarms /var/lib/prometheus99/alarms.bin \
 *                [--fb /dev/fb0] [--touch /dev/input/event0] [--role viewer|operator|engineer|admin]
 *
 * The role defaults to viewer and is a claimed role, not authentication.
 *
 * An invalid or missing configuration never falls back to a default PLC: the
 * UI shows the reason, no connection is made and writes stay disabled.
 */
#include "lvgl.h"
#include "../ui/hmi_ui.h"
#include "../hmi/plc_libmodbus.h"
#include "../hmi/hmi_watchdog.h"
#include "../hmi/hmi_cli.h"
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
        fprintf(stderr, "usage: prometheus99 [--config FILE] [--audit FILE] [--alarms FILE] [--fb DEV]\n"
                        "                    [--touch DEV] [--role viewer|operator|engineer|admin]\n");
        return 2;
    }
    const char *cfg_path = cli.config;
    const char *audit_path = cli.audit;
    const char *alarm_path = cli.alarms;
    const char *fb_path = cli.fb;
    const char *touch_path = cli.touch;
    hmi_role_t role = cli.role;
    fprintf(stderr, "prometheus99: role=%s (claimed on the command line, not authenticated)\n",
            hmi_role_name(role));

    /* lv_linux_fbdev_set_file() returns void, so probe the device ourselves.
     * Without a display the process must fail fast (non-zero) so a service
     * manager can see it, not keep polling the PLC headless. */
    int fb_probe = open(fb_path, O_RDWR);
    if (fb_probe < 0) {
        fprintf(stderr, "prometheus99: cannot open framebuffer '%s': %s\n", fb_path, strerror(errno));
        return 2;
    }
    close(fb_probe);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    char err[160];
    bool cfg_ok = hmi_config_load_file(cfg_path, &g_cfg, err, sizeof(err));
    char banner[200];
    banner[0] = '\0';
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
    lv_display_t *disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, fb_path);
    lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
    (void)touch;

    hmi_ui_enable_heartbeat(true);
    hmi_ui_init(runtime_up ? &g_rt : NULL, role, banner[0] ? banner : NULL);

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
