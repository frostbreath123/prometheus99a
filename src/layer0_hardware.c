/**
 * @file layer0_hardware.c
 * @brief Layer 0: Live Host Hardware & OS Sensor Ingestion Implementation
 *
 * Captures real-time host metrics:
 *   - CPU Temperature via PDH ACPI thermal zone counter (\_TZ.TZ01\Temperature)
 *   - CPU Load % via Win32 GetSystemTimes
 *   - RAM Load % via Win32 GlobalMemoryStatusEx
 *   - SSD Read & Write Throughput via IOCTL_DISK_PERFORMANCE on \\.\PhysicalDrive0
 *   - Dynamic Fan RPM mapped to live thermal duty curve
 *
 * Uses dynamic DLL resolution for pdh.dll (no static import).
 * Host metrics are sampled at 10 Hz and cached; every channel carries a
 * SENSOR_VALID_* bit. Temperature and fan are valid only when a real source
 * (ASUS ATKACPI EC, or PDH thermal zone) answered. ATKACPI is ASUS-specific;
 * PDH thermal zone "TZ01" exists only on some machines. Nothing is simulated.
 */

#include "../include/layer0_hardware.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>
#include <mmsystem.h>

#ifndef IOCTL_DISK_PERFORMANCE
#define IOCTL_DISK_PERFORMANCE 0x00070020
#endif

#define ASUS_ATKACPI_DEVICE      "\\\\.\\ATKACPI"
#define ASUS_CONTROL_CODE        0x0022240C
#define ASUS_DSTS_METHOD         0x53545344
#define ASUS_DEVICE_CPU_FAN      0x00110013
#define ASUS_DEVICE_CPU_TEMP     0x00120094

static HANDLE s_atkacpi_handle = INVALID_HANDLE_VALUE;

static int32_t asus_acpi_query(uint32_t device_id)
{
    if (s_atkacpi_handle == INVALID_HANDLE_VALUE) return -1;
    struct {
        uint32_t method;
        uint32_t args_len;
        uint32_t dev_id;
        uint32_t flags;
    } req = { ASUS_DSTS_METHOD, 8, device_id, 0 };
    uint32_t out_buf[4] = {0};
    DWORD ret = 0;
    if (DeviceIoControl(s_atkacpi_handle, ASUS_CONTROL_CODE, &req, sizeof(req), out_buf, sizeof(out_buf), &ret, NULL)) {
        return (int32_t)out_buf[0] - 65536;
    }
    return -1;
}

typedef struct {
    LARGE_INTEGER BytesRead;
    LARGE_INTEGER BytesWritten;
    LARGE_INTEGER ReadTime;
    LARGE_INTEGER WriteTime;
    LARGE_INTEGER IdleTime;
    DWORD         ReadCount;
    DWORD         WriteCount;
    DWORD         QueueDepth;
    DWORD         SplitCount;
    LARGE_INTEGER QueryTime;
    DWORD         StorageDeviceNumber;
    WCHAR         StorageManagerName[8];
} DISK_PERFORMANCE_T;

/* PDH Types & Function Signatures for Dynamic Loading (Zero static link bloat) */
typedef LONG PDH_STATUS;
typedef HANDLE PDH_HQUERY;
typedef HANDLE PDH_HCOUNTER;

typedef struct {
    DWORD CStatus;
    double doubleValue;
} PDH_FMT_COUNTERVALUE_T;

typedef PDH_STATUS (WINAPI *pfnPdhOpenQueryA)(LPCSTR, DWORD_PTR, PDH_HQUERY*);
typedef PDH_STATUS (WINAPI *pfnPdhAddCounterA)(PDH_HQUERY, LPCSTR, DWORD_PTR, PDH_HCOUNTER*);
typedef PDH_STATUS (WINAPI *pfnPdhCollectQueryData)(PDH_HQUERY);
typedef PDH_STATUS (WINAPI *pfnPdhCloseQuery)(PDH_HQUERY);
typedef PDH_STATUS (WINAPI *pfnPdhGetFormattedCounterValue)(PDH_HCOUNTER, DWORD, LPDWORD, PDH_FMT_COUNTERVALUE_T*);

/* Cached QPC frequency */
static LARGE_INTEGER s_qpc_frequency;

static uint64_t s_boot_time_ms = 0;
static uint64_t s_last_hardware_pet_ms = 0;
static bool s_hardware_watchdog_tripped = false;
static uint32_t s_isr_counter = 0;
static CRITICAL_SECTION s_state_cs;
static bool s_cs_ready = false;
static bool s_timer_period_set = false;
static HMODULE s_pdh_module = NULL;

/* Host Hardware Sensor Interfaces */
static HANDLE s_disk_handle = INVALID_HANDLE_VALUE;
static PDH_HQUERY s_pdh_query = NULL;
static PDH_HCOUNTER s_pdh_thermal_counter = NULL;
static bool s_pdh_available = false;

static pfnPdhOpenQueryA fnPdhOpenQueryA = NULL;
static pfnPdhAddCounterA fnPdhAddCounterA = NULL;
static pfnPdhCollectQueryData fnPdhCollectQueryData = NULL;
static pfnPdhCloseQuery fnPdhCloseQuery = NULL;
static pfnPdhGetFormattedCounterValue fnPdhGetFormattedCounterValue = NULL;

/* Baselines for delta calculation */
static uint64_t s_last_sample_time_ms = 0;
static uint64_t s_last_idle_time = 0;
static uint64_t s_last_kernel_time = 0;
static uint64_t s_last_user_time = 0;
static int64_t  s_last_bytes_read = 0;
static int64_t  s_last_bytes_written = 0;

/* Cached sensor payload (updated at 10 Hz) */
static raw_hardware_sensors_t s_cached_sensors;

static inline uint64_t filetime_to_u64(const FILETIME *ft)
{
    return ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

uint64_t layer0_get_system_time_ms(void)
{
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    /* Split to avoid 64-bit overflow of count * 1000 on long uptimes. */
    uint64_t freq = (uint64_t)s_qpc_frequency.QuadPart;
    uint64_t c = (uint64_t)count.QuadPart;
    return (c / freq) * 1000u + ((c % freq) * 1000u) / freq;
}

void layer0_hardware_init(void)
{
    QueryPerformanceFrequency(&s_qpc_frequency);
    if (!s_cs_ready) {
        InitializeCriticalSection(&s_state_cs);
        s_cs_ready = true;
    }
    /* Without this the Windows scheduler tick is ~15.6 ms, so Sleep(1)
     * loops run at ~64 Hz instead of the nominal 1000 Hz. */
    if (timeBeginPeriod(1) == TIMERR_NOERROR) s_timer_period_set = true;
    s_boot_time_ms = layer0_get_system_time_ms();
    s_last_hardware_pet_ms = s_boot_time_ms;
    s_hardware_watchdog_tripped = false;
    s_isr_counter = 0;
    s_last_sample_time_ms = s_boot_time_ms;

    /* Initialize baseline CPU load counters */
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        s_last_idle_time = filetime_to_u64(&idle);
        s_last_kernel_time = filetime_to_u64(&kernel);
        s_last_user_time = filetime_to_u64(&user);
    }

    /* Open PhysicalDrive0 for SSD performance counters */
    s_disk_handle = CreateFileA("\\\\.\\PhysicalDrive0", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (s_disk_handle != INVALID_HANDLE_VALUE) {
        DISK_PERFORMANCE_T dp;
        DWORD ret = 0;
        if (DeviceIoControl(s_disk_handle, IOCTL_DISK_PERFORMANCE, NULL, 0, &dp, sizeof(dp), &ret, NULL)) {
            s_last_bytes_read = dp.BytesRead.QuadPart;
            s_last_bytes_written = dp.BytesWritten.QuadPart;
        }
    }

    /* Dynamically load pdh.dll for thermal zone sensor */
    HMODULE hPdh = LoadLibraryA("pdh.dll");
    s_pdh_module = hPdh;
    if (hPdh) {
        fnPdhOpenQueryA = (pfnPdhOpenQueryA)(void (*)(void))GetProcAddress(hPdh, "PdhOpenQueryA");
        fnPdhAddCounterA = (pfnPdhAddCounterA)(void (*)(void))GetProcAddress(hPdh, "PdhAddCounterA");
        fnPdhCollectQueryData = (pfnPdhCollectQueryData)(void (*)(void))GetProcAddress(hPdh, "PdhCollectQueryData");
        fnPdhGetFormattedCounterValue = (pfnPdhGetFormattedCounterValue)(void (*)(void))GetProcAddress(hPdh, "PdhGetFormattedCounterValue");
        fnPdhCloseQuery = (pfnPdhCloseQuery)(void (*)(void))GetProcAddress(hPdh, "PdhCloseQuery");

        if (fnPdhOpenQueryA && fnPdhAddCounterA && fnPdhCollectQueryData && fnPdhGetFormattedCounterValue) {
            if (fnPdhOpenQueryA(NULL, 0, &s_pdh_query) == 0) {
                if (fnPdhAddCounterA(s_pdh_query, "\\Thermal Zone Information(\\_TZ.TZ01)\\Temperature", 0, &s_pdh_thermal_counter) == 0) {
                    fnPdhCollectQueryData(s_pdh_query);
                    s_pdh_available = true;
                }
            }
        }
    }

    /* Open ASUS ATKACPI device for direct EC fan and thermal telemetry */
    s_atkacpi_handle = CreateFileA(ASUS_ATKACPI_DEVICE, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    /* No sensor is valid until a real reading is taken (no invented defaults). */
    memset(&s_cached_sensors, 0, sizeof(s_cached_sensors));

    printf("[LAYER 0 HAL] Host Sensors Ready\n");
}

void layer0_read_raw_sensors(raw_hardware_sensors_t *sensors)
{
    if (!sensors) return;

    s_isr_counter++;
    uint64_t now = layer0_get_system_time_ms();

    /* Decimate live host hardware sampling to 10 Hz (every 100 ms) */
    if ((now - s_last_sample_time_ms) >= 100) {
        uint64_t dt_ms = now - s_last_sample_time_ms;
        s_last_sample_time_ms = now;
        uint8_t valid = 0;

        /* 1. Live Host CPU Load Percentage */
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user)) {
            uint64_t cur_idle = filetime_to_u64(&idle);
            uint64_t cur_kernel = filetime_to_u64(&kernel);
            uint64_t cur_user = filetime_to_u64(&user);

            uint64_t idle_diff = cur_idle - s_last_idle_time;
            uint64_t kernel_diff = cur_kernel - s_last_kernel_time;
            uint64_t user_diff = cur_user - s_last_user_time;
            uint64_t total_diff = kernel_diff + user_diff;

            if (total_diff > 0 && total_diff >= idle_diff) {
                uint64_t active_diff = total_diff - idle_diff;
                s_cached_sensors.cpu_load_pct = (uint16_t)((active_diff * 100) / total_diff);
                valid |= SENSOR_VALID_CPU;
            }
            s_last_idle_time = cur_idle;
            s_last_kernel_time = cur_kernel;
            s_last_user_time = cur_user;
        }

        /* 2. Live Host RAM Load Percentage */
        MEMORYSTATUSEX mem;
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) {
            s_cached_sensors.ram_load_pct = (uint16_t)mem.dwMemoryLoad;
            valid |= SENSOR_VALID_RAM;
        }

        /* 3. Live SSD Read / Write Throughput */
        if (s_disk_handle != INVALID_HANDLE_VALUE) {
            DISK_PERFORMANCE_T dp;
            DWORD ret = 0;
            if (DeviceIoControl(s_disk_handle, IOCTL_DISK_PERFORMANCE, NULL, 0, &dp, sizeof(dp), &ret, NULL)) {
                if (dt_ms > 0) {
                    int64_t d_read = dp.BytesRead.QuadPart - s_last_bytes_read;
                    int64_t d_write = dp.BytesWritten.QuadPart - s_last_bytes_written;
                    if (d_read < 0) d_read = 0;
                    if (d_write < 0) d_write = 0;
                    s_cached_sensors.ssd_read_kb_s = (uint32_t)((d_read * 1000) / (dt_ms * 1024));
                    s_cached_sensors.ssd_write_kb_s = (uint32_t)((d_write * 1000) / (dt_ms * 1024));
                }
                s_last_bytes_read = dp.BytesRead.QuadPart;
                s_last_bytes_written = dp.BytesWritten.QuadPart;
                valid |= SENSOR_VALID_SSD;
            }
        }

        /* 4. Live CPU / Thermal Zone Temperature */
        uint32_t temp_mC = 0;
        int32_t asus_temp = asus_acpi_query(ASUS_DEVICE_CPU_TEMP);
        if (asus_temp > 20 && asus_temp < 115) {
            temp_mC = (uint32_t)(asus_temp * 1000);
        } else if (s_pdh_available && fnPdhCollectQueryData && fnPdhGetFormattedCounterValue) {
            fnPdhCollectQueryData(s_pdh_query);
            PDH_FMT_COUNTERVALUE_T val;
            DWORD type = 0;
            if (fnPdhGetFormattedCounterValue(s_pdh_thermal_counter, 0x00000200 /* PDH_FMT_DOUBLE */, &type, &val) == 0 && val.CStatus == 0) {
                if (val.doubleValue > 250.0 && val.doubleValue < 400.0) {
                    double c = val.doubleValue - 273.15;
                    temp_mC = (uint32_t)(c * 1000.0);
                }
            }
        }
        /* No synthesized fallback: if no real thermal source answered, the
         * channel is reported invalid so alarms and UI never act on a guess. */
        if (temp_mC != 0) valid |= SENSOR_VALID_TEMP;
        s_cached_sensors.raw_adc_temp = temp_mC;

        /* 5. CPU fan speed: ASUS EC query only. Anything else is invalid. */
        uint32_t fan_rpm = 0;
        int32_t asus_fan = asus_acpi_query(ASUS_DEVICE_CPU_FAN);
        if (asus_fan >= 0) {
            uint32_t raw_spd = (uint32_t)(asus_fan & 0xFFFF);
            if (raw_spd <= 120) {
                fan_rpm = raw_spd * 100;
                valid |= SENSOR_VALID_FAN;
            }
        }
        s_cached_sensors.raw_adc_rpm = fan_rpm;
        s_cached_sensors.valid_flags = valid;

        /* General Channel Mappings */
        s_cached_sensors.raw_adc_pressure = s_cached_sensors.ssd_read_kb_s + s_cached_sensors.ssd_write_kb_s;
        s_cached_sensors.raw_adc_bus_v = (uint32_t)(s_cached_sensors.cpu_load_pct * 100);
    }

    /* Output cached sensor values */
    *sensors = s_cached_sensors;
    sensors->interrupt_counter = s_isr_counter;
}

void layer0_hardware_shutdown(void)
{
    if (s_pdh_query && fnPdhCloseQuery) fnPdhCloseQuery(s_pdh_query);
    s_pdh_query = NULL;
    s_pdh_available = false;
    if (s_pdh_module) { FreeLibrary(s_pdh_module); s_pdh_module = NULL; }
    if (s_disk_handle != INVALID_HANDLE_VALUE) { CloseHandle(s_disk_handle); s_disk_handle = INVALID_HANDLE_VALUE; }
    if (s_atkacpi_handle != INVALID_HANDLE_VALUE) { CloseHandle(s_atkacpi_handle); s_atkacpi_handle = INVALID_HANDLE_VALUE; }
    if (s_timer_period_set) { timeEndPeriod(1); s_timer_period_set = false; }
}

void layer0_state_lock(void)
{
    if (s_cs_ready) EnterCriticalSection(&s_state_cs);
}

void layer0_state_unlock(void)
{
    if (s_cs_ready) LeaveCriticalSection(&s_state_cs);
}

void layer0_pet_hardware_watchdog(void)
{
    s_last_hardware_pet_ms = layer0_get_system_time_ms();
    s_hardware_watchdog_tripped = false;
}

bool layer0_is_hardware_watchdog_tripped(void)
{
    uint64_t now = layer0_get_system_time_ms();
    if ((now - s_last_hardware_pet_ms) > WATCHDOG_MAX_AGE_MS) {
        s_hardware_watchdog_tripped = true;
    }
    return s_hardware_watchdog_tripped;
}
