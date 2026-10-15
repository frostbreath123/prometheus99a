/* Test helper (POSIX): stands in for systemd's notify socket.
 *   notify_listener <socket path> <seconds>
 * Binds a datagram socket, prints one line per message as
 *   "<ms since start> <message>" and exits after <seconds>. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", argv[1]);
    unlink(argv[1]);
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        perror("bind");
        return 3;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    long t0 = now_ms(), end = t0 + (long)(atof(argv[2]) * 1000.0);
    printf("listening\n");
    for (;;) {
        long left = end - now_ms();
        if (left <= 0) break;
        struct timeval tv = { left / 1000, (left % 1000) * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[256];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            printf("%ld %s\n", now_ms() - t0, buf);
        }
    }
    unlink(argv[1]);
    return 0;
}
