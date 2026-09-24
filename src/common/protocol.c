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

/* Goal: glue 2 bytes, big-endian (high byte first), into one number.
 * In:   p = pointer to 2 bytes, e.g. 0F A0
 * Out:  the number, e.g. 0x0FA0 = 4000 */
static uint16_t get_u16_be(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* ---- Pack ---- */

/* Goal: write a telemetry record as the 16-byte TELEMETRY payload ("write the letter").
 *       Each value goes to its fixed offset (DESIGN.md §5.2), big-endian.
 * In:   t = the values, e.g. dc_voltage = 4000
 *       out = buffer of at least 16 bytes
 * Out:  the number of bytes written (always 16); out[8..9] = 0F A0 for 4000 */
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

/* Goal: wrap a payload in a full frame ("put the letter in the envelope"):
 *       AA 55 | version | msg_type | seq | payload_len | payload | CRC16
 * In:   msg_type = e.g. MSG_TELEMETRY, seq = frame number,
 *       payload + payload_len = the data (max 256 bytes),
 *       out = buffer for the frame, out_size = its size in bytes
 * Out:  the frame length (payload_len + 10), e.g. 26 for TELEMETRY,
 *       or PROTO_ERR_ARG if the payload is too big or the frame doesn't fit in out.
 *       Nothing is written on error. */
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

/* ---- Parser ---- */

/* Positions inside p->buf (buf starts AFTER the 2 SOF bytes) */
#define HDR_LEN          6   /* version, msg_type, seq(2), payload_len(2) */
#define HDR_VERSION      0
#define HDR_MSG_TYPE     1
#define HDR_SEQ          2
#define HDR_PAYLOAD_LEN  4

/* Goal: reset the parser to its start state (waiting for AA).
 *       Called at the start, after each finished frame, and after each error.
 * In:   p = the parser
 * Out:  nothing returned */
void proto_parser_init(proto_parser_t *p)
{
    p->state = PS_WAIT_SOF1;
    p->count = 0;
    p->payload_len = 0;
}

/* Goal: feed ONE received byte to the state machine (DESIGN.md §5.3).
 *       WAIT_SOF1 -> WAIT_SOF2 -> HEADER -> PAYLOAD -> CRC -> frame ready
 * In:   p = the parser (remembers the state between calls), byte = the next byte,
 *       out = where to copy a finished frame
 * Out:  PROTO_NEED_MORE, PROTO_FRAME_READY (*out filled), or PROTO_ERR_VERSION /
 *       PROTO_ERR_LEN / PROTO_ERR_CRC (the parser resets and looks for the next AA 55) */
int proto_parse_byte(proto_parser_t *p, uint8_t byte, proto_frame_t *out)
{
    switch (p->state) {

    case PS_WAIT_SOF1:
        if (byte == PROTO_SOF1) {
            p->state = PS_WAIT_SOF2;
        }
        /* anything else is garbage: skip it */
        return PROTO_NEED_MORE;

    case PS_WAIT_SOF2:
        if (byte == PROTO_SOF2) {
            p->state = PS_HEADER;
            p->count = 0;
        } else if (byte != PROTO_SOF1) {
            p->state = PS_WAIT_SOF1;       /* false start */
        }
        /* byte == AA: stay here, it may be the real start (AA AA 55) */
        return PROTO_NEED_MORE;

    case PS_HEADER:
        p->buf[p->count++] = byte;
        if (p->count < HDR_LEN) {
            return PROTO_NEED_MORE;
        }
        /* Whole header here: check it BEFORE reading any payload */
        if (p->buf[HDR_VERSION] != PROTO_VERSION) {
            proto_parser_init(p);
            return PROTO_ERR_VERSION;
        }
        p->payload_len = get_u16_be(&p->buf[HDR_PAYLOAD_LEN]);
        if (p->payload_len > PROTO_MAX_PAYLOAD) {
            proto_parser_init(p);
            return PROTO_ERR_LEN;
        }
        p->state = (p->payload_len > 0) ? PS_PAYLOAD : PS_CRC;
        return PROTO_NEED_MORE;

    case PS_PAYLOAD:
        p->buf[p->count++] = byte;
        if (p->count == (size_t)HDR_LEN + p->payload_len) {
            p->state = PS_CRC;
        }
        return PROTO_NEED_MORE;

    case PS_CRC: {
        p->buf[p->count++] = byte;
        size_t data_len = (size_t)HDR_LEN + p->payload_len;    /* what the CRC covers */
        if (p->count < data_len + PROTO_CRC_LEN) {
            return PROTO_NEED_MORE;
        }
        uint16_t received = get_u16_be(&p->buf[data_len]);
        uint16_t computed = crc16(p->buf, data_len);
        if (received != computed) {
            proto_parser_init(p);
            return PROTO_ERR_CRC;
        }
        /* Valid frame: copy it out, then get ready for the next one */
        out->msg_type    = p->buf[HDR_MSG_TYPE];
        out->seq         = get_u16_be(&p->buf[HDR_SEQ]);
        out->payload_len = p->payload_len;
        memcpy(out->payload, &p->buf[HDR_LEN], p->payload_len);
        proto_parser_init(p);
        return PROTO_FRAME_READY;
    }
    }

    /* not reached: every state returns above */
    proto_parser_init(p);
    return PROTO_NEED_MORE;
}
