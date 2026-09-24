/* Gateway main program (DESIGN.md §6): read CAN -> decode -> keep latest values ->
 * every 1 s combine + pack a TELEMETRY frame -> send it to the cloud over TCP.
 * Single thread, event loop with poll().
 *
 * Usage: ./build/gateway [--bus vcan0] [--server 127.0.0.1:5000] */
#define _GNU_SOURCE            /* for clock_gettime, sigaction */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bus.h"
#include "can_messages.h"
#include "protocol.h"
#include "uplink.h"

#define POLL_TIMEOUT_MS     100
#define TELEMETRY_PERIOD_S  1.0
#define STALE_AFTER_S       3.0     /* older CAN data is not sent */
#define COUNTERS_PERIOD_S   10.0

/* Set to 0 by Ctrl+C. volatile sig_atomic_t = safe to change inside a signal handler */
static volatile sig_atomic_t running = 1;

/* Counters (DESIGN.md §6.6) */
static struct {
    unsigned long can_rx;             /* all CAN frames received */
    unsigned long can_bad_len;        /* DLC != 8 */
    unsigned long can_unknown_id;     /* not 0x100 / 0x101 */
    unsigned long can_out_of_range;   /* a value outside its valid range */
    unsigned long tx_frames;          /* TELEMETRY frames sent to the cloud */
    unsigned long tx_dropped;         /* built but not sent: not connected (P2: ring buffer) */
    unsigned long reconnects;         /* successful connects to the cloud */
} cnt;

/* The latest values from each CAN message, with the time they arrived (0 = never) */
static inverter_status_t last_status;
static inverter_power_t  last_power;
static double status_time = 0.0;
static double power_time  = 0.0;

static uint16_t seq = 0;              /* frame number for the uplink */

/* ---- Helpers ---- */

/* Goal: the current time in seconds, from a clock that never jumps back (for timers).
 * In:   nothing
 * Out:  seconds since some fixed point, e.g. 12345.678 */
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Goal: print one log line to stderr: [time] [LEVEL] message (DESIGN.md §6.6).
 * In:   level = "INFO" / "WARN" / "ERROR", fmt + ... = like printf
 * Out:  nothing returned */
static void log_msg(const char *level, const char *fmt, ...)
{
    char stamp[16];
    time_t t = time(NULL);
    strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&t));
    fprintf(stderr, "[%s] [%s] ", stamp, level);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* Goal: Ctrl+C / kill handler: ask the main loop to stop.
 * In:   sig = the signal number (not used)
 * Out:  nothing returned; running = 0 */
static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* Goal: print all counters (every 10 s and on exit).
 * In:   nothing
 * Out:  nothing returned */
static void print_counters(void)
{
    log_msg("INFO", "counters: can_rx=%lu can_bad_len=%lu can_unknown_id=%lu "
            "can_out_of_range=%lu tx_frames=%lu tx_dropped=%lu reconnects=%lu",
            cnt.can_rx, cnt.can_bad_len, cnt.can_unknown_id, cnt.can_out_of_range,
            cnt.tx_frames, cnt.tx_dropped, cnt.reconnects);
}

/* ---- CAN side ---- */

/* Goal: read ALL waiting CAN frames, decode + check them (M1), keep the latest values.
 *       Bad frames are dropped and counted (DESIGN.md §4 decoding rules).
 * In:   nothing (reads from the bus layer)
 * Out:  nothing returned; last_status / last_power and the counters updated */
static void handle_can_frames(void)
{
    bus_frame_t f;

    while (bus_recv(&f) == 1) {            /* 1 = got a frame; 0 = no more waiting */
        cnt.can_rx++;
        int rc;

        if (f.id == CAN_ID_INVERTER_STATUS) {
            inverter_status_t s;
            rc = can_decode_status(f.data, f.len, &s);
            if (rc == CAN_OK) {
                last_status = s;
                status_time = now_s();
            }
        } else if (f.id == CAN_ID_INVERTER_POWER) {
            inverter_power_t p;
            rc = can_decode_power(f.data, f.len, &p);
            if (rc == CAN_OK) {
                last_power = p;
                power_time = now_s();
            }
        } else {
            cnt.can_unknown_id++;          /* not ours: ignore it */
            continue;
        }

        if (rc == CAN_ERR_LEN) {
            cnt.can_bad_len++;
            log_msg("WARN", "CAN 0x%03X dropped: wrong length %u", (unsigned)f.id, f.len);
        } else if (rc == CAN_ERR_RANGE) {
            cnt.can_out_of_range++;
            log_msg("WARN", "CAN 0x%03X dropped: value out of range", (unsigned)f.id);
        }
    }
}

/* ---- Telemetry ---- */

/* Goal: once per second, combine the latest STATUS + POWER into one TELEMETRY frame (M2)
 *       and send it to the cloud. If either is missing or older than 3 s, send nothing:
 *       old data is never sent as new.
 * In:   now = the current time from now_s()
 * Out:  nothing returned; counters updated */
static void send_telemetry(double now)
{
    if (status_time == 0.0 || power_time == 0.0 ||
        now - status_time > STALE_AFTER_S || now - power_time > STALE_AFTER_S) {
        log_msg("WARN", "inverter not responding: no fresh CAN data, nothing sent");
        return;
    }

    /* 1. Combine both CAN messages + a timestamp */
    telemetry_t t;
    t.timestamp   = (uint32_t)time(NULL);
    t.state       = (uint8_t)last_status.state;
    t.fault_code  = last_status.fault_code;
    t.temperature = last_status.temperature;
    t.dc_voltage  = last_power.dc_voltage;
    t.dc_current  = last_power.dc_current;
    t.ac_power    = last_power.ac_power;
    t.ac_voltage  = last_power.ac_voltage;

    /* 2. Pack: payload ("letter"), then frame ("envelope") */
    uint8_t payload[TELEMETRY_PAYLOAD_LEN];
    uint8_t frame[PROTO_MAX_FRAME];
    proto_pack_telemetry(&t, payload);
    int n = proto_pack_frame(MSG_TELEMETRY, seq++, payload, sizeof payload, frame, sizeof frame);
    if (n < 0) {
        log_msg("ERROR", "pack failed");
        return;
    }
    /* 3. Send it. Not connected -> dropped and counted (P2 will buffer it instead) */
    const char *result;
    if (uplink_send(frame, (size_t)n) == 0) {
        cnt.tx_frames++;
        result = "sent";
    } else {
        cnt.tx_dropped++;
        result = "NOT connected, dropped";
    }
    printf("TELEMETRY seq=%-5u state=%u  %6.1f V  %5.2f A  %5u W  %5.1f V AC  %5.1f C  -> %s\n",
           (unsigned)(seq - 1), (unsigned)t.state, t.dc_voltage / 10.0, t.dc_current / 100.0,
           (unsigned)t.ac_power, t.ac_voltage / 10.0, t.temperature / 10.0, result);
    fflush(stdout);
}

/* ---- Main ---- */

/* Goal: start the gateway and run the event loop until Ctrl+C.
 * In:   command-line options: [--bus vcan0] [--server 127.0.0.1:5000]
 * Out:  exit code 0 on a clean stop, 1 on a startup error */
int main(int argc, char **argv)
{
    const char *bus_name = "vcan0";
    char host[64] = "127.0.0.1";
    long port = 5000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bus") == 0 && i + 1 < argc) {
            bus_name = argv[++i];
        } else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            /* "IP:port" -> host + port */
            const char *arg = argv[++i];
            const char *colon = strchr(arg, ':');
            size_t host_len = colon ? (size_t)(colon - arg) : 0;
            if (!colon || host_len == 0 || host_len >= sizeof host) {
                fprintf(stderr, "--server must look like 127.0.0.1:5000\n");
                return 1;
            }
            memcpy(host, arg, host_len);
            host[host_len] = '\0';
            port = atol(colon + 1);
        } else {
            fprintf(stderr, "usage: %s [--bus vcan0] [--server 127.0.0.1:5000]\n", argv[0]);
            return 1;
        }
    }
    if (port <= 0 || port > 65535 || uplink_init(host, (uint16_t)port) != 0) {
        fprintf(stderr, "bad server address: %s:%ld\n", host, port);
        return 1;
    }

    /* Ctrl+C (SIGINT) and kill (SIGTERM) -> clean shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (bus_open(bus_name) != 0) {
        log_msg("ERROR", "cannot open %s (run: sudo ./scripts/setup_vcan.sh)", bus_name);
        return 1;
    }
    log_msg("INFO", "gateway started: CAN %s -> cloud %s:%ld. Ctrl+C to stop.",
            bus_name, host, port);

    double last_telemetry = now_s();
    double last_counters  = now_s();

    while (running) {
        /* 1. Connection: DISCONNECTED -> try again every 2 s */
        int was_connected = uplink_is_connected();
        if (uplink_tick(now_s())) {
            cnt.reconnects++;
            log_msg("INFO", "connected to the cloud");
        }

        /* 2. Sleep until CAN data or cloud data arrives, or 100 ms pass.
         *    fd = -1 (not connected) is ignored by poll(). */
        struct pollfd pfd[2] = {
            { .fd = bus_fd(),    .events = POLLIN },
            { .fd = uplink_fd(), .events = POLLIN },
        };
        int ready = poll(pfd, 2, POLL_TIMEOUT_MS);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;                   /* woken by Ctrl+C: loop checks running */
            }
            log_msg("ERROR", "poll failed: %s", strerror(errno));
            break;
        }

        /* 3. CAN data waiting? */
        if (ready > 0 && (pfd[0].revents & POLLIN)) {
            handle_can_frames();
        }

        /* 4. Cloud sent something, or closed the connection? */
        if (ready > 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            uplink_handle_rx();
        }

        /* 5. Timers */
        double now = now_s();
        if (now - last_telemetry >= TELEMETRY_PERIOD_S) {
            last_telemetry = now;
            send_telemetry(now);
        }
        if (was_connected && !uplink_is_connected()) {
            log_msg("WARN", "lost the cloud connection, retrying every 2 s");
        }
        if (now - last_counters >= COUNTERS_PERIOD_S) {
            last_counters = now;
            print_counters();
        }
    }

    /* Clean shutdown */
    log_msg("INFO", "stopping");
    print_counters();
    uplink_close();
    bus_close();
    return 0;
}
