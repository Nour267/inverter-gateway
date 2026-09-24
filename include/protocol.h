/* Uplink protocol, gateway -> cloud over TCP (DESIGN.md §5).
 * Frame: AA 55 | version | msg_type | seq (2) | payload_len (2) | payload | CRC16 (2)
 * All multi-byte fields are big-endian. The CRC covers version .. end of payload. */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define PROTO_SOF1         0xAA
#define PROTO_SOF2         0x55
#define PROTO_VERSION      1
#define PROTO_HEADER_LEN   8     /* SOF(2) + version + msg_type + seq(2) + payload_len(2) */
#define PROTO_CRC_LEN      2
#define PROTO_MAX_PAYLOAD  256
#define PROTO_MAX_FRAME    (PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD + PROTO_CRC_LEN)   /* 266 */

/* Message types */
#define MSG_TELEMETRY      0x01

#define TELEMETRY_PAYLOAD_LEN  16

/* Return codes */
#define PROTO_NEED_MORE    0   /* parser: frame not complete yet, keep feeding bytes */
#define PROTO_FRAME_READY  1   /* parser: a valid frame is in *out */
#define PROTO_ERR_ARG     -1   /* payload too big, or the output buffer is too small */
#define PROTO_ERR_VERSION -2   /* parser: unknown version */
#define PROTO_ERR_LEN     -3   /* parser: payload_len > 256 */
#define PROTO_ERR_CRC     -4   /* parser: CRC mismatch (damaged frame) */

/* One TELEMETRY record: the values from both CAN frames + a timestamp.
 * Values stay scaled integers, exactly as decoded from CAN. */
typedef struct {
    uint32_t timestamp;      /* Unix seconds */
    uint8_t  state;
    uint8_t  fault_code;
    int16_t  temperature;    /* 0.1 °C */
    uint16_t dc_voltage;     /* 0.1 V  */
    uint16_t dc_current;     /* 0.01 A */
    uint16_t ac_power;       /* 1 W    */
    uint16_t ac_voltage;     /* 0.1 V  */
} telemetry_t;

/* Goal: write a telemetry record as the 16-byte TELEMETRY payload ("write the letter").
 * In:   t = the values, out = buffer of at least TELEMETRY_PAYLOAD_LEN bytes
 * Out:  the number of bytes written (always 16) */
size_t proto_pack_telemetry(const telemetry_t *t, uint8_t *out);

/* Goal: wrap a payload in a full frame: SOF, header, payload, CRC ("put it in the envelope").
 * In:   msg_type, seq, payload + payload_len (max 256),
 *       out = buffer for the frame, out_size = its size in bytes
 * Out:  the frame length (payload_len + 10), or PROTO_ERR_ARG if the payload is too big
 *       or the frame doesn't fit in out. Nothing is written on error. */
int proto_pack_frame(uint8_t msg_type, uint16_t seq,
                     const uint8_t *payload, uint16_t payload_len,
                     uint8_t *out, size_t out_size);

/* ---- Parser (receiving side) ---- */

/* The parser's states (DESIGN.md §5.3) */
typedef enum {
    PS_WAIT_SOF1,     /* waiting for 0xAA */
    PS_WAIT_SOF2,     /* waiting for 0x55 */
    PS_HEADER,        /* reading version, msg_type, seq, payload_len (6 bytes) */
    PS_PAYLOAD,       /* reading payload_len bytes */
    PS_CRC            /* reading the 2 CRC bytes */
} proto_state_t;

/* A received, checked frame */
typedef struct {
    uint8_t  msg_type;
    uint16_t seq;
    uint16_t payload_len;
    uint8_t  payload[PROTO_MAX_PAYLOAD];
} proto_frame_t;

/* The parser's memory between bytes. Fixed size, no malloc. */
typedef struct {
    proto_state_t state;
    uint8_t  buf[PROTO_MAX_FRAME];   /* bytes after AA 55: header, payload, CRC */
    size_t   count;                  /* how many bytes are in buf so far */
    uint16_t payload_len;            /* from the header */
} proto_parser_t;

/* Goal: reset the parser to its start state (waiting for AA).
 * In:   p = the parser
 * Out:  nothing returned */
void proto_parser_init(proto_parser_t *p);

/* Goal: feed ONE received byte to the parser. Call it for every byte from recv().
 * In:   p = the parser, byte = the next byte, out = where to put a finished frame
 * Out:  PROTO_NEED_MORE (keep going), PROTO_FRAME_READY (*out is a valid frame),
 *       or an error (PROTO_ERR_VERSION / _LEN / _CRC). On error the parser
 *       resets itself and looks for the next AA 55. */
int proto_parse_byte(proto_parser_t *p, uint8_t byte, proto_frame_t *out);

#endif /* PROTOCOL_H */
