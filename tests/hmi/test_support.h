/* Shared fixtures for the per-module HMI core tests.
 *
 * A scripted mock PLC driver and a fake clock make every transition
 * deterministic. Each test_*.c file is its own program and prints its own
 * "N checks, M failed" line; `make test-hmi` runs all of them. */
#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H

#include "../../src/hmi/hmi_runtime.h"
#include "../../src/hmi/hmi_decode.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define SECTION(n) printf("%s\n", n)

/* Prints the summary line and returns the process exit code. */
int test_finish(void);

/* ---- fake clock ---- */
extern uint64_t g_now;

/* ---- mock PLC ---- */
typedef struct {
    uint16_t hold[256], inp[256];
    hmi_io_status_t connect_st;
    uint32_t connect_cost;
    hmi_io_status_t read_st;       /* applied to reads at fail_addr (or all if -1) */
    int fail_addr;
    uint8_t read_exc;
    hmi_io_status_t write_st;
    bool apply_on_fail;            /* write is applied even though reply is lost */
    bool corrupt_write;            /* PLC stores value+1 (read-back mismatch) */
    uint8_t write_exc;
    hmi_io_status_t readback_st;   /* status for the read right after a write; OK = normal */
    bool fail_next_read_after_write;
    uint32_t n_connect, n_read, n_write, n_close;
    uint32_t timeout_cost;
    uint32_t hit_addr[256];
} mock_t;

void mock_init(mock_t *m);
hmi_plc_driver_t mock_drv(mock_t *m);

/* ---- fixture: 4 tags, tag db, comms, writes, optional alarms ---- */
typedef struct {
    hmi_tag_cfg_t cfg[4];
    size_t n;
    hmi_tagdb_t db;
    hmi_comms_t c;
    hmi_writes_t w;
    mock_t m;
    hmi_alarm_t al;
} fx_t;

void make_tags(hmi_tag_cfg_t *t, size_t *n);
hmi_comms_cfg_t default_cc(void);
fx_t *fx_new(bool with_alarm);
void fx_free(fx_t *f, bool with_alarm);
hmi_wr_submit_t submit(fx_t *f, size_t tag, double eng, hmi_role_t role, uint32_t *id);
/* Run comms until fake time reaches t_end. Returns steps taken. */
int run_until(fx_t *f, uint64_t t_end);
hmi_tag_value_t val(fx_t *f, size_t i);

/* ---- config text used by the config and runtime tests ---- */
extern const char *GOOD_CFG;
bool parse(const char *s, hmi_app_cfg_t *c, char *err);

#endif /* TEST_SUPPORT_H */
