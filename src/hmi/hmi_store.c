#include "hmi_store.h"
#include <stdio.h>
#include <string.h>

typedef char hmi_store_results_check[(HMI_STORE_RESULTS >= 2 * HMI_STORE_QUEUE_LEN) ? 1 : -1];

static const hmi_store_io_t s_platform_io = { hmi_fs_append_sync, hmi_fs_atomic_write, hmi_fs_size, hmi_fs_rename };

bool hmi_store_init(hmi_store_t *s, const hmi_store_cfg_t *cfg)
{
    memset(s, 0, sizeof(*s));
    if (!cfg) return false;
    unsigned used = 0;
    for (unsigned i = 0; i < HMI_STORE_TARGETS; i++) {
        if (!cfg->path[i] || cfg->path[i][0] == '\0') continue;
        if (cfg->kind[i] != HMI_STORE_APPEND && cfg->kind[i] != HMI_STORE_REPLACE) return false;
        if (strlen(cfg->path[i]) + 3u >= HMI_STORE_PATH_MAX) return false; /* room for ".N" and the terminator */
        snprintf(s->path[i], sizeof(s->path[i]), "%s", cfg->path[i]);
        used++;
    }
    if (used == 0) return false;
    if (cfg->max_bytes != 0 && (cfg->generations < 1u || cfg->generations > 9u)) return false;
    s->cfg = *cfg;
    for (unsigned i = 0; i < HMI_STORE_TARGETS; i++) s->cfg.path[i] = s->path[i];
    s->io = cfg->io ? *cfg->io : s_platform_io;
    if (!s->io.append || !s->io.replace || !s->io.size || !s->io.rename) return false;
    s->cfg.io = &s->io;
    s->next_ticket = 1;
    if (!hmi_mutex_init(&s->lock)) return false;
    if (!hmi_event_init(&s->wake)) {
        hmi_mutex_destroy(&s->lock);
        return false;
    }
    if (!hmi_event_init(&s->done)) {
        hmi_event_destroy(&s->wake);
        hmi_mutex_destroy(&s->lock);
        return false;
    }
    s->ready = true;
    return true;
}

void hmi_store_destroy(hmi_store_t *s)
{
    if (!s->ready) return;
    hmi_event_destroy(&s->done);
    hmi_event_destroy(&s->wake);
    hmi_mutex_destroy(&s->lock);
    s->ready = false;
}

static void set_result(hmi_store_t *s, uint32_t ticket, hmi_store_status_t st)
{
    hmi_store_result_t *r = &s->res[ticket % HMI_STORE_RESULTS];
    r->ticket = ticket;
    r->status = (uint8_t)st;
}

bool hmi_store_enqueue(hmi_store_t *s, unsigned target, const void *data, size_t len, hmi_store_done_fn done,
                       void *user, uint32_t arg, uint32_t *ticket)
{
    if (!s->ready || target >= HMI_STORE_TARGETS || s->path[target][0] == '\0') return false;
    if (!data || len == 0 || len > HMI_STORE_REC_MAX) return false;
    hmi_store_kind_t kind = s->cfg.kind[target];

    hmi_mutex_lock(&s->lock);
    if (hmi_flag_get(&s->stop)) {
        s->stats.refused++;
        hmi_mutex_unlock(&s->lock);
        return false;
    }
    if (kind == HMI_STORE_REPLACE) {
        /* The newest content wins: replace a request that has not been taken
         * yet. It keeps its place in the queue and its ticket. */
        for (uint8_t i = 0; i < s->q_len; i++) {
            hmi_store_item_t *it = &s->q[(s->q_head + i) % HMI_STORE_QUEUE_LEN];
            if (it->target != target) continue;
            memcpy(it->data, data, len);
            it->len = (uint16_t)len;
            it->done = done;
            it->user = user;
            it->arg = arg;
            if (ticket) *ticket = it->ticket;
            hmi_mutex_unlock(&s->lock);
            return true;
        }
    }
    if (s->q_len >= HMI_STORE_QUEUE_LEN) {
        s->stats.refused++;
        hmi_mutex_unlock(&s->lock);
        return false;
    }
    hmi_store_item_t *it = &s->q[(s->q_head + s->q_len) % HMI_STORE_QUEUE_LEN];
    it->target = (uint8_t)target;
    it->kind = (uint8_t)kind;
    it->len = (uint16_t)len;
    it->ticket = s->next_ticket++;
    if (s->next_ticket == 0) s->next_ticket = 1; /* 0 is never a ticket */
    it->done = done;
    it->user = user;
    it->arg = arg;
    memcpy(it->data, data, len);
    set_result(s, it->ticket, HMI_STORE_PENDING);
    s->q_len++;
    s->stats.enqueued++;
    if (s->q_len > s->stats.max_depth) s->stats.max_depth = s->q_len;
    if (ticket) *ticket = it->ticket;
    hmi_mutex_unlock(&s->lock);
    hmi_event_signal(&s->wake);
    return true;
}

hmi_store_status_t hmi_store_ticket(hmi_store_t *s, uint32_t ticket)
{
    hmi_store_status_t st = HMI_STORE_UNKNOWN;
    hmi_mutex_lock(&s->lock);
    const hmi_store_result_t *r = &s->res[ticket % HMI_STORE_RESULTS];
    if (ticket != 0 && r->ticket == ticket) st = (hmi_store_status_t)r->status;
    hmi_mutex_unlock(&s->lock);
    return st;
}

hmi_store_status_t hmi_store_wait(hmi_store_t *s, uint32_t ticket, uint32_t timeout_ms)
{
    uint64_t deadline = hmi_now_ms() + timeout_ms;
    for (;;) {
        hmi_store_status_t st = hmi_store_ticket(s, ticket);
        if (st != HMI_STORE_PENDING) return st;
        uint64_t now = hmi_now_ms();
        if (now >= deadline) return HMI_STORE_PENDING;
        uint64_t left = deadline - now;
        hmi_event_wait(&s->done, left > 50u ? 50u : (uint32_t)left); /* re-check: signals can be missed or merged */
    }
}

bool hmi_store_flush(hmi_store_t *s, uint32_t timeout_ms)
{
    uint64_t deadline = hmi_now_ms() + timeout_ms;
    for (;;) {
        hmi_mutex_lock(&s->lock);
        bool idle = s->q_len == 0 && !s->busy;
        hmi_mutex_unlock(&s->lock);
        if (idle) return true;
        uint64_t now = hmi_now_ms();
        if (now >= deadline) return false;
        uint64_t left = deadline - now;
        hmi_event_wait(&s->done, left > 50u ? 50u : (uint32_t)left);
    }
}

/* ---- worker side ---- */

static bool rotate(hmi_store_t *s, unsigned target)
{
    const char *p = s->path[target];
    char from[HMI_STORE_PATH_MAX], to[HMI_STORE_PATH_MAX];
    for (uint32_t i = s->cfg.generations - 1u; i >= 1u; i--) {
        uint64_t sz;
        snprintf(from, sizeof(from), "%s.%u", p, (unsigned)i);
        if (!s->io.size(from, &sz)) continue; /* this generation does not exist yet */
        snprintf(to, sizeof(to), "%s.%u", p, (unsigned)(i + 1u));
        if (!s->io.rename(from, to)) return false;
    }
    snprintf(to, sizeof(to), "%s.1", p);
    return s->io.rename(p, to);
}

static bool do_append(hmi_store_t *s, const hmi_store_item_t *it)
{
    unsigned t = it->target;
    const char *p = s->path[t];
    if (!s->size_known[t]) {
        uint64_t sz = 0;
        if (!s->io.size(p, &sz)) sz = 0; /* absent: starts empty */
        /* A power cut or a failed write can leave the last line unfinished.
         * Without a line break the next record would be glued onto it and
         * both would be lost to a reader. Finish the torn line first. */
        if (sz > 0) {
            char last = '\n';
            size_t got = 0;
            bool cut = false;
            if (hmi_fs_read_tail(p, &last, 1, &got, &cut) == HMI_FS_READ_OK && got == 1 && last != '\n') {
                if (!s->io.append(p, "\n", 1)) return false;
                sz++;
            }
        }
        s->file_size[t] = sz;
        s->size_known[t] = true;
    }
    if (s->cfg.max_bytes != 0 && s->file_size[t] > 0 && s->file_size[t] + it->len > s->cfg.max_bytes) {
        if (!rotate(s, t)) {
            s->size_known[t] = false;
            return false;
        }
        s->file_size[t] = 0;
        hmi_mutex_lock(&s->lock);
        s->stats.rotations++;
        hmi_mutex_unlock(&s->lock);
        if (s->cfg.marker[t]) {
            char line[64];
            int n = snprintf(line, sizeof(line), "%llu LOG_ROTATED generations=%u\n",
                             (unsigned long long)hmi_now_ms(), (unsigned)s->cfg.generations);
            if (n <= 0 || !s->io.append(p, line, (size_t)n)) {
                s->size_known[t] = false;
                return false;
            }
            s->file_size[t] = (uint64_t)n;
        }
    }
    if (!s->io.append(p, it->data, it->len)) {
        s->size_known[t] = false; /* a failed append may have left a partial record */
        return false;
    }
    s->file_size[t] += it->len;
    return true;
}

bool hmi_store_run_once(hmi_store_t *s)
{
    hmi_store_item_t it;
    hmi_mutex_lock(&s->lock);
    if (s->q_len == 0) {
        hmi_mutex_unlock(&s->lock);
        return false;
    }
    it = s->q[s->q_head];
    s->q_head = (uint8_t)((s->q_head + 1u) % HMI_STORE_QUEUE_LEN);
    s->q_len--;
    s->busy = true;
    hmi_mutex_unlock(&s->lock);

    bool ok = it.kind == HMI_STORE_REPLACE ? s->io.replace(s->path[it.target], it.data, it.len) : do_append(s, &it);

    hmi_mutex_lock(&s->lock);
    set_result(s, it.ticket, ok ? HMI_STORE_DONE : HMI_STORE_FAILED);
    if (ok) s->stats.written++; else s->stats.failed++;
    s->stats.fault = !ok;
    hmi_flag_set(&s->fault, ok ? 0 : 1);
    hmi_mutex_unlock(&s->lock);
    hmi_event_signal(&s->done);

    if (it.done) it.done(it.user, it.arg, ok);

    hmi_mutex_lock(&s->lock);
    s->busy = false;
    hmi_mutex_unlock(&s->lock);
    hmi_event_signal(&s->done);
    return true;
}

static void worker(void *arg)
{
    hmi_store_t *s = (hmi_store_t *)arg;
    for (;;) {
        if (hmi_store_run_once(s)) continue;
        hmi_mutex_lock(&s->lock);
        bool finished = hmi_flag_get(&s->stop) != 0 && s->q_len == 0;
        hmi_mutex_unlock(&s->lock);
        if (finished) break;
        hmi_event_wait(&s->wake, 200);
    }
}

bool hmi_store_start(hmi_store_t *s)
{
    if (!s->ready || s->running) return false;
    hmi_flag_set(&s->stop, 0);
    if (!hmi_thread_start(&s->th, worker, s)) return false;
    s->running = true;
    return true;
}

bool hmi_store_stop(hmi_store_t *s, uint32_t timeout_ms)
{
    if (!s->ready) return true;
    hmi_mutex_lock(&s->lock);
    hmi_flag_set(&s->stop, 1); /* under the lock: after this no request can be queued */
    hmi_mutex_unlock(&s->lock);
    hmi_event_signal(&s->wake);
    if (!s->running) return true;
    bool joined = hmi_thread_join(&s->th, timeout_ms);
    if (joined) s->running = false;
    return joined;
}

bool hmi_store_fault(hmi_store_t *s) { return hmi_flag_get(&s->fault) != 0; }

void hmi_store_stats(hmi_store_t *s, hmi_store_stats_t *out)
{
    hmi_mutex_lock(&s->lock);
    *out = s->stats;
    hmi_mutex_unlock(&s->lock);
}
