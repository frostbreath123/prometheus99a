/**
 * @file hmi_driver.h
 * @brief PLC transport interface. Implementations: libmodbus TCP
 *        (plc_libmodbus.c) and scripted mocks in tests.
 *
 * Every call may block for up to its timeout. They are only ever called from
 * the comms thread, never while a tag/alarm/write lock is held.
 */
#ifndef HMI_DRIVER_H
#define HMI_DRIVER_H

#include "hmi_types.h"

typedef enum {
    HMI_IO_OK = 0,
    HMI_IO_TIMEOUT,     /* no (complete) response in time */
    HMI_IO_TRANSPORT,   /* socket error, reset, closed */
    HMI_IO_EXCEPTION,   /* valid Modbus exception reply (link is alive) */
    HMI_IO_PROTOCOL     /* malformed or mismatching reply */
} hmi_io_status_t;

typedef struct {
    void *ctx;
    hmi_io_status_t (*connect)(void *ctx, uint32_t timeout_ms);
    /* Read n registers. On HMI_IO_EXCEPTION *exc holds the exception code. */
    hmi_io_status_t (*read_regs)(void *ctx, hmi_area_t area, uint16_t addr, uint16_t n,
                                 uint16_t *out, uint8_t *exc);
    /* Write n holding registers (function 06 for n==1, 16 otherwise). */
    hmi_io_status_t (*write_regs)(void *ctx, uint16_t addr, uint16_t n, const uint16_t *vals,
                                  uint8_t *exc);
    void (*close)(void *ctx);
} hmi_plc_driver_t;

#endif /* HMI_DRIVER_H */
