#include "hmi_mac.h"
#include "../../third_party/monocypher/monocypher-ed25519.h"

typedef char hmi_mac_size_check[(sizeof(crypto_sha512_hmac_ctx) <= sizeof(hmi_mac_ctx_t)) ? 1 : -1];

void hmi_mac_init(hmi_mac_ctx_t *c, const uint8_t *key, size_t key_len)
{
    crypto_sha512_hmac_init((crypto_sha512_hmac_ctx *)c, key, key_len);
}

void hmi_mac_update(hmi_mac_ctx_t *c, const uint8_t *data, size_t len)
{
    crypto_sha512_hmac_update((crypto_sha512_hmac_ctx *)c, data, len);
}

void hmi_mac_final(hmi_mac_ctx_t *c, uint8_t out[HMI_MAC_LEN])
{
    crypto_sha512_hmac_final((crypto_sha512_hmac_ctx *)c, out);
    crypto_wipe(c, sizeof(*c));
}

void hmi_mac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[HMI_MAC_LEN])
{
    crypto_sha512_hmac(out, key, key_len, data, len);
}

bool hmi_mac_equal(const uint8_t a[HMI_MAC_LEN], const uint8_t b[HMI_MAC_LEN])
{
    return crypto_verify64(a, b) == 0;
}
