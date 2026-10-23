#include "hmi_cfgsec.h"
#include "hmi_mac.h"
#include "hmi_util.h" /* hmi_crc32, hmi_wipe */
#include "hmi_platform.h"
#include <stdio.h>
#include <string.h>

#define STATE_SIZE 16u

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *hmi_cfgsec_reason_code(hmi_cfgsec_result_t r)
{
    switch (r) {
        case HMI_CFGSEC_OK: return "ok";
        case HMI_CFGSEC_CONFIG_UNREADABLE: return "config_unreadable";
        case HMI_CFGSEC_CONFIG_TOO_BIG: return "config_too_large";
        case HMI_CFGSEC_CONFIG_MODE: return "config_mode";
        case HMI_CFGSEC_SIG_MISSING: return "signature_missing";
        case HMI_CFGSEC_SIG_FORMAT: return "signature_format";
        case HMI_CFGSEC_CONFIG_CORRUPT: return "config_corrupt";
        case HMI_CFGSEC_KEY_MISSING: return "key_missing";
        case HMI_CFGSEC_KEY_FORMAT: return "key_format";
        case HMI_CFGSEC_KEY_MODE: return "key_mode";
        case HMI_CFGSEC_MAC_MISMATCH: return "signature_mismatch";
        case HMI_CFGSEC_ROLLBACK: return "rollback";
        case HMI_CFGSEC_CONFIG_INVALID: return "config_invalid";
        case HMI_CFGSEC_HOST_NOT_ALLOWED: return "host_not_allowed";
    }
    return "unknown";
}

/* ---- signature layout ---- */
void hmi_cfgsec_make_sig(const uint8_t key[HMI_CFGSEC_KEY_LEN], uint32_t serial, const uint8_t *config, size_t len,
                         uint8_t sig[HMI_CFGSEC_SIG_SIZE])
{
    memset(sig, 0, HMI_CFGSEC_SIG_SIZE);
    memcpy(sig, "P99C", 4);
    sig[4] = 1;
    put32(sig + 8, serial);
    put32(sig + 12, (uint32_t)len);
    put32(sig + 16, hmi_crc32(config, len));
    hmi_mac_ctx_t c;
    hmi_mac_init(&c, key, HMI_CFGSEC_KEY_LEN);
    hmi_mac_update(&c, sig, 24);
    hmi_mac_update(&c, config, len);
    hmi_mac_final(&c, sig + 24);
}

bool hmi_cfgsec_parse_sig(const uint8_t *sig, size_t len, uint32_t *serial, uint32_t *config_len, uint32_t *config_crc)
{
    if (!sig || len != HMI_CFGSEC_SIG_SIZE) return false;
    if (memcmp(sig, "P99C", 4) != 0 || sig[4] != 1) return false;
    if (sig[5] || sig[6] || sig[7] || sig[20] || sig[21] || sig[22] || sig[23]) return false;
    if (serial) *serial = get32(sig + 8);
    if (config_len) *config_len = get32(sig + 12);
    if (config_crc) *config_crc = get32(sig + 16);
    return true;
}

/* ---- serial state: "P99N", version 1, three zero bytes, serial, CRC-32 of the first 12 bytes ---- */
bool hmi_cfgsec_state_load(const char *path, uint32_t *serial, bool *damaged)
{
    uint8_t b[STATE_SIZE + 1];
    size_t n = 0;
    *serial = 0;
    if (damaged) *damaged = false;
    hmi_fs_read_t r = hmi_fs_read_all(path, b, sizeof(b), &n);
    if (r == HMI_FS_READ_ABSENT) return false;
    bool ok = r == HMI_FS_READ_OK && n == STATE_SIZE && memcmp(b, "P99N", 4) == 0 && b[4] == 1 && b[5] == 0 &&
              b[6] == 0 && b[7] == 0 && get32(b + 12) == hmi_crc32(b, 12);
    if (!ok) {
        if (damaged) *damaged = true;
        return false;
    }
    *serial = get32(b + 8);
    return true;
}

bool hmi_cfgsec_state_save(const char *path, uint32_t serial)
{
    uint8_t b[STATE_SIZE];
    memset(b, 0, sizeof(b));
    memcpy(b, "P99N", 4);
    b[4] = 1;
    put32(b + 8, serial);
    put32(b + 12, hmi_crc32(b, 12));
    return hmi_fs_atomic_write(path, b, sizeof(b));
}

/* ---- audit ---- */
size_t hmi_cfgsec_format_audit(uint64_t t_ms, const hmi_cfgsec_info_t *info, char *line, size_t cap)
{
    if (!info || !line || cap == 0) return 0;
    int n;
    if (info->result == HMI_CFGSEC_OK)
        n = snprintf(line, cap, "%llu CONFIG_ACCEPTED serial=%u\n", (unsigned long long)t_ms, (unsigned)info->serial);
    else
        n = snprintf(line, cap, "%llu CONFIG_REJECTED reason=%s\n", (unsigned long long)t_ms,
                     hmi_cfgsec_reason_code(info->result));
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ---- the loader ---- */
static hmi_cfgsec_result_t finish(hmi_cfgsec_info_t *info, hmi_cfgsec_result_t r, const char *msg)
{
    if (info) {
        info->result = r;
        snprintf(info->message, sizeof(info->message), "%s", msg);
    }
    return r;
}

hmi_cfgsec_result_t hmi_cfgsec_load(const hmi_cfgsec_paths_t *paths, hmi_app_cfg_t *out, hmi_cfgsec_info_t *info)
{
    static uint8_t cfg[HMI_CONFIG_MAX_BYTES + 1];
    hmi_cfgsec_info_t local;
    if (!info) info = &local;
    memset(info, 0, sizeof(*info));
    if (out) memset(out, 0, sizeof(*out));
    if (!paths || !paths->config_path || !paths->key_path || !out)
        return finish(info, HMI_CFGSEC_CONFIG_UNREADABLE, "internal: missing paths");

    char sig_path[300];
    const char *sp = paths->sig_path;
    if (!sp) {
        if (snprintf(sig_path, sizeof(sig_path), "%s.sig", paths->config_path) >= (int)sizeof(sig_path))
            return finish(info, HMI_CFGSEC_SIG_MISSING, "signature path too long");
        sp = sig_path;
    }

    /* --- the key first: without it nothing below can be trusted --- */
    uint8_t key[HMI_CFGSEC_KEY_LEN + 1];
    size_t kn = 0;
    hmi_fs_read_t kr = hmi_fs_read_all(paths->key_path, key, sizeof(key), &kn);
    if (kr == HMI_FS_READ_ABSENT) return finish(info, HMI_CFGSEC_KEY_MISSING, "the configuration key file does not exist");
    if (kr == HMI_FS_READ_ERROR) return finish(info, HMI_CFGSEC_KEY_MISSING, "the configuration key file cannot be read");
    if (kr == HMI_FS_READ_TOO_BIG || kn != HMI_CFGSEC_KEY_LEN) {
        hmi_wipe(key, sizeof(key));
        return finish(info, HMI_CFGSEC_KEY_FORMAT, "the configuration key file is not exactly 32 bytes");
    }
    uint32_t mode = 0600u;
    if (hmi_fs_mode(paths->key_path, &mode) && (mode & 027u)) {
        hmi_wipe(key, sizeof(key));
        return finish(info, HMI_CFGSEC_KEY_MODE, "the configuration key file is accessible to others or writable by the group");
    }
    info->key_writable_by_service = hmi_fs_writable_by_me(paths->key_path);

    /* --- configuration bytes (not parsed yet) --- */
    size_t cn = 0;
    hmi_fs_read_t cr = hmi_fs_read_all(paths->config_path, cfg, sizeof(cfg), &cn);
    if (cr == HMI_FS_READ_TOO_BIG) {
        hmi_wipe(key, sizeof(key));
        return finish(info, HMI_CFGSEC_CONFIG_TOO_BIG, "the configuration file is larger than the limit");
    }
    if (cr != HMI_FS_READ_OK) {
        hmi_wipe(key, sizeof(key));
        return finish(info, HMI_CFGSEC_CONFIG_UNREADABLE, "the configuration file cannot be read");
    }
    if (hmi_fs_mode(paths->config_path, &mode) && (mode & 022u)) {
        hmi_wipe(key, sizeof(key));
        return finish(info, HMI_CFGSEC_CONFIG_MODE, "the configuration file is writable by group or others");
    }
    info->config_writable_by_service = hmi_fs_writable_by_me(paths->config_path);

    /* --- signature --- */
    uint8_t sig[HMI_CFGSEC_SIG_SIZE + 1];
    size_t sn = 0;
    hmi_fs_read_t sr = hmi_fs_read_all(sp, sig, sizeof(sig), &sn);
    if (sr == HMI_FS_READ_ABSENT) {
        hmi_wipe(key, sizeof(key));
        return finish(info, HMI_CFGSEC_SIG_MISSING, "the configuration is not signed (no signature file)");
    }
    uint32_t serial = 0, slen = 0, scrc = 0;
    if (sr != HMI_FS_READ_OK || !hmi_cfgsec_parse_sig(sig, sn, &serial, &slen, &scrc)) {
        hmi_wipe(key, sizeof(key));
        return finish(info, HMI_CFGSEC_SIG_FORMAT, "the signature file is damaged or has the wrong format");
    }
    if (hmi_fs_mode(sp, &mode) && (mode & 022u)) {
        hmi_wipe(key, sizeof(key));
        return finish(info, HMI_CFGSEC_CONFIG_MODE, "the signature file is writable by group or others");
    }

    /* accidental damage is told apart from tampering by the plain CRC */
    if (slen != (uint32_t)cn || scrc != hmi_crc32(cfg, cn)) {
        hmi_wipe(key, sizeof(key));
        return finish(info, HMI_CFGSEC_CONFIG_CORRUPT,
                      "the configuration file does not match its checksum (damaged or edited without re-signing)");
    }

    /* --- the MAC: nothing is parsed before this succeeds --- */
    uint8_t expect[HMI_CFGSEC_SIG_SIZE];
    hmi_cfgsec_make_sig(key, serial, cfg, cn, expect);
    hmi_wipe(key, sizeof(key));
    if (!hmi_mac_equal(expect + 24, sig + 24)) {
        return finish(info, HMI_CFGSEC_MAC_MISMATCH,
                      "the configuration signature is not valid (altered file, altered signature or wrong key)");
    }
    info->serial = serial;

    /* --- rollback --- */
    uint32_t stored = 0;
    bool have_state = false;
    if (paths->state_path) {
        bool damaged = false;
        have_state = hmi_cfgsec_state_load(paths->state_path, &stored, &damaged);
        info->state_damaged = damaged;
        if (have_state && serial < stored)
            return finish(info, HMI_CFGSEC_ROLLBACK, "an older signed configuration was put back (serial is lower than one already accepted)");
    }

    /* --- only now is the text parsed --- */
    char perr[120];
    if (!hmi_config_parse((const char *)cfg, cn, out, perr, sizeof(perr))) {
        char m[200];
        snprintf(m, sizeof(m), "the signed configuration is invalid: %s", perr);
        return finish(info, HMI_CFGSEC_CONFIG_INVALID, m);
    }
    if (out->n_allowed == 0) {
        memset(out, 0, sizeof(*out));
        return finish(info, HMI_CFGSEC_HOST_NOT_ALLOWED, "a signed configuration needs plc.allowed_hosts");
    }
    if (!hmi_config_host_allowed(out)) {
        memset(out, 0, sizeof(*out));
        return finish(info, HMI_CFGSEC_HOST_NOT_ALLOWED, "plc.host is not in plc.allowed_hosts");
    }

    if (paths->state_path && (!have_state || serial > stored)) info->serial_recorded = hmi_cfgsec_state_save(paths->state_path, serial);
    else if (paths->state_path) info->serial_recorded = true; /* already stored */
    return finish(info, HMI_CFGSEC_OK, "configuration verified");
}
