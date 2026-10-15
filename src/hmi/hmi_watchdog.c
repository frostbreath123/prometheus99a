#include "hmi_watchdog.h"

void hmi_watchdog_init(hmi_watchdog_t *w, uint32_t ping_interval_ms, uint32_t stall_limit_ms,
                       uint64_t now_ms, const int32_t beats[HMI_WD_CHANNELS])
{
    w->ping_interval_ms = ping_interval_ms;
    w->stall_limit_ms = stall_limit_ms;
    w->last_ping_ms = now_ms;
    for (size_t i = 0; i < HMI_WD_CHANNELS; i++) {
        w->last_beat[i] = beats[i];
        w->last_change_ms[i] = now_ms;
    }
}

bool hmi_watchdog_stalled(const hmi_watchdog_t *w, size_t ch, uint64_t now_ms)
{
    if (ch >= HMI_WD_CHANNELS) return true;
    if (now_ms < w->last_change_ms[ch]) return false; /* clock went backwards: do not guess */
    return now_ms - w->last_change_ms[ch] > w->stall_limit_ms;
}

bool hmi_watchdog_step(hmi_watchdog_t *w, uint64_t now_ms, const int32_t beats[HMI_WD_CHANNELS])
{
    for (size_t i = 0; i < HMI_WD_CHANNELS; i++) {
        if (beats[i] != w->last_beat[i]) {
            w->last_beat[i] = beats[i];
            w->last_change_ms[i] = now_ms;
        }
    }
    if (w->ping_interval_ms == 0) return false;
    if (now_ms < w->last_ping_ms || now_ms - w->last_ping_ms < w->ping_interval_ms) return false;
    for (size_t i = 0; i < HMI_WD_CHANNELS; i++) {
        if (hmi_watchdog_stalled(w, i, now_ms)) return false; /* withhold: let the supervisor act */
    }
    w->last_ping_ms = now_ms;
    return true;
}

static bool parse_u64(const char *s, uint64_t *out)
{
    if (!s || !*s) return false;
    uint64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        uint64_t d = (uint64_t)(*p - '0');
        if (v > (UINT64_MAX - d) / 10u) return false;
        v = v * 10u + d;
    }
    *out = v;
    return true;
}

uint32_t hmi_watchdog_interval_from_env(const char *watchdog_usec, const char *watchdog_pid, uint32_t pid)
{
    uint64_t usec, wpid;
    if (!parse_u64(watchdog_usec, &usec) || usec == 0) return 0;
    if (watchdog_pid && *watchdog_pid) {
        if (!parse_u64(watchdog_pid, &wpid) || wpid != pid) return 0; /* meant for another process */
    }
    uint64_t ms = usec / 1000u / 2u;     /* ping at half the timeout */
    if (ms == 0) return 1;
    return ms > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)ms;
}
