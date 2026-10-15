/**
 * @file hmi_tags.h
 * @brief Typed tag table with per-tag quality, monotonic timestamps and
 *        change versions. Thread-safe; every public call takes the table lock
 *        for a bounded, allocation-free critical section.
 *
 * Consumers never "clear" anything: each consumer remembers the last version
 * it has seen (per tag or global), so multiple consumers cannot steal each
 * other's updates.
 */
#ifndef HMI_TAGS_H
#define HMI_TAGS_H

#include "hmi_types.h"
#include "hmi_platform.h"

typedef struct {
    bool en_hi, en_hihi, en_lo, en_lolo;
    double hi, hihi, lo, lolo;
    double deadband;     /* engineering units; value must retreat this far */
    uint32_t on_ms;      /* debounce before raising / escalating */
    uint32_t off_ms;     /* debounce before clearing */
} hmi_alarm_cfg_t;

typedef struct {
    char name[HMI_NAME_LEN];
    char units[HMI_UNIT_LEN];
    hmi_area_t area;
    uint16_t address;                /* 0-based wire address */
    hmi_dtype_t dtype;
    hmi_word_order_t word_order;
    hmi_byte_order_t byte_order;
    double scale, offset;            /* eng = raw_number * scale + offset */
    double eng_min, eng_max;         /* plausibility range */
    uint32_t poll_ms;
    hmi_alarm_cfg_t alarm;
    bool write_allowed;
    hmi_role_t write_role;           /* minimum role to write */
    double write_min, write_max;     /* engineering limits for writes */
} hmi_tag_cfg_t;

typedef struct {
    uint32_t raw;                    /* exact register pattern (see hmi_decode.h) */
    double eng;                      /* derived engineering value */
    hmi_quality_t quality;
    hmi_reason_t reason;
    uint64_t ts_ms;                  /* monotonic time of last accepted update */
    uint32_t version;                /* bumps when raw, quality or reason change */
} hmi_tag_value_t;

typedef struct {
    hmi_area_t area;
    uint16_t start;
    uint16_t count;                  /* registers */
    uint32_t period_ms;
    uint64_t next_due_ms;
    uint8_t first;                   /* index into order[] */
    uint8_t n_tags;
} hmi_group_t;

typedef struct {
    hmi_tag_cfg_t cfg[HMI_MAX_TAGS];
    hmi_tag_value_t val[HMI_MAX_TAGS];
    size_t n_tags;

    hmi_group_t groups[HMI_MAX_GROUPS];
    size_t n_groups;
    uint8_t order[HMI_MAX_TAGS];     /* tag indices grouped contiguously */

    uint32_t global_version;
    hmi_mutex_t lock;
    bool ready;
} hmi_tagdb_t;

/* Validates cfg (widths, address overflow, duplicates), builds poll groups.
 * Returns false and leaves db unusable on invalid input. */
bool hmi_tagdb_init(hmi_tagdb_t *db, const hmi_tag_cfg_t *cfg, size_t n, uint64_t now_ms);
void hmi_tagdb_destroy(hmi_tagdb_t *db);

/* ---- producer side (comms thread) ---- */
/* Apply a successful read of group g. Only tags of that group change. */
void hmi_tagdb_group_ok(hmi_tagdb_t *db, size_t g, const uint16_t *regs, uint64_t now_ms);
/* Valid Modbus exception or protocol error for one group. */
void hmi_tagdb_group_fail(hmi_tagdb_t *db, size_t g, hmi_reason_t reason);
/* One missed response: GOOD tags become UNCERTAIN. */
void hmi_tagdb_degrade(hmi_tagdb_t *db);
/* Link declared dead: every tag with a value becomes BAD/COMM_LOSS. */
void hmi_tagdb_link_lost(hmi_tagdb_t *db);
/* Age check, runs on the supervisor thread independent of comms I/O. */
void hmi_tagdb_age(hmi_tagdb_t *db, uint64_t now_ms);

/* ---- consumer side (any thread) ---- */
bool hmi_tagdb_get(hmi_tagdb_t *db, size_t idx, hmi_tag_value_t *out);
/* Copy all values; returns the global version at copy time. */
uint32_t hmi_tagdb_snapshot(hmi_tagdb_t *db, hmi_tag_value_t *out, size_t cap);
uint32_t hmi_tagdb_global_version(hmi_tagdb_t *db);
int hmi_tagdb_find(const hmi_tagdb_t *db, const char *name);

uint32_t hmi_tag_stale_limit_ms(const hmi_tag_cfg_t *c);

/* ---- read-only structure accessors ----
 * The table's layout (cfg[], groups[], order[]) is fixed by hmi_tagdb_init and
 * never changes afterwards, so these need no lock. Code outside hmi_tags.c
 * must use them instead of reading the struct members; `make check-tagdb-access`
 * enforces this. The only mutable scheduler state is the per-group due time,
 * touched solely by the comms thread through hmi_tagdb_group_set_due. */
bool hmi_tagdb_is_ready(const hmi_tagdb_t *db);
size_t hmi_tagdb_count(const hmi_tagdb_t *db);
/* NULL when idx is out of range. The pointer is valid until hmi_tagdb_destroy. */
const hmi_tag_cfg_t *hmi_tagdb_cfg(const hmi_tagdb_t *db, size_t idx);
size_t hmi_tagdb_group_count(const hmi_tagdb_t *db);
/* NULL when g is out of range. next_due_ms in the view is comms-thread state. */
const hmi_group_t *hmi_tagdb_group(const hmi_tagdb_t *db, size_t g);
void hmi_tagdb_group_set_due(hmi_tagdb_t *db, size_t g, uint64_t due_ms);
/* Set every group's due time (used on connect and on link recovery). */
void hmi_tagdb_groups_set_all_due(hmi_tagdb_t *db, uint64_t due_ms);

#endif /* HMI_TAGS_H */
