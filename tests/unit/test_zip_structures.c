#include "unity.h"
#include "zip_structures.h"
#include <string.h>
#include <time.h>

void setUp(void) {
    // Runs before each test
}

void tearDown(void) {
    // Runs after each test
}

// Test DOS date/time conversion with epoch
void test_dos_datetime_epoch(void) {
    uint16_t time, date;
    time_t t = 0;  // Unix epoch: 1970-01-01 00:00:00 (before DOS epoch)

    dos_datetime_from_time_t(t, &time, &date);

    // localtime() succeeds for Unix epoch, so it encodes a date/time
    // Even though it's before DOS epoch (1980), the function doesn't check
    // Just verify the function doesn't crash and produces values
    TEST_ASSERT_NOT_EQUAL(0, time);  // Should have encoded time
    TEST_ASSERT_NOT_EQUAL(0, date);  // Should have encoded date
}

// Test DOS date/time conversion with normal date
void test_dos_datetime_normal(void) {
    uint16_t time, date;
    struct tm tm = {
        .tm_year = 123,  // 2023 (years since 1900)
        .tm_mon = 11,    // December (0-indexed)
        .tm_mday = 22,
        .tm_hour = 16,
        .tm_min = 30,
        .tm_sec = 44     // Will be rounded down to 44 (22*2)
    };
    time_t t = mktime(&tm);

    dos_datetime_from_time_t(t, &time, &date);

    // Verify encoding
    TEST_ASSERT_NOT_EQUAL(0, time);
    TEST_ASSERT_NOT_EQUAL(0, date);

    // Decode time
    int hours = (time >> 11) & 0x1F;
    int minutes = (time >> 5) & 0x3F;
    int seconds = (time & 0x1F) * 2;

    TEST_ASSERT_EQUAL(16, hours);
    TEST_ASSERT_EQUAL(30, minutes);
    TEST_ASSERT_EQUAL(44, seconds);

    // Decode date
    int year = ((date >> 9) & 0x7F) + 1980;
    int month = (date >> 5) & 0x0F;
    int day = date & 0x1F;

    TEST_ASSERT_EQUAL(2023, year);
    TEST_ASSERT_EQUAL(12, month);
    TEST_ASSERT_EQUAL(22, day);
}

// Test local header size calculation
void test_get_local_header_size(void) {
    size_t size1 = get_local_header_size("test.txt");
    size_t size2 = get_local_header_size("a");
    size_t size3 = get_local_header_size("very_long_filename_test.bin");

    // Base header is 30 bytes (sizeof struct zip_local_header)
    TEST_ASSERT_EQUAL(30 + 8, size1);   // "test.txt" = 8 chars
    TEST_ASSERT_EQUAL(30 + 1, size2);   // "a" = 1 char
    TEST_ASSERT_EQUAL(30 + 27, size3);  // 27 chars
}

// Test central header size calculation
void test_get_central_header_size(void) {
    size_t size1 = get_central_header_size("test.txt");
    size_t size2 = get_central_header_size("file.bin");

    // Base header is 46 bytes (sizeof struct zip_central_header)
    TEST_ASSERT_EQUAL(46 + 8, size1);
    TEST_ASSERT_EQUAL(46 + 8, size2);
}

// Test empty filename
void test_header_size_empty_filename(void) {
    size_t local_size = get_local_header_size("");
    size_t central_size = get_central_header_size("");

    TEST_ASSERT_EQUAL(30, local_size);
    TEST_ASSERT_EQUAL(46, central_size);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_dos_datetime_epoch);
    RUN_TEST(test_dos_datetime_normal);
    RUN_TEST(test_get_local_header_size);
    RUN_TEST(test_get_central_header_size);
    RUN_TEST(test_header_size_empty_filename);

    return UNITY_END();
}
