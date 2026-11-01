/**
 * @file hmi_watchdog.h
 * @brief Liveness decision for the systemd watchdog. Pure logic, no I/O.
 *
 * The UI loop calls hmi_watchdog_step() every iteration with the worker
 * heartbeat counters. It answers "may we tell the supervisor we are alive
 * now?". A ping is only allowed while every worker counter has changed within
 * the stall limit, so a stuck comms or supervisor thread stops the pings and
 * the supervisor restarts the process. If the UI loop itself is stuck it
 * never calls step() and the pings stop the same way.
 */
#ifndef HMI_WATCHDOG_H
#define HMI_WATCHDOG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define HMI_WD_CHANNELS 2

typedef struct {
    uint32_t ping_interval_ms;   /* 0 = watchdog disabled: step() never asks for a ping */
    uint32_t stall_limit_ms;
    uint64_t last_ping_ms;
    uint64_t last_change_ms[HMI_WD_CHANNELS];
    int32_t last_beat[HMI_WD_CHANNELS];
} hmi_watchdog_t;

void hmi_watchdog_init(hmi_watchdog_t *w, uint32_t ping_interval_ms, uint32_t stall_limit_ms,
                       uint64_t now_ms, const int32_t beats[HMI_WD_CHANNELS]);

/* True when a WATCHDOG=1 ping should be sent now. Wrap-safe for counters and
 * for a clock that does not go backwards. */
bool hmi_watchdog_step(hmi_watchdog_t *w, uint64_t now_ms, const int32_t beats[HMI_WD_CHANNELS]);

/* True if channel ch has not changed for longer than the stall limit. */
bool hmi_watchdog_stalled(const hmi_watchdog_t *w, size_t ch, uint64_t now_ms);

/* Parse the systemd environment: WATCHDOG_USEC (required) and WATCHDOG_PID
 * (optional, must equal pid when present). Returns the ping interval in
 * milliseconds, half the supervisor's timeout, or 0 when the watchdog is not
 * active or the values are unusable. Both arguments may be NULL. */
uint32_t hmi_watchdog_interval_from_env(const char *watchdog_usec, const char *watchdog_pid, uint32_t pid);

#endif /* HMI_WATCHDOG_H */
