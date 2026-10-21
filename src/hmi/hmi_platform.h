/**
 * @file hmi_platform.h
 * @brief Platform services used by the portable HMI runtime.
 *
 * The runtime core (src/hmi, excluding the hmi_platform_* files,) includes only this
 * header for OS services. Two implementations exist: hmi_platform_win32.c
 * (host tests, Windows demo) and hmi_platform_posix.c (Linux target).
 */
#ifndef HMI_PLATFORM_H
#define HMI_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Monotonic milliseconds since an arbitrary point. Never goes backwards. */
uint64_t hmi_now_ms(void);
void hmi_sleep_ms(uint32_t ms);

/* Non-recursive mutex. Opaque storage keeps this header free of OS headers. */
typedef struct {
    uint64_t opaque[8];
} hmi_mutex_t;

bool hmi_mutex_init(hmi_mutex_t *m);
void hmi_mutex_lock(hmi_mutex_t *m);
void hmi_mutex_unlock(hmi_mutex_t *m);
void hmi_mutex_destroy(hmi_mutex_t *m);

/* Threads */
typedef struct {
    uint64_t opaque[2];
    bool valid;
} hmi_thread_t;

typedef void (*hmi_thread_fn)(void *arg);
bool hmi_thread_start(hmi_thread_t *t, hmi_thread_fn fn, void *arg);
/* Returns true when the thread exited within timeout_ms. */
bool hmi_thread_join(hmi_thread_t *t, uint32_t timeout_ms);

/* Sequentially consistent stop flag shared between threads. */
typedef struct {
    volatile int32_t v;
} hmi_atomic_flag_t;
void hmi_flag_set(hmi_atomic_flag_t *f, int32_t value);
int32_t hmi_flag_get(hmi_atomic_flag_t *f);

/* Supervisor notification in the systemd sd_notify format ("READY=1",
 * "WATCHDOG=1", "STOPPING=1"). Sent to the datagram socket named by the
 * NOTIFY_SOCKET environment variable. Returns false, harmlessly, when the
 * variable is unset or the platform has no supervisor. Never blocks. */
bool hmi_notify(const char *message);

/* Files. Both return false on any failure and never leave a partial target. */
bool hmi_fs_atomic_write(const char *path, const void *data, size_t len);
/* Append and flush to stable storage before returning. */
bool hmi_fs_append_sync(const char *path, const void *data, size_t len);

/* Read a whole file into buf (capacity cap). The three failure outcomes are
 * kept apart because callers treat "absent" differently from "unreadable". */
typedef enum {
    HMI_FS_READ_OK = 0,
    HMI_FS_READ_ABSENT,    /* the file does not exist */
    HMI_FS_READ_ERROR,     /* exists but cannot be read */
    HMI_FS_READ_TOO_BIG    /* longer than cap; *len is then not meaningful */
} hmi_fs_read_t;
hmi_fs_read_t hmi_fs_read_all(const char *path, void *buf, size_t cap, size_t *len);

/* The permission bits (rwxrwxrwx, 0..0777) of a file. On a platform without
 * POSIX permissions (the Windows development host) it reports 0600, so the
 * caller's "too open" checks never fire there. Returns false if the file
 * cannot be examined. */
bool hmi_fs_mode(const char *path, uint32_t *mode);

/* Cryptographically secure random bytes from the operating system. Returns
 * false on any failure; the caller must then not continue (never fall back to
 * a weak source). */
bool hmi_random_bytes(void *out, size_t n);

#endif /* HMI_PLATFORM_H */
