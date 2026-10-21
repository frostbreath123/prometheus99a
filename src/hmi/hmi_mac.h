/**
 * @file hmi_mac.h
 * @brief HMAC-SHA-512 and a constant-time comparison.
 *
 * Like hmi_kdf.c, hmi_mac.c is the only file that includes Monocypher's
 * monocypher-ed25519.h (SHA-512 and HMAC). The product uses it to check that a
 * configuration was signed by whoever holds the key (see hmi_cfgsec.h). The
 * key is a secret shared by the signing tool and the device; this is a MAC, not
 * a public-key signature, so anyone who can read the key can also sign.
 */
#ifndef HMI_MAC_H
#define HMI_MAC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define HMI_MAC_LEN 64

/* Incremental interface. The context is opaque so that this header needs no
 * third-party include. */
typedef struct {
    uint64_t opaque[64];
} hmi_mac_ctx_t;

void hmi_mac_init(hmi_mac_ctx_t *c, const uint8_t *key, size_t key_len);
void hmi_mac_update(hmi_mac_ctx_t *c, const uint8_t *data, size_t len);
void hmi_mac_final(hmi_mac_ctx_t *c, uint8_t out[HMI_MAC_LEN]); /* also wipes the context */

/* One call. */
void hmi_mac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[HMI_MAC_LEN]);

/* Constant-time comparison of two MACs. */
bool hmi_mac_equal(const uint8_t a[HMI_MAC_LEN], const uint8_t b[HMI_MAC_LEN]);

#endif /* HMI_MAC_H */
