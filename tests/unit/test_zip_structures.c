#include "unity.h"
#include "zip_structures.h"
#include "burst_writer.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>

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
    // Just verify the function doesn't crash and produces a valid date
    // Note: time may be 0 in UTC timezone (midnight), but date should be non-zero
    TEST_ASSERT_NOT_EQUAL(0, date);  // Should have encoded date (1969 or 1970 depending on timezone)
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

// Test write_padding_lfh with minimum size
void test_write_padding_lfh_min_size(void) {
    FILE *tmp = tmpfile();
    TEST_ASSERT_NOT_NULL(tmp);

    struct burst_writer *writer = burst_writer_create(tmp, 3);
    TEST_ASSERT_NOT_NULL(writer);

    // Write minimum size padding LFH (44 bytes)
    int result = write_padding_lfh(writer, PADDING_LFH_MIN_SIZE);
    TEST_ASSERT_EQUAL(0, result);

    // Flush and verify size
    burst_writer_flush(writer);
    TEST_ASSERT_EQUAL(PADDING_LFH_MIN_SIZE, writer->current_offset);
    TEST_ASSERT_EQUAL(PADDING_LFH_MIN_SIZE, writer->padding_bytes);

    // Verify the structure by reading back
    rewind(tmp);

    struct zip_local_header header;
    size_t read = fread(&header, 1, sizeof(header), tmp);
    TEST_ASSERT_EQUAL(sizeof(header), read);

    TEST_ASSERT_EQUAL_HEX32(ZIP_LOCAL_FILE_HEADER_SIG, header.signature);
    TEST_ASSERT_EQUAL(ZIP_VERSION_STORE, header.version_needed);
    TEST_ASSERT_EQUAL(0, header.flags);  // No data descriptor
    TEST_ASSERT_EQUAL(ZIP_METHOD_STORE, header.compression_method);
    TEST_ASSERT_EQUAL(0, header.crc32);
    TEST_ASSERT_EQUAL(0, header.compressed_size);
    TEST_ASSERT_EQUAL(0, header.uncompressed_size);
    TEST_ASSERT_EQUAL(PADDING_LFH_FILENAME_LEN, header.filename_length);
    TEST_ASSERT_EQUAL(0, header.extra_field_length);  // Min size = no extra field

    // Read and verify filename
    char filename[PADDING_LFH_FILENAME_LEN + 1];
    read = fread(filename, 1, PADDING_LFH_FILENAME_LEN, tmp);
    TEST_ASSERT_EQUAL(PADDING_LFH_FILENAME_LEN, read);
    filename[PADDING_LFH_FILENAME_LEN] = '\0';
    TEST_ASSERT_EQUAL_STRING(PADDING_LFH_FILENAME, filename);

    burst_writer_destroy(writer);
    fclose(tmp);
}

// Test write_padding_lfh with extra field
void test_write_padding_lfh_with_extra(void) {
    FILE *tmp = tmpfile();
    TEST_ASSERT_NOT_NULL(tmp);

    struct burst_writer *writer = burst_writer_create(tmp, 3);
    TEST_ASSERT_NOT_NULL(writer);

    // Write padding LFH with 100 extra bytes
    size_t target_size = PADDING_LFH_MIN_SIZE + 100;
    int result = write_padding_lfh(writer, target_size);
    TEST_ASSERT_EQUAL(0, result);

    // Flush and verify size
    burst_writer_flush(writer);
    TEST_ASSERT_EQUAL(target_size, writer->current_offset);
    TEST_ASSERT_EQUAL(target_size, writer->padding_bytes);

    // Verify header
    rewind(tmp);

    struct zip_local_header header;
    fread(&header, 1, sizeof(header), tmp);

    TEST_ASSERT_EQUAL_HEX32(ZIP_LOCAL_FILE_HEADER_SIG, header.signature);
    TEST_ASSERT_EQUAL(100, header.extra_field_length);

    burst_writer_destroy(writer);
    fclose(tmp);
}

// Test write_padding_lfh rejects too small target
void test_write_padding_lfh_too_small(void) {
    FILE *tmp = tmpfile();
    TEST_ASSERT_NOT_NULL(tmp);

    struct burst_writer *writer = burst_writer_create(tmp, 3);
    TEST_ASSERT_NOT_NULL(writer);

    // Try to write with size smaller than minimum
    int result = write_padding_lfh(writer, PADDING_LFH_MIN_SIZE - 1);
    TEST_ASSERT_EQUAL(-1, result);

    // Nothing should have been written
    TEST_ASSERT_EQUAL(0, writer->buffer_used);
    TEST_ASSERT_EQUAL(0, writer->padding_bytes);

    burst_writer_destroy(writer);
    fclose(tmp);
}

// Test write_padding_lfh with large extra field
void test_write_padding_lfh_large_extra(void) {
    FILE *tmp = tmpfile();
    TEST_ASSERT_NOT_NULL(tmp);

    struct burst_writer *writer = burst_writer_create(tmp, 3);
    TEST_ASSERT_NOT_NULL(writer);

    // Write padding LFH with 10000 extra bytes
    size_t target_size = PADDING_LFH_MIN_SIZE + 10000;
    int result = write_padding_lfh(writer, target_size);
    TEST_ASSERT_EQUAL(0, result);

    // Flush and verify size
    burst_writer_flush(writer);
    TEST_ASSERT_EQUAL(target_size, writer->current_offset);

    // Verify header extra field length
    rewind(tmp);

    struct zip_local_header header;
    fread(&header, 1, sizeof(header), tmp);

    TEST_ASSERT_EQUAL(10000, header.extra_field_length);

    burst_writer_destroy(writer);
    fclose(tmp);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_dos_datetime_epoch);
    RUN_TEST(test_dos_datetime_normal);
    RUN_TEST(test_get_local_header_size);
    RUN_TEST(test_get_central_header_size);
    RUN_TEST(test_header_size_empty_filename);
    RUN_TEST(test_write_padding_lfh_min_size);
    RUN_TEST(test_write_padding_lfh_with_extra);
    RUN_TEST(test_write_padding_lfh_too_small);
    RUN_TEST(test_write_padding_lfh_large_extra);

    return UNITY_END();
}
