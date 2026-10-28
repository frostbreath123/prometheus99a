/**
 * @file p99_cfg.c
 * @brief Root helper that applies a configuration change requested from the screen (Linux).
 *
 *   p99_cfg apply --spool DIR --config FILE --key FILE --audit FILE --owner USER|UID [--restart UNIT]
 *
 * The HMI service cannot write its signed configuration. An Engineer who changes a
 * setting on the screen makes the service write a small request file into the spool
 * directory (src/hmi/hmi_cfgedit.h). A systemd path unit starts this program as root.
 * It does these things in this order and stops at the first one that fails:
 *   1. Opens the spool directory without following links and checks the request file:
 *      a regular file with one link, owned by --owner (or root), at most 8 KiB, not
 *      writable by group or others. Anything else is refused.
 *   2. Verifies the current configuration exactly as the device does (signature, key,
 *      allow-list) but without touching the rollback state.
 *   3. Parses the request, requires base_serial to equal the serial of the signature
 *      now on disk, and applies it with the whitelist and bounds of hmi_cfgedit_apply.
 *   4. Appends CONFIG_CHANGED to the audit file BEFORE anything is changed. If that
 *      cannot be done, nothing is changed.
 *   5. Keeps "<config>.prev" and "<config>.sig.prev", writes the new configuration and
 *      signature (temporary file, flush, rename, mode 0644, serial plus one).
 *   6. Writes "<spool>/result" ("applied serial=N changes=K by=U" or "rejected: reason"),
 *      removes the request (always, also after a refusal), and optionally restarts the
 *      service with "systemctl restart UNIT".
 *
 * Stated limit: this program cannot tell who is really at the screen or which role the service runs with. A
 * compromised service could ask for any change on the whitelist. The whitelist, the
 * bounds, the audit record and the signed result are the protection.
 * The configuration and its signature are replaced by two renames; a restart of the
 * service between the two renames would see a mismatch and start in safe mode.
 *
 * Exit status: 0 applied, 1 refused (the result file says why), 2 wrong usage,
 * 3 failed in a way that left no result. Nothing here has run under real systemd.
 */
#define _GNU_SOURCE
#include "../hmi/hmi_cfgsec.h"
#include "../hmi/hmi_cfgedit.h"
#include "../hmi/hmi_auditfmt.h"
#include "../hmi/hmi_platform.h"
#include "../hmi/hmi_util.h"
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int usage(void)
{
    fprintf(stderr, "usage: p99_cfg apply --spool DIR --config FILE --key FILE --audit FILE --owner USER|UID [--restart UNIT]\n");
    return 2;
}

typedef struct {
    const char *spool, *config, *key, *audit, *owner, *restart;
} opts_t;

static bool parse_opts(int argc, char **argv, opts_t *o)
{
    memset(o, 0, sizeof(*o));
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc || argv[i + 1][0] == '\0') return false;
        const char **dst = NULL;
        if (strcmp(argv[i], "--spool") == 0) dst = &o->spool;
        else if (strcmp(argv[i], "--config") == 0) dst = &o->config;
        else if (strcmp(argv[i], "--key") == 0) dst = &o->key;
        else if (strcmp(argv[i], "--audit") == 0) dst = &o->audit;
        else if (strcmp(argv[i], "--owner") == 0) dst = &o->owner;
        else if (strcmp(argv[i], "--restart") == 0) dst = &o->restart;
        if (!dst || *dst) return false;
        *dst = argv[i + 1];
    }
    return o->spool && o->config && o->key && o->audit && o->owner;
}

static bool resolve_owner(const char *s, uid_t *uid)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end && *end == '\0' && end != s) {
        *uid = (uid_t)v;
        return true;
    }
    struct passwd *pw = getpwnam(s);
    if (!pw) return false;
    *uid = pw->pw_uid;
    return true;
}

static bool unit_name_ok(const char *u)
{
    if (!u[0] || strlen(u) > 64 || u[0] == '-') return false;
    for (; *u; u++)
        if (!((*u >= 'a' && *u <= 'z') || (*u >= 'A' && *u <= 'Z') || (*u >= '0' && *u <= '9') || *u == '.' ||
              *u == '-' || *u == '_' || *u == '@'))
            return false;
    return true;
}

/* ------------------------------------------------------------------ files */
static bool write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static void sync_dir_of(const char *path)
{
    char d[400];
    snprintf(d, sizeof(d), "%s", path);
    char *slash = strrchr(d, '/');
    if (!slash) snprintf(d, sizeof(d), ".");
    else if (slash == d) snprintf(d, sizeof(d), "/");
    else *slash = '\0';
    int fd = open(d, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

/* Temporary file next to the target (created new, never through a link), flushed, then renamed. */
static bool put_file(const char *path, const void *data, size_t len, mode_t mode, uid_t uid, gid_t gid)
{
    char tmp[420];
    if (snprintf(tmp, sizeof(tmp), "%s.new", path) >= (int)sizeof(tmp)) return false;
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = write_all(fd, data, len) && fchmod(fd, mode) == 0 && fsync(fd) == 0;
    if (uid != (uid_t)-1 && fchown(fd, uid, gid) != 0 && geteuid() == 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (!ok) unlink(tmp);
    else sync_dir_of(path);
    return ok;
}

static bool read_fd(int fd, uint8_t *buf, size_t cap, size_t *len)
{
    size_t n = 0;
    for (;;) {
        if (n == cap) return false;
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return false;
        if (r == 0) break;
        n += (size_t)r;
    }
    *len = n;
    return true;
}

static void clean(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) *s = '?';
}

/* ------------------------------------------------------------------ result */
static bool write_result(int dirfd, const char *text)
{
    char line[300];
    snprintf(line, sizeof(line), "%s", text);
    clean(line);
    size_t n = strlen(line);
    if (n + 1 < sizeof(line)) {
        line[n] = '\n';
        line[n + 1] = '\0';
    } else {
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
    }
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "result.tmp.%ld", (long)getpid());
    unlinkat(dirfd, tmp, 0);
    int fd = openat(dirfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = fchmod(fd, 0644) == 0 && write_all(fd, line, strlen(line)) && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok && renameat(dirfd, tmp, dirfd, "result") != 0) ok = false;
    if (!ok) unlinkat(dirfd, tmp, 0);
    return ok;
}

typedef struct {
    int dirfd;
    bool done;
} ctx_t;

/* The one exit for every outcome: result first, then the request goes away. */
static int finish(ctx_t *c, bool applied, const char *text)
{
    bool wrote = write_result(c->dirfd, text);
    bool removed = unlinkat(c->dirfd, "request", 0) == 0 || errno == ENOENT;
    fprintf(applied ? stdout : stderr, "p99_cfg: %s\n", text);
    if (!wrote || !removed) {
        fprintf(stderr, "p99_cfg: %s\n", !wrote ? "the result file could not be written" : "the request could not be removed");
        return 3;
    }
    return applied ? 0 : 1;
}

static int refuse(ctx_t *c, const char *reason)
{
    char t[300];
    snprintf(t, sizeof(t), "rejected: %s", reason);
    return finish(c, false, t);
}

/* ------------------------------------------------------------------ audit */
static bool audit_append(const char *path, uid_t owner, const char *body)
{
    int fd = open(path, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 && (st.st_uid == owner || st.st_uid == 0);
    if (ok) {
        char line[512];
        hmi_wall_t w;
        hmi_wall_now(&w);
        int n = snprintf(line, sizeof(line), "%llu %s\n", (unsigned long long)hmi_now_ms(), body);
        ok = n > 0 && (size_t)n < sizeof(line);
        if (ok) {
            size_t m = hmi_audit_stamp_line(line, (size_t)n, sizeof(line), &w);
            ok = m > 0 && write_all(fd, line, m) && fsync(fd) == 0;
        }
    }
    close(fd);
    return ok;
}

/* ------------------------------------------------------------------ restart */
static void restart_unit(const char *unit)
{
    pid_t p = fork();
    if (p == 0) {
        execl("/bin/systemctl", "systemctl", "restart", unit, (char *)NULL);
        _exit(127);
    }
    if (p > 0) {
        int st = 0;
        waitpid(p, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) fprintf(stderr, "p99_cfg: 'systemctl restart %s' did not succeed\n", unit);
    }
}

/* ------------------------------------------------------------------ apply */
static int cmd_apply(const opts_t *o)
{
    uid_t owner;
    if (!resolve_owner(o->owner, &owner)) {
        fprintf(stderr, "p99_cfg: unknown owner '%s'\n", o->owner);
        return 2;
    }
    if (o->restart && !unit_name_ok(o->restart)) {
        fprintf(stderr, "p99_cfg: bad unit name\n");
        return 2;
    }
    int dirfd = open(o->spool, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) {
        fprintf(stderr, "p99_cfg: cannot open the spool directory '%s'\n", o->spool);
        return 3;
    }
    ctx_t c = { dirfd, false };
    struct stat ds;
    if (fstat(dirfd, &ds) != 0 || !S_ISDIR(ds.st_mode) || (ds.st_uid != owner && ds.st_uid != 0) || (ds.st_mode & 0002)) {
        fprintf(stderr, "p99_cfg: the spool directory must belong to the service user or root and not be writable by others\n");
        close(dirfd);
        return 3;
    }

    /* 1. the request file */
    int rfd = openat(dirfd, "request", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (rfd < 0) {
        if (errno == ENOENT) {
            close(dirfd);
            return 0;                                        /* nothing to do */
        }
        int rc = refuse(&c, errno == ELOOP ? "the request is a symbolic link" : "the request cannot be opened");
        unlinkat(dirfd, "request", 0);
        close(dirfd);
        return rc;
    }
    struct stat rs;
    uint8_t req_text[HMI_CFGEDIT_REQUEST_MAX + 2] = { 0 };   /* on the stack, not static: the helper is a mode of the device program and static buffers would count in its data size */
    size_t req_len = 0;
    int rc;
    if (fstat(rfd, &rs) != 0 || !S_ISREG(rs.st_mode)) {
        rc = refuse(&c, "the request is not a regular file");
    } else if (rs.st_nlink != 1) {
        rc = refuse(&c, "the request has more than one link");
    } else if (rs.st_uid != owner && rs.st_uid != 0) {
        rc = refuse(&c, "the request has the wrong owner");
    } else if (rs.st_mode & 0022) {
        rc = refuse(&c, "the request is writable by group or others");
    } else if (rs.st_size > (off_t)HMI_CFGEDIT_REQUEST_MAX) {
        rc = refuse(&c, "the request is larger than 8 KiB");
    } else if (!read_fd(rfd, req_text, HMI_CFGEDIT_REQUEST_MAX + 1u, &req_len) || req_len > HMI_CFGEDIT_REQUEST_MAX) {
        rc = refuse(&c, "the request cannot be read or is larger than 8 KiB");
    } else {
        rc = -1;
    }
    close(rfd);
    if (rc >= 0) {
        close(dirfd);
        return rc;
    }

    /* 2. the current configuration, verified as the device verifies it */
    hmi_app_cfg_t cur = { 0 }, next = { 0 };
    uint8_t cur_cfg[HMI_CONFIG_MAX_BYTES + 1] = { 0 }, cur_sig[HMI_CFGSEC_SIG_SIZE + 1] = { 0 };
    hmi_cfgsec_paths_t paths = { o->config, NULL, o->key, NULL };
    hmi_cfgsec_info_t info;
    hmi_cfgsec_result_t sr = hmi_cfgsec_load(&paths, &cur, &info);
    if (sr != HMI_CFGSEC_OK) {
        char t[200];
        snprintf(t, sizeof(t), "the current configuration does not verify (%s)", hmi_cfgsec_reason_code(sr));
        rc = refuse(&c, t);
        close(dirfd);
        return rc;
    }
    char sigpath[420], prev[430], prevsig[440];
    snprintf(sigpath, sizeof(sigpath), "%s.sig", o->config);
    snprintf(prev, sizeof(prev), "%s.prev", o->config);
    snprintf(prevsig, sizeof(prevsig), "%s.sig.prev", o->config);
    size_t cur_len = 0, sig_len = 0;
    if (hmi_fs_read_all(o->config, cur_cfg, sizeof(cur_cfg), &cur_len) != HMI_FS_READ_OK ||
        hmi_fs_read_all(sigpath, cur_sig, sizeof(cur_sig), &sig_len) != HMI_FS_READ_OK || sig_len != HMI_CFGSEC_SIG_SIZE) {
        rc = refuse(&c, "the current configuration cannot be read again");
        close(dirfd);
        return rc;
    }

    /* 3. the request */
    hmi_cfgedit_request_t req;
    char err[200];
    if (!hmi_cfgedit_parse((const char *)req_text, req_len, &req, err, sizeof(err))) {
        char t[260];
        snprintf(t, sizeof(t), "bad request: %s", err);
        rc = refuse(&c, t);
        close(dirfd);
        return rc;
    }
    if (req.base_serial != info.serial) {
        char t[120];
        snprintf(t, sizeof(t), "stale request: made for serial %u, the configuration is serial %u", (unsigned)req.base_serial,
                 (unsigned)info.serial);
        rc = refuse(&c, t);
        close(dirfd);
        return rc;
    }
    if (info.serial == 0xFFFFFFFFu) {
        rc = refuse(&c, "the serial cannot be increased any further");
        close(dirfd);
        return rc;
    }
    char text[HMI_CONFIG_MAX_BYTES + 1] = { 0 };
    size_t text_len = 0, effective = 0;
    if (!hmi_cfgedit_apply(&req, &cur, &next, text, sizeof(text), &text_len, &effective, err, sizeof(err))) {
        char t[260];
        snprintf(t, sizeof(t), "%s", err);
        rc = refuse(&c, t);
        close(dirfd);
        return rc;
    }
    uint32_t new_serial = info.serial + 1u;

    /* the key is read only now, used once and wiped */
    uint8_t key[HMI_CFGSEC_KEY_LEN + 1];
    size_t kn = 0;
    if (hmi_fs_read_all(o->key, key, sizeof(key), &kn) != HMI_FS_READ_OK || kn != HMI_CFGSEC_KEY_LEN) {
        hmi_wipe(key, sizeof(key));
        rc = refuse(&c, "the key cannot be read");
        close(dirfd);
        return rc;
    }
    uint8_t sig[HMI_CFGSEC_SIG_SIZE];
    hmi_cfgsec_make_sig(key, new_serial, (const uint8_t *)text, text_len, sig);
    hmi_wipe(key, sizeof(key));

    /* 4. the record, before the effect */
    char body[300];
    snprintf(body, sizeof(body), "CONFIG_CHANGED serial=%u from_serial=%u changes=%u by=%s", (unsigned)new_serial,
             (unsigned)info.serial, (unsigned)effective, req.user[0] ? req.user : "-");
    if (!audit_append(o->audit, owner, body)) {
        rc = refuse(&c, "the audit record could not be written, so nothing was changed");
        close(dirfd);
        return rc;
    }

    /* 5. keep the old pair, then replace the pair; the owner of the old files is kept */
    struct stat cs;
    uid_t cu = (uid_t)-1;
    gid_t cg = (gid_t)-1;
    if (stat(o->config, &cs) == 0) {
        cu = cs.st_uid;
        cg = cs.st_gid;
    }
    if (!put_file(prev, cur_cfg, cur_len, 0644, cu, cg) || !put_file(prevsig, cur_sig, sig_len, 0644, cu, cg)) {
        rc = refuse(&c, "the previous configuration could not be kept, so nothing was changed");
        close(dirfd);
        return rc;
    }
    if (!put_file(o->config, text, text_len, 0644, cu, cg)) {
        rc = refuse(&c, "the new configuration could not be written, the old one is unchanged");
        close(dirfd);
        return rc;
    }
    if (!put_file(sigpath, sig, sizeof(sig), 0644, cu, cg)) {
        /* the new text sits with the old signature: put the old text back */
        bool back = put_file(o->config, cur_cfg, cur_len, 0644, cu, cg);
        rc = refuse(&c, back ? "the new signature could not be written, the old configuration was put back"
                             : "the new signature could not be written and the old configuration could not be put back");
        close(dirfd);
        return rc;
    }

    /* 6. the result */
    char res[200];
    snprintf(res, sizeof(res), "applied serial=%u changes=%u by=%s", (unsigned)new_serial, (unsigned)effective,
             req.user[0] ? req.user : "-");
    rc = finish(&c, true, res);
    close(dirfd);
    if (rc == 0 && o->restart) restart_unit(o->restart);
    return rc;
}

/* Built into the device program when it is started under the name p99_cfg (P99_MULTICALL, see main_linux.c), so that the
 * device needs one file instead of two. The stand-alone program is built the same way for the tests. */
#ifdef P99_MULTICALL
int p99_cfg_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    if (argc < 2 || strcmp(argv[1], "apply") != 0) return usage();
    opts_t o;
    if (!parse_opts(argc, argv, &o)) return usage();
    umask(022);
    return cmd_apply(&o);
}
