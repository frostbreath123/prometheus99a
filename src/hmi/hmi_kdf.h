/**
 * @file hmi_kdf.h
 * @brief Password hashing (Argon2id) and constant-time helpers.
 *
 * This is the ONLY file in the portable core that includes monocypher.h (the
 * same kind of exception as plc_libmodbus.c for libmodbus, see ADR-0001).
 *
 * The scratch memory that Argon2id needs (m KiB) is allocated for one call and
 * freed before it returns. It is a transient allocation made only while a
 * password is being checked, never in steady state.
 */
#ifndef HMI_KDF_H
#define HMI_KDF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define HMI_KDF_SALT_LEN 16
#define HMI_KDF_HASH_LEN 32

/* Argon2id cost parameters. They are stored with every user record so the
 * defaults can be retuned later without invalidating existing accounts. */
typedef struct {
    uint32_t t;      /* passes */
    uint32_t m_kib;  /* memory in KiB */
    uint32_t p;      /* lanes */
} hmi_kdf_params_t;

/* Default for a small device: 3 passes, 8 MiB, 1 lane. Timing on a Raspberry
 * Pi has NOT been measured; see docs. */
hmi_kdf_params_t hmi_kdf_default_params(void);

/* Bounds that protect against a corrupted or hostile user file asking for an
 * absurd amount of memory or time: 1..16 passes, 8 KiB..64 MiB, 1..4 lanes,
 * and m_kib >= 8 * p. */
bool hmi_kdf_params_valid(const hmi_kdf_params_t *p);

/* Hash a password. Returns false when the parameters are invalid or memory
 * cannot be allocated; out is then left untouched. The scratch memory is
 * wiped before it is freed. */
bool hmi_kdf_hash(const hmi_kdf_params_t *p, const char *pw, size_t pw_len,
                  const uint8_t salt[HMI_KDF_SALT_LEN], uint8_t out[HMI_KDF_HASH_LEN]);

/* Same with the optional Argon2 secret and associated data. Used by the
 * known-answer test (RFC 9106 section 5.3); the product passes none. */
bool hmi_kdf_hash_ex(const hmi_kdf_params_t *p, const uint8_t *pw, size_t pw_len,
                     const uint8_t *salt, size_t salt_len,
                     const uint8_t *secret, size_t secret_len,
                     const uint8_t *ad, size_t ad_len,
                     uint8_t *out, size_t out_len);

/* Constant-time comparison of two 32-byte hashes. */
bool hmi_kdf_equal(const uint8_t a[HMI_KDF_HASH_LEN], const uint8_t b[HMI_KDF_HASH_LEN]);

/* Overwrite memory in a way the compiler may not remove. */
void hmi_wipe(void *p, size_t n);

#endif /* HMI_KDF_H */
