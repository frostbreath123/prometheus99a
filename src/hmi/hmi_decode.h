/**
 * @file hmi_decode.h
 * @brief Register <-> typed value conversion. Pure functions, no I/O.
 *
 * The authoritative stored value is the exact raw bit pattern (uint32_t).
 * Engineering values are derived from it; they are never the source of truth.
 */
#ifndef HMI_DECODE_H
#define HMI_DECODE_H

#include "hmi_types.h"

/* Number of 16-bit registers a type occupies (1 or 2). */
unsigned hmi_dtype_regs(hmi_dtype_t t);

/* Registers (wire order) -> raw 32-bit pattern, applying byte and word order.
 * For 16-bit types the pattern is zero-extended (u16) or the raw 16 bits. */
uint32_t hmi_regs_to_raw(const uint16_t *regs, hmi_dtype_t t, hmi_word_order_t wo, hmi_byte_order_t bo);

/* Inverse of hmi_regs_to_raw. */
void hmi_raw_to_regs(uint32_t raw, hmi_dtype_t t, hmi_word_order_t wo, hmi_byte_order_t bo, uint16_t *regs);

/* Raw pattern -> numeric value before scaling. Returns false for F32 NaN/Inf. */
bool hmi_raw_to_number(uint32_t raw, hmi_dtype_t t, double *out);

/* Numeric (unscaled) value -> raw pattern. Returns false when not representable
 * (out of range, NaN, Inf, or non-integral for integer types). */
bool hmi_number_to_raw(double v, hmi_dtype_t t, uint32_t *raw);

#endif /* HMI_DECODE_H */
