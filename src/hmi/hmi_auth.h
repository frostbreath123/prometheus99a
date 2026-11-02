/**
 * @file hmi_auth.h
 * @brief Named-user login with a PIN: verification, lockout and automatic logout.
 *
 * The users come from the signed configuration ("users", hmi_config.h): a name, a role, a random salt and
 *
 *   pin_hash = PBKDF2-HMAC-SHA-512(password = HMAC-SHA-512(pin key, PIN), salt, HMI_PIN_ITERATIONS), first 32 bytes
 *
 * PBKDF2 is RFC 8018 (checked against a published test vector in test_auth.c) on Monocypher's HMAC-SHA-512; keying the
 * PIN with the pin key first ("pepper") means a copy of the configuration alone does not let anyone test PINs offline.
 * The pin key is derived from the configuration key (hmi_auth_derive_key); with that key, a 4 to 8 digit PIN is still
 * guessable offline, and the threat model says so (docs/AUTHENTICATION.md).
 *
 * Rules: a PIN is 4 to 8 digits. HMI_AUTH_MAX_FAILS wrong PINs in a row lock the login for HMI_AUTH_LOCK_MS, doubled
 * for each further lock without a success in between, up to HMI_AUTH_LOCK_MAX_MS; a correct PIN ends it. A logged-in
 * user is logged out after HMI_AUTH_IDLE_MS without input. The lock is kept in memory only: a restart of the program
 * clears it, which is the engineer's recovery path together with p99_sign (add-user, del-user).
 *
 * Without users (n == 0) the module is disabled and the program uses the role claimed with --role, as before.
 * Single thread (the UI thread). The core still checks the role on every privileged call; this module only decides
 * which role and name the UI passes.
 */
#ifndef HMI_AUTH_H
#define HMI_AUTH_H

#include "hmi_config.h"

#define HMI_PIN_MIN 4
#define HMI_PIN_MAX 8
#define HMI_PIN_ITERATIONS   10000u
#define HMI_AUTH_KEY_LEN     32
#define HMI_AUTH_MAX_FAILS   5u
#define HMI_AUTH_LOCK_MS     60000u
#define HMI_AUTH_LOCK_MAX_MS 960000u
#define HMI_AUTH_IDLE_MS     300000u

typedef enum {
    HMI_AUTH_OK = 0,
    HMI_AUTH_WRONG_PIN,     /* counts towards the lock */
    HMI_AUTH_LOCKED,        /* not even checked: the lock is on */
    HMI_AUTH_BAD_FORMAT,    /* not 4 to 8 digits: refused without a check, does not count */
    HMI_AUTH_NO_USER,       /* index out of range */
    HMI_AUTH_DISABLED       /* no users configured, or no pin key */
} hmi_auth_result_t;

typedef struct {
    const hmi_user_cfg_t *users;
    size_t n;
    uint8_t key[HMI_AUTH_KEY_LEN];
    bool key_valid;
    int current;               /* index of the logged-in user, -1 for none */
    uint64_t last_input_ms;
    uint32_t fails;            /* wrong PINs since the last success or lock */
    uint32_t locks;            /* locks since the last success (doubles the lock time) */
    uint64_t locked_until_ms;
} hmi_auth_t;

/* The pin key: the first 32 bytes of HMAC-SHA-512(configuration key, "prometheus99 pin hash v1"). */
void hmi_auth_derive_key(const uint8_t *cfg_key, size_t cfg_key_len, uint8_t out[HMI_AUTH_KEY_LEN]);
/* PBKDF2-HMAC-SHA-512 (RFC 8018), any output length. */
void hmi_auth_pbkdf2_sha512(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len, uint32_t iterations,
                            uint8_t *out, size_t out_len);
/* pin_hash of a PIN (any length; the format rule is checked by the callers). */
void hmi_auth_hash(const uint8_t key[HMI_AUTH_KEY_LEN], const uint8_t salt[HMI_SALT_LEN], const char *pin,
                   uint8_t out[HMI_PIN_HASH_LEN]);
/* True for 4 to 8 ASCII digits. */
bool hmi_auth_pin_format_ok(const char *pin);

/* key may be NULL (the module is then disabled, as without users). The users must outlive the module. */
void hmi_auth_init(hmi_auth_t *a, const hmi_user_cfg_t *users, size_t n, const uint8_t *key);
bool hmi_auth_enabled(const hmi_auth_t *a);
hmi_auth_result_t hmi_auth_login(hmi_auth_t *a, size_t user, const char *pin, uint64_t now_ms);
void hmi_auth_logout(hmi_auth_t *a);
/* Operator input (a touch) at now_ms: restarts the idle time of a logged-in user. A time before the last input or the
 * login is ignored, so a caller may pass "the last touch" as read from the display. */
void hmi_auth_input(hmi_auth_t *a, uint64_t now_ms);
/* True once, when the idle time ran out and the user was logged out. */
bool hmi_auth_idle_check(hmi_auth_t *a, uint64_t now_ms);
/* Milliseconds the lock still lasts, 0 if none. */
uint32_t hmi_auth_locked_for(const hmi_auth_t *a, uint64_t now_ms);
/* The logged-in user's role, HMI_ROLE_VIEWER when nobody is logged in. */
hmi_role_t hmi_auth_role(const hmi_auth_t *a);
/* The logged-in user's name, NULL when nobody is logged in. */
const char *hmi_auth_user(const hmi_auth_t *a);
/* Wipes the pin key. */
void hmi_auth_destroy(hmi_auth_t *a);

#endif /* HMI_AUTH_H */
