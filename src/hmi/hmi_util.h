/**
 * @file hmi_util.h
 * @brief Two small helpers shared by the portable core and the tools.
 *
 * hmi_crc32 detects corruption of the configuration signature file and of the
 * serial file; it is not a security mechanism. hmi_wipe overwrites memory
 * that held a key, in a way the compiler may not remove. This is the only file
 * of the core, besides hmi_mac.c, that includes Monocypher.
 */
#ifndef HMI_UTIL_H
#define HMI_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 (IEEE, as zlib). */
uint32_t hmi_crc32(const uint8_t *p, size_t n);

/* Overwrite memory in a way the compiler may not remove. */
void hmi_wipe(void *p, size_t n);

#endif /* HMI_UTIL_H */
