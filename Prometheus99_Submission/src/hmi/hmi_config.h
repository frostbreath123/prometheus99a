/**
 * @file hmi_config.h
 * @brief Strict, bounded JSON configuration loader (jsmn tokenizer).
 *
 * Policy: any problem rejects the WHOLE file. The caller then runs with no
 * PLC endpoint and writes disabled; there is no "best effort" partial load
 * and no silently substituted default endpoint.
 */
#ifndef HMI_CONFIG_H
#define HMI_CONFIG_H

#include "hmi_tags.h"

#define HMI_CONFIG_MAX_BYTES 32768u
#define HMI_MAX_ALLOWED 8   /* entries in plc.allowed_hosts */

/* Longest time one PLC request may block the comms thread. The limits keep the
 * longest legitimate stall (hmi_runtime_worst_stall_ms) below the service
 * manager's watchdog (10 s) and stop timeout (15 s) in deploy/prometheus99.service. */
#define HMI_RESPONSE_TIMEOUT_MAX_MS 2000u
#define HMI_CONNECT_TIMEOUT_MAX_MS  3000u

typedef struct {
    char host[HMI_HOST_LEN];      /* dotted-quad IPv4 literal only (no DNS) */
    uint16_t port;
    uint8_t unit;
    uint32_t response_timeout_ms;
    uint32_t connect_timeout_ms;
    bool writes_enabled;          /* global write master switch, default false */
    uint32_t write_min_interval_ms; /* at least this long between two accepted writes to one tag (100 .. 3600000, default 1000) */
    uint32_t write_max_per_minute;  /* at most this many accepted writes in any 60 s window (1 .. 600, default 30) */
    uint32_t trend_bucket_ms;       /* length of one trend bucket (1000 .. 60000, default 5000) */
    char allowed_hosts[HMI_MAX_ALLOWED][HMI_HOST_LEN]; /* plc.allowed_hosts, optional here */
    size_t n_allowed;
    hmi_tag_cfg_t tags[HMI_MAX_TAGS];
    size_t n_tags;
} hmi_app_cfg_t;

/* Returns true on success. On failure *out is zeroed (writes_enabled=false,
 * n_tags=0, empty host) and err holds a short reason. */
bool hmi_config_parse(const char *json, size_t len, hmi_app_cfg_t *out, char *err, size_t err_cap);
/* True when the list is not empty and `host` is one of its entries. A device
 * that has verified its configuration (hmi_cfgsec) refuses any other host. */
bool hmi_config_host_allowed(const hmi_app_cfg_t *cfg);
bool hmi_config_load_file(const char *path, hmi_app_cfg_t *out, char *err, size_t err_cap);

/* The configuration as text that hmi_config_parse accepts and reads back as the
 * same structure (compact JSON, one tag per line; a value that holds its default
 * is left out). Returns the length, without a terminating zero counted but one is
 * written, or 0 if cap is too small, there are no tags, or a string holds a
 * character the parser would not accept. */
size_t hmi_config_serialize(const hmi_app_cfg_t *cfg, char *buf, size_t cap);
/* Every field the parser fills, compared. The allowed hosts and the tags count only up to their counts. */
bool hmi_config_equal(const hmi_app_cfg_t *a, const hmi_app_cfg_t *b);

/* Persist the raw config text atomically (temp + fsync + rename). The text
 * is validated first, so an invalid config can never replace a good one. */
bool hmi_config_save_text(const char *path, const char *json, size_t len, char *err, size_t err_cap);

#endif /* HMI_CONFIG_H */
