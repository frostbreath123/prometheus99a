#include "hmi_runtime.h"
#include <stdio.h>
#include <string.h>

static bool file_audit(void *user, const hmi_write_rec_t *r, const char *event)
{
    const char *path = (const char *)user;
    char line[192];
    int n = snprintf(line, sizeof(line), "%llu %s id=%u tag=%u role=%d eng=%.6g raw=0x%08X rb=0x%08X exc=%u\n",
                     (unsigned long long)r->t_submit_ms, event, (unsigned)r->id, (unsigned)r->tag,
                     (int)r->role, r->eng, (unsigned)r->raw, (unsigned)r->readback_raw, (unsigned)r->exc);
    if (n <= 0 || n >= (int)sizeof(line)) return false;
    return hmi_fs_append_sync(path, line, (size_t)n);
}

/* Alarm acknowledgement attempts go to the same audit file as PLC writes.
 * One line per attempt: monotonic ms since boot, the claimed role, the action,
 * the result and the alarm names. hmi_fs_append_sync() returns true only after
 * the data was written and flushed to stable storage, so a true result here
 * means durable in the sense the operating system can promise; nothing is
 * buffered by this module. */
static bool file_ack_audit(void *user, const hmi_ack_audit_t *rec)
{
    const hmi_runtime_t *rt = (const hmi_runtime_t *)user;
    char line[512];
    size_t nt = hmi_tagdb_count(&rt->db);
    int n = snprintf(line, sizeof(line), "%llu ALARM_ACK role=%s action=%s result=%s n=%u points=",
                     (unsigned long long)rec->t_ms, hmi_role_name(rec->role), rec->all ? "ACK_ALL" : "ACK",
                     rec->result == HMI_ACK_OK ? "GRANTED" : "DENIED", (unsigned)rec->n_points);
    if (n <= 0 || n >= (int)sizeof(line)) return false;
    size_t off = (size_t)n;
    for (size_t i = 0; i < rec->n_points; i++) {
        const char *who = rec->points[i] < nt ? hmi_tagdb_cfg(&rt->db, rec->points[i])->name : "PLC_LINK";
        size_t need = strlen(who) + 1u;
        if (off + need + 12u >= sizeof(line)) { /* list would not fit: say how many were left out */
            off += (size_t)snprintf(line + off, sizeof(line) - off, ",+%u_more", (unsigned)(rec->n_points - i));
            break;
        }
        off += (size_t)snprintf(line + off, sizeof(line) - off, "%s%s", i ? "," : "", who);
    }
    line[off++] = '\n';
    return hmi_fs_append_sync(rt->audit_path, line, off);
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
        hmi_flag_set(&rt->beat_super, (int32_t)((uint32_t)hmi_flag_get(&rt->beat_super) + 1u)); /* single writer, wraps */
        hmi_sleep_ms(50);
    }
}

bool hmi_runtime_start(hmi_runtime_t *rt, const hmi_app_cfg_t *cfg, const hmi_plc_driver_t *drv,
                       const char *audit_path, const char *alarm_state_path)
{
    memset(rt, 0, sizeof(*rt));
    if (!cfg || cfg->n_tags == 0 || !drv) return false;
    rt->cfg = *cfg;
    if (audit_path) snprintf(rt->audit_path, sizeof(rt->audit_path), "%s", audit_path);
    if (alarm_state_path) snprintf(rt->alarm_state_path, sizeof(rt->alarm_state_path), "%s", alarm_state_path);

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
    if (rt->audit_path[0]) hmi_writes_set_audit(&rt->writes, file_audit, rt->audit_path);
    hmi_comms_set_writes(&rt->comms, &rt->writes);

    if (!hmi_alarm_init(&rt->alarms, &rt->db)) {
        hmi_writes_destroy(&rt->writes);
        hmi_comms_destroy(&rt->comms);
        hmi_tagdb_destroy(&rt->db);
        return false;
    }
    /* With an audit path every acknowledgement attempt is recorded, and a grant
     * is refused if the record cannot be written (see hmi_alarm.h). Without one
     * acknowledgements are allowed and are not recorded. */
    if (rt->audit_path[0]) hmi_alarm_set_ack_audit(&rt->alarms, file_ack_audit, rt);
    if (rt->alarm_state_path[0]) hmi_alarm_load(&rt->alarms, rt->alarm_state_path); /* absent/corrupt: start clean */

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
    /* One loop iteration is at most a bounded connect, or a write followed by
     * its read-back (two bounded reply waits), plus scheduling margin. */
    return 2u * rt->cfg.response_timeout_ms + rt->cfg.connect_timeout_ms + 2000u;
}

void hmi_runtime_stop(hmi_runtime_t *rt)
{
    if (!rt->started) return;
    hmi_flag_set(&rt->stop, 1);
    /* Worst case the comms thread is inside a blocking driver call bounded
     * by response_timeout_ms; wait for that plus margin. */
    uint32_t wait = rt->cfg.response_timeout_ms + rt->cfg.connect_timeout_ms + 2000u;
    bool a = hmi_thread_join(&rt->th_comms, wait);
    bool b = hmi_thread_join(&rt->th_super, wait);
    if (rt->alarm_state_path[0]) hmi_alarm_save(&rt->alarms, rt->alarm_state_path);
    rt->started = false;
    if (a && b) {
        hmi_alarm_destroy(&rt->alarms);
        hmi_writes_destroy(&rt->writes);
        hmi_comms_destroy(&rt->comms);
        hmi_tagdb_destroy(&rt->db);
    }
    /* If a thread failed to stop, shared objects are intentionally leaked
     * rather than destroyed under a running thread. */
}
