/**
 * @file hmi_writes.h
 * @brief Operator write lifecycle: validation, bounded queue, audit-before-send,
 *        expiry, read-back verification outcome. No transport knowledge.
 *
 * The connection module (hmi_comms) performs the actual PLC I/O; this module
 * decides what may be written and records what happened. It never retries:
 * a request is transmitted at most once.
 *
 * States: RESERVED -> PENDING -> SENT -> CONFIRMED | REJECTED | UNKNOWN |
 *         VERIFY_FAILED, or PENDING -> EXPIRED (never transmitted).
 * UNKNOWN means "transmitted, outcome not determinable"; it is final and is
 * never replayed.
 */
#ifndef HMI_WRITES_H
#define HMI_WRITES_H

#include "hmi_tags.h"

typedef enum {
    HMI_WR_FREE = 0,
    HMI_WR_RESERVED,       /* audit slot taken, the RESERVED record is not yet durable (see audit_done) */
    HMI_WR_PENDING,        /* queued, not yet transmitted */
    HMI_WR_SENT,           /* being transmitted (transient) */
    HMI_WR_CONFIRMED,      /* PLC accepted AND read-back matched */
    HMI_WR_REJECTED,       /* PLC exception, or audit failed before send */
    HMI_WR_EXPIRED,        /* never transmitted (link down / TTL) */
    HMI_WR_UNKNOWN,        /* transmitted, outcome not determinable. Never replayed. */
    HMI_WR_VERIFY_FAILED   /* PLC accepted but read-back differs */
} hmi_wr_state_t;

typedef enum {
    HMI_WRS_OK = 0,
    HMI_WRS_ERR_DISABLED,
    HMI_WRS_ERR_NO_TAG,
    HMI_WRS_ERR_NOT_WRITABLE,
    HMI_WRS_ERR_ROLE,
    HMI_WRS_ERR_RANGE,
    HMI_WRS_ERR_NOT_REPRESENTABLE,
    HMI_WRS_ERR_LINK_DOWN,
    HMI_WRS_ERR_QUEUE_FULL,
    HMI_WRS_ERR_AUDIT,
    HMI_WRS_ERR_RATE_LIMITED   /* too soon after the last write to this tag, or too many in the last minute */
} hmi_wr_submit_t;

typedef struct {
    uint32_t id;
    uint8_t tag;
    hmi_wr_state_t state;
    hmi_role_t role;
    char actor[HMI_ACTOR_LEN];   /* actor name, or "-" (no accounts: always "-"; see hmi_actor_copy) */
    double eng;
    uint32_t raw;
    uint32_t readback_raw;
    uint8_t exc;
    uint64_t t_submit_ms;
    uint64_t t_final_ms;
    double old_eng;              /* the tag's value when the request was submitted ... */
    uint8_t old_quality;         /* ... and its quality (hmi_quality_t); the value counts only if GOOD */
} hmi_write_rec_t;

/* Durable audit hook. Return false to signal "cannot record": the module then
 * fails closed (request refused / not transmitted). event is a short tag. */
typedef bool (*hmi_audit_fn)(void *user, const hmi_write_rec_t *rec, const char *event);

/* Asynchronous audit of the RESERVED record, for a storage worker. The hook
 * queues the record and returns true at once, or returns false when it cannot
 * (queue full, no audit possible): the request is then refused. It must never
 * block. When the record is durable (or has failed) the owner calls
 * hmi_writes_audit_done() with rec->id. Until then the request stays RESERVED
 * and cannot be transmitted. */
typedef bool (*hmi_audit_submit_fn)(void *user, const hmi_write_rec_t *rec, const char *event);

/* A transmit job handed to the transport. Registers are in wire order. */
typedef struct {
    uint8_t slot;       /* opaque to the transport */
    uint8_t tag;
    uint16_t address;
    uint16_t n_regs;    /* 1 or 2 */
    uint16_t regs[2];
    hmi_area_t read_area; /* where to read back */
} hmi_write_job_t;

typedef struct {
    uint32_t submitted, confirmed, rejected, unknown, expired, verify_failed, audit_failures;
    uint32_t rate_limited;         /* submissions refused by the rate limit */
    uint32_t revalidation_failed;  /* queued requests stopped by the second check before sending */
} hmi_writes_stats_t;

#define HMI_WRITE_RATE_MAX 600   /* largest value of max_per_minute */

typedef struct {
    hmi_tagdb_t *db;
    bool enabled;
    uint32_t ttl_ms;
    hmi_audit_fn audit;
    void *audit_user;
    hmi_audit_submit_fn audit_submit;
    void *audit_submit_user;

    hmi_mutex_t lock;
    hmi_write_rec_t ring[HMI_AUDIT_LEN];
    uint8_t queue[HMI_WRITE_QUEUE_LEN]; /* indices into ring, FIFO */
    uint8_t q_head, q_len;
    uint32_t next_id;
    hmi_writes_stats_t stats;
    bool ready;

    /* rate limit: 0 = not limited (the default, so a bare module behaves as before) */
    uint32_t min_interval_ms, max_per_minute;
    uint64_t last_ms[HMI_MAX_TAGS];
    bool last_valid[HMI_MAX_TAGS];
    uint64_t window[HMI_WRITE_RATE_MAX];  /* times of the latest accepted writes, oldest first */
    uint16_t win_head, win_count;
} hmi_writes_t;

bool hmi_writes_init(hmi_writes_t *w, hmi_tagdb_t *db, bool enabled, uint32_t ttl_ms);
void hmi_writes_destroy(hmi_writes_t *w);
void hmi_writes_set_audit(hmi_writes_t *w, hmi_audit_fn fn, void *user);
/* With an asynchronous hook the RESERVED record goes through it and the
 * synchronous hook is used only for the later records (SENT and the final
 * state). Without one, the synchronous hook records RESERVED too. */
void hmi_writes_set_audit_async(hmi_writes_t *w, hmi_audit_submit_fn fn, void *user);
/* The outcome of a RESERVED record handed to the asynchronous hook, from any
 * thread. ok: the request becomes PENDING and is queued. !ok: it becomes
 * REJECTED and is never transmitted (fail closed). A request that already
 * expired is left as it is. */
void hmi_writes_audit_done(hmi_writes_t *w, uint32_t id, bool ok, uint64_t now_ms);
void hmi_writes_set_enabled(hmi_writes_t *w, bool enabled);
/* Rate limit on accepted submissions: at least min_interval_ms between two to
 * the same tag, and at most max_per_minute (at most HMI_WRITE_RATE_MAX) in any
 * 60 s window. 0 turns a limit off. A refused request returns
 * HMI_WRS_ERR_RATE_LIMITED and uses up nothing. */
void hmi_writes_set_limits(hmi_writes_t *w, uint32_t min_interval_ms, uint32_t max_per_minute);

/* ---- operator side (UI thread). Never blocks on PLC I/O. ----
 * With an asynchronous audit hook it does not wait for the disk either: the
 * call returns HMI_WRS_OK with the id and the request is RESERVED until its
 * record is durable, then PENDING. Poll hmi_writes_status() for the state.
 * link: current result of hmi_comms_link_health(); writes are refused while
 * the link is lost. */
hmi_wr_submit_t hmi_writes_submit(hmi_writes_t *w, size_t tag, double eng, hmi_role_t role,
                                  hmi_link_health_t link, uint64_t now_ms, uint32_t *out_id);
/* The same with an actor name (the product passes none), kept in the audit record. The
 * call above records "-". The UI always uses this one. */
hmi_wr_submit_t hmi_writes_submit_as(hmi_writes_t *w, size_t tag, double eng, hmi_role_t role, const char *actor,
                                     hmi_link_health_t link, uint64_t now_ms, uint32_t *out_id);
bool hmi_writes_status(hmi_writes_t *w, uint32_t id, hmi_write_rec_t *out);
void hmi_writes_stats(hmi_writes_t *w, hmi_writes_stats_t *out);

/* ---- transport side (comms thread) ---- */
/* Pops the next queued write, checks it once more, records the send intent
 * durably, and returns a job. Returns false when nothing is eligible.
 *  - The second check repeats what submit decided that can have changed since:
 *    the master switch, that the tag is still writable, and that the role,
 *    the value range and the register encoding still hold. A request that no
 *    longer passes is finalised REJECTED (audit event REJECTED_REVALIDATION)
 *    and is NOT returned. The configuration is immutable after start, so this
 *    mostly guards against a future change of that, and against a corrupted
 *    queue entry.
 *  - A request whose audit record cannot be written is finalised REJECTED and
 *    is NOT returned. */
bool hmi_writes_take(hmi_writes_t *w, uint64_t now_ms, hmi_write_job_t *job);
/* PLC replied with a Modbus exception: nothing was applied. */
void hmi_writes_finish_exception(hmi_writes_t *w, const hmi_write_job_t *job, uint8_t exc, uint64_t now_ms);
/* Transmitted but the outcome cannot be determined (timeout, transport error,
 * lost read-back). Never retried. */
void hmi_writes_finish_unknown(hmi_writes_t *w, const hmi_write_job_t *job, uint8_t exc, uint64_t now_ms);
/* Write accepted and the registers were read back: CONFIRMED when the read-back
 * matches the request, VERIFY_FAILED otherwise. */
void hmi_writes_finish_verified(hmi_writes_t *w, const hmi_write_job_t *job, const uint16_t *readback_regs,
                                uint64_t now_ms);
/* Expire queued, never-transmitted writes older than the TTL (all=true: every
 * queued write, used when the link is declared lost). */
void hmi_writes_expire(hmi_writes_t *w, uint64_t now_ms, bool all);

#endif /* HMI_WRITES_H */
