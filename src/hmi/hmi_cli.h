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
 * --role is a CLAIMED role chosen by whoever starts the process. It is not
 * authentication. The default is the least privileged role (viewer). The
 * parser still accepts it so that a test build can use it; the production
 * executable refuses it (see main_linux.c and role_given below). Real roles
 * come from logging in (hmi_auth.h).
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
    hmi_role_t role;      /* --role    (default viewer; test builds only) */
    bool role_given;      /* --role appeared on the command line */
    const char *users;    /* --users   (default /var/lib/prometheus99/users.bin) */
} hmi_cli_t;

/* Returns true on success. On failure err holds a one-line reason and *out is
 * left with defaults only. argv[0] is the program name and is skipped. */
bool hmi_cli_parse(int argc, char **argv, hmi_cli_t *out, char *err, size_t err_cap);

#endif /* HMI_CLI_H */
