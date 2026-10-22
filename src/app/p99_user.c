/**
 * @file p99_user.c
 * @brief Offline user management for Prometheus99 (Linux).
 *
 *   p99_user [--users FILE] [--audit FILE] [--password-stdin] COMMAND
 *
 *   init NAME          create the first administrator (the user file must not exist)
 *   add NAME ROLE      add an account; ROLE is operator, engineer or admin
 *   passwd NAME        set a new password
 *   role NAME ROLE     change the role, keep the password
 *   remove NAME        delete an account (the last administrator cannot be removed)
 *   list               show accounts
 *   bench              time one password hash with the default cost on this machine
 *
 * Run it as the service user (for example `sudo -u prometheus p99_user ...`) so
 * the file keeps its owner and mode 0600. The running HMI reads the user file
 * only at start: restart the service for a change to apply.
 *
 * Passwords are read from the terminal without echo, twice for a new password.
 * With --password-stdin one line is read from standard input instead, for
 * scripted provisioning; that password then travels through whatever supplies
 * the input, so prefer the terminal. Passwords are never printed or logged.
 * With --audit every change is recorded in the same audit file as the HMI, and
 * the change is refused if that record cannot be written.
 *
 * Exit status: 0 done, 1 refused or failed, 2 wrong usage.
 */
#include "../hmi/hmi_auth.h"
#include "../hmi/hmi_authz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define DEFAULT_USERS "/var/lib/prometheus99/users.bin"

typedef struct {
    const char *users;
    const char *audit;
    bool pw_stdin;
} opts_t;

static int usage(void)
{
    fprintf(stderr,
            "usage: p99_user [--users FILE] [--audit FILE] [--password-stdin] COMMAND\n"
            "  init NAME | add NAME ROLE | passwd NAME | role NAME ROLE | remove NAME | list | bench\n"
            "  ROLE is operator, engineer or admin\n");
    return 2;
}

/* ---- password input ---- */
static bool read_line_hidden(const char *prompt, char *buf, size_t cap)
{
    struct termios old, quiet;
    bool tty = isatty(STDIN_FILENO) != 0;
    if (!tty) {
        fprintf(stderr, "p99_user: no terminal on standard input; use --password-stdin\n");
        return false;
    }
    fprintf(stderr, "%s", prompt);
    fflush(stderr);
    if (tcgetattr(STDIN_FILENO, &old) != 0) return false;
    quiet = old;
    quiet.c_lflag &= ~(tcflag_t)ECHO;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) != 0) return false;
    bool ok = fgets(buf, (int)cap, stdin) != NULL;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    fprintf(stderr, "\n");
    if (!ok) return false;
    buf[strcspn(buf, "\r\n")] = '\0';
    return true;
}

static bool read_line_stdin(char *buf, size_t cap)
{
    if (!fgets(buf, (int)cap, stdin)) return false;
    buf[strcspn(buf, "\r\n")] = '\0';
    return true;
}

/* New password: confirmed on a terminal, one line from stdin otherwise. */
static bool get_new_password(const opts_t *o, char *pw, size_t cap)
{
    if (o->pw_stdin) return read_line_stdin(pw, cap);
    char again[HMI_PASSWORD_MAX + 8];
    if (!read_line_hidden("New password: ", pw, cap)) return false;
    if (!read_line_hidden("Repeat password: ", again, sizeof(again))) {
        hmi_wipe(pw, cap);
        return false;
    }
    bool same = strcmp(pw, again) == 0;
    hmi_wipe(again, sizeof(again));
    if (!same) {
        fprintf(stderr, "p99_user: the two passwords do not match\n");
        hmi_wipe(pw, cap);
    }
    return same;
}

/* ---- audit and store helpers ---- */
/* Fail closed: with --audit, no record means no change. */
static bool record(const opts_t *o, hmi_auth_event_t ev, const char *user, hmi_role_t role)
{
    if (!o->audit) return true;
    hmi_auth_audit_rec_t r;
    r.t_ms = hmi_now_ms();
    r.ev = ev;
    r.user = user;
    r.by = NULL;
    r.role = role;
    r.failures = 0;
    r.lock_ms = 0;
    if (hmi_auth_file_audit((void *)o->audit, &r)) return true;
    fprintf(stderr, "p99_user: cannot write the audit record to '%s'; nothing was changed\n", o->audit);
    return false;
}

static bool load_ready(const opts_t *o, hmi_users_t *s)
{
    char err[120];
    uint32_t mode = 0600u;
    hmi_users_load_t l = hmi_users_load(o->users, s, err, sizeof(err));
    if (l == HMI_USERS_LOAD_ABSENT) {
        fprintf(stderr, "p99_user: no user store at '%s' (create it with: init NAME)\n", o->users);
        return false;
    }
    if (l != HMI_USERS_LOAD_OK) {
        fprintf(stderr, "p99_user: the user store '%s' is not usable: %s\n", o->users, err);
        return false;
    }
    if (hmi_fs_mode(o->users, &mode) && (mode & 077u))
        fprintf(stderr, "p99_user: warning: '%s' has mode %03o; it should be 600\n", o->users, (unsigned)mode);
    return true;
}

static bool role_arg(const char *name, hmi_role_t *r)
{
    if (!hmi_role_parse(name, r) || *r < HMI_ROLE_OPERATOR) {
        fprintf(stderr, "p99_user: role must be operator, engineer or admin\n");
        return false;
    }
    return true;
}

static int fail_err(hmi_users_err_t e, const char *why)
{
    fprintf(stderr, "p99_user: %s%s%s\n", hmi_users_err_text(e), (why && why[0]) ? ": " : "", why ? why : "");
    return 1;
}

/* ---- commands ---- */
static int cmd_init(const opts_t *o, const char *name)
{
    hmi_users_t s;
    char err[120];
    hmi_users_load_t l = hmi_users_load(o->users, &s, err, sizeof(err));
    if (l != HMI_USERS_LOAD_ABSENT) {
        fprintf(stderr, "p99_user: '%s' already exists; init only creates a new store\n", o->users);
        return 1;
    }
    char pw[HMI_PASSWORD_MAX + 8];
    if (!get_new_password(o, pw, sizeof(pw))) return 1;
    hmi_user_t u;
    const char *why = "";
    hmi_users_err_t e = hmi_user_make(&u, name, HMI_ROLE_ADMIN, pw, NULL, &why);
    hmi_wipe(pw, sizeof(pw));
    if (e != HMI_USERS_OK) return fail_err(e, why);
    memset(&s, 0, sizeof(s));
    hmi_users_add(&s, &u);
    if (!record(o, HMI_AUTH_EV_USER_CREATE, name, HMI_ROLE_ADMIN)) return 1;
    if (!hmi_users_save(o->users, &s)) {
        fprintf(stderr, "p99_user: cannot write '%s'\n", o->users);
        return 1;
    }
    printf("created administrator '%s' in %s\n", name, o->users);
    return 0;
}

static int cmd_add(const opts_t *o, const char *name, const char *role_name)
{
    hmi_role_t role;
    hmi_users_t s;
    if (!role_arg(role_name, &role) || !load_ready(o, &s)) return 1;
    if (hmi_users_find(&s, name) >= 0) return fail_err(HMI_USERS_ERR_EXISTS, name);
    char pw[HMI_PASSWORD_MAX + 8];
    if (!get_new_password(o, pw, sizeof(pw))) return 1;
    hmi_user_t u;
    const char *why = "";
    hmi_users_err_t e = hmi_user_make(&u, name, role, pw, NULL, &why);
    hmi_wipe(pw, sizeof(pw));
    if (e != HMI_USERS_OK) return fail_err(e, why);
    e = hmi_users_add(&s, &u);
    if (e != HMI_USERS_OK) return fail_err(e, NULL);
    if (!record(o, HMI_AUTH_EV_USER_ADD, name, role)) return 1;
    if (!hmi_users_save(o->users, &s)) {
        fprintf(stderr, "p99_user: cannot write '%s'\n", o->users);
        return 1;
    }
    printf("added '%s' (%s)\n", name, hmi_role_name(role));
    return 0;
}

static int cmd_passwd(const opts_t *o, const char *name)
{
    hmi_users_t s;
    if (!load_ready(o, &s)) return 1;
    int i = hmi_users_find(&s, name);
    if (i < 0) return fail_err(HMI_USERS_ERR_NOT_FOUND, name);
    hmi_role_t role = s.u[i].role;
    char pw[HMI_PASSWORD_MAX + 8];
    if (!get_new_password(o, pw, sizeof(pw))) return 1;
    hmi_user_t u;
    const char *why = "";
    hmi_users_err_t e = hmi_user_make(&u, name, role, pw, NULL, &why);
    hmi_wipe(pw, sizeof(pw));
    if (e != HMI_USERS_OK) return fail_err(e, why);
    e = hmi_users_replace(&s, &u);
    if (e != HMI_USERS_OK) return fail_err(e, NULL);
    if (!record(o, HMI_AUTH_EV_USER_PASSWORD, name, role)) return 1;
    if (!hmi_users_save(o->users, &s)) {
        fprintf(stderr, "p99_user: cannot write '%s'\n", o->users);
        return 1;
    }
    printf("password changed for '%s'\n", name);
    return 0;
}

static int cmd_role(const opts_t *o, const char *name, const char *role_name)
{
    hmi_role_t role;
    hmi_users_t s;
    if (!role_arg(role_name, &role) || !load_ready(o, &s)) return 1;
    int i = hmi_users_find(&s, name);
    if (i < 0) return fail_err(HMI_USERS_ERR_NOT_FOUND, name);
    hmi_user_t u = s.u[i];
    u.role = role;
    hmi_users_err_t e = hmi_users_replace(&s, &u);
    if (e != HMI_USERS_OK) return fail_err(e, NULL);
    if (!record(o, HMI_AUTH_EV_USER_PASSWORD, name, role)) return 1;
    if (!hmi_users_save(o->users, &s)) {
        fprintf(stderr, "p99_user: cannot write '%s'\n", o->users);
        return 1;
    }
    printf("'%s' is now %s\n", name, hmi_role_name(role));
    return 0;
}

static int cmd_remove(const opts_t *o, const char *name)
{
    hmi_users_t s;
    if (!load_ready(o, &s)) return 1;
    hmi_users_err_t e = hmi_users_remove(&s, name);
    if (e != HMI_USERS_OK) return fail_err(e, e == HMI_USERS_ERR_NOT_FOUND ? name : NULL);
    if (!record(o, HMI_AUTH_EV_USER_REMOVE, name, HMI_ROLE_VIEWER)) return 1;
    if (!hmi_users_save(o->users, &s)) {
        fprintf(stderr, "p99_user: cannot write '%s'\n", o->users);
        return 1;
    }
    printf("removed '%s'\n", name);
    return 0;
}

static int cmd_list(const opts_t *o)
{
    hmi_users_t s;
    if (!load_ready(o, &s)) return 1;
    for (size_t i = 0; i < s.n; i++)
        printf("%-24s %s  (argon2id t=%u m=%u KiB p=%u)\n", s.u[i].name, hmi_role_name(s.u[i].role),
               (unsigned)s.u[i].kdf.t, (unsigned)s.u[i].kdf.m_kib, (unsigned)s.u[i].kdf.p);
    return 0;
}

static int cmd_bench(void)
{
    hmi_kdf_params_t p = hmi_kdf_default_params();
    uint8_t salt[HMI_KDF_SALT_LEN] = { 0 }, out[HMI_KDF_HASH_LEN];
    printf("Argon2id t=%u m=%u KiB p=%u on this machine:\n", (unsigned)p.t, (unsigned)p.m_kib, (unsigned)p.p);
    for (int i = 0; i < 3; i++) {
        uint64_t t0 = hmi_now_ms();
        if (!hmi_kdf_hash(&p, "benchmark-password", 18, salt, out)) {
            fprintf(stderr, "p99_user: hash failed (memory?)\n");
            return 1;
        }
        printf("  run %d: %llu ms\n", i + 1, (unsigned long long)(hmi_now_ms() - t0));
    }
    return 0;
}

int main(int argc, char **argv)
{
    opts_t o = { DEFAULT_USERS, NULL, false };
    int i = 1;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0; i++) {
        if (strcmp(argv[i], "--users") == 0 && i + 1 < argc && argv[i + 1][0]) o.users = argv[++i];
        else if (strcmp(argv[i], "--audit") == 0 && i + 1 < argc && argv[i + 1][0]) o.audit = argv[++i];
        else if (strcmp(argv[i], "--password-stdin") == 0) o.pw_stdin = true;
        else return usage();
    }
    if (i >= argc) return usage();
    const char *cmd = argv[i++];
    int rest = argc - i;
    if (strcmp(cmd, "init") == 0 && rest == 1) return cmd_init(&o, argv[i]);
    if (strcmp(cmd, "add") == 0 && rest == 2) return cmd_add(&o, argv[i], argv[i + 1]);
    if (strcmp(cmd, "passwd") == 0 && rest == 1) return cmd_passwd(&o, argv[i]);
    if (strcmp(cmd, "role") == 0 && rest == 2) return cmd_role(&o, argv[i], argv[i + 1]);
    if (strcmp(cmd, "remove") == 0 && rest == 1) return cmd_remove(&o, argv[i]);
    if (strcmp(cmd, "list") == 0 && rest == 0) return cmd_list(&o);
    if (strcmp(cmd, "bench") == 0 && rest == 0) return cmd_bench();
    return usage();
}
