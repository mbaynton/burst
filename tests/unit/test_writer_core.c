#include "unity.h"
#include "burst_writer.h"
#include <string.h>

void setUp(void) {
}

void tearDown(void) {
}

// Test writer creation and destruction
void test_writer_create_destroy(void) {
    FILE *tmp = tmpfile();
    TEST_ASSERT_NOT_NULL(tmp);

    struct burst_writer *writer = burst_writer_create(tmp, 3);
    TEST_ASSERT_NOT_NULL(writer);
    TEST_ASSERT_EQUAL(0, writer->current_offset);
    TEST_ASSERT_EQUAL(0, writer->num_files);
    TEST_ASSERT_NOT_NULL(writer->zstd_ctx);
    TEST_ASSERT_NOT_NULL(writer->write_buffer);

    burst_writer_destroy(writer);
    fclose(tmp);
}

// Test writer creation with NULL file
void test_writer_create_null_file(void) {
    struct burst_writer *writer = burst_writer_create(NULL, 3);
    TEST_ASSERT_NULL(writer);
}

// Test buffered writing
void test_writer_buffered_write(void) {
    FILE *tmp = tmpfile();
    struct burst_writer *writer = burst_writer_create(tmp, 3);

    const char *data = "Hello, World!";
    int result = burst_writer_write(writer, data, strlen(data));

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(strlen(data), writer->buffer_used);
    TEST_ASSERT_EQUAL(0, writer->current_offset);  // Not flushed yet

    burst_writer_destroy(writer);
    fclose(tmp);
}

// Test flush operation
void test_writer_flush(void) {
    FILE *tmp = tmpfile();
    struct burst_writer *writer = burst_writer_create(tmp, 3);

    const char *data = "Test data for flushing";
    burst_writer_write(writer, data, strlen(data));

    int result = burst_writer_flush(writer);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(0, writer->buffer_used);
    TEST_ASSERT_EQUAL(strlen(data), writer->current_offset);

    burst_writer_destroy(writer);
    fclose(tmp);
}

// Test flushing empty buffer
void test_writer_flush_empty(void) {
    FILE *tmp = tmpfile();
    struct burst_writer *writer = burst_writer_create(tmp, 3);

    int result = burst_writer_flush(writer);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(0, writer->current_offset);

    burst_writer_destroy(writer);
    fclose(tmp);
}

// Test writing NULL data
void test_writer_write_null(void) {
    FILE *tmp = tmpfile();
    struct burst_writer *writer = burst_writer_create(tmp, 3);

    int result = burst_writer_write(writer, NULL, 10);
    TEST_ASSERT_EQUAL(-1, result);

    burst_writer_destroy(writer);
    fclose(tmp);
}

// Test writing zero bytes
void test_writer_write_zero_bytes(void) {
    FILE *tmp = tmpfile();
    struct burst_writer *writer = burst_writer_create(tmp, 3);

    const char *data = "test";
    int result = burst_writer_write(writer, data, 0);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(0, writer->buffer_used);

    burst_writer_destroy(writer);
    fclose(tmp);
}

// Test buffer overflow causes flush
void test_writer_buffer_overflow(void) {
    FILE *tmp = tmpfile();
    struct burst_writer *writer = burst_writer_create(tmp, 3);

    // Write more than buffer size (64 KiB)
    char large_data[70000];
    memset(large_data, 'A', sizeof(large_data));

    int result = burst_writer_write(writer, large_data, sizeof(large_data));
    TEST_ASSERT_EQUAL(0, result);

    // Buffer should have been flushed
    TEST_ASSERT(writer->current_offset > 0);
    TEST_ASSERT(writer->buffer_used < sizeof(large_data));

    burst_writer_destroy(writer);
    fclose(tmp);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_writer_create_destroy);
    RUN_TEST(test_writer_create_null_file);
    RUN_TEST(test_writer_buffered_write);
    RUN_TEST(test_writer_flush);
    RUN_TEST(test_writer_flush_empty);
    RUN_TEST(test_writer_write_null);
    RUN_TEST(test_writer_write_zero_bytes);
    RUN_TEST(test_writer_buffer_overflow);

    return UNITY_END();
}
