/* Link with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free to count
 * heap calls made by the application, LVGL and libmodbus objects.
 * It does NOT see allocations performed inside the C runtime / OS libraries.
 * Portable: GCC/Clang atomics, no OS headers. */
#include <stddef.h>
#include <stdint.h>

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);

static volatile int32_t g_malloc, g_calloc, g_realloc, g_free;
#define COUNT(x) __atomic_add_fetch(&(x), 1, __ATOMIC_SEQ_CST)

void *__wrap_malloc(size_t n) { COUNT(g_malloc); return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { COUNT(g_calloc); return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { COUNT(g_realloc); return __real_realloc(p, n); }
void __wrap_free(void *p) { if (p) COUNT(g_free); __real_free(p); }

uint32_t alloc_count_total(void) { return (uint32_t)(g_malloc + g_calloc + g_realloc); }
uint32_t alloc_count_free(void) { return (uint32_t)g_free; }
