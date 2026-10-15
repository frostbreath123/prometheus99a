#include "hmi_decode.h"
#include <math.h>
#include <string.h>

unsigned hmi_dtype_regs(hmi_dtype_t t)
{
    return (t == HMI_DT_U32 || t == HMI_DT_I32 || t == HMI_DT_F32) ? 2u : 1u;
}

static uint16_t swap_bytes(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

uint32_t hmi_regs_to_raw(const uint16_t *regs, hmi_dtype_t t, hmi_word_order_t wo, hmi_byte_order_t bo)
{
    uint16_t r0 = regs[0];
    if (bo == HMI_BYTE_SWAPPED) r0 = swap_bytes(r0);
    if (hmi_dtype_regs(t) == 1u) return r0;

    uint16_t r1 = regs[1];
    if (bo == HMI_BYTE_SWAPPED) r1 = swap_bytes(r1);
    uint16_t hi = (wo == HMI_WORD_HIGH_FIRST) ? r0 : r1;
    uint16_t lo = (wo == HMI_WORD_HIGH_FIRST) ? r1 : r0;
    return ((uint32_t)hi << 16) | lo;
}

void hmi_raw_to_regs(uint32_t raw, hmi_dtype_t t, hmi_word_order_t wo, hmi_byte_order_t bo, uint16_t *regs)
{
    if (hmi_dtype_regs(t) == 1u) {
        uint16_t v = (uint16_t)(raw & 0xFFFFu);
        regs[0] = (bo == HMI_BYTE_SWAPPED) ? swap_bytes(v) : v;
        return;
    }
    uint16_t hi = (uint16_t)(raw >> 16), lo = (uint16_t)(raw & 0xFFFFu);
    uint16_t a = (wo == HMI_WORD_HIGH_FIRST) ? hi : lo;
    uint16_t b = (wo == HMI_WORD_HIGH_FIRST) ? lo : hi;
    if (bo == HMI_BYTE_SWAPPED) {
        a = swap_bytes(a);
        b = swap_bytes(b);
    }
    regs[0] = a;
    regs[1] = b;
}

bool hmi_raw_to_number(uint32_t raw, hmi_dtype_t t, double *out)
{
    switch (t) {
        case HMI_DT_U16: *out = (double)(uint16_t)raw; return true;
        case HMI_DT_I16: *out = (double)(int16_t)(uint16_t)raw; return true;
        case HMI_DT_U32: *out = (double)raw; return true;
        case HMI_DT_I32: *out = (double)(int32_t)raw; return true;
        case HMI_DT_F32: {
            float f;
            memcpy(&f, &raw, sizeof(f));
            if (isnan(f) || isinf(f)) return false;
            *out = (double)f;
            return true;
        }
    }
    return false;
}

bool hmi_number_to_raw(double v, hmi_dtype_t t, uint32_t *raw)
{
    if (isnan(v) || isinf(v)) return false;
    if (t == HMI_DT_F32) {
        if (v > 3.4028234663852886e38 || v < -3.4028234663852886e38) return false;
        float f = (float)v;
        memcpy(raw, &f, sizeof(f));
        return true;
    }
    if (v != floor(v)) return false;
    switch (t) {
        case HMI_DT_U16:
            if (v < 0.0 || v > 65535.0) return false;
            *raw = (uint32_t)v;
            return true;
        case HMI_DT_I16:
            if (v < -32768.0 || v > 32767.0) return false;
            *raw = (uint32_t)(uint16_t)(int16_t)v;
            return true;
        case HMI_DT_U32:
            if (v < 0.0 || v > 4294967295.0) return false;
            *raw = (uint32_t)v;
            return true;
        case HMI_DT_I32:
            if (v < -2147483648.0 || v > 2147483647.0) return false;
            *raw = (uint32_t)(int32_t)v;
            return true;
        default:
            return false;
    }
}
