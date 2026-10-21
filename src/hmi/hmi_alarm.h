/**
 * @file hmi_alarm.h
 * @brief ISA-18.2 style alarm engine (limits, deadband hysteresis, on/off
 *        debounce, acknowledgement, return-to-normal-unacknowledged).
 *
 * Evaluated from tag snapshots by hmi_alarm_step(); it performs no I/O, so
 * it stays responsive while the comms thread is blocked in a Modbus call.
 * Alarms are only raised or cleared from GOOD data; BAD / UNAVAILABLE data
 * freezes the alarm state (it is never silently cleared).
 */
#ifndef HMI_ALARM_H
#define HMI_ALARM_H

#include "hmi_tags.h"
#include "hmi_authz.h"

typedef enum {
    HMI_AL_NORMAL = 0,
    HMI_AL_ACTIVE_UNACK,
    HMI_AL_ACTIVE_ACK,
    HMI_AL_RTN_UNACK
} hmi_alarm_state_t;

typedef enum {
    HMI_ALK_NONE = 0,
    HMI_ALK_LO,
    HMI_ALK_HI,
    HMI_ALK_LOLO,
    HMI_ALK_HIHI,
    HMI_ALK_COMMS /* system alarm: PLC link lost */
} hmi_alarm_kind_t;

typedef enum {
    HMI_ALE_RAISED = 1,
    HMI_ALE_ESCALATED,
    HMI_ALE_RTN,
    HMI_ALE_ACKED,
    HMI_ALE_CLEARED
} hmi_alarm_event_t;

typedef struct {
    hmi_alarm_state_t state;
    hmi_alarm_kind_t kind;
    uint8_t acked_rank;
    bool data_suppressed;   /* underlying tag data not GOOD: state frozen */
    uint64_t since_ms;
} hmi_alarm_point_t;

typedef struct {
    uint32_t seq;
    uint64_t ts_ms;
    uint8_t point;          /* tag index, or n_tags for the comms alarm */
    uint8_t kind;
    uint8_t event;
    uint8_t state_after;
    double value;
} hmi_alarm_log_t;

typedef bool (*hmi_alarm_log_sink_t)(void *user, const hmi_alarm_log_t *e);

typedef enum {
    HMI_ACK_OK = 0,        /* at least one alarm was acknowledged */
    HMI_ACK_NOTHING,       /* authorized, nothing awaiting acknowledgement */
    HMI_ACK_DENIED,        /* role not permitted */
    HMI_ACK_AUDIT_FAILED   /* audit record could not be recorded: refused */
} hmi_ack_result_t;

typedef struct {
    uint64_t t_ms;                       /* hmi_now_ms() of the attempt (monotonic, since boot) */
    hmi_role_t role;                     /* the role the caller presented */
    char actor[HMI_ACTOR_LEN];           /* logged-in user name, or "-" (see hmi_actor_copy) */
    bool all;                            /* ACK ALL (true) or a single point */
    hmi_ack_result_t result;             /* HMI_ACK_OK (granted) or HMI_ACK_DENIED */
    size_t n_points;
    uint8_t points[HMI_MAX_TAGS + 1];    /* tag indices; n_tags = the comms alarm */
} hmi_ack_audit_t;

/* Returns true only when the record was durably recorded. */
typedef bool (*hmi_ack_audit_fn)(void *user, const hmi_ack_audit_t *rec);

typedef struct {
    hmi_tagdb_t *db;
    hmi_alarm_point_t pt[HMI_MAX_TAGS + 1];
    /* debounce bookkeeping */
    hmi_alarm_kind_t cand[HMI_MAX_TAGS + 1];
    uint64_t cand_since[HMI_MAX_TAGS + 1];
    bool cand_valid[HMI_MAX_TAGS + 1];

    hmi_alarm_log_t journal[HMI_JOURNAL_LEN];
    uint8_t j_head, j_count;
    uint32_t seq;
    uint32_t sink_failures;
    hmi_alarm_log_sink_t sink;
    void *sink_user;
    hmi_ack_audit_fn ack_audit;
    void *ack_audit_user;
    uint32_t ack_audit_failures;   /* records the hook could not make durable */
    hmi_mutex_t lock;
    bool ready;
} hmi_alarm_t;

bool hmi_alarm_init(hmi_alarm_t *a, hmi_tagdb_t *db);
void hmi_alarm_destroy(hmi_alarm_t *a);
void hmi_alarm_set_sink(hmi_alarm_t *a, hmi_alarm_log_sink_t fn, void *user);

/* link: the single link-health result from hmi_comms_link_health(). */
void hmi_alarm_step(hmi_alarm_t *a, uint64_t now_ms, hmi_link_health_t link);

/* ---- acknowledgement (privileged, authorized in the core) ----
 *
 * `role` is the caller's CLAIMED role (see hmi_authz.h), not an authenticated
 * identity. Policy:
 *  - Not authorized (below Operator): HMI_ACK_DENIED. No alarm state changes,
 *    no journal entry. The attempt is reported to the ack-audit hook on a best
 *    effort basis (the hook failing cannot change the already-denied result).
 *  - Authorized but nothing is awaiting acknowledgement: HMI_ACK_NOTHING. No
 *    state change and no audit record (there is no privileged effect).
 *  - Authorized: the intent (role, action, alarm points) is passed to the
 *    ack-audit hook BEFORE any state changes. If a hook is installed and
 *    returns false, the acknowledgement is refused (HMI_ACK_AUDIT_FAILED) and
 *    nothing changes: fail closed, as for PLC writes. With no hook installed
 *    acknowledgements are allowed and are not recorded.
 *  - The hook runs outside every lock and may block on storage. A record
 *    counts as durable only if the hook returned true.
 *  - Points that changed between the audit call and the apply step (state,
 *    kind or timestamp) are left unacknowledged, so an alarm that escalated
 *    in that window is never acknowledged unseen.
 */
void hmi_alarm_set_ack_audit(hmi_alarm_t *a, hmi_ack_audit_fn fn, void *user);

/* point: tag index, or the tag count for the comms alarm. */
hmi_ack_result_t hmi_alarm_ack(hmi_alarm_t *a, size_t point, uint64_t now_ms, hmi_role_t role);
hmi_ack_result_t hmi_alarm_ack_all(hmi_alarm_t *a, uint64_t now_ms, hmi_role_t role);

/* The same with the name of the logged-in user, which goes into the audit
 * record. The calls above record "-". The UI always uses these. */
hmi_ack_result_t hmi_alarm_ack_as(hmi_alarm_t *a, size_t point, uint64_t now_ms, hmi_role_t role, const char *actor);
hmi_ack_result_t hmi_alarm_ack_all_as(hmi_alarm_t *a, uint64_t now_ms, hmi_role_t role, const char *actor);

void hmi_alarm_get(hmi_alarm_t *a, size_t point, hmi_alarm_point_t *out);
size_t hmi_alarm_journal_copy(hmi_alarm_t *a, hmi_alarm_log_t *out, size_t cap); /* oldest first */
size_t hmi_alarm_count_unacked(hmi_alarm_t *a);

/* Persistence of alarm state across restarts (CRC-protected, atomic file). */
bool hmi_alarm_save(hmi_alarm_t *a, const char *path);
bool hmi_alarm_load(hmi_alarm_t *a, const char *path);

#endif /* HMI_ALARM_H */
