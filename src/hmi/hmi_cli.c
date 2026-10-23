#include "hmi_cli.h"
#include "hmi_authz.h"
#include <stdio.h>
#include <string.h>

static bool fail(char *err, size_t cap, const char *what, const char *arg)
{
    if (err && cap) snprintf(err, cap, "%s%s%s", what, arg ? ": " : "", arg ? arg : "");
    return false;
}

static void defaults(hmi_cli_t *o)
{
    o->config = "/etc/prometheus99/hmi.json";
    o->audit = NULL;
    o->alarms = NULL;
    o->fb = "/dev/fb0";
    o->touch = "/dev/input/event0";
    o->role = HMI_ROLE_VIEWER;
    o->role_given = false;
    o->config_key = "/etc/prometheus99/config.key";
    o->config_state = "/var/lib/prometheus99/config.serial";
    o->unsigned_config = false;
    o->spool = NULL;
}

bool hmi_cli_parse(int argc, char **argv, hmi_cli_t *out, char *err, size_t err_cap)
{
    enum { F_CONFIG, F_AUDIT, F_ALARMS, F_FB, F_TOUCH, F_ROLE, F_CONFIG_KEY, F_CONFIG_STATE, F_UNSIGNED, F_SPOOL, F_COUNT };
    static const char *const FLAGS[F_COUNT] = { "--config", "--audit", "--alarms", "--fb", "--touch", "--role",
                                              "--config-key", "--config-state", "--unsigned-config", "--spool" };
    bool seen[F_COUNT] = { false };
    hmi_cli_t o;
    defaults(&o);
    defaults(out);
    if (err && err_cap) err[0] = '\0';

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        int f = -1;
        for (int k = 0; k < F_COUNT; k++) {
            if (strcmp(arg, FLAGS[k]) == 0) f = k;
        }
        if (f < 0) {
            if (strncmp(arg, "--", 2) == 0) return fail(err, err_cap, "unknown option", arg);
            return fail(err, err_cap, "unexpected argument", arg);
        }
        if (seen[f]) return fail(err, err_cap, "option given twice", arg);
        seen[f] = true;
        if (f == F_UNSIGNED) { /* the only option without a value */
            o.unsigned_config = true;
            continue;
        }
        if (i + 1 >= argc) return fail(err, err_cap, "missing value for", arg);
        const char *val = argv[++i];
        if (val[0] == '\0' || strncmp(val, "--", 2) == 0) return fail(err, err_cap, "missing value for", arg);
        switch (f) {
            case F_CONFIG: o.config = val; break;
            case F_AUDIT: o.audit = val; break;
            case F_ALARMS: o.alarms = val; break;
            case F_FB: o.fb = val; break;
            case F_TOUCH: o.touch = val; break;
            case F_CONFIG_KEY: o.config_key = val; break;
            case F_CONFIG_STATE: o.config_state = val; break;
            case F_SPOOL: o.spool = val; break;
            case F_ROLE:
                o.role_given = true;
                if (!hmi_role_parse(val, &o.role)) {
                    if (err && err_cap)
                        snprintf(err, err_cap, "unknown role '%s' (viewer, operator, engineer, admin)", val);
                    return false;
                }
                break;
        }
    }
    *out = o;
    return true;
}
