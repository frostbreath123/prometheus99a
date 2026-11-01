#include "hmi_boot.h"
#include "hmi_auditfmt.h"
#include "hmi_cfgsec.h"
#include "hmi_platform.h"
#include <stdio.h>
#include <string.h>

/* One line to the audit file, best effort: nothing depends on it. */
static void audit_verdict(const char *audit_path, const hmi_cfgsec_info_t *si)
{
    char line[200];
    size_t len = audit_path ? hmi_cfgsec_format_audit(hmi_now_ms(), si, line, sizeof(line)) : 0;
    if (len == 0) return;
    hmi_wall_t wall;
    hmi_wall_now(&wall);
    size_t stamped = hmi_audit_stamp_line(line, strlen(line), sizeof(line), &wall);
    hmi_fs_append_sync(audit_path, line, stamped ? stamped : strlen(line));
}

static void warn_about_files(const hmi_cli_t *cli, const hmi_cfgsec_info_t *si)
{
    if (si->state_damaged)
        fprintf(stderr, "prometheus99: warning: the config serial file '%s' was damaged and was treated as absent\n",
                cli->config_state);
    if (!si->serial_recorded)
        fprintf(stderr, "prometheus99: warning: the config serial could not be stored in '%s'; rollback protection is not active\n",
                cli->config_state);
    if (si->config_writable_by_service)
        fprintf(stderr, "prometheus99: warning: the service user can modify the configuration file; a compromised "
                        "service could rewrite it (the signature would still be checked at the next start)\n");
    if (si->key_writable_by_service)
        fprintf(stderr, "prometheus99: warning: the service user can modify the configuration key file\n");
}

void hmi_boot_config(const hmi_cli_t *cli, hmi_app_cfg_t *cfg, hmi_boot_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->info, sizeof(out->info), "Configuration: not loaded");
#ifdef HMI_TEST_ROLE
    if (cli->unsigned_config) {
        char err[200] = "";
        /* Test build only: no signature, no key, no allow-list. */
        out->ok = hmi_config_load_file(cli->config, cfg, err, sizeof(err));
        if (out->ok) snprintf(out->info, sizeof(out->info), "Configuration: NOT verified (test build)");
        fprintf(stderr, "prometheus99: TEST BUILD: configuration NOT verified (--unsigned-config)\n");
        if (!out->ok) snprintf(out->banner, sizeof(out->banner), "CONFIG REJECTED: %s", err);
        return;
    }
#endif
    /* The configuration is used only if its signature verifies (hmi_cfgsec.h). Any failure is safe mode. */
    hmi_cfgsec_paths_t paths = { cli->config, NULL, cli->config_key, cli->config_state };
    hmi_cfgsec_info_t si;
    hmi_cfgsec_result_t result = hmi_cfgsec_load(&paths, cfg, &si);
    out->ok = (result == HMI_CFGSEC_OK);
    out->serial = si.serial;
    audit_verdict(cli->audit, &si);
    if (out->ok) {
        snprintf(out->info, sizeof(out->info), "Configuration: signed, serial %u", (unsigned)si.serial);
        fprintf(stderr, "prometheus99: configuration verified (signature serial %u)\n", (unsigned)si.serial);
        warn_about_files(cli, &si);
        return;
    }
    out->serial = 0;
    fprintf(stderr, "prometheus99: CONFIGURATION REJECTED (%s): %s\n", hmi_cfgsec_reason_code(result), si.message);
    snprintf(out->banner, sizeof(out->banner), "CONFIG REJECTED: %s", si.message);
}
