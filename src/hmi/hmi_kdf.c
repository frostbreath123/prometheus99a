#include "hmi_kdf.h"
#include "../../third_party/monocypher/monocypher.h"
#include <stdlib.h>

hmi_kdf_params_t hmi_kdf_default_params(void)
{
    hmi_kdf_params_t p = { 3u, 8192u, 1u };
    return p;
}

bool hmi_kdf_params_valid(const hmi_kdf_params_t *p)
{
    if (!p) return false;
    if (p->t < 1u || p->t > 16u) return false;
    if (p->p < 1u || p->p > 4u) return false;
    if (p->m_kib < 8u * p->p || p->m_kib > 65536u) return false;
    return true;
}

bool hmi_kdf_hash_ex(const hmi_kdf_params_t *p, const uint8_t *pw, size_t pw_len,
                     const uint8_t *salt, size_t salt_len,
                     const uint8_t *secret, size_t secret_len,
                     const uint8_t *ad, size_t ad_len,
                     uint8_t *out, size_t out_len)
{
    if (!hmi_kdf_params_valid(p) || !out || out_len == 0u || out_len > 1024u) return false;
    if (!salt || salt_len < 8u) return false;
    if ((!pw && pw_len) || (!secret && secret_len) || (!ad && ad_len)) return false;

    size_t work_bytes = (size_t)p->m_kib * 1024u;
    void *work = malloc(work_bytes);
    if (!work) return false;

    crypto_argon2_config cfg;
    cfg.algorithm = CRYPTO_ARGON2_ID;
    cfg.nb_blocks = p->m_kib; /* one block is 1 KiB */
    cfg.nb_passes = p->t;
    cfg.nb_lanes = p->p;

    crypto_argon2_inputs in;
    in.pass = pw;
    in.pass_size = (uint32_t)pw_len;
    in.salt = salt;
    in.salt_size = (uint32_t)salt_len;

    crypto_argon2_extras ex;
    ex.key = secret;
    ex.key_size = (uint32_t)secret_len;
    ex.ad = ad;
    ex.ad_size = (uint32_t)ad_len;

    crypto_argon2(out, (uint32_t)out_len, work, cfg, in, ex);

    crypto_wipe(work, work_bytes); /* the work area held password-derived data */
    free(work);
    return true;
}

bool hmi_kdf_hash(const hmi_kdf_params_t *p, const char *pw, size_t pw_len,
                  const uint8_t salt[HMI_KDF_SALT_LEN], uint8_t out[HMI_KDF_HASH_LEN])
{
    return hmi_kdf_hash_ex(p, (const uint8_t *)pw, pw_len, salt, HMI_KDF_SALT_LEN, NULL, 0u, NULL, 0u, out,
                           HMI_KDF_HASH_LEN);
}

bool hmi_kdf_equal(const uint8_t a[HMI_KDF_HASH_LEN], const uint8_t b[HMI_KDF_HASH_LEN])
{
    return crypto_verify32(a, b) == 0;
}

void hmi_wipe(void *p, size_t n)
{
    if (p && n) crypto_wipe(p, n);
}
