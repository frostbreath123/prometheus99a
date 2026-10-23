#include "hmi_runtime.h"
#include "hmi_auditfmt.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { TGT_AUDIT = 0, TGT_JOURNAL = 1, TGT_STATE = 2 };

/* Queue one audit line. With wait, return only once it is durable. */
static bool queue_audit(hmi_runtime_t *rt, const char *line, size_t n, hmi_store_done_fn done, uint32_t arg, bool wait)
{
    uint32_t ticket = 0;
    if (!rt->store_started) return false;
    if (!hmi_store_enqueue(&rt->store, TGT_AUDIT, line, n, done, rt, arg, &ticket)) return false;
    if (!wait) return true;
    return hmi_store_wait(&rt->store, ticket, HMI_AUDIT_WAIT_MS) == HMI_STORE_DONE;
}

static void write_audit_done(void *user, uint32_t id, bool ok)
{
    hmi_runtime_t *rt = (hmi_runtime_t *)user;
    hmi_writes_audit_done(&rt->writes, id, ok, hmi_now_ms());
}

static void ack_audit_done(void *user, uint32_t token, bool ok)
{
    hmi_runtime_t *rt = (hmi_runtime_t *)user;
    hmi_alarm_ack_audit_done(&rt->alarms, token, ok, hmi_now_ms());
}

/* Records of a PLC write after RESERVED. SENT must be durable before the
 * request can be transmitted, so the comms thread waits for it; the final
 * states are queued and not waited for (the transmission already happened). */
static bool file_audit(void *user, const hmi_write_rec_t *r, const char *event)
{
    hmi_runtime_t *rt = (hmi_runtime_t *)user;
    char line[320];
    hmi_wall_t w;
    hmi_wall_now(&w);
    size_t n = hmi_audit_format_write(line, sizeof(line), r, event, &w);
    if (n == 0) return false;
    return queue_audit(rt, line, n, NULL, 0, strcmp(event, "SENT") == 0);
}

/* The RESERVED record: queued, never waited for here (the UI thread calls
 * this). The request stays RESERVED until write_audit_done() reports it durable. */
static bool file_audit_reserved(void *user, const hmi_write_rec_t *r, const char *event)
{
    hmi_runtime_t *rt = (hmi_runtime_t *)user;
    char line[320];
    hmi_wall_t w;
    hmi_wall_now(&w);
    size_t n = hmi_audit_format_write(line, sizeof(line), r, event, &w);
    if (n == 0) return false;
    return queue_audit(rt, line, n, write_audit_done, r->id, false);
}

/* Alarm acknowledgement attempts go to the same audit file as PLC writes. A
 * granted one (token != 0) is applied only when its record is durable; the
 * record of a denied attempt is best effort (token == 0). */
static bool file_ack_audit(void *user, const hmi_ack_audit_t *rec, uint32_t token)
{
    hmi_runtime_t *rt = (hmi_runtime_t *)user;
    const char *names[HMI_MAX_TAGS];
    size_t nt = hmi_tagdb_count(&rt->db);
    for (size_t i = 0; i < nt; i++) names[i] = hmi_tagdb_cfg(&rt->db, i)->name;
    char line[704];
    hmi_wall_t w;
    hmi_wall_now(&w);
    size_t n = hmi_audit_format_ack(line, sizeof(line), rec, names, nt, &w);
    if (n == 0) return false;
    return queue_audit(rt, line, n, token ? ack_audit_done : NULL, token, false);
}

/* ---- alarm journal on disk ---- */
/* Called by the alarm engine after it released its lock (see hmi_alarm.h). The
 * entry is queued for the storage worker and not waited for: the journal is a
 * log, and the audit trail records what an operator did. A refused entry is
 * counted by the alarm engine in sink_failures. */
static bool journal_sink(void *user, const hmi_alarm_log_t *e)
{
    hmi_runtime_t *rt = (hmi_runtime_t *)user;
    const char *names[HMI_MAX_TAGS];
    size_t nt = hmi_tagdb_count(&rt->db);
    for (size_t i = 0; i < nt; i++) names[i] = hmi_tagdb_cfg(&rt->db, i)->name;
    char line[320];
    hmi_wall_t w;
    hmi_wall_now(&w);
    size_t n = hmi_audit_format_journal(line, sizeof(line), e, names, nt, &w);
    if (n == 0) return false;
    return hmi_store_enqueue(&rt->store, TGT_JOURNAL, line, n, NULL, NULL, 0, NULL);
}

/* Read the end of the journal file back into the in-memory journal so the
 * screen shows the recent events after a restart. Lines that are torn, damaged
 * or name a tag that no longer exists are skipped. */
static void journal_collect(const char *path, const char *const *names, size_t nt, hmi_alarm_log_t *ev, size_t *n_ev)
{
    /* Room for the newest HMI_JOURNAL_LEN lines at their longest (320 bytes each, see journal_sink); older lines would be
     * dropped by the ring anyway. */
    static char tail[HMI_JOURNAL_LEN * 320];
    size_t len = 0, pos = 0;
    bool cut = false;
    if (hmi_fs_read_tail(path, tail, sizeof(tail), &len, &cut) != HMI_FS_READ_OK || len == 0) return;
    if (cut) { /* the buffer starts in the middle of a line: drop that piece */
        while (pos < len && tail[pos] != '\n') pos++;
        if (pos < len) pos++;
    }
    while (pos < len) {
        size_t s = pos;
        while (pos < len && tail[pos] != '\n') pos++;
        if (pos >= len) break; /* no newline: a torn last line */
        hmi_alarm_log_t e;
        if (hmi_audit_parse_journal(tail + s, pos - s, names, nt, &e)) {
            if (*n_ev == HMI_JOURNAL_LEN) { /* keep the newest */
                memmove(ev, ev + 1, (HMI_JOURNAL_LEN - 1) * sizeof(ev[0]));
                (*n_ev)--;
            }
            ev[(*n_ev)++] = e;
        }
        pos++;
    }
}

/* The live file may have been started just before the restart (a rotation
 * leaves it empty), so the newest rotated file is read first and the live file
 * after it: together they hold the most recent entries in order. */
static void journal_reload(hmi_runtime_t *rt)
{
    const char *names[HMI_MAX_TAGS];
    size_t nt = hmi_tagdb_count(&rt->db);
    for (size_t i = 0; i < nt; i++) names[i] = hmi_tagdb_cfg(&rt->db, i)->name;
    hmi_alarm_log_t ev[HMI_JOURNAL_LEN];
    size_t n = 0;
    char older[sizeof(rt->journal_path) + 4];
    snprintf(older, sizeof(older), "%s.1", rt->journal_path);
    journal_collect(older, names, nt, ev, &n);
    journal_collect(rt->journal_path, names, nt, ev, &n);
    if (n > 0) {
        hmi_alarm_journal_restore(&rt->alarms, ev, n);
        rt->journal_restored = (uint32_t)n;
    }
}

/* ---- alarm state file ---- */
static void state_done(void *user, uint32_t arg, bool ok)
{
    hmi_runtime_t *rt = (hmi_runtime_t *)user;
    (void)arg;
    if (!ok) hmi_flag_set(&rt->state_retry, 1); /* the file is not current: write again when the limits allow */
}

/* Supervisor thread. Queue a new state file when an alarm changed, within the
 * interval and hourly limits. Never waits. */
static void state_saver(hmi_runtime_t *rt, uint64_t now)
{
    if (!rt->store_started || rt->alarm_state_path[0] == '\0') return;
    bool retry = hmi_flag_get(&rt->state_retry) != 0;
    if (!retry && hmi_alarm_state_rev(&rt->alarms) == rt->state_saved_rev) return; /* nothing new */
    if (rt->state_saved_once && now - rt->state_last_ms < rt->state_min_interval_ms) return;
    if (now - rt->state_budget_t0 >= 3600000u) {
        rt->state_budget_t0 = now;
        rt->state_budget_used = 0;
        rt->state_budget_noted = false;
    }
    if (rt->state_budget_used >= rt->state_saves_per_hour) {
        if (!rt->state_budget_noted) { /* count the hold-back once per hour, not on every pass */
            rt->state_budget_noted = true;
            hmi_flag_set(&rt->n_state_deferred, hmi_flag_get(&rt->n_state_deferred) + 1);
        }
        return; /* held back until the hour is over */
    }
    uint8_t buf[HMI_ALARM_STATE_MAX];
    uint32_t rev = 0;
    size_t n = hmi_alarm_state_serialize(&rt->alarms, buf, sizeof(buf), &rev);
    if (n == 0) return;
    hmi_flag_set(&rt->state_retry, 0);
    if (!hmi_store_enqueue(&rt->store, TGT_STATE, buf, n, state_done, rt, 0, NULL)) {
        hmi_flag_set(&rt->state_retry, 1); /* queue full: try again on the next pass */
        hmi_flag_set(&rt->n_state_deferred, hmi_flag_get(&rt->n_state_deferred) + 1);
        return;
    }
    rt->state_saved_rev = rev;
    rt->state_last_ms = now;
    rt->state_saved_once = true;
    rt->state_budget_used++;
    hmi_flag_set(&rt->n_state_saves, hmi_flag_get(&rt->n_state_saves) + 1);
}

void hmi_runtime_storage_info(hmi_runtime_t *rt, hmi_runtime_storage_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state_saves = (uint32_t)hmi_flag_get(&rt->n_state_saves);
    out->state_deferred = (uint32_t)hmi_flag_get(&rt->n_state_deferred);
    out->journal_restored = rt->journal_restored;
    if (rt->store_ready) hmi_store_stats(&rt->store, &out->store);
}

bool hmi_runtime_audit_append(hmi_runtime_t *rt, const char *line, size_t len)
{
    if (!rt->started || rt->audit_path[0] == '\0') return false;
    return queue_audit(rt, line, len, NULL, 0, true);
}

bool hmi_runtime_storage_fault(hmi_runtime_t *rt)
{
    return rt->store_started && hmi_store_fault(&rt->store);
}

static void comms_thread(void *arg)
{
    hmi_runtime_t *rt = (hmi_runtime_t *)arg;
    while (!hmi_flag_get(&rt->stop)) {
        uint32_t hint = hmi_comms_step(&rt->comms);
        hmi_flag_set(&rt->beat_comms, (int32_t)((uint32_t)hmi_flag_get(&rt->beat_comms) + 1u)); /* single writer, wraps */
        if (hint > 0) hmi_sleep_ms(hint > 20u ? 20u : hint);
    }
}

static void supervisor_thread(void *arg)
{
    hmi_runtime_t *rt = (hmi_runtime_t *)arg;
    while (!hmi_flag_get(&rt->stop)) {
        uint64_t now = hmi_now_ms();
        hmi_tagdb_age(&rt->db, now);
        hmi_alarm_step(&rt->alarms, now, hmi_comms_link_health(&rt->comms));
        if (rt->trend.n_series) {
            /* the snapshot takes and releases the tag lock; the trend lock is taken after, never nested */
            hmi_tag_value_t vals[HMI_MAX_TAGS];
            hmi_tagdb_snapshot(&rt->db, vals, HMI_MAX_TAGS);
            hmi_trend_step(&rt->trend, now, vals, hmi_tagdb_count(&rt->db));
        }
        state_saver(rt, now);
        hmi_flag_set(&rt->beat_super, (int32_t)((uint32_t)hmi_flag_get(&rt->beat_super) + 1u)); /* single writer, wraps */
        hmi_sleep_ms(50);
    }
}

bool hmi_runtime_start(hmi_runtime_t *rt, const hmi_app_cfg_t *cfg, const hmi_plc_driver_t *drv,
                       const char *audit_path, const char *alarm_state_path)
{
    hmi_runtime_storage_t st;
    memset(&st, 0, sizeof(st));
    st.audit_path = audit_path;
    st.alarm_state_path = alarm_state_path;
    return hmi_runtime_start_ex(rt, cfg, drv, &st);
}

/* Clear the whole runtime except the 64 slots of the storage queue (hmi_store_zero says why). */
static void runtime_zero(hmi_runtime_t *rt)
{
    size_t from = offsetof(hmi_runtime_t, store), to = from + sizeof(rt->store);
    memset(rt, 0, from);
    hmi_store_zero(&rt->store);
    memset((char *)rt + to, 0, sizeof(*rt) - to);
}

bool hmi_runtime_start_ex(hmi_runtime_t *rt, const hmi_app_cfg_t *cfg, const hmi_plc_driver_t *drv,
                          const hmi_runtime_storage_t *storage)
{
    runtime_zero(rt);
    if (!cfg || cfg->n_tags == 0 || !drv || !storage) return false;
    rt->cfg = *cfg;
    if (storage->audit_path) snprintf(rt->audit_path, sizeof(rt->audit_path), "%s", storage->audit_path);
    if (storage->alarm_state_path) {
        snprintf(rt->alarm_state_path, sizeof(rt->alarm_state_path), "%s", storage->alarm_state_path);
        snprintf(rt->journal_path, sizeof(rt->journal_path), "%s.journal", rt->alarm_state_path);
    }

    rt->state_min_interval_ms = storage->state_min_interval_ms ? storage->state_min_interval_ms : HMI_STATE_MIN_INTERVAL_MS;
    rt->state_saves_per_hour = storage->state_saves_per_hour ? storage->state_saves_per_hour : HMI_STATE_SAVES_PER_HOUR;
    rt->state_budget_t0 = hmi_now_ms();

    /* Fail closed: no durable audit trail => no writes at all. */
    rt->writes_effective = cfg->writes_enabled && rt->audit_path[0] != '\0';

    if (!hmi_tagdb_init(&rt->db, cfg->tags, cfg->n_tags, hmi_now_ms())) return false;

    hmi_comms_cfg_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.response_timeout_ms = cfg->response_timeout_ms;
    cc.connect_timeout_ms = cfg->connect_timeout_ms;
    cc.miss_limit = 3;
    cc.backoff_min_ms = 500;
    cc.backoff_max_ms = 10000;
    cc.jitter_seed = (uint32_t)hmi_now_ms() | 1u;
    if (!hmi_comms_init(&rt->comms, &rt->db, drv, &cc)) {
        hmi_tagdb_destroy(&rt->db);
        return false;
    }
    if (!hmi_writes_init(&rt->writes, &rt->db, rt->writes_effective, 5000)) {
        hmi_comms_destroy(&rt->comms);
        hmi_tagdb_destroy(&rt->db);
        return false;
    }
    hmi_writes_set_limits(&rt->writes, cfg->write_min_interval_ms, cfg->write_max_per_minute);
    hmi_comms_set_writes(&rt->comms, &rt->writes);

    if (!hmi_alarm_init(&rt->alarms, &rt->db)) {
        hmi_writes_destroy(&rt->writes);
        hmi_comms_destroy(&rt->comms);
        hmi_tagdb_destroy(&rt->db);
        return false;
    }
#ifdef P99_LITE   /* the lite profile has no trend screen, so nothing is recorded */
    const size_t trend_tags = 0;
#else
    const size_t trend_tags = cfg->n_tags;
#endif
    if (!hmi_trend_init(&rt->trend, cfg->tags, trend_tags,
                        cfg->trend_bucket_ms ? cfg->trend_bucket_ms : HMI_TREND_BUCKET_DEFAULT_MS, hmi_now_ms())) {
        hmi_alarm_destroy(&rt->alarms);
        hmi_writes_destroy(&rt->writes);
        hmi_comms_destroy(&rt->comms);
        hmi_tagdb_destroy(&rt->db);
        return false;
    }

    /* The storage worker owns every durable write. With an audit path, a
     * worker that cannot start means no audit trail, so the runtime does not
     * start (the caller shows the failure banner). */
    if (rt->audit_path[0] || rt->alarm_state_path[0]) {
        hmi_store_cfg_t sc;
        memset(&sc, 0, sizeof(sc));
        if (rt->audit_path[0]) {
            sc.path[TGT_AUDIT] = rt->audit_path;
            sc.kind[TGT_AUDIT] = HMI_STORE_APPEND;
            sc.marker[TGT_AUDIT] = true;
        }
        if (rt->alarm_state_path[0]) {
            sc.path[TGT_JOURNAL] = rt->journal_path;
            sc.kind[TGT_JOURNAL] = HMI_STORE_APPEND;
            sc.path[TGT_STATE] = rt->alarm_state_path;
            sc.kind[TGT_STATE] = HMI_STORE_REPLACE;
        }
        sc.max_bytes = storage->log_max_bytes ? storage->log_max_bytes : HMI_LOG_MAX_BYTES;
        sc.generations = storage->log_generations ? storage->log_generations : HMI_LOG_GENERATIONS;
        sc.io = storage->io;
        if (!hmi_store_init(&rt->store, &sc)) goto fail;
        rt->store_ready = true;
        if (!hmi_store_start(&rt->store)) goto fail;
        rt->store_started = true;
    }

    if (rt->audit_path[0]) {
        /* Every PLC write and acknowledgement is recorded before it takes effect
         * and a record that cannot be made durable refuses the action. */
        hmi_writes_set_audit(&rt->writes, file_audit, rt);
        hmi_writes_set_audit_async(&rt->writes, file_audit_reserved, rt);
        hmi_alarm_set_ack_audit_async(&rt->alarms, file_ack_audit, rt);

        char line[96];
        hmi_wall_t w;
        hmi_wall_now(&w);
        size_t n = hmi_audit_format_boot(line, sizeof(line), hmi_now_ms(), &w);
        if (n) queue_audit(rt, line, n, NULL, 0, false); /* a start marker; failure shows as the storage fault flag */
    }
    if (rt->alarm_state_path[0]) {
        /* absent or corrupt: start clean */
        if (hmi_alarm_load(&rt->alarms, rt->alarm_state_path)) rt->state_saved_rev = hmi_alarm_state_rev(&rt->alarms);
        journal_reload(rt);
        hmi_alarm_set_sink(&rt->alarms, journal_sink, rt); /* from here every journal entry goes to disk */
    }

    hmi_flag_set(&rt->stop, 0);
    if (!hmi_thread_start(&rt->th_comms, comms_thread, rt)) goto fail;
    if (!hmi_thread_start(&rt->th_super, supervisor_thread, rt)) {
        hmi_flag_set(&rt->stop, 1);
        hmi_thread_join(&rt->th_comms, 5000);
        goto fail;
    }
    rt->started = true;
    return true;
fail:
    if (rt->store_ready) {
        hmi_store_stop(&rt->store, 5000); /* callbacks touch writes and alarms: stop before destroying them */
        hmi_store_destroy(&rt->store);
        rt->store_ready = false;
        rt->store_started = false;
    }
    hmi_trend_destroy(&rt->trend);
    hmi_alarm_destroy(&rt->alarms);
    hmi_writes_destroy(&rt->writes);
    hmi_comms_destroy(&rt->comms);
    hmi_tagdb_destroy(&rt->db);
    return false;
}

hmi_runtime_beats_t hmi_runtime_beats(hmi_runtime_t *rt)
{
    hmi_runtime_beats_t b;
    b.comms = hmi_flag_get(&rt->beat_comms);
    b.supervisor = hmi_flag_get(&rt->beat_super);
    return b;
}

uint32_t hmi_runtime_worst_stall_ms(const hmi_runtime_t *rt)
{
    /* One loop iteration (hmi_comms_step) is exactly one of: a bounded connect;
     * the wait for the SENT record followed by a write and its read-back (two
     * bounded reply waits); or one bounded poll. Plus scheduling margin. */
    uint32_t write_step = 2u * rt->cfg.response_timeout_ms + HMI_AUDIT_WAIT_MS;
    uint32_t longest = write_step > rt->cfg.connect_timeout_ms ? write_step : rt->cfg.connect_timeout_ms;
    return longest + 2000u;
}

bool hmi_runtime_stop(hmi_runtime_t *rt)
{
    if (!rt->started) return true;
    hmi_flag_set(&rt->stop, 1);
    /* Worst case a worker is inside the longest blocking iteration (see
     * hmi_runtime_worst_stall_ms); wait for that. */
    uint32_t wait = hmi_runtime_worst_stall_ms(rt);
    uint64_t t0 = hmi_now_ms();
    bool a = hmi_thread_join(&rt->th_comms, wait);
    uint64_t spent = hmi_now_ms() - t0; /* one budget for both threads, not one each */
    bool b = hmi_thread_join(&rt->th_super, spent >= wait ? 0u : wait - (uint32_t)spent);
    /* With the workers stopped nothing new is queued: let the storage worker
     * finish every record, then write the alarm state. */
    bool s = true;
    if (rt->alarm_state_path[0]) {
        /* The last state is always written, whatever the interval and hourly limits. */
        uint8_t buf[HMI_ALARM_STATE_MAX];
        size_t n = hmi_alarm_state_serialize(&rt->alarms, buf, sizeof(buf), NULL);
        bool queued = false;
        if (n > 0 && rt->store_started) {
            queued = hmi_store_enqueue(&rt->store, TGT_STATE, buf, n, NULL, NULL, 0, NULL);
            if (!queued && hmi_store_flush(&rt->store, 2000u)) queued = hmi_store_enqueue(&rt->store, TGT_STATE, buf, n, NULL, NULL, 0, NULL);
        }
        if (!queued) hmi_alarm_save(&rt->alarms, rt->alarm_state_path); /* no worker or a full queue: write it here */
    }
    if (rt->store_started) s = hmi_store_stop(&rt->store, HMI_STORE_STOP_MS);
    rt->started = false;
    if (a && b && s) {
        hmi_store_destroy(&rt->store);
        hmi_trend_destroy(&rt->trend);
        hmi_alarm_destroy(&rt->alarms);
        hmi_writes_destroy(&rt->writes);
        hmi_comms_destroy(&rt->comms);
        hmi_tagdb_destroy(&rt->db);
        return true;
    }
    /* If a thread failed to stop, shared objects are intentionally leaked
     * rather than destroyed under a running thread. */
    return false;
}
