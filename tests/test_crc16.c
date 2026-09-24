/* Unit tests for crc16.c */
#include "unity.h"
#include "crc16.h"

void setUp(void) {}
void tearDown(void) {}

void test_crc16_standard_check_value(void)
{
    /* The official check for CRC-16/CCITT-FALSE */
    const uint8_t data[] = "123456789";

    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16(data, 9));
}

void test_crc16_empty_input_returns_init(void)
{
    /* No bytes -> nothing mixed in -> the initial value */
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, crc16(NULL, 0));
}

void test_crc16_detects_one_changed_bit(void)
{
    /* Same data as the check, but '9' (0x39) became '8' (0x38): one bit differs */
    const uint8_t damaged[] = "123456788";

    TEST_ASSERT_NOT_EQUAL(0x29B1, crc16(damaged, 9));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc16_standard_check_value);
    RUN_TEST(test_crc16_empty_input_returns_init);
    RUN_TEST(test_crc16_detects_one_changed_bit);
    return UNITY_END();
}
