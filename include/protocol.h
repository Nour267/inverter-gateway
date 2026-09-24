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
#define PROTO_ERR_ARG     -1   /* payload too big, or the output buffer is too small */

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

#endif /* PROTOCOL_H */
