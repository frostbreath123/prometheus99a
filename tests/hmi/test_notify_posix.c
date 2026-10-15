/* POSIX test of hmi_notify (sd_notify sender) against an in-process datagram
 * socket standing in for systemd. Linux only. */
#include "test_support.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

static int listen_on(const char *path, bool abstract)
{
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    size_t n = strlen(path);
    memcpy(sa.sun_path, path, n);
    if (abstract) sa.sun_path[0] = '\0';
    if (!abstract) unlink(path);
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&sa, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n)) != 0) {
        close(fd);
        return -1;
    }
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static void test_notify(void)
{
    SECTION("notify: sd_notify datagrams reach the supervisor socket");
    char buf[64];

    unsetenv("NOTIFY_SOCKET");
    CHECK(!hmi_notify("READY=1"));                 /* no supervisor: harmless false */

    const char *path = "/tmp/p99_notify_test.sock";
    int fd = listen_on(path, false);
    CHECK(fd >= 0);
    setenv("NOTIFY_SOCKET", path, 1);
    CHECK(hmi_notify("READY=1"));
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    CHECK(n == 7 && memcmp(buf, "READY=1", 7) == 0);
    CHECK(hmi_notify("WATCHDOG=1"));
    n = recv(fd, buf, sizeof(buf) - 1, 0);
    CHECK(n == 10 && memcmp(buf, "WATCHDOG=1", 10) == 0);
    CHECK(!hmi_notify(NULL));
    close(fd);
    unlink(path);
    CHECK(!hmi_notify("WATCHDOG=1"));              /* socket gone: false, no crash, no SIGPIPE */

    /* abstract namespace: leading '@' */
    fd = listen_on("@p99_notify_abstract", true);
    CHECK(fd >= 0);
    setenv("NOTIFY_SOCKET", "@p99_notify_abstract", 1);
    CHECK(hmi_notify("STOPPING=1"));
    n = recv(fd, buf, sizeof(buf) - 1, 0);
    CHECK(n == 10 && memcmp(buf, "STOPPING=1", 10) == 0);
    close(fd);

    /* path too long for sockaddr_un is rejected, not truncated */
    char longp[300];
    memset(longp, 'x', sizeof(longp) - 1);
    longp[0] = '/';
    longp[sizeof(longp) - 1] = '\0';
    setenv("NOTIFY_SOCKET", longp, 1);
    CHECK(!hmi_notify("READY=1"));
    setenv("NOTIFY_SOCKET", "", 1);
    CHECK(!hmi_notify("READY=1"));
    unsetenv("NOTIFY_SOCKET");
}

int main(void)
{
    test_notify();
    return test_finish();
}
