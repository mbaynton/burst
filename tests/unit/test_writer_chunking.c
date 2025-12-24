#include "unity.h"
#include "Mock_compression_mock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

// Define ZIP structures needed for testing
#define ZIP_LOCAL_FILE_HEADER_SIG 0x04034b50
#define ZIP_VERSION_ZSTD 63
#define ZIP_FLAG_DATA_DESCRIPTOR 0x0008
#define ZIP_METHOD_ZSTD 93

struct zip_local_header {
    uint32_t signature;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression_method;
    uint16_t last_mod_time;
    uint16_t last_mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_length;
    uint16_t extra_field_length;
} __attribute__((packed));

// Forward declare burst_writer functions
struct burst_writer;
struct burst_writer* burst_writer_create(FILE* output, int compression_level);
void burst_writer_destroy(struct burst_writer* writer);
int burst_writer_add_file(struct burst_writer* writer, FILE* input_file,
                          struct zip_local_header* lfh, int lfh_len, int next_lfh_len);

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

// Helper to build a local file header for testing
static struct zip_local_header* build_test_header(const char *filename, int *lfh_len_out) {
    size_t filename_len = strlen(filename);
    size_t total_size = sizeof(struct zip_local_header) + filename_len;

    struct zip_local_header *lfh = malloc(total_size);
    if (!lfh) return NULL;

    memset(lfh, 0, sizeof(struct zip_local_header));
    lfh->signature = ZIP_LOCAL_FILE_HEADER_SIG;
    lfh->version_needed = ZIP_VERSION_ZSTD;
    lfh->flags = ZIP_FLAG_DATA_DESCRIPTOR;
    lfh->compression_method = ZIP_METHOD_ZSTD;
    lfh->last_mod_time = 0;
    lfh->last_mod_date = (1 << 5) | 1;  // 1980-01-01
    lfh->crc32 = 0;
    lfh->compressed_size = 0;
    lfh->uncompressed_size = 0;
    lfh->filename_length = filename_len;
    lfh->extra_field_length = 0;

    memcpy((uint8_t*)lfh + sizeof(struct zip_local_header), filename, filename_len);

    *lfh_len_out = total_size;
    return lfh;
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

    // Build header
    int lfh_len = 0;
    struct zip_local_header *lfh = build_test_header("test.dat", &lfh_len);
    TEST_ASSERT_NOT_NULL(lfh);

    // Open file handle
    FILE *input = fopen(test_file, "rb");
    TEST_ASSERT_NOT_NULL(input);

    // Call with new API (next_lfh_len = 0 for single file tests)
    int result = burst_writer_add_file(writer, input, lfh, lfh_len, 0);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(1, compress_chunk_call_count);
    TEST_ASSERT_EQUAL(1, verify_frame_content_size_call_count);

    // Cleanup
    fclose(input);
    free(lfh);
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

    // Build header
    int lfh_len = 0;
    struct zip_local_header *lfh = build_test_header("test.dat", &lfh_len);
    TEST_ASSERT_NOT_NULL(lfh);

    // Open file handle
    FILE *input = fopen(test_file, "rb");
    TEST_ASSERT_NOT_NULL(input);

    // Call with new API (next_lfh_len = 0 for single file tests)
    int result = burst_writer_add_file(writer, input, lfh, lfh_len, 0);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(2, compress_chunk_call_count);
    TEST_ASSERT_EQUAL(2, verify_frame_content_size_call_count);

    // Cleanup
    fclose(input);
    free(lfh);
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

    // Build header
    int lfh_len = 0;
    struct zip_local_header *lfh = build_test_header("test.dat", &lfh_len);
    TEST_ASSERT_NOT_NULL(lfh);

    // Open file handle
    FILE *input = fopen(test_file, "rb");
    TEST_ASSERT_NOT_NULL(input);

    // Call with new API (next_lfh_len = 0 for single file tests)
    int result = burst_writer_add_file(writer, input, lfh, lfh_len, 0);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(2, compress_chunk_call_count);

    // Cleanup
    fclose(input);
    free(lfh);
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

    // Build header
    int lfh_len = 0;
    struct zip_local_header *lfh = build_test_header("test.dat", &lfh_len);
    TEST_ASSERT_NOT_NULL(lfh);

    // Open file handle
    FILE *input = fopen(test_file, "rb");
    TEST_ASSERT_NOT_NULL(input);

    // Call with new API (next_lfh_len = 0 for single file tests)
    int result = burst_writer_add_file(writer, input, lfh, lfh_len, 0);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(3, compress_chunk_call_count);

    // Cleanup
    fclose(input);
    free(lfh);
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

    // Build header
    int lfh_len = 0;
    struct zip_local_header *lfh = build_test_header("test.dat", &lfh_len);
    TEST_ASSERT_NOT_NULL(lfh);

    // Open file handle
    FILE *input = fopen(test_file, "rb");
    TEST_ASSERT_NOT_NULL(input);

    // Call with new API (next_lfh_len = 0 for single file tests)
    int result = burst_writer_add_file(writer, input, lfh, lfh_len, 0);

    TEST_ASSERT_EQUAL(0, result);
    // 200 KiB = 128 KiB + 72 KiB → 2 chunks
    TEST_ASSERT_EQUAL(2, compress_chunk_call_count);

    // Cleanup
    fclose(input);
    free(lfh);
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
