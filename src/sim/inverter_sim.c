/* Inverter simulator (DESIGN.md §7): a fake inverter that sends CAN frames 0x100 and 0x101
 * every second on vcan0, following a sun curve over a short "day" (1 day = 2 minutes).
 *
 * Usage: ./build/inverter_sim [--bus vcan0] [--fault-at N] [--bad-frames]
 *
 * Note: this runs on a PC, so it uses float math freely. The real inverter would send the
 * same scaled integers; only the way it makes up the values differs. */
#define _DEFAULT_SOURCE        /* for sleep() */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bus.h"
#include "can_messages.h"

#define DAY_SECONDS     120        /* one simulated day = 2 minutes */
#define MAX_AC_POWER    8000.0     /* W, at noon */
#define EFFICIENCY      0.97       /* DC -> AC */
#define FAULT_CODE      17         /* an example fault code */

/* Goal: how strong the sun is right now, over the simulated day.
 * In:   t = seconds since start
 * Out:  0.0 at night, rising to 1.0 at noon, then back to 0.0 */
static double sun_level(unsigned int t)
{
    double f = (double)(t % DAY_SECONDS) / DAY_SECONDS;    /* 0.0 .. 1.0 through the day */
    if (f < 0.15 || f > 0.85) {
        return 0.0;                                         /* night */
    }
    return sin(M_PI * (f - 0.15) / 0.70);                   /* half a sine wave */
}

/* Goal: a small random number, to make the values look like real measurements.
 * In:   range = the maximum size of the noise
 * Out:  a random value between -range and +range */
static double noise(double range)
{
    return range * (2.0 * rand() / RAND_MAX - 1.0);
}

/* Goal: make up the inverter's values for one second of the simulated day.
 * In:   t = seconds since start, faulted = 1 if we are in FAULT,
 *       st / pw = the structs to fill
 * Out:  nothing returned; st and pw filled with scaled integers (ready to encode) */
static void simulate(unsigned int t, int faulted, inverter_status_t *st, inverter_power_t *pw)
{
    double sun   = faulted ? 0.0 : sun_level(t);
    double ac_w  = sun > 0.0 ? MAX_AC_POWER * sun + noise(50.0) : 0.0;
    if (ac_w < 0.0) ac_w = 0.0;
    double dc_v  = sun > 0.0 ? 350.0 + 100.0 * sun + noise(2.0) : 0.0;   /* panel voltage */
    double dc_a  = dc_v > 0.0 ? (ac_w / EFFICIENCY) / dc_v : 0.0;        /* P = V x I */
    double ac_v  = 230.0 + noise(1.5);                                   /* grid voltage */
    double temp  = 25.0 + 30.0 * sun + noise(0.3);                      /* heats up with power */

    /* Status */
    if (faulted) {
        st->state = INV_STATE_FAULT;
    } else if (sun <= 0.0) {
        st->state = INV_STATE_OFF;
    } else if (sun < 0.10) {
        st->state = INV_STATE_STARTING;
    } else {
        st->state = INV_STATE_PRODUCING;
    }
    st->fault_code  = faulted ? FAULT_CODE : 0;
    st->temperature = (int16_t)lround(temp * 10.0);     /* 0.1 °C */
    st->uptime      = t;

    /* Power: real units -> scaled integers */
    pw->dc_voltage = (uint16_t)lround(dc_v * 10.0);     /* 0.1 V  */
    pw->dc_current = (uint16_t)lround(dc_a * 100.0);    /* 0.01 A */
    pw->ac_power   = (uint16_t)lround(ac_w);            /* 1 W    */
    pw->ac_voltage = (uint16_t)lround(ac_v * 10.0);     /* 0.1 V  */
}

/* Goal: send one CAN frame through the bus layer (HAL).
 * In:   id = CAN ID, data = the bytes, len = how many (normally 8)
 * Out:  0 on success, -1 on error */
static int send_frame(uint32_t id, const uint8_t *data, uint8_t len)
{
    bus_frame_t f;
    memset(&f, 0, sizeof f);
    f.id  = id;
    f.len = len;
    memcpy(f.data, data, len);
    return bus_send(&f);
}

/* Goal: run the simulator: every second, make up values, encode them, send 0x100 + 0x101.
 * In:   command-line options (see the top of this file)
 * Out:  exit code 0 (runs until Ctrl+C), 1 on error */
int main(int argc, char **argv)
{
    const char *ifname = "vcan0";
    long fault_at = -1;            /* -1 = never */
    int bad_frames = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bus") == 0 && i + 1 < argc) {
            ifname = argv[++i];
        } else if (strcmp(argv[i], "--fault-at") == 0 && i + 1 < argc) {
            fault_at = atol(argv[++i]);
        } else if (strcmp(argv[i], "--bad-frames") == 0) {
            bad_frames = 1;
        } else {
            fprintf(stderr, "usage: %s [--bus vcan0] [--fault-at N] [--bad-frames]\n", argv[0]);
            return 1;
        }
    }

    if (bus_open(ifname) != 0) {
        fprintf(stderr, "cannot open %s (run: sudo ./scripts/setup_vcan.sh)\n", ifname);
        return 1;
    }
    printf("inverter_sim: sending on %s every 1 s (1 day = %d s). Ctrl+C to stop.\n",
           ifname, DAY_SECONDS);

    for (unsigned int t = 0; ; t++) {
        int faulted = (fault_at >= 0 && (long)t >= fault_at);
        inverter_status_t st;
        inverter_power_t  pw;
        uint8_t data[8];

        simulate(t, faulted, &st, &pw);

        can_encode_status(&st, data);
        send_frame(CAN_ID_INVERTER_STATUS, data, 8);

        can_encode_power(&pw, data);
        if (bad_frames && t % 10 == 5) {
            send_frame(CAN_ID_INVERTER_POWER, data, 7);          /* wrong DLC */
            printf("t=%3u  (sent a BAD frame: DLC 7)\n", t);
        } else if (bad_frames && t % 10 == 9) {
            data[0] = 0x98; data[1] = 0x3A;                       /* 15000 = 1500.0 V: too high */
            send_frame(CAN_ID_INVERTER_POWER, data, 8);
            printf("t=%3u  (sent a BAD frame: 1500.0 V)\n", t);
        } else {
            send_frame(CAN_ID_INVERTER_POWER, data, 8);
            printf("t=%3u  state=%u  %6.1f V DC  %5.2f A  %5u W  %5.1f V AC  %5.1f C\n",
                   t, (unsigned)st.state, pw.dc_voltage / 10.0, pw.dc_current / 100.0,
                   (unsigned)pw.ac_power, pw.ac_voltage / 10.0, st.temperature / 10.0);
        }
        fflush(stdout);
        sleep(1);
    }
}
