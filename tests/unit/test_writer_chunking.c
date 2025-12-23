#include "unity.h"
#include "Mock_compression_mock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Forward declare burst_writer functions
struct burst_writer;
struct burst_writer* burst_writer_create(FILE* output, int compression_level);
void burst_writer_destroy(struct burst_writer* writer);
int burst_writer_add_file(struct burst_writer* writer, const char* filename, const char* input_path);

// Global call counters
static int compress_chunk_call_count;
static int verify_frame_content_size_call_count;

// Mock callbacks to count calls
static struct compression_result compress_chunk_counter(uint8_t* output_buffer, size_t output_capacity,
                                                        const uint8_t* input_buffer, size_t input_size,
                                                        int compression_level, int cmock_num_calls) {
    (void)output_buffer;
    (void)output_capacity;
    (void)input_buffer;
    (void)input_size;
    (void)compression_level;
    compress_chunk_call_count = cmock_num_calls + 1;

    struct compression_result result = {
        .compressed_size = 50000,
        .error = 0,
        .error_message = NULL
    };
    return result;
}

static int verify_frame_content_size_counter(const uint8_t* compressed_data, size_t compressed_size,
                                              size_t expected_uncompressed_size, int cmock_num_calls) {
    (void)compressed_data;
    (void)compressed_size;
    (void)expected_uncompressed_size;
    verify_frame_content_size_call_count = cmock_num_calls + 1;
    return 0;
}

void setUp(void) {
    Mock_compression_mock_Init();
    compress_chunk_call_count = 0;
    verify_frame_content_size_call_count = 0;
    compress_chunk_Stub(compress_chunk_counter);
    verify_frame_content_size_Stub(verify_frame_content_size_counter);
}

void tearDown(void) {
    Mock_compression_mock_Verify();
    Mock_compression_mock_Destroy();
}

// Helper to create temp file of specific size
static char* create_temp_file(const char* name, size_t size) {
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/burst_test_%s", name);

    FILE* f = fopen(path, "wb");
    if (!f) return NULL;

    uint8_t* data = malloc(size);
    memset(data, 'A', size);
    fwrite(data, 1, size, f);
    free(data);
    fclose(f);

    return path;
}

// Test 1: File exactly 128 KiB → 1 compress_chunk call
void test_file_exactly_128k_produces_one_chunk(void) {
    const size_t SIZE_128K = 128 * 1024;
    char* test_file = create_temp_file("128k", SIZE_128K);
    TEST_ASSERT_NOT_NULL(test_file);

    FILE* output = tmpfile();
    struct burst_writer* writer = burst_writer_create(output, 3);

    int result = burst_writer_add_file(writer, "test.dat", test_file);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(1, compress_chunk_call_count);
    TEST_ASSERT_EQUAL(1, verify_frame_content_size_call_count);

    burst_writer_destroy(writer);
    fclose(output);
    unlink(test_file);
}

// Test 2: File 128 KiB + 1 byte → 2 compress_chunk calls
void test_file_128k_plus_one_produces_two_chunks(void) {
    const size_t SIZE_128K_PLUS_1 = (128 * 1024) + 1;
    char* test_file = create_temp_file("128k_plus_1", SIZE_128K_PLUS_1);
    TEST_ASSERT_NOT_NULL(test_file);

    FILE* output = tmpfile();
    struct burst_writer* writer = burst_writer_create(output, 3);

    int result = burst_writer_add_file(writer, "test.dat", test_file);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(2, compress_chunk_call_count);
    TEST_ASSERT_EQUAL(2, verify_frame_content_size_call_count);

    burst_writer_destroy(writer);
    fclose(output);
    unlink(test_file);
}

// Test 3: File 256 KiB → 2 compress_chunk calls
void test_file_256k_produces_two_chunks(void) {
    const size_t SIZE_256K = 256 * 1024;
    char* test_file = create_temp_file("256k", SIZE_256K);
    TEST_ASSERT_NOT_NULL(test_file);

    FILE* output = tmpfile();
    struct burst_writer* writer = burst_writer_create(output, 3);

    int result = burst_writer_add_file(writer, "test.dat", test_file);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(2, compress_chunk_call_count);

    burst_writer_destroy(writer);
    fclose(output);
    unlink(test_file);
}

// Test 4: File 384 KiB - 1 byte → 3 compress_chunk calls
void test_file_384k_minus_one_produces_three_chunks(void) {
    const size_t SIZE_384K_MINUS_1 = (384 * 1024) - 1;
    char* test_file = create_temp_file("384k_minus_1", SIZE_384K_MINUS_1);
    TEST_ASSERT_NOT_NULL(test_file);

    FILE* output = tmpfile();
    struct burst_writer* writer = burst_writer_create(output, 3);

    int result = burst_writer_add_file(writer, "test.dat", test_file);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(3, compress_chunk_call_count);

    burst_writer_destroy(writer);
    fclose(output);
    unlink(test_file);
}

// Test 5: Verify 200 KiB file produces 2 chunks
void test_chunks_never_exceed_128k(void) {
    const size_t SIZE_200K = 200 * 1024;
    char* test_file = create_temp_file("200k", SIZE_200K);
    TEST_ASSERT_NOT_NULL(test_file);

    FILE* output = tmpfile();
    struct burst_writer* writer = burst_writer_create(output, 3);

    int result = burst_writer_add_file(writer, "test.dat", test_file);

    TEST_ASSERT_EQUAL(0, result);
    // 200 KiB = 128 KiB + 72 KiB → 2 chunks
    TEST_ASSERT_EQUAL(2, compress_chunk_call_count);

    burst_writer_destroy(writer);
    fclose(output);
    unlink(test_file);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_file_exactly_128k_produces_one_chunk);
    RUN_TEST(test_file_128k_plus_one_produces_two_chunks);
    RUN_TEST(test_file_256k_produces_two_chunks);
    RUN_TEST(test_file_384k_minus_one_produces_three_chunks);
    RUN_TEST(test_chunks_never_exceed_128k);
    return UNITY_END();
}
