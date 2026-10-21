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
    HMI_WR_RESERVED,       /* audit slot taken, awaiting audit persistence */
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
    HMI_WRS_ERR_AUDIT
} hmi_wr_submit_t;

typedef struct {
    uint32_t id;
    uint8_t tag;
    hmi_wr_state_t state;
    hmi_role_t role;
    char actor[HMI_ACTOR_LEN];   /* logged-in user name, or "-" (see hmi_actor_copy) */
    double eng;
    uint32_t raw;
    uint32_t readback_raw;
    uint8_t exc;
    uint64_t t_submit_ms;
    uint64_t t_final_ms;
} hmi_write_rec_t;

/* Durable audit hook. Return false to signal "cannot record": the module then
 * fails closed (request refused / not transmitted). event is a short tag. */
typedef bool (*hmi_audit_fn)(void *user, const hmi_write_rec_t *rec, const char *event);

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
} hmi_writes_stats_t;

typedef struct {
    const hmi_tagdb_t *db;
    bool enabled;
    uint32_t ttl_ms;
    hmi_audit_fn audit;
    void *audit_user;

    hmi_mutex_t lock;
    hmi_write_rec_t ring[HMI_AUDIT_LEN];
    uint8_t queue[HMI_WRITE_QUEUE_LEN]; /* indices into ring, FIFO */
    uint8_t q_head, q_len;
    uint32_t next_id;
    hmi_writes_stats_t stats;
    bool ready;
} hmi_writes_t;

bool hmi_writes_init(hmi_writes_t *w, const hmi_tagdb_t *db, bool enabled, uint32_t ttl_ms);
void hmi_writes_destroy(hmi_writes_t *w);
void hmi_writes_set_audit(hmi_writes_t *w, hmi_audit_fn fn, void *user);
void hmi_writes_set_enabled(hmi_writes_t *w, bool enabled);

/* ---- operator side (UI thread). Never blocks on PLC I/O. ----
 * link: current result of hmi_comms_link_health(); writes are refused while
 * the link is lost. */
hmi_wr_submit_t hmi_writes_submit(hmi_writes_t *w, size_t tag, double eng, hmi_role_t role,
                                  hmi_link_health_t link, uint64_t now_ms, uint32_t *out_id);
/* The same with the name of the logged-in user, kept in the audit record. The
 * call above records "-". The UI always uses this one. */
hmi_wr_submit_t hmi_writes_submit_as(hmi_writes_t *w, size_t tag, double eng, hmi_role_t role, const char *actor,
                                     hmi_link_health_t link, uint64_t now_ms, uint32_t *out_id);
bool hmi_writes_status(hmi_writes_t *w, uint32_t id, hmi_write_rec_t *out);
void hmi_writes_stats(hmi_writes_t *w, hmi_writes_stats_t *out);

/* ---- transport side (comms thread) ---- */
/* Pops the next queued write, records the send intent durably, and returns a
 * job. Returns false when nothing is eligible. A request whose audit record
 * cannot be written is finalised REJECTED and is NOT returned. */
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
