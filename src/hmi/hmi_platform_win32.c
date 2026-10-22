/* Win32 implementation of hmi_platform.h (host tests and desktop demo). */
#include "hmi_platform.h"
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>

typedef char hmi_mutex_size_check[(sizeof(CRITICAL_SECTION) <= sizeof(hmi_mutex_t)) ? 1 : -1];

uint64_t hmi_now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    uint64_t f = (uint64_t)freq.QuadPart, v = (uint64_t)c.QuadPart;
    return (v / f) * 1000u + ((v % f) * 1000u) / f;
}

void hmi_sleep_ms(uint32_t ms) { Sleep(ms); }

bool hmi_mutex_init(hmi_mutex_t *m)
{
    memset(m, 0, sizeof(*m));
    InitializeCriticalSection((CRITICAL_SECTION *)m);
    return true;
}
void hmi_mutex_lock(hmi_mutex_t *m) { EnterCriticalSection((CRITICAL_SECTION *)m); }
void hmi_mutex_unlock(hmi_mutex_t *m) { LeaveCriticalSection((CRITICAL_SECTION *)m); }
void hmi_mutex_destroy(hmi_mutex_t *m) { DeleteCriticalSection((CRITICAL_SECTION *)m); }

typedef struct {
    hmi_thread_fn fn;
    void *arg;
} thread_start_t;

static unsigned __stdcall thread_tramp(void *p)
{
    thread_start_t st = *(thread_start_t *)p;
    HeapFree(GetProcessHeap(), 0, p);
    st.fn(st.arg);
    return 0;
}

bool hmi_thread_start(hmi_thread_t *t, hmi_thread_fn fn, void *arg)
{
    thread_start_t *st = (thread_start_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(*st));
    if (!st) return false;
    st->fn = fn;
    st->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, thread_tramp, st, 0, NULL);
    if (h == 0) {
        HeapFree(GetProcessHeap(), 0, st);
        return false;
    }
    t->opaque[0] = (uint64_t)h;
    t->valid = true;
    return true;
}

bool hmi_thread_join(hmi_thread_t *t, uint32_t timeout_ms)
{
    if (!t->valid) return true;
    HANDLE h = (HANDLE)(uintptr_t)t->opaque[0];
    if (WaitForSingleObject(h, timeout_ms) != WAIT_OBJECT_0) return false;
    CloseHandle(h);
    t->valid = false;
    return true;
}

void hmi_flag_set(hmi_atomic_flag_t *f, int32_t value) { InterlockedExchange((volatile LONG *)&f->v, value); }
int32_t hmi_flag_get(hmi_atomic_flag_t *f) { return InterlockedCompareExchange((volatile LONG *)&f->v, 0, 0); }

bool hmi_event_init(hmi_event_t *e)
{
    memset(e, 0, sizeof(*e));
    HANDLE h = CreateEventA(NULL, FALSE, FALSE, NULL); /* auto-reset, initially clear */
    if (!h) return false;
    e->opaque[0] = (uint64_t)(uintptr_t)h;
    return true;
}

void hmi_event_signal(hmi_event_t *e) { SetEvent((HANDLE)(uintptr_t)e->opaque[0]); }

bool hmi_event_wait(hmi_event_t *e, uint32_t timeout_ms)
{
    return WaitForSingleObject((HANDLE)(uintptr_t)e->opaque[0], timeout_ms) == WAIT_OBJECT_0;
}

void hmi_event_destroy(hmi_event_t *e) { CloseHandle((HANDLE)(uintptr_t)e->opaque[0]); }

bool hmi_fs_atomic_write(const char *path, const void *data, size_t len)
{
    char tmp[512];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return false;
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(h, data, (DWORD)len, &wrote, NULL) && wrote == (DWORD)len && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileA(tmp);
        return false;
    }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp);
        return false;
    }
    return true;
}

bool hmi_fs_append_sync(const char *path, const void *data, size_t len)
{
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(h, data, (DWORD)len, &wrote, NULL) && wrote == (DWORD)len && FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

hmi_fs_read_t hmi_fs_read_all(const char *path, void *buf, size_t cap, size_t *len)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? HMI_FS_READ_ABSENT : HMI_FS_READ_ERROR;
    }
    size_t got = 0;
    hmi_fs_read_t res = HMI_FS_READ_OK;
    for (;;) {
        char extra;
        char *dst = got < cap ? (char *)buf + got : &extra;
        DWORD want = got < cap ? (DWORD)(cap - got) : 1u, n = 0;
        if (!ReadFile(h, dst, want, &n, NULL)) {
            res = HMI_FS_READ_ERROR;
            break;
        }
        if (n == 0) break;
        if (got >= cap) {
            res = HMI_FS_READ_TOO_BIG;
            break;
        }
        got += n;
    }
    CloseHandle(h);
    if (res == HMI_FS_READ_OK && len) *len = got;
    return res;
}

hmi_fs_read_t hmi_fs_read_tail(const char *path, void *buf, size_t cap, size_t *len, bool *truncated)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? HMI_FS_READ_ABSENT : HMI_FS_READ_ERROR;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        CloseHandle(h);
        return HMI_FS_READ_ERROR;
    }
    uint64_t size = (uint64_t)sz.QuadPart;
    uint64_t start = size > (uint64_t)cap ? size - (uint64_t)cap : 0;
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)start;
    if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) {
        CloseHandle(h);
        return HMI_FS_READ_ERROR;
    }
    size_t want = (size_t)(size - start), got = 0;
    hmi_fs_read_t res = HMI_FS_READ_OK;
    while (got < want) {
        DWORD n = 0;
        if (!ReadFile(h, (char *)buf + got, (DWORD)(want - got), &n, NULL)) {
            res = HMI_FS_READ_ERROR;
            break;
        }
        if (n == 0) break;
        got += n;
    }
    CloseHandle(h);
    if (res == HMI_FS_READ_OK) {
        if (len) *len = got;
        if (truncated) *truncated = start > 0;
    }
    return res;
}

bool hmi_fs_mode(const char *path, uint32_t *mode)
{
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return false;
    *mode = 0600u; /* no POSIX permission bits here */
    return true;
}

bool hmi_fs_writable_by_me(const char *path)
{
    (void)path; /* no per-user write test here; callers treat false as "unknown" */
    return false;
}

bool hmi_fs_size(const char *path, uint64_t *size)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return false;
    *size = ((uint64_t)d.nFileSizeHigh << 32) | (uint64_t)d.nFileSizeLow;
    return true;
}

bool hmi_fs_rename(const char *from, const char *to)
{
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool hmi_wall_ms(uint64_t *unix_ms, bool *synced)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 ns since 1601 */
    const uint64_t epoch_diff = 116444736000000000ull;                    /* 1601 to 1970 */
    if (t < epoch_diff) return false;
    *unix_ms = (t - epoch_diff) / 10000u;
    *synced = false; /* the development host does not report time-source status */
    return true;
}

/* RtlGenRandom (exported as SystemFunction036 by advapi32): the documented
 * way to get random bytes without a CryptoAPI context, and available to
 * MinGW without extra link flags. */
BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);

bool hmi_random_bytes(void *out, size_t n)
{
    unsigned char *p = (unsigned char *)out;
    while (n > 0) {
        ULONG chunk = n > 65536u ? 65536u : (ULONG)n;
        if (!SystemFunction036(p, chunk)) return false;
        p += chunk;
        n -= chunk;
    }
    return true;
}

bool hmi_notify(const char *message)
{
    (void)message; /* no supervisor protocol on the Windows development host */
    return false;
}
