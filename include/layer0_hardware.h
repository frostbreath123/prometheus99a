/**
 * @file layer0_hardware.h
 * @brief Layer 0: Hardware & I/O Abstraction Layer
 * 
 * Manages raw hardware interrupts, physical watchdog timer interface,
 * button GPIO pin registers, digitizer touch hardware, and display panel interface.
 */

#ifndef LAYER0_HARDWARE_H
#define LAYER0_HARDWARE_H

#include "config.h"

/* --- Raw Sensor & Live Host Hardware Telemetry Interface --- */
typedef struct {
    uint32_t raw_adc_temp;      /* Live CPU Temperature in millidegrees C (e.g. 45200 = 45.2 C) */
    uint32_t raw_adc_pressure;  /* Live SSD Read/Write total throughput in KB/s */
    uint32_t raw_adc_rpm;       /* Live Dynamic Cooling Fan RPM */
    uint32_t raw_adc_bus_v;     /* Live System CPU Load percentage (0 - 100) */
    uint32_t interrupt_counter; /* Total ISR trigger count */
    uint16_t cpu_load_pct;      /* Live Host CPU utilization (0 - 100%) */
    uint16_t ram_load_pct;      /* Live Host RAM utilization (0 - 100%) */
    uint32_t ssd_read_kb_s;     /* Live SSD Read Throughput in KB/s */
    uint32_t ssd_write_kb_s;    /* Live SSD Write Throughput in KB/s */
} raw_hardware_sensors_t;

/* --- Raw Hardware Touch Event --- */
typedef struct {
    bool is_pressed;
    int16_t x;
    int16_t y;
} raw_touch_data_t;

/* --- Function Declarations --- */
void layer0_hardware_init(void);
void layer0_read_raw_sensors(raw_hardware_sensors_t *sensors);
void layer0_pet_hardware_watchdog(void);
bool layer0_is_hardware_watchdog_tripped(void);
uint64_t layer0_get_system_time_ms(void);

#endif /* LAYER0_HARDWARE_H */
