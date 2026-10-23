#include "hmi_util.h"
#include "../../third_party/monocypher/monocypher.h"

uint32_t hmi_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

void hmi_wipe(void *p, size_t n)
{
    if (p && n) crypto_wipe(p, n);
}
