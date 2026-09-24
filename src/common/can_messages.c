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

/* Goal: glue 2 received bytes (little-endian, low byte first) into one number.
 * In:   p = pointer to the first of 2 bytes, e.g. A0 0F
 * Out:  the number as uint16_t, e.g. 0x0FA0 = 4000 */
static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Goal: glue 4 received bytes (little-endian, low byte first) into one number.
 *       Used for uptime, which is too big for 2 bytes.
 * In:   p = pointer to the first of 4 bytes, e.g. 10 0E 00 00
 * Out:  the number as uint32_t, e.g. 0x00000E10 = 3600 */
static uint32_t get_u32_le(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Goal: split one number into 2 bytes, little-endian (low byte first). Opposite of get_u16_le.
 * In:   out = where to write 2 bytes, v = the number, e.g. 4000 = 0x0FA0
 * Out:  nothing returned; out[0..1] = A0 0F */
static void put_u16_le(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v);
    out[1] = (uint8_t)(v >> 8);
}

/* Goal: split one number into 4 bytes, little-endian (low byte first). Opposite of get_u32_le.
 * In:   out = where to write 4 bytes, v = the number, e.g. 3600 = 0x00000E10
 * Out:  nothing returned; out[0..3] = 10 0E 00 00 */
static void put_u32_le(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v);
    out[1] = (uint8_t)(v >> 8);
    out[2] = (uint8_t)(v >> 16);
    out[3] = (uint8_t)(v >> 24);
}

/* ---- Decoders ---- */

/* Goal: turn the raw bytes of frame 0x100 INVERTER_STATUS into checked values
 *       (state, fault code, temperature, uptime).
 * In:   data = the frame's data bytes, len = how many arrived (must be 8),
 *       out  = the caller's struct to fill
 * Out:  CAN_OK (and *out filled), CAN_ERR_LEN (len != 8) or CAN_ERR_RANGE (bad value).
 *       On error, *out is not touched. */
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

/* Goal: turn the raw bytes of frame 0x101 INVERTER_POWER into checked values
 *       (DC voltage, DC current, AC power, AC voltage).
 * In:   data = the frame's data bytes, len = how many arrived (must be 8),
 *       out  = the caller's struct to fill
 * Out:  CAN_OK (and *out filled), CAN_ERR_LEN (len != 8) or CAN_ERR_RANGE (bad value).
 *       On error, *out is not touched. */
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

/* ---- Encoders (used by the simulator: the inverter side) ---- */

/* Goal: turn status values into the 8 data bytes of frame 0x100. Opposite of can_decode_status.
 * In:   s = the values, data = buffer of 8 bytes to fill
 * Out:  nothing returned; data = state, fault, temperature (2), uptime (4), little-endian */
void can_encode_status(const inverter_status_t *s, uint8_t data[8])
{
    data[0] = (uint8_t)s->state;
    data[1] = s->fault_code;
    put_u16_le(&data[2], (uint16_t)s->temperature);   /* same bits: -50 -> CE FF */
    put_u32_le(&data[4], s->uptime);
}

/* Goal: turn power values into the 8 data bytes of frame 0x101. Opposite of can_decode_power.
 * In:   p = the values, data = buffer of 8 bytes to fill
 * Out:  nothing returned; data = 4 values x 2 bytes, little-endian (4000 -> A0 0F) */
void can_encode_power(const inverter_power_t *p, uint8_t data[8])
{
    put_u16_le(&data[0], p->dc_voltage);
    put_u16_le(&data[2], p->dc_current);
    put_u16_le(&data[4], p->ac_power);
    put_u16_le(&data[6], p->ac_voltage);
}
