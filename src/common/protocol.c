/* Uplink protocol pack (DESIGN.md §5). Pure logic, no I/O. */
#include <string.h>
#include "protocol.h"
#include "crc16.h"

/* ---- Big-endian helpers (private to this file) ---- */

/* Goal: split one number into 2 bytes, big-endian (high byte first).
 * In:   out = where to write 2 bytes, v = the number, e.g. 4000 = 0x0FA0
 * Out:  nothing returned; out[0..1] = 0F A0 */
static void put_u16_be(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v >> 8);
    out[1] = (uint8_t)(v);
}

/* Goal: split one number into 4 bytes, big-endian (high byte first).
 * In:   out = where to write 4 bytes, v = the number, e.g. 0x12345678
 * Out:  nothing returned; out[0..3] = 12 34 56 78 */
static void put_u32_be(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)(v);
}

/* ---- Pack ---- */

size_t proto_pack_telemetry(const telemetry_t *t, uint8_t *out)
{
    put_u32_be(&out[0], t->timestamp);
    out[4] = t->state;
    out[5] = t->fault_code;
    put_u16_be(&out[6],  (uint16_t)t->temperature);   /* same bits: -50 -> FF CE */
    put_u16_be(&out[8],  t->dc_voltage);
    put_u16_be(&out[10], t->dc_current);
    put_u16_be(&out[12], t->ac_power);
    put_u16_be(&out[14], t->ac_voltage);
    return TELEMETRY_PAYLOAD_LEN;
}

int proto_pack_frame(uint8_t msg_type, uint16_t seq,
                     const uint8_t *payload, uint16_t payload_len,
                     uint8_t *out, size_t out_size)
{
    size_t frame_len = PROTO_HEADER_LEN + (size_t)payload_len + PROTO_CRC_LEN;

    /* 1. Safety checks BEFORE writing anything */
    if (payload_len > PROTO_MAX_PAYLOAD || frame_len > out_size) {
        return PROTO_ERR_ARG;
    }

    /* 2. Header */
    out[0] = PROTO_SOF1;
    out[1] = PROTO_SOF2;
    out[2] = PROTO_VERSION;
    out[3] = msg_type;
    put_u16_be(&out[4], seq);
    put_u16_be(&out[6], payload_len);

    /* 3. Payload */
    if (payload_len > 0) {
        memcpy(&out[PROTO_HEADER_LEN], payload, payload_len);
    }

    /* 4. CRC over version .. end of payload (everything after the 2 SOF bytes) */
    uint16_t crc = crc16(&out[2], (PROTO_HEADER_LEN - 2) + (size_t)payload_len);
    put_u16_be(&out[PROTO_HEADER_LEN + payload_len], crc);

    return (int)frame_len;
}
