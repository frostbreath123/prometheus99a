/**
 * @file hmi_cli.h
 * @brief Strict command-line parsing for the Linux HMI executable.
 *
 * Anything it does not understand is an error: an unknown flag, an unknown
 * role, a flag without a value, a repeated flag, a stray argument. The caller
 * exits with status 2. There is no silent fallback: a typo in `--role` must
 * never turn into a more privileged role, and a typo in `--audit` must never
 * turn into "no audit trail".
 *
 * --role is a CLAIMED role chosen by whoever starts the process (for example
 * in the service unit). It is not authentication. The default is the least
 * privileged role (viewer), which cannot acknowledge alarms or write.
 */
#ifndef HMI_CLI_H
#define HMI_CLI_H

#include <stddef.h>
#include "hmi_types.h"

typedef struct {
    const char *config;   /* --config  (default /etc/prometheus99/hmi.json) */
    const char *audit;    /* --audit   (default none: PLC writes forced off) */
    const char *alarms;   /* --alarms  (default none: alarm state not persisted) */
    const char *fb;       /* --fb      (default /dev/fb0) */
    const char *touch;    /* --touch   (default /dev/input/event0) */
    hmi_role_t role;      /* --role    (default viewer; claimed, not authenticated) */
    bool role_given;      /* --role appeared on the command line */
    const char *config_key;   /* --config-key   (default /etc/prometheus99/config.key) */
    const char *config_state; /* --config-state (default /var/lib/prometheus99/config.serial) */
    const char *spool;    /* --spool DIR (default none: settings cannot be changed from the screen). The directory
                             where requests for p99_cfg are left (hmi_cfgedit.h). */
    bool unsigned_config; /* --unsigned-config (takes no value): test builds only, skips the
                             signature check. The production executable refuses it. */
} hmi_cli_t;

/* Returns true on success. On failure err holds a one-line reason and *out is
 * left with defaults only. argv[0] is the program name and is skipped. */
bool hmi_cli_parse(int argc, char **argv, hmi_cli_t *out, char *err, size_t err_cap);

#endif /* HMI_CLI_H */
