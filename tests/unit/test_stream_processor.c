#include "unity.h"
#include "Mock_btrfs_writer_mock.h"
#include "stream_processor.h"
#include "central_dir_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

// ZIP format constants
#define ZIP_LOCAL_FILE_HEADER_SIG 0x04034b50
#define ZIP_DATA_DESCRIPTOR_SIG 0x08074b50
#define ZIP_METHOD_ZSTD 93
#define ZIP_FLAG_DATA_DESCRIPTOR 0x0008

// Zstandard magic
#define ZSTD_MAGIC 0xFD2FB528

// BURST skippable frame magic
#define BURST_MAGIC 0x184D2A5B

// Test tracking variables
static int write_encoded_call_count;
static uint64_t total_uncompressed_bytes;
static uint64_t last_file_offset;
static size_t last_frame_len;

// Callback stub for do_write_encoded
static int do_write_encoded_stub(
    int fd,
    const uint8_t *zstd_frame,
    size_t frame_len,
    uint64_t uncompressed_len,
    uint64_t file_offset,
    int cmock_num_calls)
{
    (void)fd;
    (void)zstd_frame;
    write_encoded_call_count = cmock_num_calls + 1;
    total_uncompressed_bytes += uncompressed_len;
    last_file_offset = file_offset;
    last_frame_len = frame_len;
    return BTRFS_WRITER_SUCCESS;
}

static int do_write_unencoded_stub(
    int fd,
    const uint8_t *zstd_frame,
    size_t frame_len,
    uint64_t uncompressed_len,
    uint64_t file_offset,
    int cmock_num_calls)
{
    (void)fd;
    (void)zstd_frame;
    (void)frame_len;
    (void)uncompressed_len;
    (void)file_offset;
    (void)cmock_num_calls;
    return BTRFS_WRITER_SUCCESS;
}

static bool is_btrfs_filesystem_stub(int fd, int cmock_num_calls)
{
    (void)fd;
    (void)cmock_num_calls;
    return true;
}

// Test output directory
static char test_output_dir[256];

void setUp(void) {
    Mock_btrfs_writer_mock_Init();
    write_encoded_call_count = 0;
    total_uncompressed_bytes = 0;
    last_file_offset = 0;
    last_frame_len = 0;

    do_write_encoded_Stub(do_write_encoded_stub);
    do_write_unencoded_Stub(do_write_unencoded_stub);
    is_btrfs_filesystem_Stub(is_btrfs_filesystem_stub);

    // Create temporary output directory
    snprintf(test_output_dir, sizeof(test_output_dir), "/tmp/burst_test_%d", getpid());
    mkdir(test_output_dir, 0755);
}

void tearDown(void) {
    Mock_btrfs_writer_mock_Verify();
    Mock_btrfs_writer_mock_Destroy();

    // Clean up output directory
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_output_dir);
    system(cmd);
}

// Helper to create a minimal Zstandard frame
// This creates a valid-looking frame header for testing
static size_t create_test_zstd_frame(uint8_t *buffer, size_t buffer_len,
                                     size_t uncompressed_size) {
    if (buffer_len < 16) return 0;

    // Zstd frame format (simplified for testing):
    // 4 bytes: magic (0xFD2FB528)
    // 1 byte: frame header descriptor
    // 1+ bytes: frame content size (if FCS flag set)
    // N bytes: compressed data (we'll just put zeros)

    size_t offset = 0;

    // Magic number
    uint32_t magic = ZSTD_MAGIC;
    memcpy(buffer + offset, &magic, 4);
    offset += 4;

    // Frame header descriptor:
    // Bit 7-6: Frame_Content_Size_flag (11 = 8 bytes)
    // Bit 5: Single_Segment_flag (1)
    // Bit 4-2: reserved (0)
    // Bit 1-0: Dictionary_ID_flag (0)
    uint8_t fhd = 0xE0;  // FCS_flag=3, single segment, no dict
    buffer[offset++] = fhd;

    // Frame content size (8 bytes since FCS_flag=3)
    uint64_t fcs = uncompressed_size;
    memcpy(buffer + offset, &fcs, 8);
    offset += 8;

    // Add minimal compressed block (raw block, empty)
    // Block header: 3 bytes
    // Bit 0: Last_Block (1)
    // Bit 1-2: Block_Type (00 = raw)
    // Bit 3-23: Block_Size (0)
    buffer[offset++] = 0x01;  // Last block, raw, size 0
    buffer[offset++] = 0x00;
    buffer[offset++] = 0x00;

    return offset;
}

// Helper to create a ZIP local file header
static size_t create_local_header(uint8_t *buffer, const char *filename) {
    size_t filename_len = strlen(filename);
    size_t header_size = 30 + filename_len;

    // Signature
    uint32_t sig = ZIP_LOCAL_FILE_HEADER_SIG;
    memcpy(buffer, &sig, 4);

    // Version needed (63 for Zstd)
    uint16_t version = 63;
    memcpy(buffer + 4, &version, 2);

    // Flags (data descriptor)
    uint16_t flags = ZIP_FLAG_DATA_DESCRIPTOR;
    memcpy(buffer + 6, &flags, 2);

    // Compression method (93 = Zstd)
    uint16_t method = ZIP_METHOD_ZSTD;
    memcpy(buffer + 8, &method, 2);

    // Time/date (zeros)
    memset(buffer + 10, 0, 4);

    // CRC, compressed size, uncompressed size (zeros, deferred)
    memset(buffer + 14, 0, 12);

    // Filename length
    uint16_t fn_len = (uint16_t)filename_len;
    memcpy(buffer + 26, &fn_len, 2);

    // Extra field length (0)
    uint16_t extra_len = 0;
    memcpy(buffer + 28, &extra_len, 2);

    // Filename
    memcpy(buffer + 30, filename, filename_len);

    return header_size;
}

// Helper to create a data descriptor
static size_t create_data_descriptor(uint8_t *buffer, uint32_t crc,
                                     uint32_t compressed_size,
                                     uint32_t uncompressed_size) {
    uint32_t sig = ZIP_DATA_DESCRIPTOR_SIG;
    memcpy(buffer, &sig, 4);
    memcpy(buffer + 4, &crc, 4);
    memcpy(buffer + 8, &compressed_size, 4);
    memcpy(buffer + 12, &uncompressed_size, 4);
    return 16;
}

// Helper to create a BURST padding frame
static size_t create_padding_frame(uint8_t *buffer, size_t padding_size) {
    uint32_t magic = BURST_MAGIC;
    uint32_t payload_size = (uint32_t)padding_size;

    memcpy(buffer, &magic, 4);
    memcpy(buffer + 4, &payload_size, 4);

    if (padding_size > 0) {
        memset(buffer + 8, 0, padding_size);  // Type byte 0 = padding
    }

    return 8 + padding_size;
}

// Helper to create a Start-of-Part frame
static size_t create_start_of_part_frame(uint8_t *buffer, uint64_t uncompressed_offset) {
    uint32_t magic = BURST_MAGIC;
    uint32_t payload_size = 16;

    memcpy(buffer, &magic, 4);
    memcpy(buffer + 4, &payload_size, 4);

    // Type byte
    buffer[8] = 0x01;

    // Uncompressed offset
    memcpy(buffer + 9, &uncompressed_offset, 8);

    // Reserved
    memset(buffer + 17, 0, 7);

    return 24;  // 8 header + 16 payload
}

// Helper to create a minimal central directory parse result
static struct central_dir_parse_result *create_test_cd_result(
    const char *filename,
    uint64_t local_header_offset,
    uint64_t compressed_size,
    uint64_t uncompressed_size)
{
    struct central_dir_parse_result *result = calloc(1, sizeof(struct central_dir_parse_result));

    result->num_files = 1;
    result->files = calloc(1, sizeof(struct file_metadata));
    result->files[0].filename = strdup(filename);
    result->files[0].local_header_offset = local_header_offset;
    result->files[0].compressed_size = compressed_size;
    result->files[0].uncompressed_size = uncompressed_size;
    result->files[0].crc32 = 0;
    result->files[0].compression_method = ZIP_METHOD_ZSTD;
    result->files[0].part_index = 0;

    result->num_parts = 1;
    result->parts = calloc(1, sizeof(struct part_files));
    result->parts[0].num_entries = 1;
    result->parts[0].entries = calloc(1, sizeof(struct part_file_entry));
    result->parts[0].entries[0].file_index = 0;
    result->parts[0].entries[0].offset_in_part = local_header_offset;
    result->parts[0].continuing_file = NULL;

    return result;
}

static void free_test_cd_result(struct central_dir_parse_result *result) {
    if (result == NULL) return;
    if (result->files) {
        free(result->files[0].filename);
        free(result->files);
    }
    if (result->parts) {
        free(result->parts[0].entries);
        free(result->parts);
    }
    free(result);
}

// Test 1: Single Zstd frame in single callback
void test_single_frame_single_callback(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Create local header
    offset += create_local_header(buffer + offset, "test.txt");

    // Create Zstd frame (100 bytes uncompressed)
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;

    // Create data descriptor
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    // Create central directory result
    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);

    // Create processor
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // Process data
    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    // Finalize
    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    // Verify: should have called do_write_encoded once
    TEST_ASSERT_EQUAL(1, write_encoded_call_count);
    TEST_ASSERT_EQUAL(100, total_uncompressed_bytes);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 2: Frame spanning two callbacks (partial header)
void test_frame_spanning_callbacks_partial_header(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Create local header
    size_t header_size = create_local_header(buffer + offset, "test.txt");
    offset += header_size;

    // Create Zstd frame (100 bytes uncompressed)
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;

    // Create data descriptor
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    // Create central directory result
    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);

    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // Split at middle of Zstd frame header (after magic, before size is known)
    size_t split_point = header_size + 2;  // Just after magic bytes

    // First callback: partial data
    int rc = part_processor_process_data(state, buffer, split_point);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(0, write_encoded_call_count);  // Not enough data yet

    // Second callback: rest of data
    rc = part_processor_process_data(state, buffer + split_point, offset - split_point);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    // Finalize
    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    // Should have called do_write_encoded once
    TEST_ASSERT_EQUAL(1, write_encoded_call_count);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 3: Frame spanning two callbacks (partial compressed payload)
void test_frame_spanning_callbacks_partial_payload(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Create local header
    size_t header_size = create_local_header(buffer + offset, "test.txt");
    offset += header_size;

    // Create Zstd frame (100 bytes uncompressed)
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;

    // Create data descriptor
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // Split in middle of Zstd frame payload (after header is complete)
    size_t split_point = header_size + 10;  // Header complete, mid-frame

    // First callback
    int rc = part_processor_process_data(state, buffer, split_point);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    // Frame may or may not be processed depending on if size is known

    // Second callback
    rc = part_processor_process_data(state, buffer + split_point, offset - split_point);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    TEST_ASSERT_EQUAL(1, write_encoded_call_count);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 4: Multiple frames in single callback
void test_multiple_frames_single_callback(void) {
    uint8_t buffer[2048];
    size_t offset = 0;

    // Create local header
    offset += create_local_header(buffer + offset, "test.txt");

    // Create 3 Zstd frames (simulating chunked compression)
    size_t zstd1 = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd1;
    size_t zstd2 = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd2;
    size_t zstd3 = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd3;

    // Data descriptor
    uint32_t total_compressed = (uint32_t)(zstd1 + zstd2 + zstd3);
    offset += create_data_descriptor(buffer + offset, 0, total_compressed, 300);

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, total_compressed, 300);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    // Should have 3 write calls
    TEST_ASSERT_EQUAL(3, write_encoded_call_count);
    TEST_ASSERT_EQUAL(300, total_uncompressed_bytes);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 5: Start-of-Part frame extraction
void test_start_of_part_metadata(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Start-of-Part frame at beginning (continuing file)
    uint64_t uncompressed_offset = 1000;
    offset += create_start_of_part_frame(buffer + offset, uncompressed_offset);

    // Zstd frame
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;

    // Data descriptor
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    // Create CD result with continuing file
    struct central_dir_parse_result *cd = calloc(1, sizeof(struct central_dir_parse_result));
    cd->num_files = 1;
    cd->files = calloc(1, sizeof(struct file_metadata));
    cd->files[0].filename = strdup("continuing.txt");
    cd->files[0].local_header_offset = 0;  // Started in previous part
    cd->files[0].uncompressed_size = 1100;  // Total size
    cd->files[0].part_index = 0;

    cd->num_parts = 2;
    cd->parts = calloc(2, sizeof(struct part_files));
    cd->parts[0].num_entries = 0;  // No files start in part 0
    cd->parts[0].entries = NULL;
    cd->parts[0].continuing_file = NULL;
    cd->parts[1].num_entries = 0;  // No files start in part 1
    cd->parts[1].entries = NULL;
    cd->parts[1].continuing_file = &cd->files[0];  // Continuing from part 0

    struct part_processor_state *state = part_processor_create(1, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    // Verify write was called with correct offset
    TEST_ASSERT_EQUAL(1, write_encoded_call_count);
    TEST_ASSERT_EQUAL(uncompressed_offset, last_file_offset);

    part_processor_destroy(state);
    free(cd->files[0].filename);
    free(cd->files);
    free(cd->parts);
    free(cd);
}

// Test 6: Local header parsing (variable filename length)
void test_local_header_parsing(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Use a longer filename
    const char *long_filename = "path/to/deeply/nested/file.txt";

    offset += create_local_header(buffer + offset, long_filename);
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 50);
    offset += zstd_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 50);

    struct central_dir_parse_result *cd = create_test_cd_result(long_filename, 0, zstd_size, 50);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    TEST_ASSERT_EQUAL(1, write_encoded_call_count);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 7: Data descriptor detection
void test_data_descriptor_closes_file(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    offset += create_local_header(buffer + offset, "test.txt");
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;

    // Add data descriptor
    size_t desc_offset = offset;
    offset += create_data_descriptor(buffer + offset, 0xDEADBEEF, (uint32_t)zstd_size, 100);

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);

    // Process up to just before descriptor
    int rc = part_processor_process_data(state, buffer, desc_offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(1, write_encoded_call_count);

    // Process descriptor
    rc = part_processor_process_data(state, buffer + desc_offset, offset - desc_offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 8: Empty file handling
void test_empty_file_handling(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Local header for empty file
    offset += create_local_header(buffer + offset, "empty.txt");

    // Empty Zstd frame (0 bytes uncompressed)
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 0);
    offset += zstd_size;

    // Data descriptor
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 0);

    struct central_dir_parse_result *cd = create_test_cd_result("empty.txt", 0, zstd_size, 0);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    // Should still call write for the empty frame
    TEST_ASSERT_EQUAL(1, write_encoded_call_count);
    TEST_ASSERT_EQUAL(0, total_uncompressed_bytes);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 9: Skippable padding frame skipping
void test_skippable_padding_frame_skipping(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Padding frame before local header
    offset += create_padding_frame(buffer + offset, 32);

    // Local header
    offset += create_local_header(buffer + offset, "test.txt");

    // Zstd frame
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;

    // More padding before descriptor
    offset += create_padding_frame(buffer + offset, 16);

    // Data descriptor
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    // Adjust CD result - local header now at offset 40 (8 + 32)
    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 40, zstd_size, 100);
    cd->parts[0].entries[0].offset_in_part = 40;

    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    // Should have processed exactly 1 Zstd frame (padding skipped)
    TEST_ASSERT_EQUAL(1, write_encoded_call_count);
    TEST_ASSERT_EQUAL(100, total_uncompressed_bytes);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// ============================================================================
// Additional Coverage Tests (10-25)
// ============================================================================

// Test 10: NULL arguments to part_processor_create()
void test_null_arguments_to_create(void) {
    // Create a valid CD for testing
    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, 100, 100);

    // NULL cd_result should return NULL
    struct part_processor_state *state = part_processor_create(0, NULL, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NULL(state);

    // NULL output_dir should return NULL
    state = part_processor_create(0, cd, NULL, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NULL(state);

    free_test_cd_result(cd);
}

// Test 11: Invalid part_index (>= num_parts)
void test_invalid_part_index(void) {
    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, 100, 100);
    // cd->num_parts is 1, so index 5 is invalid

    struct part_processor_state *state = part_processor_create(5, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NULL(state);

    // Also test exactly at boundary
    state = part_processor_create(1, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NULL(state);

    free_test_cd_result(cd);
}

// Test 12: NULL arguments to part_processor_process_data()
void test_null_arguments_to_process_data(void) {
    uint8_t buffer[64];
    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, 100, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // NULL state should return error
    int rc = part_processor_process_data(NULL, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_ARGS, rc);

    // NULL data with non-zero length should return error
    rc = part_processor_process_data(state, NULL, 10);
    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_ARGS, rc);

    // NULL data with zero length should be OK (no-op)
    rc = part_processor_process_data(state, NULL, 0);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Stub that returns an error for BTRFS write failure test
static int do_write_encoded_fail_stub(
    int fd,
    const uint8_t *zstd_frame,
    size_t frame_len,
    uint64_t uncompressed_len,
    uint64_t file_offset,
    int cmock_num_calls)
{
    (void)fd;
    (void)zstd_frame;
    (void)frame_len;
    (void)uncompressed_len;
    (void)file_offset;
    (void)cmock_num_calls;
    return BTRFS_WRITER_ERR_IOCTL_FAILED;
}

// Test 13: BTRFS write failure
void test_btrfs_write_failure(void) {
    // Override stub to return failure
    do_write_encoded_Stub(do_write_encoded_fail_stub);

    uint8_t buffer[1024];
    size_t offset = 0;

    offset += create_local_header(buffer + offset, "test.txt");
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_BTRFS_WRITE, rc);

    // Verify error message is set
    const char *err = part_processor_get_error(state);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(strlen(err) > 0);

    part_processor_destroy(state);
    free_test_cd_result(cd);

    // Restore normal stub
    do_write_encoded_Stub(do_write_encoded_stub);
}

// Helper to create CD result with multiple files
static struct central_dir_parse_result *create_test_cd_result_multi(
    const char *filename1, uint64_t offset1, uint64_t comp1, uint64_t uncomp1,
    const char *filename2, uint64_t offset2, uint64_t comp2, uint64_t uncomp2)
{
    struct central_dir_parse_result *result = calloc(1, sizeof(struct central_dir_parse_result));

    result->num_files = 2;
    result->files = calloc(2, sizeof(struct file_metadata));
    result->files[0].filename = strdup(filename1);
    result->files[0].local_header_offset = offset1;
    result->files[0].compressed_size = comp1;
    result->files[0].uncompressed_size = uncomp1;
    result->files[0].crc32 = 0;
    result->files[0].compression_method = ZIP_METHOD_ZSTD;
    result->files[0].part_index = 0;

    result->files[1].filename = strdup(filename2);
    result->files[1].local_header_offset = offset2;
    result->files[1].compressed_size = comp2;
    result->files[1].uncompressed_size = uncomp2;
    result->files[1].crc32 = 0;
    result->files[1].compression_method = ZIP_METHOD_ZSTD;
    result->files[1].part_index = 0;

    result->num_parts = 1;
    result->parts = calloc(1, sizeof(struct part_files));
    result->parts[0].num_entries = 2;
    result->parts[0].entries = calloc(2, sizeof(struct part_file_entry));
    result->parts[0].entries[0].file_index = 0;
    result->parts[0].entries[0].offset_in_part = offset1;
    result->parts[0].entries[1].file_index = 1;
    result->parts[0].entries[1].offset_in_part = offset2;
    result->parts[0].continuing_file = NULL;

    return result;
}

static void free_test_cd_result_multi(struct central_dir_parse_result *result) {
    if (result == NULL) return;
    if (result->files) {
        free(result->files[0].filename);
        free(result->files[1].filename);
        free(result->files);
    }
    if (result->parts) {
        free(result->parts[0].entries);
        free(result->parts);
    }
    free(result);
}

// Test 14: Multiple files in single part
void test_multiple_files_in_single_part(void) {
    uint8_t buffer[2048];
    size_t offset = 0;

    // File 1
    size_t file1_start = offset;
    offset += create_local_header(buffer + offset, "file1.txt");
    size_t zstd1_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd1_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd1_size, 100);

    // File 2
    size_t file2_start = offset;
    offset += create_local_header(buffer + offset, "file2.txt");
    size_t zstd2_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 200);
    offset += zstd2_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd2_size, 200);

    struct central_dir_parse_result *cd = create_test_cd_result_multi(
        "file1.txt", file1_start, zstd1_size, 100,
        "file2.txt", file2_start, zstd2_size, 200);

    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    // Should have 2 write calls (one per file)
    TEST_ASSERT_EQUAL(2, write_encoded_call_count);
    TEST_ASSERT_EQUAL(300, total_uncompressed_bytes);

    part_processor_destroy(state);
    free_test_cd_result_multi(cd);
}

// Test 15: Wrong frame type at continuing file (expected Start-of-Part, got padding)
void test_wrong_frame_at_continuing_file(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Put a padding frame instead of Start-of-Part frame
    offset += create_padding_frame(buffer + offset, 16);

    // Create CD result with continuing_file set
    struct central_dir_parse_result *cd = calloc(1, sizeof(struct central_dir_parse_result));
    cd->num_files = 1;
    cd->files = calloc(1, sizeof(struct file_metadata));
    cd->files[0].filename = strdup("continuing.txt");
    cd->files[0].uncompressed_size = 1000;
    cd->files[0].part_index = 0;

    cd->num_parts = 2;
    cd->parts = calloc(2, sizeof(struct part_files));
    cd->parts[0].num_entries = 0;
    cd->parts[0].entries = NULL;
    cd->parts[0].continuing_file = NULL;
    cd->parts[1].num_entries = 0;
    cd->parts[1].entries = NULL;
    cd->parts[1].continuing_file = &cd->files[0];  // Expects Start-of-Part

    struct part_processor_state *state = part_processor_create(1, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_FRAME, rc);

    part_processor_destroy(state);
    free(cd->files[0].filename);
    free(cd->files);
    free(cd->parts);
    free(cd);
}

// Test 16: Wrong frame type when expecting local header (got Zstd frame)
void test_wrong_frame_at_local_header(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Put a Zstd frame instead of local header
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_FRAME, rc);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 17: Process data when already in ERROR state
void test_process_data_in_error_state(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Create invalid data to trigger error
    uint32_t invalid_magic = 0xDEADBEEF;
    memcpy(buffer, &invalid_magic, 4);
    offset = 4;

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, 100, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // First call should fail and put state in ERROR
    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_FRAME, rc);

    // Second call should immediately return the stored error
    rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_FRAME, rc);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 18: Process data when already in DONE state
void test_process_data_in_done_state(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    offset += create_local_header(buffer + offset, "test.txt");
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // Process all data
    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    // Process more data when already done - should succeed (no-op)
    rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    // Write count should still be 1 (no additional processing)
    TEST_ASSERT_EQUAL(1, write_encoded_call_count);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 19: Finalize with incomplete frame buffered
void test_finalize_with_incomplete_frame(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    offset += create_local_header(buffer + offset, "test.txt");

    // Only provide partial Zstd frame (just the magic)
    uint32_t zstd_magic = ZSTD_MAGIC;
    memcpy(buffer + offset, &zstd_magic, 4);
    offset += 4;

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, 100, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // Process data - should buffer the partial frame
    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    // Finalize should fail because there's buffered incomplete data
    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_UNEXPECTED_EOF, rc);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 20: Unexpected Start-of-Part frame in middle of processing
void test_unexpected_start_of_part_mid_processing(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    offset += create_local_header(buffer + offset, "test.txt");
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;

    // Inject a Start-of-Part frame in the middle (unexpected)
    offset += create_start_of_part_frame(buffer + offset, 500);

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_FRAME, rc);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 23: Unknown/corrupted frame magic number
void test_unknown_frame_magic(void) {
    uint8_t buffer[64];

    // Create a frame with unknown magic
    uint32_t unknown_magic = 0xCAFEBABE;
    memcpy(buffer, &unknown_magic, 4);

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, 100, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    int rc = part_processor_process_data(state, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_FRAME, rc);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test 24: Zstd frame without content size in header (CONTENTSIZE_UNKNOWN)
// BURST archives require content size to be in the frame header
void test_zstd_frame_without_content_size(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    offset += create_local_header(buffer + offset, "test.txt");

    // Create a Zstd frame WITHOUT Frame_Content_Size
    // Zstd frame header descriptor: FCS_flag=0 (no content size), Single_Segment=0
    // This creates a valid frame that ZSTD can parse, but has unknown content size
    uint32_t zstd_magic = ZSTD_MAGIC;
    memcpy(buffer + offset, &zstd_magic, 4);
    offset += 4;

    // Frame header descriptor: FCS_flag=0, Single_Segment=0, no dict
    // Bits: FCS(00) Single(0) Reserved(0) Content_Checksum(0) Dict_ID(0) = 0x00
    buffer[offset++] = 0x00;

    // Window descriptor (required when Single_Segment=0)
    buffer[offset++] = 0x00;  // Window size

    // Block header for an empty raw block (last block)
    buffer[offset++] = 0x01;  // Last_Block=1, Block_Type=0 (raw), Block_Size=0
    buffer[offset++] = 0x00;
    buffer[offset++] = 0x00;

    // Optional checksum (none in this case since Content_Checksum=0)

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, 100, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_FRAME, rc);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test: NULL argument to finalize
void test_null_argument_to_finalize(void) {
    int rc = part_processor_finalize(NULL);
    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_ARGS, rc);
}

// Test: get_error with NULL state
void test_get_error_null_state(void) {
    const char *err = part_processor_get_error(NULL);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(strlen(err) > 0);  // Should return "NULL state"
}

// ============================================================================
// Integration Buffering Tests - Test NEED_MORE_DATA scenarios at integration level
// ============================================================================

// Test: Split data before magic number can be read (< 4 bytes)
void test_split_before_magic_number(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    offset += create_local_header(buffer + offset, "test.txt");
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // First callback: only 2 bytes (less than 4-byte magic)
    int rc = part_processor_process_data(state, buffer, 2);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(0, write_encoded_call_count);  // Nothing written yet

    // Second callback: rest of data
    rc = part_processor_process_data(state, buffer + 2, offset - 2);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    TEST_ASSERT_EQUAL(1, write_encoded_call_count);
    TEST_ASSERT_EQUAL(100, total_uncompressed_bytes);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test: Split in middle of ZIP local header (after magic, before variable fields)
void test_split_mid_zip_local_header(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    size_t header_size = create_local_header(buffer + offset, "test.txt");
    offset += header_size;
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // Split at byte 10 (magic + 6 bytes, but header is 30+ bytes)
    size_t split_point = 10;

    int rc = part_processor_process_data(state, buffer, split_point);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(0, write_encoded_call_count);

    rc = part_processor_process_data(state, buffer + split_point, offset - split_point);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    TEST_ASSERT_EQUAL(1, write_encoded_call_count);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test: Split in middle of BURST skippable header (after magic, before payload_size)
void test_split_mid_burst_skippable_header(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Padding frame at start
    offset += create_padding_frame(buffer + offset, 32);  // 8 + 32 = 40 bytes

    // Then local header and file data
    size_t local_header_start = offset;
    offset += create_local_header(buffer + offset, "test.txt");
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", local_header_start, zstd_size, 100);
    cd->parts[0].entries[0].offset_in_part = local_header_start;
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // Split at byte 6 (magic seen, but payload_size field incomplete)
    size_t split_point = 6;

    int rc = part_processor_process_data(state, buffer, split_point);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_process_data(state, buffer + split_point, offset - split_point);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    TEST_ASSERT_EQUAL(1, write_encoded_call_count);
    TEST_ASSERT_EQUAL(100, total_uncompressed_bytes);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test: Split in middle of BURST skippable payload
void test_split_mid_burst_skippable_payload(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Padding frame with 32-byte payload (total = 40 bytes)
    offset += create_padding_frame(buffer + offset, 32);

    // Then local header and file data
    size_t local_header_start = offset;
    offset += create_local_header(buffer + offset, "test.txt");
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", local_header_start, zstd_size, 100);
    cd->parts[0].entries[0].offset_in_part = local_header_start;
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // Split at byte 20 (header complete at 8, payload partial at 12 bytes)
    size_t split_point = 20;

    int rc = part_processor_process_data(state, buffer, split_point);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_process_data(state, buffer + split_point, offset - split_point);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    TEST_ASSERT_EQUAL(1, write_encoded_call_count);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test: Split in middle of local header variable fields (long filename)
void test_split_mid_local_header_variable_fields(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    const char *long_filename = "path/to/deeply/nested/directory/with/many/levels/file.txt";
    size_t header_size = create_local_header(buffer + offset, long_filename);
    offset += header_size;
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    struct central_dir_parse_result *cd = create_test_cd_result(long_filename, 0, zstd_size, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // Split at byte 35 (fixed header is 30 bytes, so 5 bytes into filename)
    size_t split_point = 35;

    int rc = part_processor_process_data(state, buffer, split_point);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_process_data(state, buffer + split_point, offset - split_point);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    TEST_ASSERT_EQUAL(1, write_encoded_call_count);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// Test: Feed data in very small chunks (3 bytes at a time)
void test_split_at_multiple_boundaries(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    offset += create_local_header(buffer + offset, "test.txt");
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, BURST_BASE_PART_SIZE);
    TEST_ASSERT_NOT_NULL(state);

    // Feed data 3 bytes at a time
    size_t chunk_size = 3;
    size_t pos = 0;
    int rc;

    while (pos < offset) {
        size_t remaining = offset - pos;
        size_t to_send = (remaining < chunk_size) ? remaining : chunk_size;
        rc = part_processor_process_data(state, buffer + pos, to_send);
        TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
        pos += to_send;
    }

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    TEST_ASSERT_EQUAL(1, write_encoded_call_count);
    TEST_ASSERT_EQUAL(100, total_uncompressed_bytes);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

// =============================================================================
// Central Directory Detection Tests
// =============================================================================

// Helper to create a Central Directory header signature
static size_t create_central_dir_header(uint8_t *buffer) {
    uint32_t sig = 0x02014b50;  // ZIP_CENTRAL_DIR_HEADER_SIG
    memcpy(buffer, &sig, 4);
    return 4;
}

void test_central_directory_at_expected_offset(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Create: local header + zstd frame + data descriptor + CD header
    offset += create_local_header(buffer + offset, "test.txt");
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    // Record where CD starts
    size_t cd_offset = offset;

    // Add CD header signature
    offset += create_central_dir_header(buffer + offset);

    // Create cd_result with matching central_dir_offset
    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    cd->central_dir_offset = cd_offset;  // Set expected CD location

    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, 8 * 1024 * 1024);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

void test_central_directory_before_expected_offset(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Create: local header + zstd frame + data descriptor + CD header
    offset += create_local_header(buffer + offset, "test.txt");
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    // Record where CD starts
    size_t cd_offset = offset;

    // Add CD header signature
    offset += create_central_dir_header(buffer + offset);

    // Create cd_result with central_dir_offset set LARGER than actual
    // (CD found earlier than expected - e.g., truncated archive)
    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    cd->central_dir_offset = cd_offset + 1000;  // Expected later than actual

    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, 8 * 1024 * 1024);

    // Should still succeed (warning printed to stderr but no error)
    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

void test_central_directory_after_expected_offset(void) {
    uint8_t buffer[1024];
    size_t offset = 0;

    // Create: local header + zstd frame + data descriptor + CD header
    offset += create_local_header(buffer + offset, "test.txt");
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;
    offset += create_data_descriptor(buffer + offset, 0, (uint32_t)zstd_size, 100);

    // Record where CD starts
    size_t cd_offset = offset;

    // Add CD header signature
    offset += create_central_dir_header(buffer + offset);

    // Create cd_result with central_dir_offset set SMALLER than actual
    // (CD found later than expected - e.g., extra padding before CD)
    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    cd->central_dir_offset = cd_offset > 100 ? cd_offset - 100 : 0;  // Expected earlier than actual

    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, 8 * 1024 * 1024);

    // Should still succeed (warning printed to stderr but no error)
    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

void test_central_directory_in_processing_frames_state(void) {
    // Test CD detection when in STATE_PROCESSING_FRAMES (with open file)
    uint8_t buffer[1024];
    size_t offset = 0;

    // Create: local header + zstd frame (no data descriptor) + CD header
    // This tests the FRAME_ZIP_CENTRAL_DIRECTORY case in STATE_PROCESSING_FRAMES
    offset += create_local_header(buffer + offset, "test.txt");
    size_t zstd_size = create_test_zstd_frame(buffer + offset, sizeof(buffer) - offset, 100);
    offset += zstd_size;

    // No data descriptor - go directly to CD
    size_t cd_offset = offset;
    offset += create_central_dir_header(buffer + offset);

    // Create cd_result - file uses no data descriptor (for this test)
    struct central_dir_parse_result *cd = create_test_cd_result("test.txt", 0, zstd_size, 100);
    cd->central_dir_offset = cd_offset;

    struct part_processor_state *state = part_processor_create(0, cd, test_output_dir, 8 * 1024 * 1024);

    int rc = part_processor_process_data(state, buffer, offset);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    rc = part_processor_finalize(state);
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);

    part_processor_destroy(state);
    free_test_cd_result(cd);
}

int main(void) {
    UNITY_BEGIN();

    // Core functionality tests (1-9)
    RUN_TEST(test_single_frame_single_callback);
    RUN_TEST(test_frame_spanning_callbacks_partial_header);
    RUN_TEST(test_frame_spanning_callbacks_partial_payload);
    RUN_TEST(test_multiple_frames_single_callback);
    RUN_TEST(test_start_of_part_metadata);
    RUN_TEST(test_local_header_parsing);
    RUN_TEST(test_data_descriptor_closes_file);
    RUN_TEST(test_empty_file_handling);
    RUN_TEST(test_skippable_padding_frame_skipping);

    // Error handling tests (10-13)
    RUN_TEST(test_null_arguments_to_create);
    RUN_TEST(test_invalid_part_index);
    RUN_TEST(test_null_arguments_to_process_data);
    RUN_TEST(test_btrfs_write_failure);

    // Multiple files and wrong frame types (14-16)
    RUN_TEST(test_multiple_files_in_single_part);
    RUN_TEST(test_wrong_frame_at_continuing_file);
    RUN_TEST(test_wrong_frame_at_local_header);

    // State machine tests (17-20)
    RUN_TEST(test_process_data_in_error_state);
    RUN_TEST(test_process_data_in_done_state);
    RUN_TEST(test_finalize_with_incomplete_frame);
    RUN_TEST(test_unexpected_start_of_part_mid_processing);

    // Frame parsing error tests (23-24)
    RUN_TEST(test_unknown_frame_magic);
    RUN_TEST(test_zstd_frame_without_content_size);

    // Additional coverage tests
    RUN_TEST(test_null_argument_to_finalize);
    RUN_TEST(test_get_error_null_state);

    // Integration buffering tests (NEED_MORE_DATA scenarios)
    RUN_TEST(test_split_before_magic_number);
    RUN_TEST(test_split_mid_zip_local_header);
    RUN_TEST(test_split_mid_burst_skippable_header);
    RUN_TEST(test_split_mid_burst_skippable_payload);
    RUN_TEST(test_split_mid_local_header_variable_fields);
    RUN_TEST(test_split_at_multiple_boundaries);

    // Central Directory detection tests
    RUN_TEST(test_central_directory_at_expected_offset);
    RUN_TEST(test_central_directory_before_expected_offset);
    RUN_TEST(test_central_directory_after_expected_offset);
    RUN_TEST(test_central_directory_in_processing_frames_state);

    return UNITY_END();
}
