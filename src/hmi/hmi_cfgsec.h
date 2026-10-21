/**
 * @file hmi_cfgsec.h
 * @brief Configuration integrity: a detached HMAC-SHA-512 signature, a key file,
 *        rollback protection and the PLC allow-list.
 *
 * A configuration is only used if
 *   1. its detached signature file is present and well formed,
 *   2. the CRC in the signature matches the file (this separates accidental
 *      corruption from tampering in the message shown to the operator),
 *   3. the HMAC-SHA-512 over the signature header and the exact bytes of the
 *      configuration matches, computed with the device key,
 *   4. the serial number in the signature is not lower than the highest one
 *      this device has accepted before (so an older, genuinely signed
 *      configuration cannot be put back),
 *   5. the signed text then passes the strict parser of hmi_config.c, and
 *   6. plc.host is one of plc.allowed_hosts, which must be present.
 * The bytes are not parsed before step 3 succeeds. Any failure gives no
 * configuration at all: the caller runs in safe mode (banner, no PLC
 * endpoint, writes off), exactly as for an invalid configuration.
 *
 * Signature file (88 bytes, little endian):
 *   0   magic "P99C"        4   version (1)         5   three zero bytes
 *   8   serial (u32)       12   config length (u32) 16  CRC-32 of the config (u32)
 *   20  four zero bytes    24   HMAC-SHA-512 (64 bytes) over bytes 0..23 followed by the config
 *
 * Key file: exactly 32 random bytes. It must not be accessible to "others" and
 * must not be writable by the group (modes 0400, 0440, 0600 and 0640 pass).
 * The configuration and the signature must not be writable by group or others.
 *
 * Limits, stated plainly: this is a shared-secret MAC. Anyone who can read the
 * key can sign. The design intends root to own the configuration, the signature
 * and the key (group-readable by the service user), so that a compromised
 * service cannot change what it will accept. The serial state file is written
 * by the service itself, so someone who can delete it as the service user
 * resets rollback protection. A signed configuration says who approved the
 * text; it says nothing about whether the text is a good idea.
 */
#ifndef HMI_CFGSEC_H
#define HMI_CFGSEC_H

#include "hmi_config.h"

#define HMI_CFGSEC_SIG_SIZE 88u
#define HMI_CFGSEC_KEY_LEN 32u

typedef enum {
    HMI_CFGSEC_OK = 0,
    HMI_CFGSEC_CONFIG_UNREADABLE,
    HMI_CFGSEC_CONFIG_TOO_BIG,
    HMI_CFGSEC_CONFIG_MODE,       /* writable by group or others */
    HMI_CFGSEC_SIG_MISSING,
    HMI_CFGSEC_SIG_FORMAT,        /* unreadable, wrong size, magic, version or reserved bytes */
    HMI_CFGSEC_CONFIG_CORRUPT,    /* length or CRC differs from the signature: accidental damage */
    HMI_CFGSEC_KEY_MISSING,
    HMI_CFGSEC_KEY_FORMAT,        /* not exactly 32 bytes */
    HMI_CFGSEC_KEY_MODE,          /* accessible to others, or writable by the group */
    HMI_CFGSEC_MAC_MISMATCH,      /* altered configuration, altered signature or wrong key */
    HMI_CFGSEC_ROLLBACK,          /* serial lower than one already accepted */
    HMI_CFGSEC_CONFIG_INVALID,    /* signed, but the strict parser rejects it */
    HMI_CFGSEC_HOST_NOT_ALLOWED   /* no plc.allowed_hosts, or plc.host is not in it */
} hmi_cfgsec_result_t;

typedef struct {
    const char *config_path;
    const char *sig_path;     /* NULL selects "<config_path>.sig" */
    const char *key_path;
    const char *state_path;   /* serial state file; NULL disables rollback protection */
} hmi_cfgsec_paths_t;

typedef struct {
    hmi_cfgsec_result_t result;
    uint32_t serial;              /* the signature's serial (valid from MAC success on) */
    bool serial_recorded;         /* the new highest serial was stored */
    bool state_damaged;           /* the state file existed but could not be read: treated as absent */
    bool config_writable_by_service; /* the running user could modify the configuration */
    bool key_writable_by_service;
    char message[200];            /* for the on-screen banner and standard error */
} hmi_cfgsec_info_t;

/* Short stable code for the audit file, e.g. "signature_mismatch". */
const char *hmi_cfgsec_reason_code(hmi_cfgsec_result_t r);

/* Verifies and loads. On any failure *out is zeroed (as hmi_config_parse does)
 * and the result says why. info may be NULL. */
hmi_cfgsec_result_t hmi_cfgsec_load(const hmi_cfgsec_paths_t *paths, hmi_app_cfg_t *out, hmi_cfgsec_info_t *info);

/* "<ms> CONFIG_ACCEPTED serial=N" or "<ms> CONFIG_REJECTED reason=code".
 * Returns the length including the newline, 0 on overflow. */
size_t hmi_cfgsec_format_audit(uint64_t t_ms, const hmi_cfgsec_info_t *info, char *line, size_t cap);

/* ---- building blocks, also used by the signing tool and the tests ---- */
void hmi_cfgsec_make_sig(const uint8_t key[HMI_CFGSEC_KEY_LEN], uint32_t serial, const uint8_t *config, size_t len,
                         uint8_t sig[HMI_CFGSEC_SIG_SIZE]);
/* Checks only the header layout; returns the serial and length it carries. */
bool hmi_cfgsec_parse_sig(const uint8_t *sig, size_t len, uint32_t *serial, uint32_t *config_len, uint32_t *config_crc);

/* Serial state file (16 bytes): "P99N", version, three zero bytes, serial, CRC-32 of the first 12 bytes. */
bool hmi_cfgsec_state_load(const char *path, uint32_t *serial, bool *damaged);
bool hmi_cfgsec_state_save(const char *path, uint32_t serial);

#endif /* HMI_CFGSEC_H */
