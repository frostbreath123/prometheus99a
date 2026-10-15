#include "hmi_writes.h"
#include "hmi_authz.h"
#include "hmi_decode.h"
#include <math.h>
#include <string.h>

/* The queue can never hold more unresolved requests than the audit ring has
 * slots (+1 for the one in flight), so "audit ring exhausted" is a defensive
 * state, not a reachable steady-state one. */
typedef char hmi_audit_capacity_check[(HMI_AUDIT_LEN >= HMI_WRITE_QUEUE_LEN + 2) ? 1 : -1];

static bool is_final(hmi_wr_state_t s)
{
    return s == HMI_WR_CONFIRMED || s == HMI_WR_REJECTED || s == HMI_WR_EXPIRED ||
           s == HMI_WR_UNKNOWN || s == HMI_WR_VERIFY_FAILED;
}

static const char *state_event(hmi_wr_state_t s)
{
    switch (s) {
        case HMI_WR_CONFIRMED: return "CONFIRMED";
        case HMI_WR_REJECTED: return "REJECTED";
        case HMI_WR_EXPIRED: return "EXPIRED";
        case HMI_WR_UNKNOWN: return "UNKNOWN";
        case HMI_WR_VERIFY_FAILED: return "VERIFY_FAILED";
        case HMI_WR_SENT: return "SENT";
        case HMI_WR_PENDING: return "PENDING";
        default: return "?";
    }
}

bool hmi_writes_init(hmi_writes_t *w, const hmi_tagdb_t *db, bool enabled, uint32_t ttl_ms)
{
    memset(w, 0, sizeof(*w));
    if (!hmi_tagdb_is_ready(db) || ttl_ms == 0) return false;
    w->db = db;
    w->enabled = enabled;
    w->ttl_ms = ttl_ms;
    w->next_id = 1;
    if (!hmi_mutex_init(&w->lock)) return false;
    w->ready = true;
    return true;
}

void hmi_writes_destroy(hmi_writes_t *w)
{
    if (w->ready) hmi_mutex_destroy(&w->lock);
    w->ready = false;
}

void hmi_writes_set_audit(hmi_writes_t *w, hmi_audit_fn fn, void *user)
{
    hmi_mutex_lock(&w->lock);
    w->audit = fn;
    w->audit_user = user;
    hmi_mutex_unlock(&w->lock);
}

void hmi_writes_set_enabled(hmi_writes_t *w, bool enabled)
{
    hmi_mutex_lock(&w->lock);
    w->enabled = enabled;
    hmi_mutex_unlock(&w->lock);
}

void hmi_writes_stats(hmi_writes_t *w, hmi_writes_stats_t *out)
{
    hmi_mutex_lock(&w->lock);
    *out = w->stats;
    hmi_mutex_unlock(&w->lock);
}

/* Audit notification outside the lock; counts a failure. */
static bool audit_call(hmi_writes_t *w, const hmi_write_rec_t *rec, const char *ev)
{
    hmi_audit_fn fn;
    void *user;
    hmi_mutex_lock(&w->lock);
    fn = w->audit;
    user = w->audit_user;
    hmi_mutex_unlock(&w->lock);
    if (!fn) return true;
    if (fn(user, rec, ev)) return true;
    hmi_mutex_lock(&w->lock);
    w->stats.audit_failures++;
    hmi_mutex_unlock(&w->lock);
    return false;
}

/* Set a final state; returns a copy of the record for the audit call. */
static hmi_write_rec_t finalize(hmi_writes_t *w, size_t slot, hmi_wr_state_t st, uint64_t now, uint8_t exc,
                                uint32_t readback)
{
    hmi_write_rec_t copy;
    hmi_mutex_lock(&w->lock);
    hmi_write_rec_t *r = &w->ring[slot];
    r->state = st;
    r->t_final_ms = now;
    r->exc = exc;
    r->readback_raw = readback;
    switch (st) {
        case HMI_WR_CONFIRMED: w->stats.confirmed++; break;
        case HMI_WR_REJECTED: w->stats.rejected++; break;
        case HMI_WR_EXPIRED: w->stats.expired++; break;
        case HMI_WR_UNKNOWN: w->stats.unknown++; break;
        case HMI_WR_VERIFY_FAILED: w->stats.verify_failed++; break;
        default: break;
    }
    copy = *r;
    hmi_mutex_unlock(&w->lock);
    return copy;
}

/* Drop non-PENDING entries from the FIFO. Caller holds lock. */
static void compact_queue(hmi_writes_t *w)
{
    uint8_t tmp[HMI_WRITE_QUEUE_LEN];
    uint8_t n = 0;
    for (uint8_t i = 0; i < w->q_len; i++) {
        uint8_t idx = w->queue[(w->q_head + i) % HMI_WRITE_QUEUE_LEN];
        if (w->ring[idx].state == HMI_WR_PENDING) tmp[n++] = idx;
    }
    memcpy(w->queue, tmp, n);
    w->q_head = 0;
    w->q_len = n;
}

hmi_wr_submit_t hmi_writes_submit(hmi_writes_t *w, size_t tag, double eng, hmi_role_t role,
                                  hmi_link_health_t link, uint64_t now_ms, uint32_t *out_id)
{
    hmi_mutex_lock(&w->lock);
    bool enabled = w->enabled;
    hmi_mutex_unlock(&w->lock);
    if (!enabled) return HMI_WRS_ERR_DISABLED;
    if (tag >= hmi_tagdb_count(w->db)) return HMI_WRS_ERR_NO_TAG;
    const hmi_tag_cfg_t *t = hmi_tagdb_cfg(w->db, tag);
    if (!t->write_allowed || t->area != HMI_AREA_HOLDING) return HMI_WRS_ERR_NOT_WRITABLE;
    if (!hmi_authz_allows(role, HMI_ACT_WRITE_TAG, t->write_role)) return HMI_WRS_ERR_ROLE;
    if (isnan(eng) || isinf(eng) || eng < t->write_min || eng > t->write_max) return HMI_WRS_ERR_RANGE;

    double num = (eng - t->offset) / t->scale;
    if (t->dtype != HMI_DT_F32) num = floor(num + 0.5);
    uint32_t raw;
    if (!hmi_number_to_raw(num, t->dtype, &raw)) return HMI_WRS_ERR_NOT_REPRESENTABLE;
    if (hmi_link_health_is_lost(link)) return HMI_WRS_ERR_LINK_DOWN;

    hmi_mutex_lock(&w->lock);
    if (w->q_len >= HMI_WRITE_QUEUE_LEN) {
        hmi_mutex_unlock(&w->lock);
        return HMI_WRS_ERR_QUEUE_FULL;
    }
    /* Reserve an audit slot: FREE, else the oldest final record. */
    int slot = -1;
    uint32_t best_id = 0;
    for (int i = 0; i < HMI_AUDIT_LEN; i++) {
        hmi_write_rec_t *r = &w->ring[i];
        if (r->state == HMI_WR_FREE) {
            slot = i;
            break;
        }
        if (is_final(r->state) && (slot < 0 || r->id < best_id)) {
            slot = i;
            best_id = r->id;
        }
    }
    if (slot < 0) {
        hmi_mutex_unlock(&w->lock);
        return HMI_WRS_ERR_AUDIT; /* every record unresolved: refuse (fail closed) */
    }
    hmi_write_rec_t *r = &w->ring[slot];
    memset(r, 0, sizeof(*r));
    r->id = w->next_id++;
    r->tag = (uint8_t)tag;
    r->state = HMI_WR_RESERVED;
    r->role = role;
    r->eng = eng;
    r->raw = raw;
    r->t_submit_ms = now_ms;
    hmi_write_rec_t copy = *r;
    hmi_mutex_unlock(&w->lock);

    if (!audit_call(w, &copy, "RESERVED")) {
        hmi_mutex_lock(&w->lock);
        w->ring[slot].state = HMI_WR_FREE;
        hmi_mutex_unlock(&w->lock);
        return HMI_WRS_ERR_AUDIT;
    }

    hmi_mutex_lock(&w->lock);
    w->ring[slot].state = HMI_WR_PENDING;
    w->queue[(w->q_head + w->q_len) % HMI_WRITE_QUEUE_LEN] = (uint8_t)slot;
    w->q_len++;
    w->stats.submitted++;
    hmi_mutex_unlock(&w->lock);

    if (out_id) *out_id = copy.id;
    return HMI_WRS_OK;
}

bool hmi_writes_status(hmi_writes_t *w, uint32_t id, hmi_write_rec_t *out)
{
    bool found = false;
    hmi_mutex_lock(&w->lock);
    for (int i = 0; i < HMI_AUDIT_LEN; i++) {
        if (w->ring[i].state != HMI_WR_FREE && w->ring[i].id == id) {
            *out = w->ring[i];
            found = true;
            break;
        }
    }
    hmi_mutex_unlock(&w->lock);
    return found;
}

bool hmi_writes_take(hmi_writes_t *w, uint64_t now_ms, hmi_write_job_t *job)
{
    for (;;) {
        int slot = -1;
        hmi_write_rec_t rec;
        hmi_mutex_lock(&w->lock);
        while (w->q_len > 0) {
            uint8_t idx = w->queue[w->q_head];
            w->q_head = (uint8_t)((w->q_head + 1) % HMI_WRITE_QUEUE_LEN);
            w->q_len--;
            if (w->ring[idx].state == HMI_WR_PENDING) {
                slot = idx;
                break;
            }
        }
        if (slot >= 0) {
            w->ring[slot].state = HMI_WR_SENT;
            rec = w->ring[slot];
        }
        hmi_mutex_unlock(&w->lock);
        if (slot < 0) return false;

        /* Record intent durably BEFORE it can be transmitted. */
        if (!audit_call(w, &rec, "SENT")) {
            hmi_write_rec_t f = finalize(w, (size_t)slot, HMI_WR_REJECTED, now_ms, 0, 0);
            audit_call(w, &f, "REJECTED");
            continue; /* never transmitted; look at the next request */
        }

        const hmi_tag_cfg_t *t = hmi_tagdb_cfg(w->db, rec.tag);
        job->slot = (uint8_t)slot;
        job->tag = rec.tag;
        job->address = t->address;
        job->n_regs = (uint16_t)hmi_dtype_regs(t->dtype);
        job->read_area = t->area;
        hmi_raw_to_regs(rec.raw, t->dtype, t->word_order, t->byte_order, job->regs);
        return true;
    }
}

void hmi_writes_finish_exception(hmi_writes_t *w, const hmi_write_job_t *job, uint8_t exc, uint64_t now_ms)
{
    hmi_write_rec_t f = finalize(w, job->slot, HMI_WR_REJECTED, now_ms, exc, 0);
    audit_call(w, &f, "REJECTED");
}

void hmi_writes_finish_unknown(hmi_writes_t *w, const hmi_write_job_t *job, uint8_t exc, uint64_t now_ms)
{
    hmi_write_rec_t f = finalize(w, job->slot, HMI_WR_UNKNOWN, now_ms, exc, 0);
    audit_call(w, &f, "UNKNOWN");
}

void hmi_writes_finish_verified(hmi_writes_t *w, const hmi_write_job_t *job, const uint16_t *readback_regs,
                                uint64_t now_ms)
{
    const hmi_tag_cfg_t *t = hmi_tagdb_cfg(w->db, job->tag);
    uint32_t back = hmi_regs_to_raw(readback_regs, t->dtype, t->word_order, t->byte_order);
    hmi_write_rec_t cur;
    hmi_mutex_lock(&w->lock);
    cur = w->ring[job->slot];
    hmi_mutex_unlock(&w->lock);
    bool match = (job->n_regs == 1u) ? ((back & 0xFFFFu) == (cur.raw & 0xFFFFu)) : (back == cur.raw);
    hmi_write_rec_t f = finalize(w, job->slot, match ? HMI_WR_CONFIRMED : HMI_WR_VERIFY_FAILED, now_ms, 0, back);
    audit_call(w, &f, state_event(f.state));
}

void hmi_writes_expire(hmi_writes_t *w, uint64_t now_ms, bool all)
{
    hmi_write_rec_t done[HMI_WRITE_QUEUE_LEN];
    uint8_t n = 0;
    hmi_mutex_lock(&w->lock);
    for (uint8_t i = 0; i < w->q_len && n < HMI_WRITE_QUEUE_LEN; i++) {
        uint8_t idx = w->queue[(w->q_head + i) % HMI_WRITE_QUEUE_LEN];
        hmi_write_rec_t *r = &w->ring[idx];
        if (r->state == HMI_WR_PENDING && (all || now_ms - r->t_submit_ms > w->ttl_ms)) {
            r->state = HMI_WR_EXPIRED;
            r->t_final_ms = now_ms;
            w->stats.expired++;
            done[n++] = *r;
        }
    }
    if (n) compact_queue(w);
    hmi_mutex_unlock(&w->lock);
    for (uint8_t i = 0; i < n; i++) audit_call(w, &done[i], "EXPIRED");
}
