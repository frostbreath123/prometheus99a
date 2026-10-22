/**
 * @file hmi_types.h
 * @brief Shared types and compile-time limits of the portable HMI runtime.
 */
#ifndef HMI_TYPES_H
#define HMI_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HMI_MAX_TAGS        64
#define HMI_MAX_GROUPS      32
#define HMI_NAME_LEN        24
#define HMI_UNIT_LEN        8
#define HMI_HOST_LEN        16      /* dotted IPv4 literal */
#define HMI_MAX_REGS_PER_READ 125u  /* Modbus function 03/04 limit */
#define HMI_WRITE_QUEUE_LEN 8
#define HMI_AUDIT_LEN       32
#define HMI_JOURNAL_LEN     64
#define HMI_TREND_MAX_SERIES 8      /* tags that may have "trend": true */

typedef enum {
    HMI_Q_UNAVAILABLE = 0, /* never received a value */
    HMI_Q_BAD,             /* value not usable (comm loss, exception, stale) */
    HMI_Q_UNCERTAIN,       /* last value may be outdated or out of range */
    HMI_Q_GOOD
} hmi_quality_t;

typedef enum {
    HMI_REASON_NONE = 0,
    HMI_REASON_NOT_POLLED,
    HMI_REASON_COMM_LOSS,
    HMI_REASON_EXCEPTION,
    HMI_REASON_STALE,
    HMI_REASON_MISSED_POLL,
    HMI_REASON_OUT_OF_RANGE,
    HMI_REASON_PROTOCOL
} hmi_reason_t;

/* Application-level PLC responsiveness. Owned and computed ONLY by the comms
 * module (hmi_comms_link_health); every consumer (alarm engine, UI, runtime)
 * uses that single result. Distinct from the TCP connection state: a socket
 * can be connected while the PLC never answers (NO_RESPONSE). */
typedef enum {
    HMI_LINK_DOWN = 0,     /* no usable connection (disconnected/connecting/backoff) */
    HMI_LINK_NO_RESPONSE,  /* connection may be up but no valid reply for too long */
    HMI_LINK_DEGRADED,     /* replies late or recent misses; link still serving data */
    HMI_LINK_HEALTHY       /* recent valid replies */
} hmi_link_health_t;

/* Health values that must raise the communications alarm and mark data stale. */
static inline bool hmi_link_health_is_lost(hmi_link_health_t h)
{
    return h == HMI_LINK_DOWN || h == HMI_LINK_NO_RESPONSE;
}

typedef enum { HMI_AREA_HOLDING = 0, HMI_AREA_INPUT } hmi_area_t;

typedef enum {
    HMI_DT_U16 = 0,
    HMI_DT_I16,
    HMI_DT_U32,
    HMI_DT_I32,
    HMI_DT_F32
} hmi_dtype_t;

typedef enum { HMI_WORD_HIGH_FIRST = 0, HMI_WORD_LOW_FIRST } hmi_word_order_t;
typedef enum { HMI_BYTE_BIG = 0, HMI_BYTE_SWAPPED } hmi_byte_order_t;

typedef enum {
    HMI_ROLE_VIEWER = 0,
    HMI_ROLE_OPERATOR,
    HMI_ROLE_ENGINEER,
    HMI_ROLE_ADMIN
} hmi_role_t;

/* How urgent an alarm is (ISA-18.2 calls it priority). Configured per tag; the
 * communications alarm is always HIGH. A larger value is more urgent. The value
 * zero is LOW, so a configuration structure that was only cleared has the lowest
 * priority; the configuration parser sets MEDIUM unless the file says otherwise. */
typedef enum {
    HMI_PRI_LOW = 0,
    HMI_PRI_MEDIUM,
    HMI_PRI_HIGH,
    HMI_PRI_CRITICAL
} hmi_priority_t;

/* Who performed a privileged action, for the audit trail: the name of a logged-in
 * account (hmi_auth.h). Only a valid account name (3 to 24 characters from
 * a-z 0-9 _ . -) is kept; anything else, including NULL and a test build's
 * unauthenticated role, becomes "-". An audit line therefore never contains a
 * space, a newline or an '=' supplied by a caller. The rule is the same as
 * hmi_user_name_valid() and is checked against it in the tests. */
#define HMI_ACTOR_LEN 25
static inline void hmi_actor_copy(char dst[HMI_ACTOR_LEN], const char *src)
{
    size_t n = 0;
    if (src) {
        while (src[n]) {
            char c = src[n];
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
            if (!ok || n >= HMI_ACTOR_LEN - 1) {
                n = 0;
                break;
            }
            n++;
        }
    }
    if (n < 3) {
        dst[0] = '-';
        dst[1] = '\0';
        return;
    }
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = '\0';
}

#endif /* HMI_TYPES_H */
