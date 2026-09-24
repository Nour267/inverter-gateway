/* Uplink: the TCP connection from the gateway to the cloud (DESIGN.md §6.3).
 * DISCONNECTED --(retry every 2 s)--> CONNECTED --(send error / peer closed)--> DISCONNECTED */
#ifndef UPLINK_H
#define UPLINK_H

#include <stddef.h>
#include <stdint.h>

/* Goal: remember the server address and ignore SIGPIPE. Does not connect yet.
 * In:   host = server IP, e.g. "127.0.0.1", port = e.g. 5000
 * Out:  0 on success, -1 if host is not a valid IP address */
int uplink_init(const char *host, uint16_t port);

/* Goal: if disconnected and 2 s have passed since the last try, try to connect.
 *       Call it on every loop turn.
 * In:   now = the current time in seconds (monotonic clock)
 * Out:  1 = just connected, 0 = nothing changed */
int uplink_tick(double now);

/* Goal: tell whether we are connected.
 * In:   nothing
 * Out:  1 = connected, 0 = not */
int uplink_is_connected(void);

/* Goal: send ALL len bytes (loops over partial sends). On error, disconnect.
 * In:   data + len = the bytes to send, e.g. one 26-byte TELEMETRY frame
 * Out:  0 = all sent, -1 = error (now disconnected) */
int uplink_send(const uint8_t *data, size_t len);

/* Goal: give the socket's file descriptor, so the main loop can poll() it.
 * In:   nothing
 * Out:  the fd, or -1 if not connected */
int uplink_fd(void);

/* Goal: handle data from the cloud. P1: read and ignore it (P2: commands).
 *       recv() == 0 means the cloud closed the connection -> disconnect.
 * In:   nothing
 * Out:  nothing returned */
void uplink_handle_rx(void);

/* Goal: close the connection (safe to call when not connected).
 * In:   nothing
 * Out:  nothing returned */
void uplink_close(void);

#endif /* UPLINK_H */
