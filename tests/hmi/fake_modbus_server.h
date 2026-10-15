/* Minimal Modbus TCP server with fault injection, for end-to-end tests of
 * the real libmodbus driver. Test tool only: single client, no security. */
#ifndef FAKE_MODBUS_SERVER_H
#define FAKE_MODBUS_SERVER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FMS_NORMAL = 0,
    FMS_DROP_CONNECTION,   /* close the socket on receiving a request */
    FMS_HANG,              /* read the request, never answer */
    FMS_TRUNCATE,          /* send half of the reply, then close */
    FMS_EXCEPTION,         /* answer fc|0x80 with exception_code */
    FMS_BAD_TXID,          /* answer with a wrong transaction id */
    FMS_APPLY_NO_REPLY     /* writes: apply, then close without replying */
} fms_mode_t;

typedef struct fake_modbus fake_modbus_t;

fake_modbus_t *fms_start(uint16_t *out_port);   /* binds 127.0.0.1, port 0 */
void fms_stop(fake_modbus_t *s);
void fms_set_mode(fake_modbus_t *s, fms_mode_t m, uint8_t exception_code);
void fms_set_holding(fake_modbus_t *s, uint16_t addr, uint16_t v);
uint16_t fms_get_holding(fake_modbus_t *s, uint16_t addr);
void fms_set_input(fake_modbus_t *s, uint16_t addr, uint16_t v);
uint32_t fms_request_count(fake_modbus_t *s);
uint32_t fms_write_count(fake_modbus_t *s);
void fms_kick_client(fake_modbus_t *s); /* close current client socket */

#endif
