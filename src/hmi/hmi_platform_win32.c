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

bool hmi_notify(const char *message)
{
    (void)message; /* no supervisor protocol on the Windows development host */
    return false;
}
