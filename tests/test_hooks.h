#ifndef TEST_HOOKS_H
#define TEST_HOOKS_H
#include "../include/layer0_hardware.h"
void test_set_time(uint64_t ms);
void test_advance_time(uint64_t ms);
void test_set_raw(const raw_hardware_sensors_t *r);
uint32_t test_get_pets(void);
void test_reset_pets(void);
#endif
