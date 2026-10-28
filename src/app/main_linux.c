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
#include "display.h"
#include "../ui/hmi_ui.h"
#include "../hmi/plc_libmodbus.h"
#include "../hmi/hmi_watchdog.h"
#include "../hmi/hmi_cli.h"
#include "../hmi/hmi_boot.h"
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

#ifdef P99_MULTICALL
int p99_cfg_main(int argc, char **argv);
#endif

int main(int argc, char **argv)
{
#ifdef P99_MULTICALL
    /* Started under the name p99_cfg (a link to this file) it is the root helper that applies a configuration request. */
    const char *self = strrchr(argv[0], '/');
    self = self ? self + 1 : argv[0];
    if (strcmp(self, "p99_cfg") == 0) return p99_cfg_main(argc, argv);
#endif
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

    /* Without a display the process must fail fast (non-zero) so a service manager can see it, not keep polling the
     * PLC headless: the display is created before anything else is touched. LVGL always draws RGB565 (the only format
     * compiled into config/lv_conf.h); the backend (display.h) turns each strip into what the screen needs, so a default
     * Raspberry Pi framebuffer at 32 bits per pixel works without changing its depth. "--fb none" runs without one. */
    lv_init();
    lv_tick_set_cb(tick_cb);
    if (!p99_display_create(fb_path, touch_path)) return 2;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    hmi_boot_t boot;   /* signed configuration, or safe mode: banner on screen, no PLC endpoint, writes off (hmi_boot.h) */
    hmi_boot_config(&cli, &g_cfg, &boot);
    bool cfg_ok = boot.ok;
    char banner[240];
    snprintf(banner, sizeof(banner), "%s", boot.banner);

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

    hmi_ui_enable_heartbeat(true);
    hmi_ui_init(runtime_up ? &g_rt : NULL, role, banner[0] ? banner : NULL);
    hmi_ui_set_config_info(boot.info);
    /* Changing settings from the screen needs a verified configuration, a running runtime (it writes the
     * audit record) and a spool directory for the root helper p99_cfg. Without all three the view stays off. */
#ifdef P99_LITE
    if (cli.spool) fprintf(stderr, "prometheus99: note: --spool is ignored: the lite profile has no CONFIG view\n");
#else
    if (cli.spool && cfg_ok && runtime_up) hmi_ui_set_config_edit(cli.spool, boot.serial);
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
        if (!p99_display_pump()) break;
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
