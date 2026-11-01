/* Test helper (POSIX): a "PLC" that accepts TCP connections on 127.0.0.1 and
 * never answers. Exercises the NO_RESPONSE path of the runtime.
 *   hang_server <port> <seconds> */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)atoi(argv[1]));
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 8) != 0) {
        perror("bind/listen");
        return 3;
    }
    time_t end = time(NULL) + atoi(argv[2]);
    int conns[64], n = 0;
    while (time(NULL) < end) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(ls, &rs);
        struct timeval tv = { 0, 200000 };
        if (select(ls + 1, &rs, NULL, NULL, &tv) > 0) {
            int c = accept(ls, NULL, NULL);
            if (c >= 0 && n < 64) conns[n++] = c; /* hold it open, never read or reply */
            else if (c >= 0) close(c);
        }
    }
    for (int i = 0; i < n; i++) close(conns[i]);
    close(ls);
    return 0;
}
