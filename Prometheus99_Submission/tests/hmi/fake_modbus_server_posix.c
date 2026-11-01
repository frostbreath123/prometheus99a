/* POSIX (Linux) implementation of fake_modbus_server.h. Same behaviour as the
 * Win32 version in fake_modbus_server.c. Test tool only: loopback, single
 * client, no security.
 * STATUS: written to be compiled on Linux/POSIX. It has not been compiled in
 * the Windows development environment (no POSIX socket headers there). */
#define _GNU_SOURCE
#include "fake_modbus_server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

struct fake_modbus {
    int listen_s;
    volatile int client_s;
    volatile int stop;
    volatile int mode;
    volatile int exc;
    volatile uint32_t requests, writes;
    uint16_t hold[65536];
    uint16_t inp[65536];
    pthread_t thread;
};

#define LD(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define ST(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define INC(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)

static bool recv_all(int s, uint8_t *buf, int n, volatile int *stop)
{
    int got = 0;
    while (got < n) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        struct timeval tv = { 0, 50000 };
        int r = select(s + 1, &fds, NULL, NULL, &tv);
        if (LD(stop)) return false;
        if (r == 0 || (r < 0 && errno == EINTR)) continue;
        if (r < 0) return false;
        ssize_t k = recv(s, buf + got, (size_t)(n - got), 0);
        if (k <= 0) return false;
        got += (int)k;
    }
    return true;
}

static void send_frame(int s, uint16_t txid, uint8_t unit, const uint8_t *pdu, int pdu_len, bool truncate)
{
    uint8_t f[300];
    f[0] = (uint8_t)(txid >> 8);
    f[1] = (uint8_t)txid;
    f[2] = f[3] = 0;
    f[4] = (uint8_t)((pdu_len + 1) >> 8);
    f[5] = (uint8_t)(pdu_len + 1);
    f[6] = unit;
    memcpy(f + 7, pdu, (size_t)pdu_len);
    int total = 7 + pdu_len;
    (void)send(s, f, (size_t)(truncate ? total / 2 : total), MSG_NOSIGNAL);
}

static void serve_client(fake_modbus_t *m, int c)
{
    while (!LD(&m->stop)) {
        uint8_t h[7];
        if (!recv_all(c, h, 7, &m->stop)) return;
        int len = (h[4] << 8) | h[5];
        if (len < 2 || len > 254) return;
        uint8_t pdu[260];
        if (!recv_all(c, pdu, len - 1, &m->stop)) return;
        uint16_t txid = (uint16_t)((h[0] << 8) | h[1]);
        INC(&m->requests);
        int mode = LD(&m->mode);
        uint8_t fc = pdu[0];
        if (mode == FMS_DROP_CONNECTION) return;
        if (mode == FMS_HANG) {
            while (!LD(&m->stop) && LD(&m->mode) == FMS_HANG) usleep(10000);
            return;
        }
        if (mode == FMS_EXCEPTION) {
            uint8_t r[2] = { (uint8_t)(fc | 0x80), (uint8_t)LD(&m->exc) };
            send_frame(c, txid, h[6], r, 2, false);
            continue;
        }
        uint8_t r[260];
        int rl = 0;
        if (fc == 3 || fc == 4) {
            int addr = (pdu[1] << 8) | pdu[2], n = (pdu[3] << 8) | pdu[4];
            if (n < 1 || n > 125 || addr + n > 65536) {
                r[0] = (uint8_t)(fc | 0x80);
                r[1] = 2;
                rl = 2;
            } else {
                r[0] = fc;
                r[1] = (uint8_t)(n * 2);
                const uint16_t *src = (fc == 3) ? m->hold : m->inp;
                for (int i = 0; i < n; i++) {
                    r[2 + 2 * i] = (uint8_t)(__atomic_load_n(&src[addr + i], __ATOMIC_RELAXED) >> 8);
                    r[3 + 2 * i] = (uint8_t)__atomic_load_n(&src[addr + i], __ATOMIC_RELAXED);
                }
                rl = 2 + 2 * n;
            }
        } else if (fc == 6) {
            int addr = (pdu[1] << 8) | pdu[2];
            __atomic_store_n(&m->hold[addr], (uint16_t)((pdu[3] << 8) | pdu[4]), __ATOMIC_RELAXED);
            INC(&m->writes);
            if (mode == FMS_APPLY_NO_REPLY) return;
            memcpy(r, pdu, 5);
            rl = 5;
        } else if (fc == 16) {
            int addr = (pdu[1] << 8) | pdu[2], n = (pdu[3] << 8) | pdu[4];
            for (int i = 0; i < n; i++) __atomic_store_n(&m->hold[addr + i], (uint16_t)((pdu[6 + 2 * i] << 8) | pdu[7 + 2 * i]), __ATOMIC_RELAXED);
            INC(&m->writes);
            if (mode == FMS_APPLY_NO_REPLY) return;
            memcpy(r, pdu, 5);
            rl = 5;
        } else {
            r[0] = (uint8_t)(fc | 0x80);
            r[1] = 1;
            rl = 2;
        }
        if (mode == FMS_BAD_TXID) txid ^= 0x5555;
        send_frame(c, txid, h[6], r, rl, mode == FMS_TRUNCATE);
        if (mode == FMS_TRUNCATE) return;
    }
}

static void *server_thread(void *p)
{
    fake_modbus_t *m = (fake_modbus_t *)p;
    while (!LD(&m->stop)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m->listen_s, &fds);
        struct timeval tv = { 0, 50000 };
        if (select(m->listen_s + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        int c = accept(m->listen_s, NULL, NULL);
        if (c < 0) continue;
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ST(&m->client_s, c);
        serve_client(m, c);
        ST(&m->client_s, -1);
        close(c);
    }
    return NULL;
}

fake_modbus_t *fms_start(uint16_t *out_port)
{
    signal(SIGPIPE, SIG_IGN);
    fake_modbus_t *m = (fake_modbus_t *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->client_s = -1;
    m->listen_s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(m->listen_s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(m->listen_s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(m->listen_s, 2) != 0) {
        close(m->listen_s);
        free(m);
        return NULL;
    }
    socklen_t al = sizeof(a);
    getsockname(m->listen_s, (struct sockaddr *)&a, &al);
    *out_port = ntohs(a.sin_port);
    if (pthread_create(&m->thread, NULL, server_thread, m) != 0) {
        close(m->listen_s);
        free(m);
        return NULL;
    }
    return m;
}

void fms_stop(fake_modbus_t *s)
{
    if (!s) return;
    ST(&s->stop, 1);
    pthread_join(s->thread, NULL);
    close(s->listen_s);
    free(s);
}

void fms_set_mode(fake_modbus_t *s, fms_mode_t m, uint8_t exc)
{
    ST(&s->exc, (int)exc);
    ST(&s->mode, (int)m);
}
/* The registers are touched by the test thread and by the server thread: relaxed atomics, so there is no data race. */
void fms_set_holding(fake_modbus_t *s, uint16_t a, uint16_t v) { __atomic_store_n(&s->hold[a], v, __ATOMIC_RELAXED); }
uint16_t fms_get_holding(fake_modbus_t *s, uint16_t a) { return __atomic_load_n(&s->hold[a], __ATOMIC_RELAXED); }
void fms_set_input(fake_modbus_t *s, uint16_t a, uint16_t v) { __atomic_store_n(&s->inp[a], v, __ATOMIC_RELAXED); }
uint32_t fms_request_count(fake_modbus_t *s) { return LD(&s->requests); }
uint32_t fms_write_count(fake_modbus_t *s) { return LD(&s->writes); }

void fms_kick_client(fake_modbus_t *s)
{
    int c = LD(&s->client_s);
    if (c >= 0) shutdown(c, SHUT_RDWR);
}
