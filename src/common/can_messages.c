/* CAN message decoding (DESIGN.md §4). Pure logic, no I/O. */
#include "can_messages.h"

/* Valid ranges from DESIGN.md §4 (scaled integer units) */
#define DC_VOLTAGE_MAX  10000   /* 1000.0 V */
#define DC_CURRENT_MAX   5000   /*   50.00 A */
#define AC_POWER_MAX    20000   /* 20000 W   */
#define AC_VOLTAGE_MAX   3000   /*  300.0 V  */
#define TEMP_MIN         -400   /*  -40.0 °C */
#define TEMP_MAX         1500   /*  150.0 °C */

/* ---- Little-endian helpers (private to this file) ---- */

/* Glue 2 received bytes (low byte first) into one number.
 * In: pointer to 2 bytes, e.g. A0 0F.  Out: uint16_t, e.g. 0x0FA0 = 4000. */
static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Glue 4 received bytes (low byte first) into one number (used for uptime).
 * In: pointer to 4 bytes, e.g. 10 0E 00 00.  Out: uint32_t, e.g. 0x00000E10 = 3600. */
static uint32_t get_u32_le(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ---- Decoders ---- */

/* Decode frame 0x100 INVERTER_STATUS.
 * In: 8 data bytes + their length.  Out: *out filled; returns CAN_OK, CAN_ERR_LEN or CAN_ERR_RANGE. */
int can_decode_status(const uint8_t *data, uint8_t len, inverter_status_t *out)
{
    /* 1. Check the length */
    if (len != CAN_FRAME_LEN) {
        return CAN_ERR_LEN;
    }

    /* 2. Check the state before storing it in the enum (valid: 0..3) */
    if (data[0] > INV_STATE_FAULT) {
        return CAN_ERR_RANGE;
    }

    /* 3. Glue the bytes into numbers (into a local copy first) */
    inverter_status_t s;
    s.state       = (inverter_state_t)data[0];
    s.fault_code  = data[1];                          /* any value is valid */
    s.temperature = (int16_t)get_u16_le(&data[2]);    /* signed: CE FF -> -50 */
    s.uptime      = get_u32_le(&data[4]);             /* any value is valid */

    /* 4. Check the temperature range */
    if (s.temperature < TEMP_MIN || s.temperature > TEMP_MAX) {
        return CAN_ERR_RANGE;
    }

    /* 5. All good: give the result to the caller */
    *out = s;
    return CAN_OK;
}

/* Decode frame 0x101 INVERTER_POWER.
 * In: 8 data bytes + their length.  Out: *out filled; returns CAN_OK, CAN_ERR_LEN or CAN_ERR_RANGE. */
int can_decode_power(const uint8_t *data, uint8_t len, inverter_power_t *out)
{
    /* 1. Check the length */
    if (len != CAN_FRAME_LEN) {
        return CAN_ERR_LEN;
    }

    /* 2. Glue the bytes into numbers (into a local copy first) */
    inverter_power_t p;
    p.dc_voltage = get_u16_le(&data[0]);
    p.dc_current = get_u16_le(&data[2]);
    p.ac_power   = get_u16_le(&data[4]);
    p.ac_voltage = get_u16_le(&data[6]);

    /* 3. Check the ranges */
    if (p.dc_voltage > DC_VOLTAGE_MAX || p.dc_current > DC_CURRENT_MAX ||
        p.ac_power   > AC_POWER_MAX   || p.ac_voltage > AC_VOLTAGE_MAX) {
        return CAN_ERR_RANGE;
    }

    /* 4. All good: give the result to the caller */
    *out = p;
    return CAN_OK;
}
