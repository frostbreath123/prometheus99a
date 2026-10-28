/**
 * @file hmi_boot.h
 * @brief Start-up decision of the Linux program: use the configuration, or run in safe mode.
 *
 * One call does what the program must do before it touches the PLC: verify the signed configuration (hmi_cfgsec.h),
 * write the acceptance or rejection record to the audit file, print every warning about file ownership, and decide what
 * the screen shows. The result is a plain struct, so the decision can be tested without a process, a display or a PLC.
 *
 * Safe mode: when ok is false, cfg is zeroed, banner says why (shown on the screen), no PLC connection may be made and
 * writes stay off. An invalid or missing configuration never falls back to a default PLC.
 */
#ifndef HMI_BOOT_H
#define HMI_BOOT_H

#include "hmi_cli.h"
#include "hmi_config.h"

typedef struct {
    bool ok;               /* the configuration may be used */
    uint32_t serial;       /* serial of the signature in use (the base of a change request); 0 when not ok or unsigned */
    char banner[240];      /* "CONFIG REJECTED: <reason>" when not ok, otherwise empty */
    char info[96];         /* one line for the settings screen, e.g. "Configuration: signed, serial 3" */
} hmi_boot_t;

/* Verifies and loads the configuration named by cli (--config, --config-key, --config-state) into *cfg, appends the
 * CONFIG_ACCEPTED or CONFIG_REJECTED record to cli->audit when one is given, and writes warnings and the verdict to
 * standard error. Test builds (HMI_TEST_ROLE) with cli->unsigned_config skip the signature. */
void hmi_boot_config(const hmi_cli_t *cli, hmi_app_cfg_t *cfg, hmi_boot_t *out);

#endif
