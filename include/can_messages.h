/* CAN message map (DESIGN.md §4): IDs, decoded structs, decode functions.
 * Pure logic, no I/O: the caller gives raw bytes, we give back numbers. */
#ifndef CAN_MESSAGES_H
#define CAN_MESSAGES_H

#include <stdint.h>

/* CAN IDs */
#define CAN_ID_INVERTER_STATUS  0x100
#define CAN_ID_INVERTER_POWER   0x101

#define CAN_FRAME_LEN  8   /* all our frames have DLC = 8 */

/* Return codes */
#define CAN_OK           0
#define CAN_ERR_LEN     -1   /* DLC != 8 */
#define CAN_ERR_RANGE   -2   /* a value is outside its valid range */

/* Inverter state (byte 0 of 0x100) */
typedef enum {
    INV_STATE_OFF       = 0,
    INV_STATE_STARTING  = 1,
    INV_STATE_PRODUCING = 2,
    INV_STATE_FAULT     = 3
} inverter_state_t;

/* 0x100 INVERTER_STATUS, decoded. Values stay scaled integers. */
typedef struct {
    inverter_state_t state;
    uint8_t  fault_code;     /* 0 = no fault */
    int16_t  temperature;    /* 0.1 °C  (453 = 45.3 °C, -50 = -5.0 °C) */
    uint32_t uptime;         /* seconds */
} inverter_status_t;

/* 0x101 INVERTER_POWER, decoded. Values stay scaled integers. */
typedef struct {
    uint16_t dc_voltage;     /* 0.1 V   (4000 = 400.0 V) */
    uint16_t dc_current;     /* 0.01 A  (850  = 8.50 A)  */
    uint16_t ac_power;       /* 1 W     (3200 = 3200 W)  */
    uint16_t ac_voltage;     /* 0.1 V   (2305 = 230.5 V) */
} inverter_power_t;

/* Goal: turn the raw bytes of frame 0x100 into checked status values.
 * In:   data = the frame's data bytes, len = how many arrived (must be 8), out = struct to fill
 * Out:  CAN_OK (and *out filled), CAN_ERR_LEN or CAN_ERR_RANGE. On error, *out is not touched. */
int can_decode_status(const uint8_t *data, uint8_t len, inverter_status_t *out);

/* Goal: turn the raw bytes of frame 0x101 into checked power values.
 * In:   data = the frame's data bytes, len = how many arrived (must be 8), out = struct to fill
 * Out:  CAN_OK (and *out filled), CAN_ERR_LEN or CAN_ERR_RANGE. On error, *out is not touched. */
int can_decode_power(const uint8_t *data, uint8_t len, inverter_power_t *out);

#endif /* CAN_MESSAGES_H */
