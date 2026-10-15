/* Tests for hmi_decode: types, byte and word order, scaling, limits.
 * Part of the HMI core host tests; shared fixtures are in test_support.h. */
#include "test_support.h"

static void test_decode(void)
{
    SECTION("decode: types, byte/word order, scaling, limits");
    uint16_t r[2];
    CHECK(hmi_regs_to_raw((uint16_t[]){0xFFFE}, HMI_DT_I16, HMI_WORD_HIGH_FIRST, HMI_BYTE_BIG) == 0xFFFE);
    double d;
    CHECK(hmi_raw_to_number(0xFFFE, HMI_DT_I16, &d) && d == -2.0);
    CHECK(hmi_raw_to_number(0xFFFE, HMI_DT_U16, &d) && d == 65534.0);
    r[0] = 0x1234; r[1] = 0x5678;
    CHECK(hmi_regs_to_raw(r, HMI_DT_U32, HMI_WORD_HIGH_FIRST, HMI_BYTE_BIG) == 0x12345678u);
    CHECK(hmi_regs_to_raw(r, HMI_DT_U32, HMI_WORD_LOW_FIRST, HMI_BYTE_BIG) == 0x56781234u);
    CHECK(hmi_regs_to_raw(r, HMI_DT_U32, HMI_WORD_HIGH_FIRST, HMI_BYTE_SWAPPED) == 0x34127856u);
    CHECK(hmi_regs_to_raw(r, HMI_DT_U32, HMI_WORD_LOW_FIRST, HMI_BYTE_SWAPPED) == 0x78563412u);
    CHECK(hmi_raw_to_number(0x80000000u, HMI_DT_I32, &d) && d == -2147483648.0);
    CHECK(hmi_raw_to_number(0xFFFFFFFFu, HMI_DT_U32, &d) && d == 4294967295.0); /* exact, no float loss */
    float f = 3.25f;
    uint32_t fb;
    memcpy(&fb, &f, 4);
    CHECK(hmi_raw_to_number(fb, HMI_DT_F32, &d) && d == 3.25);
    CHECK(!hmi_raw_to_number(0x7FC00000u, HMI_DT_F32, &d)); /* NaN */
    CHECK(!hmi_raw_to_number(0x7F800000u, HMI_DT_F32, &d)); /* +Inf */
    /* round trip for all four orders */
    for (int wo = 0; wo < 2; wo++) {
        for (int bo = 0; bo < 2; bo++) {
            uint16_t rr[2];
            hmi_raw_to_regs(0xDEADBEEFu, HMI_DT_U32, (hmi_word_order_t)wo, (hmi_byte_order_t)bo, rr);
            CHECK(hmi_regs_to_raw(rr, HMI_DT_U32, (hmi_word_order_t)wo, (hmi_byte_order_t)bo) == 0xDEADBEEFu);
        }
    }
    uint32_t raw;
    CHECK(hmi_number_to_raw(-1.0, HMI_DT_I16, &raw) && raw == 0xFFFFu);
    CHECK(!hmi_number_to_raw(-1.0, HMI_DT_U16, &raw));
    CHECK(!hmi_number_to_raw(65536.0, HMI_DT_U16, &raw));
    CHECK(!hmi_number_to_raw(1.5, HMI_DT_U16, &raw));
    CHECK(!hmi_number_to_raw(NAN, HMI_DT_F32, &raw));
    CHECK(!hmi_number_to_raw(1e300, HMI_DT_F32, &raw));
    CHECK(hmi_number_to_raw(4294967295.0, HMI_DT_U32, &raw) && raw == 0xFFFFFFFFu);
    CHECK(!hmi_number_to_raw(4294967296.0, HMI_DT_U32, &raw));
}


int main(void)
{
    test_decode();
    return test_finish();
}
