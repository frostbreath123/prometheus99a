/**
 * @file p99_sign.c
 * @brief Signs and checks Prometheus99 configurations (Linux).
 *
 *   p99_sign keygen --key FILE
 *   p99_sign sign   --config FILE --key FILE [--serial N]
 *   p99_sign verify --config FILE --key FILE [--state FILE]
 *   p99_sign show   --config FILE
 *
 * keygen  writes 32 random bytes (refuses to overwrite an existing file).
 * sign    writes "<config>.sig". The serial defaults to the serial of the
 *         existing signature plus one (1 if there is none). Give --serial N to
 *         choose it; the device refuses a serial lower than one it has accepted.
 *         The configuration must pass the strict parser and carry
 *         plc.allowed_hosts containing plc.host, or nothing is signed.
 * verify  runs exactly the checks the device runs and prints the result.
 * show    prints the serial and length held in the signature.
 *
 * Intended use: run as root. Keep the key, the configuration and its signature
 * owned by root. Make the key and the configuration group-readable by the
 * service group (mode 0440 or 0640 for the key, 0644 for the others) so the
 * service can read them but a compromised service cannot rewrite them. The
 * running HMI reads the configuration only when it starts.
 *
 * Exit status: 0 done, 1 refused or failed, 2 wrong usage.
 */
#include "../hmi/hmi_cfgsec.h"
#include "../hmi/hmi_util.h"
#include "../hmi/hmi_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr,
            "usage: p99_sign keygen --key FILE\n"
            "       p99_sign sign   --config FILE --key FILE [--serial N]\n"
            "       p99_sign verify --config FILE --key FILE [--state FILE]\n"
            "       p99_sign show   --config FILE\n");
    return 2;
}

typedef struct {
    const char *config;
    const char *key;
    const char *state;
    bool have_serial;
    uint32_t serial;
} opts_t;

static bool parse_serial(const char *s, uint32_t *out)
{
    if (!s[0]) return false;
    unsigned long long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10u + (unsigned)(*p - '0');
        if (v > 0xFFFFFFFFull) return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool parse_opts(int argc, char **argv, int from, opts_t *o)
{
    memset(o, 0, sizeof(*o));
    for (int i = from; i < argc; i++) {
        if (i + 1 >= argc || argv[i + 1][0] == '\0') return false;
        const char *v = argv[i + 1];
        if (strcmp(argv[i], "--config") == 0 && !o->config) o->config = v;
        else if (strcmp(argv[i], "--key") == 0 && !o->key) o->key = v;
        else if (strcmp(argv[i], "--state") == 0 && !o->state) o->state = v;
        else if (strcmp(argv[i], "--serial") == 0 && !o->have_serial) {
            if (!parse_serial(v, &o->serial)) return false;
            o->have_serial = true;
        } else return false;
        i++;
    }
    return true;
}

static int cmd_keygen(const opts_t *o)
{
    if (!o->key) return usage();
    uint8_t probe[1];
    size_t n = 0;
    if (hmi_fs_read_all(o->key, probe, sizeof(probe), &n) != HMI_FS_READ_ABSENT) {
        fprintf(stderr, "p99_sign: '%s' already exists; not overwriting a key\n", o->key);
        return 1;
    }
    uint8_t key[HMI_CFGSEC_KEY_LEN];
    if (!hmi_random_bytes(key, sizeof(key))) {
        fprintf(stderr, "p99_sign: no secure random source\n");
        return 1;
    }
    bool ok = hmi_fs_atomic_write(o->key, key, sizeof(key));
    hmi_wipe(key, sizeof(key));
    if (!ok) {
        fprintf(stderr, "p99_sign: cannot write '%s'\n", o->key);
        return 1;
    }
    printf("wrote a new 32-byte key to %s (mode 0600). Set its owner and group, for example root:prometheus with mode 0440.\n",
           o->key);
    return 0;
}

static bool read_sig(const char *config, uint8_t sig[HMI_CFGSEC_SIG_SIZE], uint32_t *serial, uint32_t *len, uint32_t *crc)
{
    char path[300];
    size_t n = 0;
    if (snprintf(path, sizeof(path), "%s.sig", config) >= (int)sizeof(path)) return false;
    uint8_t b[HMI_CFGSEC_SIG_SIZE + 1];
    if (hmi_fs_read_all(path, b, sizeof(b), &n) != HMI_FS_READ_OK) return false;
    if (!hmi_cfgsec_parse_sig(b, n, serial, len, crc)) return false;
    memcpy(sig, b, HMI_CFGSEC_SIG_SIZE);
    return true;
}

static int cmd_sign(const opts_t *o)
{
    static uint8_t cfg[HMI_CONFIG_MAX_BYTES + 1];
    if (!o->config || !o->key) return usage();
    uint8_t key[HMI_CFGSEC_KEY_LEN + 1];
    size_t kn = 0, cn = 0;
    if (hmi_fs_read_all(o->key, key, sizeof(key), &kn) != HMI_FS_READ_OK || kn != HMI_CFGSEC_KEY_LEN) {
        fprintf(stderr, "p99_sign: '%s' is missing or is not a 32-byte key\n", o->key);
        return 1;
    }
    hmi_fs_read_t cr = hmi_fs_read_all(o->config, cfg, sizeof(cfg), &cn);
    if (cr != HMI_FS_READ_OK) {
        fprintf(stderr, "p99_sign: cannot read '%s'%s\n", o->config, cr == HMI_FS_READ_TOO_BIG ? " (larger than the limit)" : "");
        hmi_wipe(key, sizeof(key));
        return 1;
    }
    static hmi_app_cfg_t parsed;
    char err[120];
    if (!hmi_config_parse((const char *)cfg, cn, &parsed, err, sizeof(err))) {
        fprintf(stderr, "p99_sign: not signing: the configuration is invalid: %s\n", err);
        hmi_wipe(key, sizeof(key));
        return 1;
    }
    if (parsed.n_allowed == 0 || !hmi_config_host_allowed(&parsed)) {
        fprintf(stderr, "p99_sign: not signing: plc.allowed_hosts must exist and contain plc.host (%s)\n", parsed.host);
        hmi_wipe(key, sizeof(key));
        return 1;
    }
    uint32_t serial = 1;
    if (o->have_serial) {
        serial = o->serial;
    } else {
        uint8_t old[HMI_CFGSEC_SIG_SIZE];
        uint32_t os, ol, oc;
        if (read_sig(o->config, old, &os, &ol, &oc)) {
            if (os == 0xFFFFFFFFu) {
                fprintf(stderr, "p99_sign: the serial cannot be increased any further\n");
                hmi_wipe(key, sizeof(key));
                return 1;
            }
            serial = os + 1u;
        }
    }
    uint8_t sig[HMI_CFGSEC_SIG_SIZE];
    hmi_cfgsec_make_sig(key, serial, cfg, cn, sig);
    hmi_wipe(key, sizeof(key));
    char path[300];
    if (snprintf(path, sizeof(path), "%s.sig", o->config) >= (int)sizeof(path) || !hmi_fs_atomic_write(path, sig, sizeof(sig))) {
        fprintf(stderr, "p99_sign: cannot write the signature file\n");
        return 1;
    }
    printf("signed %s: serial %u, %u bytes, plc %s, writes %s\n", o->config, (unsigned)serial, (unsigned)cn, parsed.host,
           parsed.writes_enabled ? "ENABLED" : "disabled");
    printf("wrote %s (mode 0600): set the owner and mode as for the configuration\n", path);
    return 0;
}

static int cmd_verify(const opts_t *o)
{
    if (!o->config || !o->key) return usage();
    static hmi_app_cfg_t cfg;
    hmi_cfgsec_paths_t p;
    p.config_path = o->config;
    p.sig_path = NULL;
    p.key_path = o->key;
    p.state_path = NULL; /* verify must not change the device's rollback state */
    hmi_cfgsec_info_t info;
    hmi_cfgsec_result_t r = hmi_cfgsec_load(&p, &cfg, &info);
    if (r != HMI_CFGSEC_OK) {
        printf("REJECTED (%s): %s\n", hmi_cfgsec_reason_code(r), info.message);
        return 1;
    }
    printf("OK: serial %u, plc %s, %u tags, writes %s\n", (unsigned)info.serial, cfg.host, (unsigned)cfg.n_tags,
           cfg.writes_enabled ? "ENABLED" : "disabled");
    if (o->state) {
        uint32_t stored = 0;
        bool damaged = false;
        if (hmi_cfgsec_state_load(o->state, &stored, &damaged)) {
            printf("device state: highest accepted serial %u%s\n", (unsigned)stored,
                   info.serial < stored ? " (this configuration would be REFUSED as a rollback)" : "");
            if (info.serial < stored) return 1;
        } else {
            printf("device state: %s\n", damaged ? "damaged (treated as absent)" : "none yet");
        }
    }
    if (info.config_writable_by_service) printf("note: the current user can modify the configuration file\n");
    return 0;
}

static int cmd_show(const opts_t *o)
{
    if (!o->config) return usage();
    uint8_t sig[HMI_CFGSEC_SIG_SIZE];
    uint32_t serial, len, crc;
    if (!read_sig(o->config, sig, &serial, &len, &crc)) {
        fprintf(stderr, "p99_sign: no readable signature for '%s'\n", o->config);
        return 1;
    }
    printf("serial %u, length %u, crc %08x\n", (unsigned)serial, (unsigned)len, (unsigned)crc);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    opts_t o;
    if (!parse_opts(argc, argv, 2, &o)) return usage();
    if (strcmp(cmd, "keygen") == 0) return cmd_keygen(&o);
    if (strcmp(cmd, "sign") == 0) return cmd_sign(&o);
    if (strcmp(cmd, "verify") == 0) return cmd_verify(&o);
    if (strcmp(cmd, "show") == 0) return cmd_show(&o);
    return usage();
}
