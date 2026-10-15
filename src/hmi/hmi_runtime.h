/**
 * @file hmi_runtime.h
 * @brief Wires tag table, comms thread, alarm/supervisor thread and the
 *        file-backed write audit trail. Platform independent.
 */
#ifndef HMI_RUNTIME_H
#define HMI_RUNTIME_H

#include "hmi_config.h"
#include "hmi_comms.h"
#include "hmi_writes.h"
#include "hmi_alarm.h"

typedef struct {
    hmi_app_cfg_t cfg;
    hmi_tagdb_t db;
    hmi_comms_t comms;
    hmi_writes_t writes;
    hmi_alarm_t alarms;
    hmi_thread_t th_comms, th_super;
    hmi_atomic_flag_t stop;
    /* Incremented once per loop iteration by the owning thread only. */
    hmi_atomic_flag_t beat_comms, beat_super;
    char audit_path[256];
    char alarm_state_path[256];
    bool writes_effective;   /* cfg.writes_enabled AND audit trail available */
    bool started;
} hmi_runtime_t;

/* paths may be NULL. Without audit_path, writes are forced off (fail closed).
 * The driver must outlive the runtime. Returns false if nothing could start. */
bool hmi_runtime_start(hmi_runtime_t *rt, const hmi_app_cfg_t *cfg, const hmi_plc_driver_t *drv,
                       const char *audit_path, const char *alarm_state_path);
/* Stops threads, persists alarm state. Safe to call twice. */
void hmi_runtime_stop(hmi_runtime_t *rt);

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
