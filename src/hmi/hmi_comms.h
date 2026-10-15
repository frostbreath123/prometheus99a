/**
 * @file hmi_comms.h
 * @brief Connection state machine, poll scheduler and PLC link health.
 *
 * Single-threaded step function: the caller (a comms thread or a test)
 * drives it; the query functions are thread-safe. Write transmission is
 * delegated to hmi_writes (this module only moves bytes and reports the
 * outcome). This module is the sole owner of "is the PLC alive?".
 */
#ifndef HMI_COMMS_H
#define HMI_COMMS_H

#include "hmi_tags.h"
#include "hmi_driver.h"
#include "hmi_writes.h"

typedef enum {
    HMI_CONN_DISCONNECTED = 0,
    HMI_CONN_CONNECTING,
    HMI_CONN_CONNECTED,
    HMI_CONN_DEGRADED,
    HMI_CONN_BACKOFF
} hmi_conn_state_t;

typedef struct {
    uint32_t response_timeout_ms;
    uint32_t connect_timeout_ms;
    uint32_t miss_limit;        /* consecutive link misses before link is declared lost */
    uint32_t backoff_min_ms;
    uint32_t backoff_max_ms;
    /* Link-health thresholds on the age of the last valid reply. 0 selects
     * the defaults: degraded after 2 x response_timeout, no-response after
     * max(6 x response_timeout, 1000 ms). Strict ">": age == threshold is
     * still the better state. */
    uint32_t link_degraded_ms;
    uint32_t link_no_response_ms;
    uint32_t jitter_seed;
} hmi_comms_cfg_t;

typedef struct {
    uint32_t polls_ok, polls_failed, exceptions, protocol_errors;
    uint32_t connects_ok, connects_failed, links_lost;
    uint32_t last_poll_latency_ms, max_poll_latency_ms;
} hmi_comms_stats_t;

typedef struct {
    hmi_tagdb_t *db;
    hmi_plc_driver_t drv;
    hmi_comms_cfg_t cfg;
    hmi_writes_t *writes;          /* optional; NULL = read-only */
    uint64_t (*clock)(void *);
    void *clock_ctx;

    /* comms-thread private */
    hmi_conn_state_t state;
    uint32_t misses;
    uint32_t backoff_ms;
    uint32_t link_degraded_ms;     /* resolved thresholds */
    uint32_t link_no_response_ms;
    uint64_t next_attempt_ms;
    uint32_t rng;

    /* shared (guarded by lock) */
    hmi_mutex_t lock;
    hmi_conn_state_t pub_state;
    uint64_t last_ok_ms;           /* last valid reply of any kind (data or exception) */
    hmi_comms_stats_t stats;
    bool ready;
} hmi_comms_t;

bool hmi_comms_init(hmi_comms_t *c, hmi_tagdb_t *db, const hmi_plc_driver_t *drv,
                    const hmi_comms_cfg_t *cfg);
void hmi_comms_destroy(hmi_comms_t *c);
void hmi_comms_set_clock(hmi_comms_t *c, uint64_t (*clock)(void *), void *ctx);
/* Attach the write pipeline. Must be done before the first step. */
void hmi_comms_set_writes(hmi_comms_t *c, hmi_writes_t *w);

/* Performs at most one blocking driver operation. Returns the suggested
 * sleep (ms) until the next useful call. */
uint32_t hmi_comms_step(hmi_comms_t *c);

/* Thread-safe queries */
hmi_conn_state_t hmi_comms_state(hmi_comms_t *c);   /* TCP-level state machine */
void hmi_comms_stats(hmi_comms_t *c, hmi_comms_stats_t *out);
/* Monotonic time of the last valid PLC reply (diagnostics only; consumers
 * must use hmi_comms_link_health instead of deriving their own threshold). */
uint64_t hmi_comms_last_ok_ms(hmi_comms_t *c);

/* THE link-health evaluation. Uses the comms clock. */
hmi_link_health_t hmi_comms_link_health(hmi_comms_t *c);
/* Same evaluation at an explicit time (tests, consumers with their own now). */
hmi_link_health_t hmi_comms_link_health_at(hmi_comms_t *c, uint64_t now_ms);
const char *hmi_link_health_text(hmi_link_health_t h);

#endif /* HMI_COMMS_H */
