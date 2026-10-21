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
 *                --users /var/lib/prometheus99/users.bin \
 *                --config-key /etc/prometheus99/config.key \
 *                --config-state /var/lib/prometheus99/config.serial \
 *                [--fb /dev/fb0] [--touch /dev/input/event0]
 *
 * The configuration is used only if "<config>.sig" verifies against the key
 * (hmi_cfgsec.h, tool p99_sign). A missing, damaged, altered, wrongly keyed or
 * rolled-back configuration, or one whose plc.host is not in plc.allowed_hosts,
 * is refused: the screen shows the reason, no PLC connection is made and writes
 * stay off. --unsigned-config exists only in the simulation build.
 *
 * Roles come from logging in on the screen (hmi_auth.h). Until someone logs in
 * the role is Viewer. The first time, while no user file exists, the login
 * screen offers to create the administrator.
 *
 * --role viewer|operator|engineer|admin exists only in a test build (compile
 * with -DHMI_TEST_ROLE=1, make target hmi-sim). The production build refuses it
 * with exit status 2, so a started process can never be given a role from the
 * command line. A test build shows a permanent "TEST ROLE ... NOT AUTHENTICATED"
 * banner.
 *
 * An invalid or missing configuration never falls back to a default PLC: the
 * UI shows the reason, no connection is made and writes stay disabled.
 */
#include "lvgl.h"
#include "../ui/hmi_ui.h"
#include "../hmi/plc_libmodbus.h"
#include "../hmi/hmi_watchdog.h"
#include "../hmi/hmi_cli.h"
#include "../hmi/hmi_auth.h"
#include "../hmi/hmi_cfgsec.h"
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
#ifndef HMI_TEST_ROLE
static hmi_auth_t g_auth;
#endif

int main(int argc, char **argv)
{
    /* Strict parsing: anything not understood is exit status 2, before any
     * hardware is touched. The role is a CLAIMED role chosen by whoever starts
     * the process, not an authenticated identity (see hmi_cli.h). */
    hmi_cli_t cli;
    char cli_err[160];
    if (!hmi_cli_parse(argc, argv, &cli, cli_err, sizeof(cli_err))) {
        fprintf(stderr, "prometheus99: %s\n", cli_err);
        fprintf(stderr, "usage: prometheus99 [--config FILE] [--audit FILE] [--alarms FILE] [--users FILE]\n"
                        "                    [--config-key FILE] [--config-state FILE] [--fb DEV] [--touch DEV]"
#ifdef HMI_TEST_ROLE
                        " [--role viewer|operator|engineer|admin] [--unsigned-config]"
#endif
                        "\n");
        return 2;
    }
#ifndef HMI_TEST_ROLE
    if (cli.role_given) {
        fprintf(stderr, "prometheus99: --role is not available in this build: roles come from logging in on the "
                        "screen. (The simulation build 'hmi-sim' accepts it for tests.)\n");
        return 2;
    }
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
#ifdef HMI_TEST_ROLE
    hmi_role_t role = cli.role;
    fprintf(stderr, "prometheus99: role=%s (claimed on the command line, not authenticated)\n",
            hmi_role_name(role));
    fprintf(stderr, "prometheus99: TEST BUILD: --role is honoured and nothing is authenticated\n");
#else
    {
        hmi_auth_cfg_t acfg;
        acfg.users_path = cli.users;
        acfg.idle_ms = 0;
        acfg.kdf = NULL;
        if (!hmi_auth_init(&g_auth, &acfg, hmi_now_ms())) {
            fprintf(stderr, "prometheus99: cannot use the user store path '%s'\n", cli.users);
            return 2;
        }
        if (audit_path) hmi_auth_set_audit(&g_auth, hmi_auth_file_audit, (void *)audit_path);
        hmi_auth_store_t st = hmi_auth_store_state(&g_auth);
        if (st == HMI_AUTH_STORE_READY)
            fprintf(stderr, "prometheus99: user store %s: ready\n", cli.users);
        else if (st == HMI_AUTH_STORE_NONE)
            fprintf(stderr, "prometheus99: user store %s: absent (first boot: the login screen creates the administrator)\n",
                    cli.users);
        else
            fprintf(stderr, "prometheus99: user store %s: INVALID, login disabled (%s)\n", cli.users,
                    hmi_auth_store_error(&g_auth));
        if (hmi_auth_store_readable_by_others(&g_auth))
            fprintf(stderr, "prometheus99: warning: the user store can be read by group or others (use mode 0600)\n");
        if (!audit_path) fprintf(stderr, "prometheus99: warning: no --audit file: logins will not be recorded\n");
    }
#endif

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

    char err[200];
    bool cfg_ok;
    char banner[240];
    banner[0] = '\0';
#ifdef HMI_TEST_ROLE
    if (cli.unsigned_config) {
        /* Test build only: no signature, no key, no allow-list. */
        cfg_ok = hmi_config_load_file(cfg_path, &g_cfg, err, sizeof(err));
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
        snprintf(err, sizeof(err), "%s", si.message);
        char aline[100];
        if (audit_path && hmi_cfgsec_format_audit(hmi_now_ms(), &si, aline, sizeof(aline)) > 0)
            hmi_fs_append_sync(audit_path, aline, strlen(aline)); /* best effort: nothing depends on it */
        if (cfg_ok) {
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
    lv_display_t *disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, fb_path);
    lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
    (void)touch;

    hmi_ui_enable_heartbeat(true);
#ifdef HMI_TEST_ROLE
    hmi_ui_init(runtime_up ? &g_rt : NULL, role, banner[0] ? banner : NULL);
#else
    hmi_ui_init_auth(runtime_up ? &g_rt : NULL, &g_auth, banner[0] ? banner : NULL);
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
#ifndef HMI_TEST_ROLE
    hmi_auth_deinit(&g_auth);
#endif
    return 0;
}
