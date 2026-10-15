/**
 * @file plc_libmodbus.h
 * @brief hmi_plc_driver_t backed by libmodbus 3.1.10 (LGPL-2.1-or-later).
 */
#ifndef PLC_LIBMODBUS_H
#define PLC_LIBMODBUS_H

#include "hmi_driver.h"

typedef struct plc_libmodbus plc_libmodbus_t;

/* host must be a dotted IPv4 literal. Returns NULL on failure. */
plc_libmodbus_t *plc_libmodbus_create(const char *host, uint16_t port, uint8_t unit,
                                      uint32_t response_timeout_ms);
void plc_libmodbus_destroy(plc_libmodbus_t *p);
void plc_libmodbus_driver(plc_libmodbus_t *p, hmi_plc_driver_t *out);

#endif /* PLC_LIBMODBUS_H */
