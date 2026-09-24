/* Bus layer on Linux SocketCAN (DESIGN.md §3). Linux only.
 * CAN works like a network socket here: socket() -> bind() to "vcan0" -> read()/write(). */
#define _GNU_SOURCE            /* for SOCK_NONBLOCK */
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include "bus.h"

static int s_fd = -1;          /* the CAN socket; -1 = not open. static = private to this file */

/* Goal: open a raw CAN socket and attach it to one interface (like bind() for TCP).
 * In:   ifname = interface name, e.g. "vcan0"
 * Out:  0 on success, -1 on error (e.g. vcan0 doesn't exist) */
int bus_open(const char *ifname)
{
    /* 1. Create a CAN socket. NONBLOCK: read() returns at once if no frame is waiting */
    s_fd = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK, CAN_RAW);
    if (s_fd < 0) {
        return -1;
    }

    /* 2. Find the interface's number from its name ("vcan0" -> e.g. 3) */
    unsigned int ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        bus_close();
        return -1;
    }

    /* 3. Attach the socket to that interface */
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof addr);
    addr.can_family  = AF_CAN;
    addr.can_ifindex = (int)ifindex;
    if (bind(s_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        bus_close();
        return -1;
    }
    return 0;
}

/* Goal: send one frame: copy our bus_frame_t into Linux's struct can_frame and write it.
 * In:   frame = the frame to send
 * Out:  0 on success, -1 on error */
int bus_send(const bus_frame_t *frame)
{
    if (frame->len > 8) {
        return -1;
    }

    struct can_frame cf;
    memset(&cf, 0, sizeof cf);
    cf.can_id = frame->id & CAN_SFF_MASK;          /* keep the 11-bit standard ID */
    cf.len    = frame->len;
    memcpy(cf.data, frame->data, frame->len);

    /* One write() = exactly one CAN frame (not a byte stream like TCP) */
    if (write(s_fd, &cf, sizeof cf) != (ssize_t)sizeof cf) {
        return -1;
    }
    return 0;
}

/* Goal: take one received frame, if one is waiting, and copy it into our bus_frame_t.
 * In:   frame = where to put it
 * Out:  1 = got a frame, 0 = nothing waiting (or a frame type we ignore), -1 = error */
int bus_recv(bus_frame_t *frame)
{
    struct can_frame cf;
    ssize_t n = read(s_fd, &cf, sizeof cf);

    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;   /* 0 = nothing waiting */
    }
    if (n != (ssize_t)sizeof cf) {
        return -1;
    }

    /* We only use standard data frames: skip extended-ID, remote and error frames */
    if (cf.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) {
        return 0;
    }

    frame->id  = cf.can_id & CAN_SFF_MASK;
    frame->len = cf.len;
    memcpy(frame->data, cf.data, sizeof frame->data);
    return 1;
}

/* Goal: give the socket's file descriptor, for poll() in the main loop.
 * In:   nothing
 * Out:  the file descriptor, or -1 if not open */
int bus_fd(void)
{
    return s_fd;
}

/* Goal: close the socket (safe to call twice).
 * In:   nothing
 * Out:  nothing returned */
void bus_close(void)
{
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}
