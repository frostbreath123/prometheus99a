/* Test double for Layer 0: controllable clock and sensor values, real mutex. */
#include "../include/layer0_hardware.h"
#include "test_hooks.h"
#include <windows.h>

static volatile LONG64 s_now_ms = 0;
static CRITICAL_SECTION s_cs;
static int s_cs_init = 0;
static raw_hardware_sensors_t s_raw;
static uint32_t s_pets = 0;

void test_set_time(uint64_t ms) { InterlockedExchange64(&s_now_ms, (LONG64)ms); }
void test_advance_time(uint64_t ms) { InterlockedExchangeAdd64(&s_now_ms, (LONG64)ms); }
void test_set_raw(const raw_hardware_sensors_t *r) { s_raw = *r; }
uint32_t test_get_pets(void) { return s_pets; }
void test_reset_pets(void) { s_pets = 0; }

void layer0_hardware_init(void)
{
    if (!s_cs_init) { InitializeCriticalSection(&s_cs); s_cs_init = 1; }
}
void layer0_hardware_shutdown(void) {}
void layer0_read_raw_sensors(raw_hardware_sensors_t *s) { *s = s_raw; }
void layer0_pet_hardware_watchdog(void) { s_pets++; }
bool layer0_is_hardware_watchdog_tripped(void) { return false; }
uint64_t layer0_get_system_time_ms(void) { return (uint64_t)InterlockedCompareExchange64(&s_now_ms, 0, 0); }
void layer0_state_lock(void) { if (s_cs_init) EnterCriticalSection(&s_cs); }
void layer0_state_unlock(void) { if (s_cs_init) LeaveCriticalSection(&s_cs); }
