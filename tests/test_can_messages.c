/* Unit tests for can_messages.c, using the values decoded by hand in Session 4. */
#include "unity.h"
#include "can_messages.h"

/* Unity calls these before and after every test. We need nothing here. */
void setUp(void) {}
void tearDown(void) {}

/* ---- 0x101 INVERTER_POWER ---- */

void test_power_decodes_known_frame(void)
{
    const uint8_t data[8] = {0xA0, 0x0F, 0x52, 0x03, 0x80, 0x0C, 0x01, 0x09};
    inverter_power_t p;

    TEST_ASSERT_EQUAL_INT(CAN_OK, can_decode_power(data, 8, &p));
    TEST_ASSERT_EQUAL_UINT16(4000, p.dc_voltage);   /* 400.0 V */
    TEST_ASSERT_EQUAL_UINT16(850,  p.dc_current);   /* 8.50 A  */
    TEST_ASSERT_EQUAL_UINT16(3200, p.ac_power);     /* 3200 W  */
    TEST_ASSERT_EQUAL_UINT16(2305, p.ac_voltage);   /* 230.5 V */
}

void test_power_rejects_bad_length(void)
{
    const uint8_t data[8] = {0};
    inverter_power_t p;

    TEST_ASSERT_EQUAL_INT(CAN_ERR_LEN, can_decode_power(data, 7, &p));
}

void test_power_accepts_max_voltage(void)
{
    /* 10000 = 0x2710 -> 1000.0 V, exactly the limit */
    const uint8_t data[8] = {0x10, 0x27, 0, 0, 0, 0, 0, 0};
    inverter_power_t p;

    TEST_ASSERT_EQUAL_INT(CAN_OK, can_decode_power(data, 8, &p));
}

void test_power_rejects_voltage_too_high(void)
{
    /* 10001 = 0x2711 -> 1000.1 V, one step over the limit */
    const uint8_t data[8] = {0x11, 0x27, 0, 0, 0, 0, 0, 0};
    inverter_power_t p;

    TEST_ASSERT_EQUAL_INT(CAN_ERR_RANGE, can_decode_power(data, 8, &p));
}

/* ---- 0x100 INVERTER_STATUS ---- */

void test_status_decodes_known_frame(void)
{
    const uint8_t data[8] = {0x02, 0x00, 0xC5, 0x01, 0x10, 0x0E, 0x00, 0x00};
    inverter_status_t s;

    TEST_ASSERT_EQUAL_INT(CAN_OK, can_decode_status(data, 8, &s));
    TEST_ASSERT_EQUAL_INT(INV_STATE_PRODUCING, s.state);
    TEST_ASSERT_EQUAL_UINT8(0, s.fault_code);
    TEST_ASSERT_EQUAL_INT16(453, s.temperature);    /* 45.3 °C */
    TEST_ASSERT_EQUAL_UINT32(3600, s.uptime);       /* 1 hour  */
}

void test_status_decodes_negative_temperature(void)
{
    /* CE FF = 0xFFCE -> as int16_t = -50 -> -5.0 °C */
    const uint8_t data[8] = {0x02, 0x00, 0xCE, 0xFF, 0x10, 0x0E, 0x00, 0x00};
    inverter_status_t s;

    TEST_ASSERT_EQUAL_INT(CAN_OK, can_decode_status(data, 8, &s));
    TEST_ASSERT_EQUAL_INT16(-50, s.temperature);
}

void test_status_decodes_large_uptime(void)
{
    /* 90 5F 01 00 = 0x00015F90 = 90000 s (25 hours): needs all 4 bytes */
    const uint8_t data[8] = {0x02, 0x00, 0xC5, 0x01, 0x90, 0x5F, 0x01, 0x00};
    inverter_status_t s;

    TEST_ASSERT_EQUAL_INT(CAN_OK, can_decode_status(data, 8, &s));
    TEST_ASSERT_EQUAL_UINT32(90000, s.uptime);
}

void test_status_rejects_bad_length(void)
{
    const uint8_t data[8] = {0};
    inverter_status_t s;

    TEST_ASSERT_EQUAL_INT(CAN_ERR_LEN, can_decode_status(data, 3, &s));
}

void test_status_rejects_unknown_state(void)
{
    const uint8_t data[8] = {0x04, 0x00, 0xC5, 0x01, 0x10, 0x0E, 0x00, 0x00};
    inverter_status_t s;

    TEST_ASSERT_EQUAL_INT(CAN_ERR_RANGE, can_decode_status(data, 8, &s));
}

void test_status_rejects_temperature_too_high(void)
{
    /* 1501 = 0x05DD -> 150.1 °C */
    const uint8_t data[8] = {0x02, 0x00, 0xDD, 0x05, 0x10, 0x0E, 0x00, 0x00};
    inverter_status_t s;

    TEST_ASSERT_EQUAL_INT(CAN_ERR_RANGE, can_decode_status(data, 8, &s));
}

void test_status_rejects_temperature_too_low(void)
{
    /* -401 = 0xFE6F -> -40.1 °C */
    const uint8_t data[8] = {0x02, 0x00, 0x6F, 0xFE, 0x10, 0x0E, 0x00, 0x00};
    inverter_status_t s;

    TEST_ASSERT_EQUAL_INT(CAN_ERR_RANGE, can_decode_status(data, 8, &s));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_power_decodes_known_frame);
    RUN_TEST(test_power_rejects_bad_length);
    RUN_TEST(test_power_accepts_max_voltage);
    RUN_TEST(test_power_rejects_voltage_too_high);
    RUN_TEST(test_status_decodes_known_frame);
    RUN_TEST(test_status_decodes_negative_temperature);
    RUN_TEST(test_status_decodes_large_uptime);
    RUN_TEST(test_status_rejects_bad_length);
    RUN_TEST(test_status_rejects_unknown_state);
    RUN_TEST(test_status_rejects_temperature_too_high);
    RUN_TEST(test_status_rejects_temperature_too_low);
    return UNITY_END();
}
