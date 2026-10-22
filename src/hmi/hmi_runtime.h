/**
 * @file hmi_runtime.h
 * @brief Wires tag table, comms thread, alarm/supervisor thread, the storage
 *        worker and the file-backed audit trail. Platform independent.
 *
 * Every durable file write of the runtime (audit file, alarm journal, alarm
 * state) goes through the storage worker (hmi_store.h), so neither the UI
 * thread nor the supervisor thread waits for a disk. The comms thread waits
 * for exactly one record: the SENT record of a PLC write, which must be
 * durable before the request is transmitted.
 */
#ifndef HMI_RUNTIME_H
#define HMI_RUNTIME_H

#include "hmi_config.h"
#include "hmi_comms.h"
#include "hmi_writes.h"
#include "hmi_alarm.h"
#include "hmi_store.h"
#include "hmi_trend.h"

/* Defaults for the size cap and the number of rotated files of the audit file
 * and the alarm journal. */
#define HMI_LOG_MAX_BYTES    (1024u * 1024u)
#define HMI_LOG_GENERATIONS  4u
/* The alarm state file is rewritten when an alarm changes, but not more often
 * than every HMI_STATE_MIN_INTERVAL_MS and not more than HMI_STATE_SAVES_PER_HOUR
 * times in an hour, so a flapping alarm cannot wear out the flash. The worst
 * case is 120 writes an hour, 2880 a day. A change that is held back is written
 * as soon as the limit allows, and always at a clean shutdown. */
#define HMI_STATE_MIN_INTERVAL_MS 10000u
#define HMI_STATE_SAVES_PER_HOUR  120u
/* Longest the comms thread waits for the SENT record to become durable. If it
 * is not durable in time the write is not transmitted (fail closed). */
#define HMI_AUDIT_WAIT_MS    2000u
/* Longest the storage worker is given to finish its queue at shutdown. With the
 * worst stall of the other threads (at most 2 x 2000 + 2000 + 2000 = 8000 ms for
 * the largest timeouts the configuration allows) the whole stop stays under the
 * 15 s TimeoutStopSec of the service unit. */
#define HMI_STORE_STOP_MS    3000u

typedef struct {
    const char *audit_path;          /* NULL: no audit trail, writes are forced off */
    const char *alarm_state_path;    /* NULL: alarm state is not persisted */
    uint64_t log_max_bytes;          /* 0 = HMI_LOG_MAX_BYTES */
    uint32_t log_generations;        /* 0 = HMI_LOG_GENERATIONS */
    const hmi_store_io_t *io;        /* NULL = platform; tests inject failures here */
    uint32_t state_min_interval_ms;  /* 0 = HMI_STATE_MIN_INTERVAL_MS */
    uint32_t state_saves_per_hour;   /* 0 = HMI_STATE_SAVES_PER_HOUR */
} hmi_runtime_storage_t;

typedef struct {
    uint32_t state_saves;       /* alarm state files queued while running */
    uint32_t state_deferred;    /* changes held back by the interval or hourly limit, or by a full queue */
    uint32_t journal_restored;  /* journal entries read back at start */
    uint32_t sink_failures;     /* journal entries the storage queue refused */
    hmi_store_stats_t store;
} hmi_runtime_storage_info_t;

typedef struct {
    hmi_app_cfg_t cfg;
    hmi_tagdb_t db;
    hmi_comms_t comms;
    hmi_writes_t writes;
    hmi_alarm_t alarms;
    hmi_store_t store;
    hmi_trend_t trend;       /* history for the trend screen; written by the supervisor thread */
    hmi_thread_t th_comms, th_super;
    hmi_atomic_flag_t stop;
    /* Incremented once per loop iteration by the owning thread only. */
    hmi_atomic_flag_t beat_comms, beat_super;
    char audit_path[256];
    char alarm_state_path[256];
    char journal_path[264];  /* alarm_state_path + ".journal" */
    bool writes_effective;   /* cfg.writes_enabled AND audit trail available */
    bool store_ready;
    bool store_started;
    bool started;
    /* alarm state saver, supervisor thread only (counters are read from any thread) */
    uint32_t state_saved_rev;
    uint64_t state_last_ms;
    bool state_saved_once;
    uint32_t state_min_interval_ms, state_saves_per_hour;
    uint32_t state_budget_used;
    uint64_t state_budget_t0;
    bool state_budget_noted;
    hmi_atomic_flag_t state_retry;   /* set by the storage worker when a state write failed */
    hmi_atomic_flag_t n_state_saves, n_state_deferred;
    uint32_t journal_restored;
} hmi_runtime_t;

/* paths may be NULL. Without audit_path, writes are forced off (fail closed).
 * The driver must outlive the runtime. Returns false if nothing could start. */
bool hmi_runtime_start(hmi_runtime_t *rt, const hmi_app_cfg_t *cfg, const hmi_plc_driver_t *drv,
                       const char *audit_path, const char *alarm_state_path);
bool hmi_runtime_start_ex(hmi_runtime_t *rt, const hmi_app_cfg_t *cfg, const hmi_plc_driver_t *drv,
                          const hmi_runtime_storage_t *storage);
/* Stops threads, drains the storage queue, persists alarm state. Safe to call
 * twice. Returns true if every thread stopped and every object was released.
 * Returns false if a thread did not stop within hmi_runtime_worst_stall_ms (the
 * longest a healthy thread can be blocked); the shared objects are then
 * deliberately left allocated rather than destroyed under a running thread. */
bool hmi_runtime_stop(hmi_runtime_t *rt);

/* Append one line to the audit file through the storage worker and wait until
 * it is durable (at most HMI_AUDIT_WAIT_MS). True only if it is durable. For
 * records that must not be lost, such as a login. Without an audit path it
 * returns false. */
bool hmi_runtime_audit_append(hmi_runtime_t *rt, const char *line, size_t len);

/* True while the last file operation of the storage worker failed. The UI shows
 * this as a banner; a success clears it. */
bool hmi_runtime_storage_fault(hmi_runtime_t *rt);
void hmi_runtime_storage_info(hmi_runtime_t *rt, hmi_runtime_storage_info_t *out);

/* Liveness: each worker thread bumps its counter once per loop iteration. A
 * counter that stops changing for longer than the worst blocking call means
 * that thread is stuck. Counters wrap; compare only for equality. */
typedef struct {
    int32_t comms;
    int32_t supervisor;
} hmi_runtime_beats_t;
hmi_runtime_beats_t hmi_runtime_beats(hmi_runtime_t *rt);

/* Longest time a healthy worker can go without bumping its counter: bounded
 * Modbus connect plus response timeout, plus margin. */
uint32_t hmi_runtime_worst_stall_ms(const hmi_runtime_t *rt);

#endif /* HMI_RUNTIME_H */
