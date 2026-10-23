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

/* Called for every journal entry, in order, from the thread that caused it and
 * never while the alarm lock is held, so a sink may take another lock or queue
 * a record for a storage worker. Only one thread runs the sink at a time. A
 * sink that returns false is counted in sink_failures. */
typedef bool (*hmi_alarm_log_sink_t)(void *user, const hmi_alarm_log_t *e);

#define HMI_SINK_PENDING     128   /* entries waiting for the sink; more than any one step can add */
#define HMI_ALARM_STATE_MAX  256   /* bytes of the persisted state (see hmi_alarm_state_serialize) */

typedef enum {
    HMI_ACK_OK = 0,        /* at least one alarm was acknowledged */
    HMI_ACK_NOTHING,       /* authorized, nothing awaiting acknowledgement */
    HMI_ACK_DENIED,        /* role not permitted */
    HMI_ACK_AUDIT_FAILED,  /* audit record could not be recorded: refused */
    HMI_ACK_PENDING        /* accepted; takes effect only when its audit record is durable (see below) */
} hmi_ack_result_t;

typedef struct {
    uint64_t t_ms;                       /* hmi_now_ms() of the attempt (monotonic, since boot) */
    hmi_role_t role;                     /* the role the caller presented */
    char actor[HMI_ACTOR_LEN];           /* actor name, or "-" (no accounts: always "-"; see hmi_actor_copy) */
    bool all;                            /* ACK ALL (true) or a single point */
    hmi_ack_result_t result;             /* HMI_ACK_OK (granted) or HMI_ACK_DENIED */
    size_t n_points;
    uint8_t points[HMI_MAX_TAGS + 1];    /* tag indices; n_tags = the comms alarm */
} hmi_ack_audit_t;

/* Returns true only when the record was durably recorded. */
typedef bool (*hmi_ack_audit_fn)(void *user, const hmi_ack_audit_t *rec);

/* Asynchronous form, for a storage worker. The hook queues the record and
 * returns true at once (it must never block), or returns false when it cannot.
 * token is not zero for a granted request: the owner then calls
 * hmi_alarm_ack_audit_done() with it once the record is durable or has
 * failed. token is zero for the best-effort record of a denied attempt, which
 * needs no completion. */
typedef bool (*hmi_ack_audit_submit_fn)(void *user, const hmi_ack_audit_t *rec, uint32_t token);

/* What was awaiting acknowledgement when the request arrived. */
typedef struct {
    size_t n;
    uint8_t point[HMI_MAX_TAGS + 1];
    hmi_alarm_state_t state[HMI_MAX_TAGS + 1];
    hmi_alarm_kind_t kind[HMI_MAX_TAGS + 1];
    uint64_t since_ms[HMI_MAX_TAGS + 1];
} hmi_ack_set_t;

#define HMI_ACK_PENDING_MAX    4        /* granted requests waiting for their record at one time */
#define HMI_ACK_PENDING_TTL_MS 10000u   /* a request whose record never completes is dropped, not applied */

typedef struct {
    bool used;
    uint32_t token;
    uint64_t t_start_ms;
    hmi_ack_set_t set;
} hmi_ack_pending_t;

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
    hmi_alarm_log_t sink_q[HMI_SINK_PENDING];
    uint8_t sq_head, sq_count;
    bool sink_flushing;
    uint32_t sink_dropped;         /* entries lost because the sink queue was full */
    uint32_t state_rev;            /* grows with every change of an alarm's state */
    hmi_ack_audit_fn ack_audit;
    void *ack_audit_user;
    uint32_t ack_audit_failures;   /* records the hook could not make durable */
    hmi_ack_audit_submit_fn ack_submit;
    void *ack_submit_user;
    hmi_ack_pending_t pend[HMI_ACK_PENDING_MAX];
    uint32_t next_token;
    uint32_t last_ack_seq;         /* counts resolved asynchronous requests */
    hmi_ack_result_t last_ack_result;
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

/* With an asynchronous hook a granted request is not applied in the call: it
 * returns HMI_ACK_PENDING, and the acknowledgement takes effect when the owner
 * reports the record durable through hmi_alarm_ack_audit_done(). The same
 * checks as above apply at that moment (an alarm that changed meanwhile is left
 * unacknowledged). If the record fails, or does not complete within
 * HMI_ACK_PENDING_TTL_MS, nothing changes (fail closed). At most
 * HMI_ACK_PENDING_MAX requests wait at once; another is refused with
 * HMI_ACK_AUDIT_FAILED. The asynchronous hook takes precedence over the
 * synchronous one. */
void hmi_alarm_set_ack_audit_async(hmi_alarm_t *a, hmi_ack_audit_submit_fn fn, void *user);
void hmi_alarm_ack_audit_done(hmi_alarm_t *a, uint32_t token, bool ok, uint64_t now_ms);
/* The outcome of the latest resolved asynchronous request: HMI_ACK_OK,
 * HMI_ACK_NOTHING (alarm changed meanwhile) or HMI_ACK_AUDIT_FAILED. Returns a
 * counter that grows with every resolved request, so a caller can tell a new
 * outcome from the one it has already shown. */
uint32_t hmi_alarm_last_ack(hmi_alarm_t *a, hmi_ack_result_t *result);

/* point: tag index, or the tag count for the comms alarm. */
hmi_ack_result_t hmi_alarm_ack(hmi_alarm_t *a, size_t point, uint64_t now_ms, hmi_role_t role);
hmi_ack_result_t hmi_alarm_ack_all(hmi_alarm_t *a, uint64_t now_ms, hmi_role_t role);

/* The same with an actor name (the product passes none), which goes into the audit
 * record. The calls above record "-". The UI always uses these. */
hmi_ack_result_t hmi_alarm_ack_as(hmi_alarm_t *a, size_t point, uint64_t now_ms, hmi_role_t role, const char *actor);
hmi_ack_result_t hmi_alarm_ack_all_as(hmi_alarm_t *a, uint64_t now_ms, hmi_role_t role, const char *actor);

void hmi_alarm_get(hmi_alarm_t *a, size_t point, hmi_alarm_point_t *out);
size_t hmi_alarm_journal_copy(hmi_alarm_t *a, hmi_alarm_log_t *out, size_t cap); /* oldest first */
size_t hmi_alarm_count_unacked(hmi_alarm_t *a);

/* Priority of a point (a tag index, or the tag count for the communications
 * alarm, which is always HIGH). Constant after start, so it takes no lock. */
hmi_priority_t hmi_alarm_priority(const hmi_alarm_t *a, size_t point);

typedef struct {
    uint8_t point;               /* tag index, or the tag count for the comms alarm */
    hmi_priority_t priority;
    hmi_alarm_point_t pt;        /* copy of the point as of the call */
} hmi_alarm_row_t;

/* The points that are not NORMAL, most urgent first: by priority (CRITICAL
 * first), then those waiting for acknowledgement before those already
 * acknowledged, then the most recent change first, then the lower point index.
 * One consistent copy is taken under the lock. Returns the number of rows
 * written, at most cap; if cap is too small the least urgent rows are left out. */
size_t hmi_alarm_list(hmi_alarm_t *a, hmi_alarm_row_t *out, size_t cap);

/* Persistence of alarm state across restarts (CRC-protected, atomic file). */
bool hmi_alarm_save(hmi_alarm_t *a, const char *path);
bool hmi_alarm_load(hmi_alarm_t *a, const char *path);

/* The persisted state as bytes, for a storage worker that writes the file.
 * Returns the number of bytes written to buf (at most HMI_ALARM_STATE_MAX), or
 * 0 if cap is too small. *rev (optional) receives hmi_alarm_state_rev() as of
 * the copy. hmi_alarm_state_apply() is the reverse and rejects anything that
 * is not exactly what serialize produced for the same tag count (bad CRC,
 * magic, version, size, point count or value). */
size_t hmi_alarm_state_serialize(hmi_alarm_t *a, void *buf, size_t cap, uint32_t *rev);
bool hmi_alarm_state_apply(hmi_alarm_t *a, const void *buf, size_t len);
/* Changes whenever an alarm's state or kind changes: a saver compares it with
 * the value at its last save to know whether there is anything new to write. */
uint32_t hmi_alarm_state_rev(hmi_alarm_t *a);

/* Put entries read back from the on-disk journal into the in-memory journal,
 * oldest first (at most HMI_JOURNAL_LEN, the newest are kept). The sink is not
 * called. The sequence counter continues after the highest restored number. */
void hmi_alarm_journal_restore(hmi_alarm_t *a, const hmi_alarm_log_t *ev, size_t n);

#endif /* HMI_ALARM_H */
