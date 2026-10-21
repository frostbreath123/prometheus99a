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

typedef struct {
    char host[HMI_HOST_LEN];      /* dotted-quad IPv4 literal only (no DNS) */
    uint16_t port;
    uint8_t unit;
    uint32_t response_timeout_ms;
    uint32_t connect_timeout_ms;
    bool writes_enabled;          /* global write master switch, default false */
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

/* Persist the raw config text atomically (temp + fsync + rename). The text
 * is validated first, so an invalid config can never replace a good one. */
bool hmi_config_save_text(const char *path, const char *json, size_t len, char *err, size_t err_cap);

#endif /* HMI_CONFIG_H */
