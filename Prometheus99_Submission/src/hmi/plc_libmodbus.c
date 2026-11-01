#include "plc_libmodbus.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <modbus.h>

struct plc_libmodbus {
    modbus_t *mb;
    bool connected;
    uint32_t response_timeout_ms;
};

plc_libmodbus_t *plc_libmodbus_create(const char *host, uint16_t port, uint8_t unit,
                                      uint32_t response_timeout_ms)
{
    plc_libmodbus_t *p = (plc_libmodbus_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->mb = modbus_new_tcp(host, port);
    if (!p->mb) {
        free(p);
        return NULL;
    }
    p->response_timeout_ms = response_timeout_ms;
    if (modbus_set_slave(p->mb, unit) != 0) {
        modbus_free(p->mb);
        free(p);
        return NULL;
    }
    return p;
}

void plc_libmodbus_destroy(plc_libmodbus_t *p)
{
    if (!p) return;
    if (p->connected) modbus_close(p->mb);
    modbus_free(p->mb);
    free(p);
}

static void set_timeouts(plc_libmodbus_t *p, uint32_t ms)
{
    uint32_t sec = ms / 1000u, usec = (ms % 1000u) * 1000u;
    modbus_set_response_timeout(p->mb, sec, usec);
    modbus_set_byte_timeout(p->mb, sec, usec);
}

/* Map a libmodbus failure to our status classes. errno carries either a
 * system error (ETIMEDOUT, ECONNRESET, ...) or 112345678+code for exception. */
static hmi_io_status_t classify(int err, uint8_t *exc)
{
    if (err >= MODBUS_ENOBASE && err < MODBUS_ENOBASE + 0x100) {
        int code = err - MODBUS_ENOBASE;
        /* Modbus exception codes 1..11 are valid exception replies */
        if (code >= 1 && code <= 11) {
            if (exc) *exc = (uint8_t)code;
            return HMI_IO_EXCEPTION;
        }
        switch (err) {
            case EMBBADCRC:
            case EMBBADDATA:
            case EMBBADEXC:
            case EMBMDATA:
            case EMBUNKEXC:
                return HMI_IO_PROTOCOL;
            default:
                return HMI_IO_PROTOCOL;
        }
    }
    if (err == ETIMEDOUT) return HMI_IO_TIMEOUT;
    return HMI_IO_TRANSPORT;
}

static hmi_io_status_t drv_connect(void *ctx, uint32_t timeout_ms)
{
    plc_libmodbus_t *p = (plc_libmodbus_t *)ctx;
    if (p->connected) {
        modbus_close(p->mb);
        p->connected = false;
    }
    /* libmodbus 3.1.10 bounds connect() by the response timeout. */
    set_timeouts(p, timeout_ms);
    if (modbus_connect(p->mb) != 0) return errno == ETIMEDOUT ? HMI_IO_TIMEOUT : HMI_IO_TRANSPORT;
    set_timeouts(p, p->response_timeout_ms);
    p->connected = true;
    return HMI_IO_OK;
}

static hmi_io_status_t drv_read(void *ctx, hmi_area_t area, uint16_t addr, uint16_t n, uint16_t *out,
                                uint8_t *exc)
{
    plc_libmodbus_t *p = (plc_libmodbus_t *)ctx;
    if (!p->connected) return HMI_IO_TRANSPORT;
    int rc = (area == HMI_AREA_INPUT) ? modbus_read_input_registers(p->mb, addr, n, out)
                                      : modbus_read_registers(p->mb, addr, n, out);
    if (rc == (int)n) return HMI_IO_OK;
    if (rc >= 0) return HMI_IO_PROTOCOL; /* short reply */
    return classify(errno, exc);
}

static hmi_io_status_t drv_write(void *ctx, uint16_t addr, uint16_t n, const uint16_t *vals, uint8_t *exc)
{
    plc_libmodbus_t *p = (plc_libmodbus_t *)ctx;
    if (!p->connected) return HMI_IO_TRANSPORT;
    int rc = (n == 1) ? modbus_write_register(p->mb, addr, vals[0])
                      : modbus_write_registers(p->mb, addr, n, vals);
    if (rc == 1 || rc == (int)n) return HMI_IO_OK;
    if (rc >= 0) return HMI_IO_PROTOCOL;
    return classify(errno, exc);
}

static void drv_close(void *ctx)
{
    plc_libmodbus_t *p = (plc_libmodbus_t *)ctx;
    if (p->connected) {
        modbus_close(p->mb);
        p->connected = false;
    }
}

void plc_libmodbus_driver(plc_libmodbus_t *p, hmi_plc_driver_t *out)
{
    out->ctx = p;
    out->connect = drv_connect;
    out->read_regs = drv_read;
    out->write_regs = drv_write;
    out->close = drv_close;
}
