/* POSIX implementation of hmi_platform.h (Linux target).
 * NOT compiled or run in the Windows development environment used so far;
 * see docs/ADR-0001 and README "Unverified" list. */
#define _GNU_SOURCE /* pthread_timedjoin_np */
#include "hmi_platform.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef char hmi_mutex_size_check[(sizeof(pthread_mutex_t) <= sizeof(hmi_mutex_t)) ? 1 : -1];
typedef char hmi_thread_size_check[(sizeof(pthread_t) <= sizeof(((hmi_thread_t *)0)->opaque)) ? 1 : -1];

uint64_t hmi_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void hmi_sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

bool hmi_mutex_init(hmi_mutex_t *m)
{
    memset(m, 0, sizeof(*m));
    return pthread_mutex_init((pthread_mutex_t *)m, NULL) == 0;
}
void hmi_mutex_lock(hmi_mutex_t *m) { pthread_mutex_lock((pthread_mutex_t *)m); }
void hmi_mutex_unlock(hmi_mutex_t *m) { pthread_mutex_unlock((pthread_mutex_t *)m); }
void hmi_mutex_destroy(hmi_mutex_t *m) { pthread_mutex_destroy((pthread_mutex_t *)m); }

typedef struct {
    hmi_thread_fn fn;
    void *arg;
} thread_start_t;

static thread_start_t s_slots[8];
static pthread_mutex_t s_slot_mx = PTHREAD_MUTEX_INITIALIZER;

static void *thread_tramp(void *p)
{
    thread_start_t st = *(thread_start_t *)p;
    pthread_mutex_lock(&s_slot_mx);
    ((thread_start_t *)p)->fn = NULL; /* free slot */
    pthread_mutex_unlock(&s_slot_mx);
    st.fn(st.arg);
    return NULL;
}

bool hmi_thread_start(hmi_thread_t *t, hmi_thread_fn fn, void *arg)
{
    thread_start_t *slot = NULL;
    pthread_mutex_lock(&s_slot_mx);
    for (size_t i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
        if (!s_slots[i].fn) {
            slot = &s_slots[i];
            slot->fn = fn;
            slot->arg = arg;
            break;
        }
    }
    pthread_mutex_unlock(&s_slot_mx);
    if (!slot) return false;
    pthread_t th;
    if (pthread_create(&th, NULL, thread_tramp, slot) != 0) {
        pthread_mutex_lock(&s_slot_mx);
        slot->fn = NULL;
        pthread_mutex_unlock(&s_slot_mx);
        return false;
    }
    memcpy(t->opaque, &th, sizeof(th));
    t->valid = true;
    return true;
}

bool hmi_thread_join(hmi_thread_t *t, uint32_t timeout_ms)
{
    if (!t->valid) return true;
    pthread_t th;
    memcpy(&th, t->opaque, sizeof(th));
    /* The deadline comes from the monotonic clock: pthread_timedjoin_np takes
     * a CLOCK_REALTIME deadline, so a wall-clock step (NTP on a Pi without an
     * RTC) would shorten or lengthen the wait. Polling pthread_tryjoin_np
     * every millisecond keeps the join edge visible to ThreadSanitizer, which
     * pthread_clockjoin_np is not. */
    uint64_t deadline = hmi_now_ms() + timeout_ms;
    for (;;) {
        if (pthread_tryjoin_np(th, NULL) == 0) {
            t->valid = false;
            return true;
        }
        if (hmi_now_ms() >= deadline) return false;
        hmi_sleep_ms(1);
    }
}

void hmi_flag_set(hmi_atomic_flag_t *f, int32_t value) { __atomic_store_n(&f->v, value, __ATOMIC_SEQ_CST); }
int32_t hmi_flag_get(hmi_atomic_flag_t *f) { return __atomic_load_n(&f->v, __ATOMIC_SEQ_CST); }

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    bool set;
} posix_event_t;
typedef char hmi_event_size_check[(sizeof(posix_event_t) <= sizeof(hmi_event_t)) ? 1 : -1];

bool hmi_event_init(hmi_event_t *e)
{
    posix_event_t *p = (posix_event_t *)e;
    pthread_condattr_t at;
    memset(e, 0, sizeof(*e));
    if (pthread_mutex_init(&p->m, NULL) != 0) return false;
    if (pthread_condattr_init(&at) != 0) {
        pthread_mutex_destroy(&p->m);
        return false;
    }
    bool ok = pthread_condattr_setclock(&at, CLOCK_MONOTONIC) == 0 && pthread_cond_init(&p->c, &at) == 0;
    pthread_condattr_destroy(&at);
    if (!ok) pthread_mutex_destroy(&p->m);
    return ok;
}

void hmi_event_signal(hmi_event_t *e)
{
    posix_event_t *p = (posix_event_t *)e;
    pthread_mutex_lock(&p->m);
    p->set = true;
    pthread_cond_signal(&p->c);
    pthread_mutex_unlock(&p->m);
}

bool hmi_event_wait(hmi_event_t *e, uint32_t timeout_ms)
{
    posix_event_t *p = (posix_event_t *)e;
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += (time_t)(timeout_ms / 1000u);
    dl.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&p->m);
    while (!p->set) {
        if (pthread_cond_timedwait(&p->c, &p->m, &dl) == ETIMEDOUT) break;
    }
    bool got = p->set;
    p->set = false;
    pthread_mutex_unlock(&p->m);
    return got;
}

void hmi_event_destroy(hmi_event_t *e)
{
    posix_event_t *p = (posix_event_t *)e;
    pthread_cond_destroy(&p->c);
    pthread_mutex_destroy(&p->m);
}

static bool write_all(int fd, const void *data, size_t len)
{
    const char *p = (const char *)data;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

bool hmi_fs_atomic_write(const char *path, const void *data, size_t len)
{
    char tmp[512], dir[512];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = write_all(fd, data, len) && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    /* fsync the directory so the rename itself survives power loss */
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0'; else strcpy(dir, ".");
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return true;
}

bool hmi_fs_append_sync(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return false;
    bool ok = write_all(fd, data, len) && fsync(fd) == 0;
    close(fd);
    return ok;
}

hmi_fs_read_t hmi_fs_read_all(const char *path, void *buf, size_t cap, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? HMI_FS_READ_ABSENT : HMI_FS_READ_ERROR;
    size_t got = 0;
    hmi_fs_read_t res = HMI_FS_READ_OK;
    for (;;) {
        if (got == cap) { /* one more byte means the file is longer than cap */
            char extra;
            ssize_t e = read(fd, &extra, 1);
            if (e < 0 && errno == EINTR) continue;
            if (e < 0) res = HMI_FS_READ_ERROR;
            else if (e > 0) res = HMI_FS_READ_TOO_BIG;
            break;
        }
        ssize_t n = read(fd, (char *)buf + got, cap - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            res = HMI_FS_READ_ERROR;
            break;
        }
        if (n == 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (res == HMI_FS_READ_OK && len) *len = got;
    return res;
}

hmi_fs_read_t hmi_fs_read_tail(const char *path, void *buf, size_t cap, size_t *len, bool *truncated)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? HMI_FS_READ_ABSENT : HMI_FS_READ_ERROR;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        close(fd);
        return HMI_FS_READ_ERROR;
    }
    off_t size = st.st_size;
    off_t start = size > (off_t)cap ? size - (off_t)cap : 0;
    size_t want = (size_t)(size - start), got = 0;
    hmi_fs_read_t res = HMI_FS_READ_OK;
    while (got < want) {
        ssize_t n = pread(fd, (char *)buf + got, want - got, start + (off_t)got);
        if (n < 0) {
            if (errno == EINTR) continue;
            res = HMI_FS_READ_ERROR;
            break;
        }
        if (n == 0) break; /* the file shrank while reading: use what was read */
        got += (size_t)n;
    }
    close(fd);
    if (res == HMI_FS_READ_OK) {
        if (len) *len = got;
        if (truncated) *truncated = start > 0;
    }
    return res;
}

bool hmi_fs_mode(const char *path, uint32_t *mode)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    *mode = (uint32_t)(st.st_mode & 0777);
    return true;
}

bool hmi_fs_writable_by_me(const char *path)
{
    return access(path, W_OK) == 0;
}

bool hmi_fs_size(const char *path, uint64_t *size)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 0) return false;
    *size = (uint64_t)st.st_size;
    return true;
}

bool hmi_fs_rename(const char *from, const char *to)
{
    char dir[512];
    if (rename(from, to) != 0) return false;
    /* fsync the directory so the rename itself survives power loss */
    if (snprintf(dir, sizeof(dir), "%s", to) >= (int)sizeof(dir)) return true; /* renamed; only the flush is skipped */
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0'; else strcpy(dir, ".");
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return true;
}

bool hmi_wall_ms(uint64_t *unix_ms, bool *synced)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0) return false;
    *unix_ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    struct timex tx;
    memset(&tx, 0, sizeof(tx)); /* modes == 0: read only, the clock is not changed */
    int st = adjtimex(&tx);
    *synced = st >= 0 && st != TIME_ERROR && (tx.status & STA_UNSYNC) == 0;
    return true;
}

bool hmi_random_bytes(void *out, size_t n)
{
    char *p = (char *)out;
    while (n > 0) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

bool hmi_notify(const char *message)
{
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || !message || path[0] == '\0') return false;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    size_t plen = strlen(path);
    if (plen >= sizeof(sa.sun_path)) return false;
    memcpy(sa.sun_path, path, plen);
    if (sa.sun_path[0] == '@') sa.sun_path[0] = '\0'; /* Linux abstract namespace */
    socklen_t salen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + plen);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    ssize_t n = sendto(fd, message, strlen(message), MSG_NOSIGNAL | MSG_DONTWAIT, (struct sockaddr *)&sa, salen);
    close(fd);
    return n == (ssize_t)strlen(message);
}
