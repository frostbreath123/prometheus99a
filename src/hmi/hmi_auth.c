#include "hmi_auth.h"
#include "hmi_mac.h"
#include "hmi_util.h" /* hmi_wipe */
#include <string.h>

void hmi_auth_derive_key(const uint8_t *cfg_key, size_t cfg_key_len, uint8_t out[HMI_AUTH_KEY_LEN])
{
    static const char LABEL[] = "prometheus99 pin hash v1";
    uint8_t mac[HMI_MAC_LEN];
    hmi_mac(cfg_key, cfg_key_len, (const uint8_t *)LABEL, sizeof(LABEL) - 1, mac);
    memcpy(out, mac, HMI_AUTH_KEY_LEN);
    hmi_wipe(mac, sizeof(mac));
}

void hmi_auth_pbkdf2_sha512(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len, uint32_t iterations,
                            uint8_t *out, size_t out_len)
{
    /* RFC 8018, section 5.2: T_i = U_1 ^ ... ^ U_c, U_1 = HMAC(P, S || INT(i)), U_j = HMAC(P, U_(j-1)) */
    hmi_mac_ctx_t c;
    uint8_t u[HMI_MAC_LEN], t[HMI_MAC_LEN];
    for (uint32_t block = 1; out_len > 0; block++) {
        uint8_t be[4] = { (uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block };
        hmi_mac_init(&c, pw, pw_len);
        hmi_mac_update(&c, salt, salt_len);
        hmi_mac_update(&c, be, 4);
        hmi_mac_final(&c, u);
        memcpy(t, u, sizeof(t));
        for (uint32_t j = 1; j < iterations; j++) {
            hmi_mac(pw, pw_len, u, sizeof(u), u);
            for (size_t k = 0; k < sizeof(t); k++) t[k] ^= u[k];
        }
        size_t n = out_len < sizeof(t) ? out_len : sizeof(t);
        memcpy(out, t, n);
        out += n;
        out_len -= n;
    }
    hmi_wipe(u, sizeof(u));
    hmi_wipe(t, sizeof(t));
}

void hmi_auth_hash(const uint8_t key[HMI_AUTH_KEY_LEN], const uint8_t salt[HMI_SALT_LEN], const char *pin,
                   uint8_t out[HMI_PIN_HASH_LEN])
{
    uint8_t peppered[HMI_MAC_LEN]; /* the PIN keyed with the device's pin key, then the standard PBKDF2 */
    hmi_mac(key, HMI_AUTH_KEY_LEN, (const uint8_t *)pin, strlen(pin), peppered);
    hmi_auth_pbkdf2_sha512(peppered, sizeof(peppered), salt, HMI_SALT_LEN, HMI_PIN_ITERATIONS, out, HMI_PIN_HASH_LEN);
    hmi_wipe(peppered, sizeof(peppered));
}

bool hmi_auth_pin_format_ok(const char *pin)
{
    size_t n = 0;
    for (; pin[n]; n++)
        if (pin[n] < '0' || pin[n] > '9' || n >= HMI_PIN_MAX) return false;
    return n >= HMI_PIN_MIN;
}

void hmi_auth_init(hmi_auth_t *a, const hmi_user_cfg_t *users, size_t n, const uint8_t *key)
{
    memset(a, 0, sizeof(*a));
    a->current = -1;
    a->users = users;
    a->n = users ? (n > HMI_MAX_USERS ? HMI_MAX_USERS : n) : 0;
    if (key) {
        memcpy(a->key, key, HMI_AUTH_KEY_LEN);
        a->key_valid = true;
    }
}

bool hmi_auth_enabled(const hmi_auth_t *a) { return a->n > 0 && a->key_valid; }

uint32_t hmi_auth_locked_for(const hmi_auth_t *a, uint64_t now_ms)
{
    return a->locked_until_ms > now_ms ? (uint32_t)(a->locked_until_ms - now_ms) : 0u;
}

hmi_auth_result_t hmi_auth_login(hmi_auth_t *a, size_t user, const char *pin, uint64_t now_ms)
{
    if (!hmi_auth_enabled(a)) return HMI_AUTH_DISABLED;
    if (hmi_auth_locked_for(a, now_ms)) return HMI_AUTH_LOCKED;
    if (user >= a->n) return HMI_AUTH_NO_USER;
    if (!pin || !hmi_auth_pin_format_ok(pin)) return HMI_AUTH_BAD_FORMAT;
    uint8_t h[HMI_PIN_HASH_LEN];
    hmi_auth_hash(a->key, a->users[user].salt, pin, h);
    uint8_t diff = 0; /* constant time: the comparison does not stop at the first different byte */
    for (size_t i = 0; i < HMI_PIN_HASH_LEN; i++) diff |= (uint8_t)(h[i] ^ a->users[user].pin_hash[i]);
    hmi_wipe(h, sizeof(h));
    if (diff == 0) {
        a->current = (int)user;
        a->last_input_ms = now_ms;
        a->fails = 0;
        a->locks = 0;
        return HMI_AUTH_OK;
    }
    if (++a->fails >= HMI_AUTH_MAX_FAILS) {
        uint64_t ms = (uint64_t)HMI_AUTH_LOCK_MS << (a->locks < 4u ? a->locks : 4u);
        if (ms > HMI_AUTH_LOCK_MAX_MS) ms = HMI_AUTH_LOCK_MAX_MS;
        a->locked_until_ms = now_ms + ms;
        a->locks++;
        a->fails = 0;
        a->current = -1; /* a failed attempt never leaves someone else logged in at the panel */
    }
    return HMI_AUTH_WRONG_PIN;
}

void hmi_auth_logout(hmi_auth_t *a) { a->current = -1; }

void hmi_auth_input(hmi_auth_t *a, uint64_t now_ms)
{
    if (a->current >= 0 && now_ms > a->last_input_ms) a->last_input_ms = now_ms; /* never moves back */
}

bool hmi_auth_idle_check(hmi_auth_t *a, uint64_t now_ms)
{
    if (a->current < 0 || now_ms - a->last_input_ms < HMI_AUTH_IDLE_MS) return false;
    a->current = -1;
    return true;
}

hmi_role_t hmi_auth_role(const hmi_auth_t *a)
{
    return a->current >= 0 ? a->users[a->current].role : HMI_ROLE_VIEWER;
}

const char *hmi_auth_user(const hmi_auth_t *a)
{
    return a->current >= 0 ? a->users[a->current].name : NULL;
}

void hmi_auth_destroy(hmi_auth_t *a)
{
    hmi_wipe(a->key, sizeof(a->key));
    a->key_valid = false;
    a->current = -1;
}
