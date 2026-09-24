/* Uplink: TCP client to the cloud with automatic reconnect (DESIGN.md §6.3). Linux only. */
#define _GNU_SOURCE            /* for MSG_NOSIGNAL */
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "uplink.h"

#define RETRY_PERIOD_S  2.0

static struct sockaddr_in s_addr;          /* the server address */
static int    s_fd = -1;                   /* the TCP socket; -1 = DISCONNECTED */
static double s_last_try = -RETRY_PERIOD_S; /* so the first try happens at once */

/* Goal: remember the server address and ignore SIGPIPE. Does not connect yet.
 * In:   host = server IP, e.g. "127.0.0.1", port = e.g. 5000
 * Out:  0 on success, -1 if host is not a valid IP address */
int uplink_init(const char *host, uint16_t port)
{
    memset(&s_addr, 0, sizeof s_addr);
    s_addr.sin_family = AF_INET;
    s_addr.sin_port   = htons(port);               /* port in network byte order (big-endian) */
    if (inet_pton(AF_INET, host, &s_addr.sin_addr) != 1) {
        return -1;
    }

    /* Writing to a closed socket must return an error, not kill the whole gateway */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/* Goal: DISCONNECTED -> try connect(), at most once every 2 s.
 * In:   now = the current time in seconds (monotonic clock)
 * Out:  1 = just connected, 0 = nothing changed */
int uplink_tick(double now)
{
    if (s_fd >= 0 || now - s_last_try < RETRY_PERIOD_S) {
        return 0;                                  /* connected, or too soon to retry */
    }
    s_last_try = now;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    /* P1 limitation: connect() blocks. Instant on localhost; a product would use non-blocking. */
    if (connect(fd, (struct sockaddr *)&s_addr, sizeof s_addr) < 0) {
        close(fd);                                 /* server not there: try again in 2 s */
        return 0;
    }
    s_fd = fd;
    return 1;
}

/* Goal: tell whether we are connected.
 * In:   nothing
 * Out:  1 = connected, 0 = not */
int uplink_is_connected(void)
{
    return s_fd >= 0;
}

/* Goal: send ALL len bytes, looping over partial sends. On error, disconnect.
 * In:   data + len = the bytes, e.g. one 26-byte TELEMETRY frame
 * Out:  0 = all sent, -1 = error (now DISCONNECTED) */
int uplink_send(const uint8_t *data, size_t len)
{
    if (s_fd < 0) {
        return -1;
    }
    size_t sent = 0;
    while (sent < len) {                           /* send() may send only part of it */
        ssize_t n = send(s_fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;                          /* interrupted by a signal: try again */
            }
            uplink_close();                        /* connection broken */
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* Goal: give the socket's file descriptor for poll().
 * In:   nothing
 * Out:  the fd, or -1 if not connected */
int uplink_fd(void)
{
    return s_fd;
}

/* Goal: read what the cloud sent. recv() == 0 means it closed the connection.
 * In:   nothing
 * Out:  nothing returned; disconnects if the cloud closed or on error */
void uplink_handle_rx(void)
{
    uint8_t buf[256];
    ssize_t n = recv(s_fd, buf, sizeof buf, 0);
    if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
        uplink_close();                            /* 0 = the cloud closed the connection */
    }
    /* n > 0: P1 ignores data from the cloud. P2 feeds it to proto_parse_byte (commands). */
}

/* Goal: close the socket -> DISCONNECTED (safe to call twice).
 * In:   nothing
 * Out:  nothing returned */
void uplink_close(void)
{
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}
