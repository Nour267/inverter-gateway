/* Unit tests for protocol.c (pack) */
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
    return UNITY_END();
}
