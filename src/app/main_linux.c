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
 *                [--report FILE]
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
#include "../hmi/hmi_util.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop;
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint32_t tick_cb(void) { return (uint32_t)hmi_now_ms(); }

static hmi_runtime_t g_rt;   /* g_rt.cfg holds the configuration from boot on */

/* --report: run statistics, collected only when a report path was given (README, docs/OPTIMIZATION_CHANGELOG.md).
 * Compiled into the measurement builds only (P99_REPORT: prometheus99-bench, the SDL window), so the device program
 * keeps its size; the device program refuses the option. */
#ifdef P99_REPORT
typedef struct {
    uint64_t t0_ms, loops, ui_calls, ui_sum_us, ui_max_us;
} run_stats_t;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* One "key=value" per line, read by bench scripts. Written before the runtime stops, while its counters are valid. */
static void write_report(const char *path, const run_stats_t *s, bool runtime_up)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "prometheus99: cannot write the report %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(f, "run_ms=%llu\nloops=%llu\nui_handler_calls=%llu\nui_handler_mean_us=%llu\nui_handler_max_us=%llu\n",
            (unsigned long long)(hmi_now_ms() - s->t0_ms), (unsigned long long)s->loops, (unsigned long long)s->ui_calls,
            (unsigned long long)(s->ui_calls ? s->ui_sum_us / s->ui_calls : 0), (unsigned long long)s->ui_max_us);
    if (runtime_up) {
        hmi_comms_stats_t c;
        hmi_writes_stats_t w;
        hmi_runtime_storage_info_t st;
        hmi_comms_stats(&g_rt.comms, &c);
        hmi_writes_stats(&g_rt.writes, &w);
        hmi_runtime_storage_info(&g_rt, &st);
        fprintf(f, "polls_ok=%u\npolls_failed=%u\nexceptions=%u\nprotocol_errors=%u\nconnects_ok=%u\nconnects_failed=%u\n"
                   "links_lost=%u\nmax_poll_latency_ms=%u\n",
                (unsigned)c.polls_ok, (unsigned)c.polls_failed, (unsigned)c.exceptions, (unsigned)c.protocol_errors,
                (unsigned)c.connects_ok, (unsigned)c.connects_failed, (unsigned)c.links_lost, (unsigned)c.max_poll_latency_ms);
        fprintf(f, "writes_submitted=%u\nwrites_confirmed=%u\nwrites_rejected=%u\nwrites_unknown=%u\nwrites_expired=%u\n",
                (unsigned)w.submitted, (unsigned)w.confirmed, (unsigned)w.rejected, (unsigned)w.unknown, (unsigned)w.expired);
        fprintf(f, "store_written=%u\nstore_failed=%u\nstore_refused=%u\nstore_max_depth=%u\nstore_rotations=%u\n",
                (unsigned)st.store.written, (unsigned)st.store.failed, (unsigned)st.store.refused,
                (unsigned)st.store.max_depth, (unsigned)st.store.rotations);
    }
#ifdef HMI_LOCK_STATS
    char locks[2048];
    hmi_lock_stats_text(locks, sizeof(locks));
    fputs(locks, f);
#endif
    fclose(f);
}
#endif

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
                        "                    [--role viewer|operator|engineer|admin] [--fb DEV] [--touch DEV] [--report FILE]"
#ifdef HMI_TEST_ROLE
                        " [--unsigned-config]"
#endif
                        "\n");
        return 2;
    }
#ifndef P99_REPORT
    if (cli.report) {
        fprintf(stderr, "prometheus99: --report is available only in the measurement builds (hmi-bench, hmi-sdl)\n");
        return 2;
    }
#endif
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
    hmi_boot_config(&cli, &g_rt.cfg, &boot);   /* loaded in place: hmi_runtime_start uses it without a copy */
    bool cfg_ok = boot.ok;
    char banner[240];
    snprintf(banner, sizeof(banner), "%s", boot.banner);

    plc_libmodbus_t *plc = NULL;
    bool runtime_up = false;
    if (cfg_ok) {
#ifdef HMI_RTU
        if (g_rt.cfg.transport == HMI_TRANSPORT_RTU)
            plc = plc_libmodbus_create_rtu(g_rt.cfg.device, g_rt.cfg.baud, (char)g_rt.cfg.parity, g_rt.cfg.stop_bits,
                                           g_rt.cfg.unit, g_rt.cfg.response_timeout_ms);
        else
#endif
        plc = plc_libmodbus_create(g_rt.cfg.host, g_rt.cfg.port, g_rt.cfg.unit, g_rt.cfg.response_timeout_ms);
        if (plc) {
            hmi_plc_driver_t drv;
            plc_libmodbus_driver(plc, &drv);
            hmi_runtime_storage_t st;
            memset(&st, 0, sizeof(st));
            st.audit_path = audit_path;
            st.alarm_state_path = alarm_path;
            st.audit_key = boot.audit_key_valid ? boot.audit_key : NULL; /* the audit file is sealed (hmi_auditchain.h) */
            runtime_up = hmi_runtime_start_ex(&g_rt, &g_rt.cfg, &drv, &st);
        }
        if (!runtime_up) snprintf(banner, sizeof(banner), "RUNTIME START FAILED");
    }
    hmi_wipe(boot.audit_key, sizeof(boot.audit_key)); /* the storage worker keeps its own copy */
    /* Named users (hmi_auth.h): with users in a verified configuration a login is required and --role means nothing.
     * Without users the claimed role is used as before. */
    if (runtime_up && g_rt.cfg.n_users > 0) {
        if (boot.audit_key_valid) {
            hmi_ui_set_users(g_rt.cfg.users, g_rt.cfg.n_users, boot.pin_key);
            fprintf(stderr, "prometheus99: %u named users configured: log in on the screen to act\n",
                    (unsigned)g_rt.cfg.n_users);
            if (cli.role_given) fprintf(stderr, "prometheus99: note: --role is ignored when the configuration has users\n");
        } else {
            fprintf(stderr, "prometheus99: warning: the configuration has users but was not verified, so login is off "
                            "and the claimed role is used\n");
        }
    }
    hmi_wipe(boot.pin_key, sizeof(boot.pin_key)); /* the login keeps its own copy */

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
#ifdef P99_REPORT
    run_stats_t rs;
    memset(&rs, 0, sizeof(rs));
    rs.t0_ms = hmi_now_ms();
#endif

    while (!g_stop) {
        if (!p99_display_pump()) break;
        if (p99_display_take_resize()) hmi_ui_relayout();
#ifdef P99_REPORT
        uint64_t ui_t0 = cli.report ? now_us() : 0;
#endif
        uint32_t wait = lv_timer_handler();
#ifdef P99_REPORT
        if (cli.report) {
            uint64_t dt = now_us() - ui_t0;
            rs.loops++;
            rs.ui_calls++;
            rs.ui_sum_us += dt;
            if (dt > rs.ui_max_us) rs.ui_max_us = dt;
        }
#endif
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
#ifdef P99_REPORT
    if (cli.report) write_report(cli.report, &rs, runtime_up);
#endif
    if (runtime_up) hmi_runtime_stop(&g_rt);
    if (plc) plc_libmodbus_destroy(plc);
    lv_deinit();
    return 0;
}
