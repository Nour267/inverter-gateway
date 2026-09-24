/* Unit tests for protocol.c (pack + parse) */
#include "unity.h"
#include "protocol.h"
#include "crc16.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- proto_pack_telemetry ---- */

void test_pack_telemetry_is_big_endian(void)
{
    /* The values from our M1 CAN test frames */
    const telemetry_t t = {
        .timestamp = 0x12345678, .state = 2, .fault_code = 0, .temperature = 453,
        .dc_voltage = 4000, .dc_current = 850, .ac_power = 3200, .ac_voltage = 2305
    };
    const uint8_t expected[16] = {
        0x12, 0x34, 0x56, 0x78,   /* timestamp                      */
        0x02,                     /* state = PRODUCING              */
        0x00,                     /* fault_code                     */
        0x01, 0xC5,               /* temperature 453  = 45.3 °C     */
        0x0F, 0xA0,               /* dc_voltage  4000 = 400.0 V     */
        0x03, 0x52,               /* dc_current  850  = 8.50 A      */
        0x0C, 0x80,               /* ac_power    3200 W             */
        0x09, 0x01                /* ac_voltage  2305 = 230.5 V     */
    };
    uint8_t out[16];

    TEST_ASSERT_EQUAL(16, proto_pack_telemetry(&t, out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 16);
}

void test_pack_telemetry_negative_temperature(void)
{
    const telemetry_t t = { .temperature = -50 };   /* -5.0 °C */
    uint8_t out[16];

    proto_pack_telemetry(&t, out);
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[6]);
    TEST_ASSERT_EQUAL_HEX8(0xCE, out[7]);
}

/* ---- proto_pack_frame ---- */

void test_pack_frame_layout(void)
{
    const uint8_t payload[2] = {0xAB, 0xCD};
    uint8_t out[PROTO_MAX_FRAME];

    int n = proto_pack_frame(MSG_TELEMETRY, 0x1234, payload, 2, out, sizeof out);

    TEST_ASSERT_EQUAL_INT(12, n);                  /* 8 header + 2 payload + 2 CRC */
    const uint8_t expected_start[10] = {
        0xAA, 0x55,     /* SOF                */
        0x01,           /* version            */
        0x01,           /* msg_type TELEMETRY */
        0x12, 0x34,     /* seq, big-endian    */
        0x00, 0x02,     /* payload_len = 2    */
        0xAB, 0xCD      /* payload            */
    };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_start, out, 10);
}

void test_pack_frame_crc_covers_version_to_payload(void)
{
    const uint8_t payload[2] = {0xAB, 0xCD};
    uint8_t out[PROTO_MAX_FRAME];

    proto_pack_frame(MSG_TELEMETRY, 0x1234, payload, 2, out, sizeof out);

    /* CRC over bytes 2..9 (version .. end of payload), stored big-endian at 10..11 */
    uint16_t crc = crc16(&out[2], 8);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(crc >> 8), out[10]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)crc,        out[11]);
}

void test_pack_frame_empty_payload(void)
{
    uint8_t out[PROTO_MAX_FRAME];

    TEST_ASSERT_EQUAL_INT(10, proto_pack_frame(MSG_TELEMETRY, 0, NULL, 0, out, sizeof out));
}

void test_pack_frame_rejects_payload_too_big(void)
{
    static uint8_t payload[PROTO_MAX_PAYLOAD + 1];
    uint8_t out[PROTO_MAX_FRAME + 1];

    TEST_ASSERT_EQUAL_INT(PROTO_ERR_ARG,
        proto_pack_frame(MSG_TELEMETRY, 0, payload, 257, out, sizeof out));
}

void test_pack_frame_rejects_small_buffer(void)
{
    const uint8_t payload[16] = {0};
    uint8_t out[20];                               /* the frame needs 26 bytes */

    TEST_ASSERT_EQUAL_INT(PROTO_ERR_ARG,
        proto_pack_frame(MSG_TELEMETRY, 0, payload, 16, out, sizeof out));
}

/* ---- Parser ---- */

static proto_parser_t parser;
static proto_frame_t  frame;

/* Test helper: feed n bytes one at a time. Returns the result of the LAST byte,
 * or the first result that isn't PROTO_NEED_MORE (a frame or an error). */
static int feed(const uint8_t *bytes, size_t n)
{
    int r = PROTO_NEED_MORE;
    for (size_t i = 0; i < n; i++) {
        r = proto_parse_byte(&parser, bytes[i], &frame);
        if (r != PROTO_NEED_MORE) {
            return r;
        }
    }
    return r;
}

/* Test helper: build a TELEMETRY frame with the given seq, return its length */
static int make_frame(uint16_t seq, uint8_t *out)
{
    const telemetry_t t = { .timestamp = 1000, .state = 2, .temperature = 453,
                            .dc_voltage = 4000, .dc_current = 850,
                            .ac_power = 3200, .ac_voltage = 2305 };
    uint8_t payload[TELEMETRY_PAYLOAD_LEN];
    proto_pack_telemetry(&t, payload);
    return proto_pack_frame(MSG_TELEMETRY, seq, payload, sizeof payload, out, PROTO_MAX_FRAME);
}

void test_parse_round_trip(void)
{
    /* pack -> parse gives back exactly what was packed */
    uint8_t buf[PROTO_MAX_FRAME];
    int n = make_frame(42, buf);
    proto_parser_init(&parser);

    TEST_ASSERT_EQUAL_INT(PROTO_FRAME_READY, feed(buf, (size_t)n));
    TEST_ASSERT_EQUAL_HEX8(MSG_TELEMETRY, frame.msg_type);
    TEST_ASSERT_EQUAL_UINT16(42, frame.seq);
    TEST_ASSERT_EQUAL_UINT16(16, frame.payload_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&buf[8], frame.payload, 16);
}

void test_parse_needs_every_byte(void)
{
    /* A frame split into single bytes: no frame until the very last byte */
    uint8_t buf[PROTO_MAX_FRAME];
    int n = make_frame(1, buf);
    proto_parser_init(&parser);

    TEST_ASSERT_EQUAL_INT(PROTO_NEED_MORE, feed(buf, (size_t)n - 1));
    TEST_ASSERT_EQUAL_INT(PROTO_FRAME_READY, feed(&buf[n - 1], 1));
}

void test_parse_skips_garbage_before_sof(void)
{
    uint8_t buf[5 + PROTO_MAX_FRAME] = {0x13, 0x37, 0xAA, 0x00, 0x55};  /* garbage */
    int n = make_frame(7, &buf[5]);
    proto_parser_init(&parser);

    TEST_ASSERT_EQUAL_INT(PROTO_FRAME_READY, feed(buf, 5 + (size_t)n));
    TEST_ASSERT_EQUAL_UINT16(7, frame.seq);
}

void test_parse_handles_double_aa(void)
{
    /* AA AA 55 ...: the second AA is the real start */
    uint8_t buf[1 + PROTO_MAX_FRAME] = {0xAA};
    int n = make_frame(8, &buf[1]);
    proto_parser_init(&parser);

    TEST_ASSERT_EQUAL_INT(PROTO_FRAME_READY, feed(buf, 1 + (size_t)n));
}

void test_parse_two_frames_back_to_back(void)
{
    uint8_t buf[2 * PROTO_MAX_FRAME];
    int n1 = make_frame(1, buf);
    int n2 = make_frame(2, &buf[n1]);
    proto_parser_init(&parser);

    TEST_ASSERT_EQUAL_INT(PROTO_FRAME_READY, feed(buf, (size_t)n1));
    TEST_ASSERT_EQUAL_UINT16(1, frame.seq);
    TEST_ASSERT_EQUAL_INT(PROTO_FRAME_READY, feed(&buf[n1], (size_t)n2));
    TEST_ASSERT_EQUAL_UINT16(2, frame.seq);
}

void test_parse_rejects_bad_crc_then_recovers(void)
{
    uint8_t buf[2 * PROTO_MAX_FRAME];
    int n1 = make_frame(1, buf);
    int n2 = make_frame(2, &buf[n1]);
    buf[10] ^= 0x01;                       /* damage one bit in the first frame's payload */
    proto_parser_init(&parser);

    TEST_ASSERT_EQUAL_INT(PROTO_ERR_CRC, feed(buf, (size_t)n1));
    TEST_ASSERT_EQUAL_INT(PROTO_FRAME_READY, feed(&buf[n1], (size_t)n2));   /* resync */
    TEST_ASSERT_EQUAL_UINT16(2, frame.seq);
}

void test_parse_rejects_payload_len_257(void)
{
    /* Header says 257 bytes (0x0101): rejected right after the header, before any payload */
    const uint8_t buf[] = {0xAA, 0x55, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01};
    proto_parser_init(&parser);

    TEST_ASSERT_EQUAL_INT(PROTO_ERR_LEN, feed(buf, sizeof buf));
}

void test_parse_rejects_bad_version(void)
{
    const uint8_t buf[] = {0xAA, 0x55, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00};
    proto_parser_init(&parser);

    TEST_ASSERT_EQUAL_INT(PROTO_ERR_VERSION, feed(buf, sizeof buf));
}

void test_parse_seq_wraparound(void)
{
    /* seq 65535 is followed by 0: both must parse correctly */
    uint8_t buf[2 * PROTO_MAX_FRAME];
    int n1 = make_frame(65535, buf);
    int n2 = make_frame(0, &buf[n1]);
    proto_parser_init(&parser);

    TEST_ASSERT_EQUAL_INT(PROTO_FRAME_READY, feed(buf, (size_t)n1));
    TEST_ASSERT_EQUAL_UINT16(65535, frame.seq);
    TEST_ASSERT_EQUAL_INT(PROTO_FRAME_READY, feed(&buf[n1], (size_t)n2));
    TEST_ASSERT_EQUAL_UINT16(0, frame.seq);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pack_telemetry_is_big_endian);
    RUN_TEST(test_pack_telemetry_negative_temperature);
    RUN_TEST(test_pack_frame_layout);
    RUN_TEST(test_pack_frame_crc_covers_version_to_payload);
    RUN_TEST(test_pack_frame_empty_payload);
    RUN_TEST(test_pack_frame_rejects_payload_too_big);
    RUN_TEST(test_pack_frame_rejects_small_buffer);
    RUN_TEST(test_parse_round_trip);
    RUN_TEST(test_parse_needs_every_byte);
    RUN_TEST(test_parse_skips_garbage_before_sof);
    RUN_TEST(test_parse_handles_double_aa);
    RUN_TEST(test_parse_two_frames_back_to_back);
    RUN_TEST(test_parse_rejects_bad_crc_then_recovers);
    RUN_TEST(test_parse_rejects_payload_len_257);
    RUN_TEST(test_parse_rejects_bad_version);
    RUN_TEST(test_parse_seq_wraparound);
    return UNITY_END();
}
