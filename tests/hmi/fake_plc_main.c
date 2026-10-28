/* Test and measurement helper (POSIX): a standalone fake Modbus TCP PLC on 127.0.0.1.
 *
 *   fake_plc [seconds [step_ms [speed [stats_file]]]]
 *
 * Prints "port <n>" on stdout, then serves until the time is up or SIGTERM. step_ms is how often the data moves
 * (default 100) and speed multiplies how fast it moves (default 1: one cycle in 60 s); "fake_plc 600 10 40" is the
 * heavy-update load used by bench/measure_linux.sh. With stats_file, "requests=<n> writes=<n>" is rewritten there
 * about once a second, so a script can count what the client sent during an outage.
 *
 * The data matches config/hmi.example.json (TANK_LEVEL, TEMP, BATCH_COUNT, FLOW) and moves slowly so the
 * alarm limits are crossed now and then. Fault injection by signal, for recovery measurements:
 *   SIGUSR1  drop the client and refuse service (the connection is closed on every request)
 *   SIGUSR2  hang: read requests and never answer
 *   SIGHUP   back to normal service
 * Test tool only: single client, no security. */
#include "fake_modbus_server.h"
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop, g_mode_req;

static void on_term(int s) { (void)s; g_stop = 1; }
static void on_usr1(int s) { (void)s; g_mode_req = 1; }
static void on_usr2(int s) { (void)s; g_mode_req = 2; }
static void on_hup(int s) { (void)s; g_mode_req = 3; }

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 3600;
    int step_ms = argc > 2 ? atoi(argv[2]) : 100;
    double speed = argc > 3 ? atof(argv[3]) : 1.0;
    const char *stats_path = argc > 4 ? argv[4] : NULL;
    if (step_ms < 1) step_ms = 1;
    uint16_t port = 0;
    fake_modbus_t *s = fms_start(&port);
    if (!s) return 3;
    printf("port %u\n", (unsigned)port);
    fflush(stdout);

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, on_usr2);
    signal(SIGHUP, on_hup);

    time_t t0 = time(NULL);
    unsigned step = 0;
    while (!g_stop && time(NULL) - t0 < seconds) {
        double ph = (double)step * step_ms / 1000.0 / 60.0 * 6.283185307 * speed; /* one cycle per 60 s at speed 1 */
        /* TANK_LEVEL 11.0 .. 99.0 %; once an operator has written a setpoint it keeps that value, so the written value can be seen */
        if (fms_write_count(s) == 0) fms_set_holding(s, 0, (uint16_t)(550.0 + 440.0 * sin(ph)));
        fms_set_holding(s, 1, (uint16_t)(int16_t)(500.0 + 450.0 * sin(ph * 0.5)));  /* TEMP 5.0 .. 95.0 C */
        uint32_t batch = (uint32_t)((double)step * step_ms / 1000.0);   /* BATCH_COUNT counts seconds */
        fms_set_input(s, 100, (uint16_t)(batch >> 16));                      /* high word first */
        fms_set_input(s, 101, (uint16_t)(batch & 0xFFFFu));
        float flow = (float)(250.0 + 200.0 * sin(ph * 2.0));
        uint32_t fb;
        memcpy(&fb, &flow, 4);
        fms_set_holding(s, 10, (uint16_t)(fb & 0xFFFFu));                    /* low word first */
        fms_set_holding(s, 11, (uint16_t)(fb >> 16));
        int m = g_mode_req;
        if (m) {
            g_mode_req = 0;
            if (m == 1) { fms_set_mode(s, FMS_DROP_CONNECTION, 0); fms_kick_client(s); }
            else if (m == 2) fms_set_mode(s, FMS_HANG, 0);
            else fms_set_mode(s, FMS_NORMAL, 0);
        }
        if (stats_path && step % (1000u / (unsigned)step_ms ? 1000u / (unsigned)step_ms : 1u) == 0) {
            FILE *sf = fopen(stats_path, "w");
            if (sf) {
                fprintf(sf, "requests=%u writes=%u\n", (unsigned)fms_request_count(s), (unsigned)fms_write_count(s));
                fclose(sf);
            }
        }
        step++;
        usleep((useconds_t)step_ms * 1000u);
    }
    fprintf(stderr, "fake_plc: requests=%u writes=%u\n", (unsigned)fms_request_count(s), (unsigned)fms_write_count(s));
    fms_stop(s);
    return 0;
}
