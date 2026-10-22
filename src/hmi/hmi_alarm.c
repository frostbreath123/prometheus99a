#include "hmi_alarm.h"
#include <stdio.h>
#include <string.h>

static uint8_t rank_of(hmi_alarm_kind_t k)
{
    switch (k) {
        case HMI_ALK_LO:
        case HMI_ALK_HI: return 1;
        case HMI_ALK_LOLO:
        case HMI_ALK_HIHI:
        case HMI_ALK_COMMS: return 2;
        default: return 0;
    }
}

/* Raw (un-debounced) condition with deadband hysteresis relative to cur. */
static hmi_alarm_kind_t raw_kind(const hmi_alarm_cfg_t *c, double v, hmi_alarm_kind_t cur)
{
    double db = c->deadband;
    if (c->en_hihi && (v >= c->hihi || (cur == HMI_ALK_HIHI && v >= c->hihi - db))) return HMI_ALK_HIHI;
    if (c->en_lolo && (v <= c->lolo || (cur == HMI_ALK_LOLO && v <= c->lolo + db))) return HMI_ALK_LOLO;
    if (c->en_hi && (v >= c->hi || (cur == HMI_ALK_HI && v >= c->hi - db))) return HMI_ALK_HI;
    if (c->en_lo && (v <= c->lo || (cur == HMI_ALK_LO && v <= c->lo + db))) return HMI_ALK_LO;
    return HMI_ALK_NONE;
}

bool hmi_alarm_init(hmi_alarm_t *a, hmi_tagdb_t *db)
{
    memset(a, 0, sizeof(*a));
    if (!hmi_tagdb_is_ready(db)) return false;
    a->db = db;
    if (!hmi_mutex_init(&a->lock)) return false;
    a->ready = true;
    return true;
}

void hmi_alarm_destroy(hmi_alarm_t *a)
{
    if (a->ready) hmi_mutex_destroy(&a->lock);
    a->ready = false;
}

void hmi_alarm_set_sink(hmi_alarm_t *a, hmi_alarm_log_sink_t fn, void *user)
{
    hmi_mutex_lock(&a->lock);
    a->sink = fn;
    a->sink_user = user;
    hmi_mutex_unlock(&a->lock);
}

/* Caller holds lock. The entry goes into the in-memory journal and, if a sink
 * is installed, into a small queue that flush_sink() empties after the caller
 * has released the lock: the sink is never called under the alarm lock. */
static void log_event(hmi_alarm_t *a, size_t point, uint64_t now, hmi_alarm_event_t ev, double value)
{
    hmi_alarm_log_t *e = &a->journal[a->j_head];
    e->seq = ++a->seq;
    e->ts_ms = now;
    e->point = (uint8_t)point;
    e->kind = (uint8_t)a->pt[point].kind;
    e->event = (uint8_t)ev;
    e->state_after = (uint8_t)a->pt[point].state;
    e->value = value;
    a->j_head = (uint8_t)((a->j_head + 1) % HMI_JOURNAL_LEN);
    if (a->j_count < HMI_JOURNAL_LEN) a->j_count++;
    a->state_rev++;
    if (a->sink) {
        if (a->sq_count >= HMI_SINK_PENDING) a->sink_dropped++;
        else a->sink_q[(a->sq_head + a->sq_count++) % HMI_SINK_PENDING] = *e;
    }
}

/* Called without the lock. Hands queued entries to the sink in order. If another
 * thread is already doing so it will also take the entries queued meanwhile, so
 * the sink sees them in sequence and never concurrently. */
static void flush_sink(hmi_alarm_t *a)
{
    hmi_alarm_log_t batch[16];
    hmi_mutex_lock(&a->lock);
    if (a->sink_flushing || !a->sink) {
        hmi_mutex_unlock(&a->lock);
        return;
    }
    a->sink_flushing = true;
    for (;;) {
        size_t n = a->sq_count < 16u ? a->sq_count : 16u;
        if (n == 0) break;
        for (size_t i = 0; i < n; i++) batch[i] = a->sink_q[(a->sq_head + i) % HMI_SINK_PENDING];
        a->sq_head = (uint8_t)((a->sq_head + n) % HMI_SINK_PENDING);
        a->sq_count = (uint8_t)(a->sq_count - n);
        hmi_alarm_log_sink_t fn = a->sink;
        void *user = a->sink_user;
        hmi_mutex_unlock(&a->lock);
        uint32_t failed = 0;
        for (size_t i = 0; i < n; i++) {
            if (fn && !fn(user, &batch[i])) failed++;
        }
        hmi_mutex_lock(&a->lock);
        a->sink_failures += failed;
    }
    a->sink_flushing = false;
    hmi_mutex_unlock(&a->lock);
}

static void apply_kind(hmi_alarm_t *a, size_t p, hmi_alarm_kind_t k, uint64_t now, double v)
{
    hmi_alarm_point_t *pt = &a->pt[p];
    uint8_t new_rank = rank_of(k), old_rank = rank_of(pt->kind);

    switch (pt->state) {
        case HMI_AL_NORMAL:
            if (new_rank > 0) {
                pt->kind = k;
                pt->state = HMI_AL_ACTIVE_UNACK;
                pt->since_ms = now;
                log_event(a, p, now, HMI_ALE_RAISED, v);
            }
            break;
        case HMI_AL_ACTIVE_UNACK:
            if (new_rank == 0) {
                pt->state = HMI_AL_RTN_UNACK;
                pt->since_ms = now;
                log_event(a, p, now, HMI_ALE_RTN, v);
            } else if (k != pt->kind) {
                pt->kind = k;
                a->state_rev++; /* the persisted kind changed, with or without a journal entry */
                if (new_rank > old_rank) log_event(a, p, now, HMI_ALE_ESCALATED, v);
            }
            break;
        case HMI_AL_ACTIVE_ACK:
            if (new_rank == 0) {
                pt->state = HMI_AL_NORMAL;
                pt->since_ms = now;
                log_event(a, p, now, HMI_ALE_CLEARED, v);
                pt->kind = HMI_ALK_NONE;
                pt->acked_rank = 0;
            } else if (new_rank > pt->acked_rank) {
                pt->kind = k;
                pt->state = HMI_AL_ACTIVE_UNACK;
                pt->since_ms = now;
                log_event(a, p, now, HMI_ALE_ESCALATED, v);
            } else {
                if (pt->kind != k) a->state_rev++;
                pt->kind = k;
            }
            break;
        case HMI_AL_RTN_UNACK:
            if (new_rank > 0) {
                pt->kind = k;
                pt->state = HMI_AL_ACTIVE_UNACK;
                pt->since_ms = now;
                log_event(a, p, now, HMI_ALE_RAISED, v);
            }
            break;
    }
}

/* Debounce wrapper. want = raw condition now; returns kind to apply. */
static hmi_alarm_kind_t debounced(hmi_alarm_t *a, size_t p, hmi_alarm_kind_t want, uint64_t now,
                                  uint32_t on_ms, uint32_t off_ms)
{
    hmi_alarm_kind_t cur = a->pt[p].state == HMI_AL_RTN_UNACK ? HMI_ALK_NONE : a->pt[p].kind;
    uint8_t wr = rank_of(want), cr = rank_of(cur);
    if (want == cur) {
        a->cand_valid[p] = false;
        return cur;
    }
    uint32_t need = (wr > cr) ? on_ms : off_ms; /* raising/escalating vs relaxing */
    if (!a->cand_valid[p] || a->cand[p] != want) {
        a->cand_valid[p] = true;
        a->cand[p] = want;
        a->cand_since[p] = now;
    }
    if (now - a->cand_since[p] >= need) {
        a->cand_valid[p] = false;
        return want;
    }
    return cur;
}

static void note_ack_outcome(hmi_alarm_t *a, hmi_ack_result_t r);

void hmi_alarm_step(hmi_alarm_t *a, uint64_t now, hmi_link_health_t link)
{
    hmi_tag_value_t vals[HMI_MAX_TAGS];
    size_t n = hmi_tagdb_count(a->db);
    hmi_tagdb_snapshot(a->db, vals, HMI_MAX_TAGS);

    hmi_mutex_lock(&a->lock);
    for (size_t i = 0; i < n; i++) {
        const hmi_alarm_cfg_t *c = &hmi_tagdb_cfg(a->db, i)->alarm;
        if (!(c->en_hi || c->en_hihi || c->en_lo || c->en_lolo)) continue;
        if (vals[i].quality != HMI_Q_GOOD) {
            a->pt[i].data_suppressed = true;
            a->cand_valid[i] = false;
            continue;
        }
        a->pt[i].data_suppressed = false;
        hmi_alarm_kind_t cur = a->pt[i].state == HMI_AL_RTN_UNACK ? HMI_ALK_NONE : a->pt[i].kind;
        hmi_alarm_kind_t want = raw_kind(c, vals[i].eng, cur);
        hmi_alarm_kind_t k = debounced(a, i, want, now, c->on_ms, c->off_ms);
        if (k != cur || a->pt[i].state == HMI_AL_NORMAL || a->pt[i].state == HMI_AL_RTN_UNACK ||
            a->pt[i].state == HMI_AL_ACTIVE_ACK || a->pt[i].state == HMI_AL_ACTIVE_UNACK)
            apply_kind(a, i, k, now, vals[i].eng);
    }
    /* System alarm: PLC link lost (1 s on, 0 off). */
    size_t sp = n;
    hmi_alarm_kind_t want = hmi_link_health_is_lost(link) ? HMI_ALK_COMMS : HMI_ALK_NONE;
    hmi_alarm_kind_t cur = a->pt[sp].state == HMI_AL_RTN_UNACK ? HMI_ALK_NONE : a->pt[sp].kind;
    hmi_alarm_kind_t k = debounced(a, sp, want, now, 1000u, 0u);
    (void)cur;
    apply_kind(a, sp, k, now, 0.0);
    /* An acknowledgement whose audit record never completed is dropped, not applied. */
    for (int i = 0; i < HMI_ACK_PENDING_MAX; i++) {
        hmi_ack_pending_t *p = &a->pend[i];
        if (p->used && now - p->t_start_ms > HMI_ACK_PENDING_TTL_MS) {
            p->used = false;
            a->ack_audit_failures++;
            note_ack_outcome(a, HMI_ACK_AUDIT_FAILED);
        }
    }
    hmi_mutex_unlock(&a->lock);
    flush_sink(a);
}

/* ---- acknowledgement ---- */

static bool awaiting_ack(const hmi_alarm_point_t *pt)
{
    return pt->state == HMI_AL_ACTIVE_UNACK || pt->state == HMI_AL_RTN_UNACK;
}

/* Caller holds the lock and has checked awaiting_ack(). */
static void ack_point_locked(hmi_alarm_t *a, size_t point, uint64_t now)
{
    hmi_alarm_point_t *pt = &a->pt[point];
    if (pt->state == HMI_AL_ACTIVE_UNACK) {
        pt->state = HMI_AL_ACTIVE_ACK;
        pt->acked_rank = rank_of(pt->kind);
    } else { /* HMI_AL_RTN_UNACK */
        pt->state = HMI_AL_NORMAL;
        pt->kind = HMI_ALK_NONE;
        pt->acked_rank = 0;
    }
    pt->since_ms = now;
    log_event(a, point, now, HMI_ALE_ACKED, 0.0);
}

void hmi_alarm_set_ack_audit(hmi_alarm_t *a, hmi_ack_audit_fn fn, void *user)
{
    hmi_mutex_lock(&a->lock);
    a->ack_audit = fn;
    a->ack_audit_user = user;
    hmi_mutex_unlock(&a->lock);
}

void hmi_alarm_set_ack_audit_async(hmi_alarm_t *a, hmi_ack_audit_submit_fn fn, void *user)
{
    hmi_mutex_lock(&a->lock);
    a->ack_submit = fn;
    a->ack_submit_user = user;
    hmi_mutex_unlock(&a->lock);
}

/* Caller holds the lock. Acknowledges only the exact alarms the operator was
 * shown and the audit record named. Returns how many were acknowledged. */
static size_t apply_ack_locked(hmi_alarm_t *a, const hmi_ack_set_t *set, uint64_t now)
{
    size_t acked = 0;
    for (size_t i = 0; i < set->n; i++) {
        size_t p = set->point[i];
        const hmi_alarm_point_t *pt = &a->pt[p];
        if (pt->state != set->state[i] || pt->kind != set->kind[i] || pt->since_ms != set->since_ms[i]) continue;
        ack_point_locked(a, p, now);
        acked++;
    }
    return acked;
}

/* Caller holds the lock. */
static void note_ack_outcome(hmi_alarm_t *a, hmi_ack_result_t r)
{
    a->last_ack_result = r;
    a->last_ack_seq++;
}

void hmi_alarm_ack_audit_done(hmi_alarm_t *a, uint32_t token, bool ok, uint64_t now)
{
    hmi_mutex_lock(&a->lock);
    for (int i = 0; i < HMI_ACK_PENDING_MAX; i++) {
        hmi_ack_pending_t *p = &a->pend[i];
        if (!p->used || p->token != token) continue;
        p->used = false;
        if (!ok) {
            a->ack_audit_failures++;
            note_ack_outcome(a, HMI_ACK_AUDIT_FAILED);
        } else {
            note_ack_outcome(a, apply_ack_locked(a, &p->set, now) ? HMI_ACK_OK : HMI_ACK_NOTHING);
        }
        break;
    }
    hmi_mutex_unlock(&a->lock);
    flush_sink(a);
}

uint32_t hmi_alarm_last_ack(hmi_alarm_t *a, hmi_ack_result_t *result)
{
    hmi_mutex_lock(&a->lock);
    uint32_t seq = a->last_ack_seq;
    if (result) *result = a->last_ack_result;
    hmi_mutex_unlock(&a->lock);
    return seq;
}

static hmi_ack_result_t ack_request(hmi_alarm_t *a, bool all, size_t point, uint64_t now, hmi_role_t role,
                                    const char *actor)
{
    hmi_ack_set_t set;
    hmi_ack_audit_fn audit;
    void *audit_user;
    hmi_ack_audit_submit_fn submit;
    void *submit_user;
    set.n = 0;

    hmi_mutex_lock(&a->lock);
    size_t last = hmi_tagdb_count(a->db);
    for (size_t i = all ? 0 : point; i <= (all ? last : point) && i <= last; i++) {
        if (!awaiting_ack(&a->pt[i])) continue;
        set.point[set.n] = (uint8_t)i;
        set.state[set.n] = a->pt[i].state;
        set.kind[set.n] = a->pt[i].kind;
        set.since_ms[set.n] = a->pt[i].since_ms;
        set.n++;
    }
    audit = a->ack_audit;
    audit_user = a->ack_audit_user;
    submit = a->ack_submit;
    submit_user = a->ack_submit_user;
    hmi_mutex_unlock(&a->lock);

    bool allowed = hmi_authz_allows(role, HMI_ACT_ACK_ALARM, HMI_ROLE_VIEWER);
    if (allowed && set.n == 0) return HMI_ACK_NOTHING; /* no privileged effect: nothing to record */
    /* A denied single-point attempt is recorded with the alarm it named (if valid). */
    if (!allowed && set.n == 0 && !all && point <= last) set.point[set.n++] = (uint8_t)point;

    hmi_ack_audit_t rec;
    rec.t_ms = now;
    rec.role = role;
    hmi_actor_copy(rec.actor, actor);
    rec.all = all;
    rec.result = allowed ? HMI_ACK_OK : HMI_ACK_DENIED;
    rec.n_points = set.n;
    for (size_t i = 0; i < set.n; i++) rec.points[i] = set.point[i];

    if (!allowed) {
        /* Best effort: the request is refused whether or not this is recorded. */
        bool recorded = submit ? submit(submit_user, &rec, 0u) : (audit ? audit(audit_user, &rec) : true);
        if (!recorded) {
            hmi_mutex_lock(&a->lock);
            a->ack_audit_failures++;
            hmi_mutex_unlock(&a->lock);
        }
        return HMI_ACK_DENIED;
    }

    if (submit) {
        /* Two-phase: take a pending slot, queue the record, return at once. The
         * acknowledgement is applied by hmi_alarm_ack_audit_done(). */
        uint32_t token = 0;
        hmi_mutex_lock(&a->lock);
        for (int i = 0; i < HMI_ACK_PENDING_MAX; i++) {
            hmi_ack_pending_t *p = &a->pend[i];
            if (p->used) continue;
            if (++a->next_token == 0) a->next_token = 1; /* 0 means "no completion" */
            token = a->next_token;
            p->used = true;
            p->token = token;
            p->t_start_ms = now;
            p->set = set;
            break;
        }
        if (token == 0) a->ack_audit_failures++; /* every pending slot is busy: refuse */
        hmi_mutex_unlock(&a->lock);
        if (token == 0) return HMI_ACK_AUDIT_FAILED;
        if (!submit(submit_user, &rec, token)) {
            hmi_mutex_lock(&a->lock);
            for (int i = 0; i < HMI_ACK_PENDING_MAX; i++) {
                if (a->pend[i].used && a->pend[i].token == token) a->pend[i].used = false;
            }
            a->ack_audit_failures++;
            hmi_mutex_unlock(&a->lock);
            return HMI_ACK_AUDIT_FAILED;
        }
        return HMI_ACK_PENDING;
    }

    /* Audit before effect; no lock is held while the hook touches storage. */
    if (audit && !audit(audit_user, &rec)) {
        hmi_mutex_lock(&a->lock);
        a->ack_audit_failures++;
        hmi_mutex_unlock(&a->lock);
        return HMI_ACK_AUDIT_FAILED;
    }

    hmi_mutex_lock(&a->lock);
    size_t acked = apply_ack_locked(a, &set, now); /* only the exact alarms the operator was shown and we recorded */
    hmi_mutex_unlock(&a->lock);
    flush_sink(a);
    return acked ? HMI_ACK_OK : HMI_ACK_NOTHING;
}

hmi_ack_result_t hmi_alarm_ack(hmi_alarm_t *a, size_t point, uint64_t now, hmi_role_t role)
{
    return ack_request(a, false, point, now, role, NULL);
}

hmi_ack_result_t hmi_alarm_ack_all(hmi_alarm_t *a, uint64_t now, hmi_role_t role)
{
    return ack_request(a, true, 0, now, role, NULL);
}

hmi_ack_result_t hmi_alarm_ack_as(hmi_alarm_t *a, size_t point, uint64_t now, hmi_role_t role, const char *actor)
{
    return ack_request(a, false, point, now, role, actor);
}

hmi_ack_result_t hmi_alarm_ack_all_as(hmi_alarm_t *a, uint64_t now, hmi_role_t role, const char *actor)
{
    return ack_request(a, true, 0, now, role, actor);
}

void hmi_alarm_get(hmi_alarm_t *a, size_t point, hmi_alarm_point_t *out)
{
    hmi_mutex_lock(&a->lock);
    *out = a->pt[point <= HMI_MAX_TAGS ? point : HMI_MAX_TAGS];
    hmi_mutex_unlock(&a->lock);
}

size_t hmi_alarm_journal_copy(hmi_alarm_t *a, hmi_alarm_log_t *out, size_t cap)
{
    hmi_mutex_lock(&a->lock);
    size_t n = a->j_count < cap ? a->j_count : cap;
    size_t start = (size_t)(a->j_head + HMI_JOURNAL_LEN - a->j_count) % HMI_JOURNAL_LEN;
    /* if cap smaller than count, return the newest n entries */
    start = (start + (a->j_count - n)) % HMI_JOURNAL_LEN;
    for (size_t i = 0; i < n; i++) out[i] = a->journal[(start + i) % HMI_JOURNAL_LEN];
    hmi_mutex_unlock(&a->lock);
    return n;
}

size_t hmi_alarm_count_unacked(hmi_alarm_t *a)
{
    size_t n = 0;
    hmi_mutex_lock(&a->lock);
    for (size_t i = 0; i <= hmi_tagdb_count(a->db); i++) {
        if (a->pt[i].state == HMI_AL_ACTIVE_UNACK || a->pt[i].state == HMI_AL_RTN_UNACK) n++;
    }
    hmi_mutex_unlock(&a->lock);
    return n;
}

/* ---- persistence ---- */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_points;
    uint32_t crc;
    uint8_t state[HMI_MAX_TAGS + 1];
    uint8_t kind[HMI_MAX_TAGS + 1];
    uint8_t acked_rank[HMI_MAX_TAGS + 1];
} alarm_blob_t;

static uint32_t crc32_calc(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

typedef char hmi_alarm_state_size_check[(sizeof(alarm_blob_t) <= HMI_ALARM_STATE_MAX) ? 1 : -1];

size_t hmi_alarm_state_serialize(hmi_alarm_t *a, void *buf, size_t cap, uint32_t *rev)
{
    alarm_blob_t b;
    if (cap < sizeof(b)) return 0;
    memset(&b, 0, sizeof(b));
    hmi_mutex_lock(&a->lock);
    b.magic = 0x504D4C41u; /* 'ALMP' */
    b.version = 1;
    b.n_points = (uint32_t)hmi_tagdb_count(a->db) + 1u;
    for (size_t i = 0; i < b.n_points; i++) {
        b.state[i] = (uint8_t)a->pt[i].state;
        b.kind[i] = (uint8_t)a->pt[i].kind;
        b.acked_rank[i] = a->pt[i].acked_rank;
    }
    if (rev) *rev = a->state_rev;
    hmi_mutex_unlock(&a->lock);
    b.crc = 0;
    b.crc = crc32_calc((const uint8_t *)&b, sizeof(b));
    memcpy(buf, &b, sizeof(b));
    return sizeof(b);
}

uint32_t hmi_alarm_state_rev(hmi_alarm_t *a)
{
    hmi_mutex_lock(&a->lock);
    uint32_t r = a->state_rev;
    hmi_mutex_unlock(&a->lock);
    return r;
}

bool hmi_alarm_state_apply(hmi_alarm_t *a, const void *buf, size_t len)
{
    alarm_blob_t b;
    if (len != sizeof(b)) return false;
    memcpy(&b, buf, sizeof(b));
    uint32_t crc = b.crc;
    b.crc = 0;
    if (crc32_calc((const uint8_t *)&b, sizeof(b)) != crc) return false;
    if (b.magic != 0x504D4C41u || b.version != 1 || b.n_points != (uint32_t)hmi_tagdb_count(a->db) + 1u) return false;
    for (size_t i = 0; i < b.n_points; i++) {
        if (b.state[i] > HMI_AL_RTN_UNACK || b.kind[i] > HMI_ALK_COMMS || b.acked_rank[i] > 2) return false;
    }
    hmi_mutex_lock(&a->lock);
    for (size_t i = 0; i < b.n_points; i++) {
        a->pt[i].state = (hmi_alarm_state_t)b.state[i];
        a->pt[i].kind = (hmi_alarm_kind_t)b.kind[i];
        a->pt[i].acked_rank = b.acked_rank[i];
    }
    a->state_rev++;
    hmi_mutex_unlock(&a->lock);
    return true;
}

bool hmi_alarm_save(hmi_alarm_t *a, const char *path)
{
    uint8_t buf[HMI_ALARM_STATE_MAX];
    size_t n = hmi_alarm_state_serialize(a, buf, sizeof(buf), NULL);
    return n > 0 && hmi_fs_atomic_write(path, buf, n);
}

bool hmi_alarm_load(hmi_alarm_t *a, const char *path)
{
    uint8_t buf[HMI_ALARM_STATE_MAX];
    size_t len = 0;
    if (hmi_fs_read_all(path, buf, sizeof(buf), &len) != HMI_FS_READ_OK) return false; /* absent, unreadable or too long */
    return hmi_alarm_state_apply(a, buf, len);
}

void hmi_alarm_journal_restore(hmi_alarm_t *a, const hmi_alarm_log_t *ev, size_t n)
{
    hmi_mutex_lock(&a->lock);
    if (n > HMI_JOURNAL_LEN) {
        ev += n - HMI_JOURNAL_LEN; /* keep the newest */
        n = HMI_JOURNAL_LEN;
    }
    a->j_head = 0;
    a->j_count = 0;
    for (size_t i = 0; i < n; i++) {
        a->journal[a->j_head] = ev[i];
        a->j_head = (uint8_t)((a->j_head + 1) % HMI_JOURNAL_LEN);
        a->j_count++;
        if (ev[i].seq > a->seq) a->seq = ev[i].seq;
    }
    hmi_mutex_unlock(&a->lock);
}
