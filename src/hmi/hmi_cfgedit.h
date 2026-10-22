/**
 * @file hmi_cfgedit.h
 * @brief Changing the configuration from the screen without giving the HMI the
 *        right to change it: a narrow request, and the rules that apply it.
 *
 * The configuration is signed and owned by root; the service only reads it
 * (docs/CONFIG_INTEGRITY.md). To let an Engineer tune settings on the device the
 * service writes a small REQUEST file into a spool directory. A separate program
 * running as root (p99_cfg, started by a systemd path unit) reads the request,
 * decides whether it is allowed, writes and signs the new configuration and
 * restarts the service. The service can read the key (it must verify the signature)
 * but cannot write the configuration or the signature, so nothing in this file widens
 * what a compromised service can do beyond what the whitelist below allows.
 *
 * What a request can change (everything else needs the offline tools):
 *   - plc.host, but only to a host that is already in plc.allowed_hosts, plc.port, plc.unit
 *   - per tag: the four alarm limits (each can be switched off), deadband, the
 *     on and off delays and the alarm priority
 * It cannot change the write switch, who may write, the write limits, the rate
 * limits, the timeouts, the allow-list, tag addresses, types or scaling, or add or
 * remove tags. A change that makes the configuration invalid for the strict
 * parser is refused as a whole, and so is one that changes nothing.
 *
 * Request text (all lines end with a line feed, printable ASCII only, at most
 * HMI_CFGEDIT_REQUEST_MAX bytes):
 *   P99REQ 1
 *   base_serial=7            the serial of the signature the request was made against
 *   user=eng1                who asked (as the service knows it; "-" if unknown)
 *   plc.host=192.168.0.11
 *   tag.TANK_LEVEL.alarm.high=82
 *   tag.TANK_LEVEL.alarm.low_low=off
 *   END
 * A line may appear once; any unknown key, a missing END or a byte after it makes
 * the whole request invalid.
 *
 * Limits, stated plainly: the root program cannot tell who is really logged in on
 * the screen. A compromised service could ask for any change on the whitelist, for
 * example to raise an alarm limit. The whitelist, the bounds below, the audit
 * record and the signed result are the protection, not a proof of identity.
 */
#ifndef HMI_CFGEDIT_H
#define HMI_CFGEDIT_H

#include "hmi_config.h"
#include "hmi_authz.h"

#define HMI_CFGEDIT_REQUEST_MAX 8192u
#define HMI_CFGEDIT_MAX_CHANGES 64u
#define HMI_CFGEDIT_LINE_MAX 160u

typedef enum {
    HMI_CE_PLC_HOST = 1,
    HMI_CE_PLC_PORT,
    HMI_CE_PLC_UNIT,
    HMI_CE_ALARM_HI,
    HMI_CE_ALARM_HIHI,
    HMI_CE_ALARM_LO,
    HMI_CE_ALARM_LOLO,
    HMI_CE_ALARM_DEADBAND,
    HMI_CE_ALARM_ON_MS,
    HMI_CE_ALARM_OFF_MS,
    HMI_CE_ALARM_PRIORITY
} hmi_ce_field_t;

typedef struct {
    hmi_ce_field_t field;
    char tag[HMI_NAME_LEN];     /* the tag, empty for the PLC fields */
    double num;                 /* a number; for the priority an hmi_priority_t */
    bool off;                   /* an alarm limit that is switched off */
    char host[HMI_HOST_LEN];    /* HMI_CE_PLC_HOST */
} hmi_ce_change_t;

typedef struct {
    uint32_t base_serial;
    char user[HMI_ACTOR_LEN];
    size_t n;
    hmi_ce_change_t c[HMI_CFGEDIT_MAX_CHANGES];
} hmi_cfgedit_request_t;

/* Starts an empty request for a configuration signed with `base_serial`. */
void hmi_cfgedit_init(hmi_cfgedit_request_t *r, uint32_t base_serial, const char *user);
/* Adds or replaces (same field and tag) one change. False if the table is full or
 * a string is too long or holds a character that is not allowed. */
bool hmi_cfgedit_set_host(hmi_cfgedit_request_t *r, const char *host);
bool hmi_cfgedit_set_number(hmi_cfgedit_request_t *r, hmi_ce_field_t field, const char *tag, double value);
bool hmi_cfgedit_set_limit_off(hmi_cfgedit_request_t *r, hmi_ce_field_t field, const char *tag);
bool hmi_cfgedit_set_priority(hmi_cfgedit_request_t *r, const char *tag, hmi_priority_t p);

/* Request text. Returns the length written (a zero byte is added), 0 if cap is too small. */
size_t hmi_cfgedit_format(const hmi_cfgedit_request_t *r, char *buf, size_t cap);
/* Strict parser. On failure *out is cleared and err says why in one line. */
bool hmi_cfgedit_parse(const char *text, size_t len, hmi_cfgedit_request_t *out, char *err, size_t err_cap);

/* Applies a request to a verified configuration.
 *  - every change is checked against the whitelist and the bounds above;
 *  - the changed configuration is written out as text (`text`, at least
 *    HMI_CONFIG_MAX_BYTES + 1 bytes) and read back with hmi_config_parse, so the
 *    result is exactly what the device will load; plc.host must be in the allow-list;
 *  - `out` is that re-read structure; *n_effective is the number of changes that
 *    differ from the current values.
 * Returns false, with `out` cleared and a one-line reason in err, if anything is
 * refused. Nothing else about the configuration can change. */
bool hmi_cfgedit_apply(const hmi_cfgedit_request_t *req, const hmi_app_cfg_t *base, hmi_app_cfg_t *out,
                       char *text, size_t text_cap, size_t *text_len, size_t *n_effective, char *err, size_t err_cap);

/* ---- the service side: handing a request to the root program ---- */
typedef enum {
    HMI_CFGREQ_OK = 0,        /* the request is in the spool directory */
    HMI_CFGREQ_DENIED,        /* the role may not ask for changes */
    HMI_CFGREQ_EMPTY,         /* nothing to ask for */
    HMI_CFGREQ_BUSY,          /* an earlier request has not been dealt with yet */
    HMI_CFGREQ_AUDIT_FAILED,  /* the record could not be made durable: nothing was written */
    HMI_CFGREQ_WRITE_FAILED,  /* the spool directory cannot be written */
    HMI_CFGREQ_TOO_LARGE
} hmi_cfgreq_result_t;

/* Must return true only when the line is durably recorded. */
typedef bool (*hmi_cfgreq_audit_fn)(void *user, const char *line, size_t len);

/* Checks the role in the core (HMI_ACT_EDIT_SETTINGS), records the request BEFORE it
 * leaves (a record that cannot be made refuses it), then writes <spool>/request
 * atomically. audit may be NULL (no audit trail: the request is still written). */
hmi_cfgreq_result_t hmi_cfgreq_submit(const hmi_cfgedit_request_t *req, hmi_role_t role, const char *spool_dir,
                                      hmi_cfgreq_audit_fn audit, void *audit_user, uint64_t now_ms);

typedef enum {
    HMI_CFGREQ_NONE = 0,      /* no request pending and no result */
    HMI_CFGREQ_PENDING,       /* a request is waiting for the root program */
    HMI_CFGREQ_APPLIED,       /* the last result says applied */
    HMI_CFGREQ_REJECTED       /* the last result says rejected (or is unreadable) */
} hmi_cfgreq_state_t;

/* The state of the spool directory. `text` receives the first line of the result file
 * ("applied serial=8 changes=3 by=eng1" or "rejected: reason"); it is empty without one. */
hmi_cfgreq_state_t hmi_cfgreq_state(const char *spool_dir, char *text, size_t text_cap);

#endif /* HMI_CFGEDIT_H */
