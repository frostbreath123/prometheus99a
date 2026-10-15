#include "hmi_comms.h"
#include "hmi_decode.h"
#include <string.h>

/* ---- helpers ---- */

static uint64_t default_clock(void *ctx)
{
    (void)ctx;
    return hmi_now_ms();
}

static uint64_t now_ms(hmi_comms_t *c)
{
    return c->clock(c->clock_ctx);
}

static void set_state(hmi_comms_t *c, hmi_conn_state_t s)
{
    c->state = s;
    hmi_mutex_lock(&c->lock);
    c->pub_state = s;
    hmi_mutex_unlock(&c->lock);
}

static void bump(hmi_comms_t *c, uint32_t *counter)
{
    hmi_mutex_lock(&c->lock);
    (*counter)++;
    hmi_mutex_unlock(&c->lock);
}

static uint32_t jitter(hmi_comms_t *c, uint32_t base)
{
    c->rng = c->rng * 1664525u + 1013904223u;
    return (base / 4u) ? (c->rng >> 8) % (base / 4u) : 0u;
}

/* ---- init ---- */

bool hmi_comms_init(hmi_comms_t *c, hmi_tagdb_t *db, const hmi_plc_driver_t *drv,
                    const hmi_comms_cfg_t *cfg)
{
    memset(c, 0, sizeof(*c));
    if (!hmi_tagdb_is_ready(db) || !drv || !drv->connect || !drv->read_regs || !drv->close || !cfg) return false;
    if (cfg->response_timeout_ms == 0 || cfg->miss_limit == 0 || cfg->backoff_min_ms == 0 ||
        cfg->backoff_max_ms < cfg->backoff_min_ms)
        return false;
    c->db = db;
    c->drv = *drv;
    c->cfg = *cfg;
    c->clock = default_clock;
    c->state = HMI_CONN_DISCONNECTED;
    c->pub_state = HMI_CONN_DISCONNECTED;
    c->backoff_ms = cfg->backoff_min_ms;
    c->link_degraded_ms = cfg->link_degraded_ms ? cfg->link_degraded_ms : cfg->response_timeout_ms * 2u;
    c->link_no_response_ms = cfg->link_no_response_ms ? cfg->link_no_response_ms
                                                      : cfg->response_timeout_ms * 6u;
    if (!cfg->link_no_response_ms && c->link_no_response_ms < 1000u) c->link_no_response_ms = 1000u;
    if (c->link_degraded_ms > c->link_no_response_ms) return false; /* inconsistent thresholds */
    c->rng = cfg->jitter_seed ? cfg->jitter_seed : 1u;
    c->last_ok_ms = c->clock(c->clock_ctx); /* grace period from start */
    if (!hmi_mutex_init(&c->lock)) return false;
    c->ready = true;
    return true;
}

void hmi_comms_destroy(hmi_comms_t *c)
{
    if (c->ready) hmi_mutex_destroy(&c->lock);
    c->ready = false;
}

void hmi_comms_set_clock(hmi_comms_t *c, uint64_t (*clock)(void *), void *ctx)
{
    c->clock = clock ? clock : default_clock;
    c->clock_ctx = ctx;
}

void hmi_comms_set_writes(hmi_comms_t *c, hmi_writes_t *w)
{
    c->writes = w;
}

/* ---- queries ---- */

hmi_conn_state_t hmi_comms_state(hmi_comms_t *c)
{
    hmi_mutex_lock(&c->lock);
    hmi_conn_state_t s = c->pub_state;
    hmi_mutex_unlock(&c->lock);
    return s;
}

uint64_t hmi_comms_last_ok_ms(hmi_comms_t *c)
{
    hmi_mutex_lock(&c->lock);
    uint64_t t = c->last_ok_ms;
    hmi_mutex_unlock(&c->lock);
    return t;
}

hmi_link_health_t hmi_comms_link_health_at(hmi_comms_t *c, uint64_t now)
{
    hmi_mutex_lock(&c->lock);
    hmi_conn_state_t s = c->pub_state;
    uint64_t last = c->last_ok_ms;
    hmi_mutex_unlock(&c->lock);

    if (s != HMI_CONN_CONNECTED && s != HMI_CONN_DEGRADED) return HMI_LINK_DOWN;
    /* Unsigned subtraction is wrap-safe. If the clock stepped backwards the
     * difference is ~2^64, which exceeds every threshold: fails safe. */
    uint64_t age = now - last;
    if (age > c->link_no_response_ms) return HMI_LINK_NO_RESPONSE;
    if (s == HMI_CONN_DEGRADED || age > c->link_degraded_ms) return HMI_LINK_DEGRADED;
    return HMI_LINK_HEALTHY;
}

hmi_link_health_t hmi_comms_link_health(hmi_comms_t *c)
{
    return hmi_comms_link_health_at(c, now_ms(c));
}

const char *hmi_link_health_text(hmi_link_health_t h)
{
    switch (h) {
        case HMI_LINK_HEALTHY: return "HEALTHY";
        case HMI_LINK_DEGRADED: return "DEGRADED";
        case HMI_LINK_NO_RESPONSE: return "NO RESPONSE";
        default: return "DOWN";
    }
}

void hmi_comms_stats(hmi_comms_t *c, hmi_comms_stats_t *out)
{
    hmi_mutex_lock(&c->lock);
    *out = c->stats;
    hmi_mutex_unlock(&c->lock);
}

/* ---- link bookkeeping ---- */

static void expire_queued_writes(hmi_comms_t *c, uint64_t now, bool all)
{
    if (c->writes) hmi_writes_expire(c->writes, now, all);
}

static void link_miss(hmi_comms_t *c, uint64_t now)
{
    c->misses++;
    if (c->misses >= c->cfg.miss_limit) {
        c->drv.close(c->drv.ctx);
        hmi_tagdb_link_lost(c->db);
        expire_queued_writes(c, now, true); /* never transmitted: never sent later */
        bump(c, &c->stats.links_lost);
        c->next_attempt_ms = now + c->backoff_ms + jitter(c, c->backoff_ms);
        c->backoff_ms = c->backoff_ms * 2u > c->cfg.backoff_max_ms ? c->cfg.backoff_max_ms : c->backoff_ms * 2u;
        set_state(c, HMI_CONN_BACKOFF);
        return;
    }
    hmi_tagdb_degrade(c->db);
    set_state(c, HMI_CONN_DEGRADED);
    /* Probe immediately so dead-link detection is independent of group count. */
    hmi_tagdb_groups_set_all_due(c->db, now);
}

/* A valid reply of any kind (data or Modbus exception) proves the link. */
static void link_ok(hmi_comms_t *c)
{
    uint64_t t = now_ms(c);
    hmi_mutex_lock(&c->lock);
    c->last_ok_ms = t;
    hmi_mutex_unlock(&c->lock);
    c->misses = 0;
    if (c->state == HMI_CONN_DEGRADED) set_state(c, HMI_CONN_CONNECTED);
}

/* ---- one write (transport only; outcomes are recorded by hmi_writes) ---- */

static void execute_write(hmi_comms_t *c, const hmi_write_job_t *job)
{
    uint8_t exc = 0;
    hmi_io_status_t st = c->drv.write_regs(c->drv.ctx, job->address, job->n_regs, job->regs, &exc);
    uint64_t now = now_ms(c);

    if (st == HMI_IO_EXCEPTION) {
        hmi_writes_finish_exception(c->writes, job, exc, now);
        link_ok(c);
        return;
    }
    if (st != HMI_IO_OK) {
        /* May or may not have been applied. Never replay. */
        hmi_writes_finish_unknown(c->writes, job, 0, now);
        link_miss(c, now);
        return;
    }

    uint16_t back[2];
    st = c->drv.read_regs(c->drv.ctx, job->read_area, job->address, job->n_regs, back, &exc);
    now = now_ms(c);
    if (st != HMI_IO_OK) {
        hmi_writes_finish_unknown(c->writes, job, exc, now);
        if (st != HMI_IO_EXCEPTION) link_miss(c, now);
        return;
    }
    link_ok(c);
    hmi_writes_finish_verified(c->writes, job, back, now);
}

/* ---- one poll ---- */

static void execute_poll(hmi_comms_t *c, size_t g)
{
    const hmi_group_t *grp = hmi_tagdb_group(c->db, g);
    uint16_t regs[HMI_MAX_REGS_PER_READ];
    uint8_t exc = 0;
    uint64_t t0 = now_ms(c);
    hmi_io_status_t st = c->drv.read_regs(c->drv.ctx, grp->area, grp->start, grp->count, regs, &exc);
    uint64_t now = now_ms(c);

    /* phase-aligned reschedule, no cumulative drift, skips missed periods */
    uint64_t due = grp->next_due_ms + grp->period_ms;
    if (due <= now) {
        uint64_t missed = (now - due) / grp->period_ms + 1u;
        due += missed * grp->period_ms;
    }
    hmi_tagdb_group_set_due(c->db, g, due);

    hmi_mutex_lock(&c->lock);
    c->stats.last_poll_latency_ms = (uint32_t)(now - t0);
    if (c->stats.last_poll_latency_ms > c->stats.max_poll_latency_ms)
        c->stats.max_poll_latency_ms = c->stats.last_poll_latency_ms;
    hmi_mutex_unlock(&c->lock);

    switch (st) {
        case HMI_IO_OK:
            hmi_tagdb_group_ok(c->db, g, regs, now);
            bump(c, &c->stats.polls_ok);
            link_ok(c);
            break;
        case HMI_IO_EXCEPTION:
            /* Valid reply: the link is alive; only this group's data is bad. */
            hmi_tagdb_group_fail(c->db, g, HMI_REASON_EXCEPTION);
            bump(c, &c->stats.exceptions);
            bump(c, &c->stats.polls_failed);
            link_ok(c);
            break;
        case HMI_IO_PROTOCOL:
            hmi_tagdb_group_fail(c->db, g, HMI_REASON_PROTOCOL);
            bump(c, &c->stats.protocol_errors);
            bump(c, &c->stats.polls_failed);
            link_miss(c, now);
            break;
        default: /* timeout / transport */
            bump(c, &c->stats.polls_failed);
            link_miss(c, now);
            break;
    }
}

/* ---- scheduler ---- */

static uint32_t step_connect(hmi_comms_t *c)
{
    set_state(c, HMI_CONN_CONNECTING);
    hmi_io_status_t st = c->drv.connect(c->drv.ctx, c->cfg.connect_timeout_ms);
    uint64_t now = now_ms(c);
    if (st == HMI_IO_OK) {
        c->misses = 0;
        c->backoff_ms = c->cfg.backoff_min_ms;
        hmi_tagdb_groups_set_all_due(c->db, now);
        bump(c, &c->stats.connects_ok);
        set_state(c, HMI_CONN_CONNECTED);
    } else {
        c->drv.close(c->drv.ctx);
        bump(c, &c->stats.connects_failed);
        c->next_attempt_ms = now + c->backoff_ms + jitter(c, c->backoff_ms);
        c->backoff_ms = c->backoff_ms * 2u > c->cfg.backoff_max_ms ? c->cfg.backoff_max_ms : c->backoff_ms * 2u;
        set_state(c, HMI_CONN_BACKOFF);
    }
    return 0;
}

static uint32_t step_serve(hmi_comms_t *c, uint64_t now)
{
    /* 1. at most one queued write */
    hmi_write_job_t job;
    if (c->writes && hmi_writes_take(c->writes, now, &job)) {
        execute_write(c, &job);
        return 0;
    }
    /* 2. earliest due poll group */
    int best = -1;
    size_t n_groups = hmi_tagdb_group_count(c->db);
    for (size_t g = 0; g < n_groups; g++) {
        uint64_t due = hmi_tagdb_group(c->db, g)->next_due_ms;
        if (due <= now && (best < 0 || due < hmi_tagdb_group(c->db, (size_t)best)->next_due_ms))
            best = (int)g;
    }
    if (best >= 0) {
        execute_poll(c, (size_t)best);
        return 0;
    }
    uint64_t soonest = UINT64_MAX;
    for (size_t g = 0; g < n_groups; g++) {
        uint64_t due = hmi_tagdb_group(c->db, g)->next_due_ms;
        if (due < soonest) soonest = due;
    }
    return soonest > now ? (uint32_t)(soonest - now) : 0u;
}

uint32_t hmi_comms_step(hmi_comms_t *c)
{
    uint64_t now = now_ms(c);
    expire_queued_writes(c, now, false);

    switch (c->state) {
        case HMI_CONN_DISCONNECTED:
        case HMI_CONN_CONNECTING:
            return step_connect(c);
        case HMI_CONN_BACKOFF:
            if (now >= c->next_attempt_ms) {
                set_state(c, HMI_CONN_CONNECTING);
                return 0;
            }
            return (uint32_t)(c->next_attempt_ms - now);
        case HMI_CONN_CONNECTED:
        case HMI_CONN_DEGRADED:
            return step_serve(c, now);
    }
    return 10;
}
