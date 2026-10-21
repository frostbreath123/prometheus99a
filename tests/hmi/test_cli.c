/* Tests for hmi_cli: strict command-line parsing and the claimed role. */
#include "test_support.h"
#include "../../src/hmi/hmi_cli.h"

/* argv helper: the program name plus up to six arguments; n counts argv[0]. */
static bool run(hmi_cli_t *o, char *err, size_t cap, int n, const char *a1, const char *a2, const char *a3,
                const char *a4, const char *a5, const char *a6)
{
    char *argv[8] = { (char *)"prometheus99", (char *)a1, (char *)a2, (char *)a3, (char *)a4, (char *)a5,
                      (char *)a6, NULL };
    return hmi_cli_parse(n, argv, o, err, cap);
}

static void test_defaults_and_valid(void)
{
    SECTION("cli: defaults are least privileged, valid options are accepted");
    hmi_cli_t o;
    char err[128];
    CHECK(run(&o, err, sizeof(err), 1, 0, 0, 0, 0, 0, 0));
    CHECK(o.role == HMI_ROLE_VIEWER);                   /* default: viewer, not operator */
    CHECK(strcmp(o.config, "/etc/prometheus99/hmi.json") == 0);
    CHECK(strcmp(o.fb, "/dev/fb0") == 0 && strcmp(o.touch, "/dev/input/event0") == 0);
    CHECK(o.audit == NULL && o.alarms == NULL);

    const char *names[] = { "viewer", "operator", "engineer", "admin" };
    for (int r = 0; r < 4; r++) {
        CHECK(run(&o, err, sizeof(err), 3, "--role", names[r], 0, 0, 0, 0));
        CHECK((int)o.role == r);
    }
    CHECK(run(&o, err, sizeof(err), 7, "--config", "c.json", "--audit", "a.log", "--role", "operator"));
    CHECK(strcmp(o.config, "c.json") == 0 && strcmp(o.audit, "a.log") == 0 && o.role == HMI_ROLE_OPERATOR);
    CHECK(run(&o, err, sizeof(err), 7, "--fb", "/dev/null", "--touch", "/dev/x", "--alarms", "s.bin"));
    CHECK(strcmp(o.fb, "/dev/null") == 0 && strcmp(o.touch, "/dev/x") == 0 && strcmp(o.alarms, "s.bin") == 0);
}

static void test_rejections(void)
{
    SECTION("cli: unknown role, missing value, unknown flag, duplicates all fail");
    hmi_cli_t o;
    char err[128];

    /* misspelled and wrongly cased roles never become a privileged role */
    const char *bad_roles[] = { "viewr", "operatr", "Operator", "ADMIN", "root", "operator,admin", "",
                                "operator ", "0", "1" };
    for (size_t i = 0; i < sizeof(bad_roles) / sizeof(bad_roles[0]); i++) {
        CHECK(!run(&o, err, sizeof(err), 3, "--role", bad_roles[i], 0, 0, 0, 0));
        CHECK(o.role == HMI_ROLE_VIEWER);               /* *out stays at defaults */
        CHECK(err[0] != '\0');
    }
    CHECK(!run(&o, err, sizeof(err), 3, "--role", "viewr", 0, 0, 0, 0));
    CHECK(strstr(err, "unknown role 'viewr'") != NULL);

    /* missing values */
    CHECK(!run(&o, err, sizeof(err), 2, "--role", 0, 0, 0, 0, 0));          /* flag is last */
    CHECK(strstr(err, "missing value for: --role") != NULL);
    CHECK(!run(&o, err, sizeof(err), 4, "--role", "--audit", "a.log", 0, 0, 0)); /* value is a flag */
    CHECK(!run(&o, err, sizeof(err), 3, "--audit", "", 0, 0, 0, 0));        /* empty value */
    CHECK(!run(&o, err, sizeof(err), 2, "--config", 0, 0, 0, 0, 0));

    /* unknown or misspelled flags, stray arguments */
    CHECK(!run(&o, err, sizeof(err), 2, "--bogus", 0, 0, 0, 0, 0));
    CHECK(strstr(err, "unknown option: --bogus") != NULL);
    CHECK(!run(&o, err, sizeof(err), 3, "--audt", "a.log", 0, 0, 0, 0));    /* typo must not drop the audit trail */
    CHECK(!run(&o, err, sizeof(err), 3, "--Role", "admin", 0, 0, 0, 0));
    CHECK(!run(&o, err, sizeof(err), 3, "-r", "admin", 0, 0, 0, 0));
    CHECK(!run(&o, err, sizeof(err), 2, "stray", 0, 0, 0, 0, 0));
    CHECK(strstr(err, "unexpected argument: stray") != NULL);
    CHECK(!run(&o, err, sizeof(err), 2, "--role=admin", 0, 0, 0, 0, 0));   /* only "--role admin" form */

    /* repeated options are ambiguous: reject rather than pick one */
    CHECK(!run(&o, err, sizeof(err), 5, "--role", "viewer", "--role", "admin", 0, 0));
    CHECK(strstr(err, "option given twice: --role") != NULL);
    CHECK(!run(&o, err, sizeof(err), 5, "--audit", "a", "--audit", "b", 0, 0));

    /* a failed parse reports through a NULL err without crashing */
    CHECK(!run(&o, NULL, 0, 2, "--bogus", 0, 0, 0, 0, 0));
}

static void test_users_and_role_given(void)
{
    SECTION("cli: --users path and whether --role was given");
    hmi_cli_t o;
    char err[128];
    CHECK(run(&o, err, sizeof(err), 1, 0, 0, 0, 0, 0, 0));
    CHECK(strcmp(o.users, "/var/lib/prometheus99/users.bin") == 0);
    CHECK(!o.role_given);                                   /* no --role: nothing was claimed */
    CHECK(run(&o, err, sizeof(err), 3, "--users", "u.bin", 0, 0, 0, 0));
    CHECK(strcmp(o.users, "u.bin") == 0 && !o.role_given);
    CHECK(run(&o, err, sizeof(err), 3, "--role", "viewer", 0, 0, 0, 0));
    CHECK(o.role_given && o.role == HMI_ROLE_VIEWER);       /* even the default role counts as given */
    CHECK(run(&o, err, sizeof(err), 5, "--role", "admin", "--users", "u.bin", 0, 0));
    CHECK(o.role_given && o.role == HMI_ROLE_ADMIN && strcmp(o.users, "u.bin") == 0);

    CHECK(!run(&o, err, sizeof(err), 5, "--users", "a", "--users", "b", 0, 0));          /* repeated */
    CHECK(strstr(err, "option given twice: --users") != NULL);
    CHECK(!run(&o, err, sizeof(err), 2, "--users", 0, 0, 0, 0, 0));                      /* no value */
    CHECK(!run(&o, err, sizeof(err), 3, "--users", "", 0, 0, 0, 0));                     /* empty value */
    CHECK(!run(&o, err, sizeof(err), 4, "--users", "--role", "admin", 0, 0, 0));         /* value is a flag */
    CHECK(!run(&o, err, sizeof(err), 3, "--user", "u.bin", 0, 0, 0, 0));                 /* typo must not be ignored */
    CHECK(!run(&o, err, sizeof(err), 3, "--role", "bogus", 0, 0, 0, 0));
    CHECK(!o.role_given && strcmp(o.users, "/var/lib/prometheus99/users.bin") == 0);   /* defaults after failure */
}

static void test_config_security_options(void)
{
    SECTION("cli: --config-key, --config-state and --unsigned-config");
    hmi_cli_t o;
    char err[128];
    CHECK(run(&o, err, sizeof(err), 1, 0, 0, 0, 0, 0, 0));
    CHECK(strcmp(o.config_key, "/etc/prometheus99/config.key") == 0);
    CHECK(strcmp(o.config_state, "/var/lib/prometheus99/config.serial") == 0);
    CHECK(!o.unsigned_config);                              /* signed configuration is the default */

    CHECK(run(&o, err, sizeof(err), 5, "--config-key", "k.bin", "--config-state", "s.bin", 0, 0));
    CHECK(strcmp(o.config_key, "k.bin") == 0 && strcmp(o.config_state, "s.bin") == 0 && !o.unsigned_config);

    CHECK(run(&o, err, sizeof(err), 2, "--unsigned-config", 0, 0, 0, 0, 0));
    CHECK(o.unsigned_config);
    CHECK(run(&o, err, sizeof(err), 4, "--unsigned-config", "--config", "c.json", 0, 0, 0)); /* takes no value */
    CHECK(o.unsigned_config && strcmp(o.config, "c.json") == 0);
    CHECK(run(&o, err, sizeof(err), 4, "--config", "c.json", "--unsigned-config", 0, 0, 0));
    CHECK(o.unsigned_config && strcmp(o.config, "c.json") == 0);

    CHECK(!run(&o, err, sizeof(err), 3, "--unsigned-config", "--unsigned-config", 0, 0, 0, 0));
    CHECK(strstr(err, "option given twice: --unsigned-config") != NULL);
    CHECK(!run(&o, err, sizeof(err), 3, "--unsigned-config", "yes", 0, 0, 0, 0));          /* stray argument */
    CHECK(strstr(err, "unexpected argument: yes") != NULL);
    CHECK(!run(&o, err, sizeof(err), 2, "--unsigned-config=1", 0, 0, 0, 0, 0));
    CHECK(run(&o, err, sizeof(err), 3, "--config-key", "a", 0, 0, 0, 0));                   /* valid: one value */
    CHECK(!run(&o, err, sizeof(err), 2, "--config-key", 0, 0, 0, 0, 0));                   /* no value */
    CHECK(!run(&o, err, sizeof(err), 3, "--config-state", "", 0, 0, 0, 0));                /* empty value */
    CHECK(!run(&o, err, sizeof(err), 5, "--config-key", "a", "--config-key", "b", 0, 0));  /* repeated */
    CHECK(!run(&o, err, sizeof(err), 3, "--configkey", "k", 0, 0, 0, 0));                  /* typo */
    CHECK(!run(&o, err, sizeof(err), 3, "--unsigned-config", "--role", "bogus", 0, 0, 0));
    CHECK(!o.unsigned_config);                              /* defaults after a failed parse */
}

int main(void)
{
    test_config_security_options();
    test_defaults_and_valid();
    test_rejections();
    test_users_and_role_given();
    return test_finish();
}
