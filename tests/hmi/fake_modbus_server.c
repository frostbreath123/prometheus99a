#include "fake_modbus_server.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>

struct fake_modbus {
    SOCKET listen_s;
    volatile SOCKET client_s;
    volatile LONG stop;
    volatile LONG mode;
    volatile LONG exc;
    volatile LONG requests, writes;
    uint16_t hold[65536];
    uint16_t inp[65536];
    HANDLE thread;
};

static bool recv_all(SOCKET s, uint8_t *buf, int n, volatile LONG *stop)
{
    int got = 0;
    while (got < n) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        struct timeval tv = { 0, 50000 };
        int r = select(0, &fds, NULL, NULL, &tv);
        if (InterlockedCompareExchange(stop, 0, 0)) return false;
        if (r == 0) continue;
        if (r < 0) return false;
        int k = recv(s, (char *)buf + got, n - got, 0);
        if (k <= 0) return false;
        got += k;
    }
    return true;
}

static void send_frame(SOCKET s, uint16_t txid, uint8_t unit, const uint8_t *pdu, int pdu_len, bool truncate)
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
    send(s, (const char *)f, truncate ? total / 2 : total, 0);
}

static void serve_client(fake_modbus_t *m, SOCKET c)
{
    while (!InterlockedCompareExchange(&m->stop, 0, 0)) {
        uint8_t h[7];
        if (!recv_all(c, h, 7, &m->stop)) return;
        int len = (h[4] << 8) | h[5];
        if (len < 2 || len > 254) return;
        uint8_t pdu[260];
        if (!recv_all(c, pdu, len - 1, &m->stop)) return;
        uint16_t txid = (uint16_t)((h[0] << 8) | h[1]);
        InterlockedIncrement(&m->requests);
        int mode = (int)InterlockedCompareExchange(&m->mode, 0, 0);
        uint8_t fc = pdu[0];
        if (mode == FMS_DROP_CONNECTION) return;
        if (mode == FMS_HANG) {
            while (!InterlockedCompareExchange(&m->stop, 0, 0) &&
                   (int)InterlockedCompareExchange(&m->mode, 0, 0) == FMS_HANG)
                Sleep(10);
            return;
        }
        if (mode == FMS_EXCEPTION) {
            uint8_t r[2] = { (uint8_t)(fc | 0x80), (uint8_t)InterlockedCompareExchange(&m->exc, 0, 0) };
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
                    r[2 + 2 * i] = (uint8_t)(src[addr + i] >> 8);
                    r[3 + 2 * i] = (uint8_t)src[addr + i];
                }
                rl = 2 + 2 * n;
            }
        } else if (fc == 6) {
            int addr = (pdu[1] << 8) | pdu[2];
            m->hold[addr] = (uint16_t)((pdu[3] << 8) | pdu[4]);
            InterlockedIncrement(&m->writes);
            if (mode == FMS_APPLY_NO_REPLY) return;
            memcpy(r, pdu, 5);
            rl = 5;
        } else if (fc == 16) {
            int addr = (pdu[1] << 8) | pdu[2], n = (pdu[3] << 8) | pdu[4];
            for (int i = 0; i < n; i++) m->hold[addr + i] = (uint16_t)((pdu[6 + 2 * i] << 8) | pdu[7 + 2 * i]);
            InterlockedIncrement(&m->writes);
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

static unsigned __stdcall server_thread(void *p)
{
    fake_modbus_t *m = (fake_modbus_t *)p;
    while (!InterlockedCompareExchange(&m->stop, 0, 0)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m->listen_s, &fds);
        struct timeval tv = { 0, 50000 };
        if (select(0, &fds, NULL, NULL, &tv) <= 0) continue;
        SOCKET c = accept(m->listen_s, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        m->client_s = c;
        serve_client(m, c);
        m->client_s = INVALID_SOCKET;
        closesocket(c);
    }
    return 0;
}

fake_modbus_t *fms_start(uint16_t *out_port)
{
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    fake_modbus_t *m = (fake_modbus_t *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->client_s = INVALID_SOCKET;
    m->listen_s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(m->listen_s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(m->listen_s, 2) != 0) {
        free(m);
        return NULL;
    }
    int al = sizeof(a);
    getsockname(m->listen_s, (struct sockaddr *)&a, &al);
    *out_port = ntohs(a.sin_port);
    m->thread = (HANDLE)_beginthreadex(NULL, 0, server_thread, m, 0, NULL);
    return m;
}

void fms_stop(fake_modbus_t *s)
{
    if (!s) return;
    InterlockedExchange(&s->stop, 1);
    WaitForSingleObject(s->thread, 3000);
    CloseHandle(s->thread);
    closesocket(s->listen_s);
    free(s);
}

void fms_set_mode(fake_modbus_t *s, fms_mode_t m, uint8_t exc)
{
    InterlockedExchange(&s->exc, exc);
    InterlockedExchange(&s->mode, (LONG)m);
}
void fms_set_holding(fake_modbus_t *s, uint16_t a, uint16_t v) { s->hold[a] = v; }
uint16_t fms_get_holding(fake_modbus_t *s, uint16_t a) { return s->hold[a]; }
void fms_set_input(fake_modbus_t *s, uint16_t a, uint16_t v) { s->inp[a] = v; }
uint32_t fms_request_count(fake_modbus_t *s) { return (uint32_t)InterlockedCompareExchange(&s->requests, 0, 0); }
uint32_t fms_write_count(fake_modbus_t *s) { return (uint32_t)InterlockedCompareExchange(&s->writes, 0, 0); }

void fms_kick_client(fake_modbus_t *s)
{
    SOCKET c = s->client_s;
    if (c != INVALID_SOCKET) shutdown(c, SD_BOTH);
}
