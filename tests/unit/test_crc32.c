#include "unity.h"
#include <zlib.h>
#include <string.h>

void setUp(void) {
}

void tearDown(void) {
}

// Test CRC32 of empty data
void test_crc32_empty(void) {
    uint32_t crc = crc32(0, NULL, 0);
    TEST_ASSERT_EQUAL(0, crc);
}

// Test CRC32 with known value
void test_crc32_known_value(void) {
    const char *data = "Hello, World!";
    uint32_t crc = crc32(0, (const unsigned char *)data, strlen(data));

    // Known CRC32 value for "Hello, World!"
    TEST_ASSERT_EQUAL_HEX32(0xec4ac3d0, crc);
}

// Test CRC32 incremental calculation
void test_crc32_incremental(void) {
    const char *part1 = "Hello, ";
    const char *part2 = "World!";

    uint32_t crc = crc32(0, (const unsigned char *)part1, strlen(part1));
    crc = crc32(crc, (const unsigned char *)part2, strlen(part2));

    // Should equal same as calculating in one go
    TEST_ASSERT_EQUAL_HEX32(0xec4ac3d0, crc);
}

// Test CRC32 of single byte
void test_crc32_single_byte(void) {
    const unsigned char data = 'A';
    uint32_t crc = crc32(0, &data, 1);

    TEST_ASSERT_NOT_EQUAL(0, crc);
}

// Test CRC32 of zeros
void test_crc32_zeros(void) {
    unsigned char zeros[100];
    memset(zeros, 0, sizeof(zeros));

    uint32_t crc = crc32(0, zeros, sizeof(zeros));
    TEST_ASSERT_NOT_EQUAL(0, crc);
}

// Test CRC32 different data produces different CRC
void test_crc32_different_data(void) {
    const char *data1 = "test1";
    const char *data2 = "test2";

    uint32_t crc1 = crc32(0, (const unsigned char *)data1, strlen(data1));
    uint32_t crc2 = crc32(0, (const unsigned char *)data2, strlen(data2));

    TEST_ASSERT_NOT_EQUAL(crc1, crc2);
}

// Test CRC32 is order-sensitive
void test_crc32_order_sensitive(void) {
    const char *data1 = "AB";
    const char *data2 = "BA";

    uint32_t crc1 = crc32(0, (const unsigned char *)data1, strlen(data1));
    uint32_t crc2 = crc32(0, (const unsigned char *)data2, strlen(data2));

    TEST_ASSERT_NOT_EQUAL(crc1, crc2);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_crc32_empty);
    RUN_TEST(test_crc32_known_value);
    RUN_TEST(test_crc32_incremental);
    RUN_TEST(test_crc32_single_byte);
    RUN_TEST(test_crc32_zeros);
    RUN_TEST(test_crc32_different_data);
    RUN_TEST(test_crc32_order_sensitive);

    return UNITY_END();
}
