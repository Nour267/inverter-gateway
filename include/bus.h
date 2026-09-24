/* Bus layer: the HAL (DESIGN.md §3).
 * The gateway and the simulator only call these functions. They don't know whether
 * frames go to a real CAN chip, Linux SocketCAN (vcan0) or something else.
 * Implementation today: src/bus/bus_socketcan.c */
#ifndef BUS_H
#define BUS_H

#include <stdint.h>

/* One CAN frame, independent of any OS or hardware */
typedef struct {
    uint32_t id;       /* 11-bit standard CAN ID, e.g. 0x101 */
    uint8_t  len;      /* 0..8 data bytes (the DLC) */
    uint8_t  data[8];
} bus_frame_t;

/* Goal: open the bus and attach it to one interface.
 * In:   ifname = interface name, e.g. "vcan0"
 * Out:  0 on success, -1 on error (e.g. vcan0 doesn't exist) */
int bus_open(const char *ifname);

/* Goal: send one frame on the bus.
 * In:   frame = the frame to send (id, len, data)
 * Out:  0 on success, -1 on error */
int bus_send(const bus_frame_t *frame);

/* Goal: take one received frame, if one is waiting. Never blocks.
 * In:   frame = where to put the received frame
 * Out:  1 = got a frame, 0 = nothing waiting, -1 = error */
int bus_recv(bus_frame_t *frame);

/* Goal: give the bus's file descriptor, so a main loop can wait on it with poll().
 * In:   nothing
 * Out:  the file descriptor, or -1 if the bus isn't open */
int bus_fd(void);

/* Goal: close the bus.
 * In:   nothing
 * Out:  nothing returned */
void bus_close(void);

#endif /* BUS_H */
