#include "unity.h"
#include "frame_parser.h"
#include "stream_processor.h"

#include <string.h>
#include <stdint.h>

// ZIP format constants
#define ZIP_LOCAL_FILE_HEADER_SIG 0x04034b50
#define ZIP_DATA_DESCRIPTOR_SIG 0x08074b50

// Zstandard magic
#define ZSTD_MAGIC 0xFD2FB528

// BURST skippable frame magic
#define BURST_MAGIC 0x184D2A5B

void setUp(void) {
    // No setup needed for these tests
}

void tearDown(void) {
    // No teardown needed
}

// =============================================================================
// Helper Functions
// =============================================================================

// Helper to create a minimal ZIP local file header
static size_t create_zip_local_header(uint8_t *buffer, const char *filename) {
    size_t filename_len = strlen(filename);
    size_t header_size = 30 + filename_len;  // Fixed header + filename

    // Signature
    uint32_t sig = ZIP_LOCAL_FILE_HEADER_SIG;
    memcpy(buffer, &sig, 4);

    // Version needed (63 for Zstd)
    uint16_t version = 63;
    memcpy(buffer + 4, &version, 2);

    // Flags (data descriptor)
    uint16_t flags = 0x0008;
    memcpy(buffer + 6, &flags, 2);

    // Compression method (93 = Zstd)
    uint16_t method = 93;
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

// Helper to create a ZIP data descriptor
static size_t create_data_descriptor(uint8_t *buffer) {
    uint32_t sig = ZIP_DATA_DESCRIPTOR_SIG;
    memcpy(buffer, &sig, 4);
    memset(buffer + 4, 0, 12);  // CRC, compressed size, uncompressed size
    return 16;
}

// Helper to create a valid Zstd frame with known content size
static size_t create_zstd_frame(uint8_t *buffer, size_t uncompressed_size) {
    size_t offset = 0;

    // Zstd magic number
    uint32_t magic = ZSTD_MAGIC;
    memcpy(buffer + offset, &magic, 4);
    offset += 4;

    // Frame header descriptor:
    // Bit 7-6: Frame_Content_Size_flag (11 = 8 bytes FCS)
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

// =============================================================================
// Test Group: Frame Magic Detection (Success Cases)
// =============================================================================

void test_parse_zip_local_header(void) {
    uint8_t buffer[256];
    const char *filename = "test.txt";
    size_t header_size = create_zip_local_header(buffer, filename);

    struct frame_info info;
    int rc = parse_next_frame(buffer, header_size, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(FRAME_ZIP_LOCAL_HEADER, info.type);
    TEST_ASSERT_EQUAL(header_size, info.frame_size);
}

void test_parse_zip_local_header_long_filename(void) {
    uint8_t buffer[256];
    const char *filename = "path/to/deeply/nested/directory/file.txt";
    size_t header_size = create_zip_local_header(buffer, filename);

    struct frame_info info;
    int rc = parse_next_frame(buffer, header_size, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(FRAME_ZIP_LOCAL_HEADER, info.type);
    TEST_ASSERT_EQUAL(header_size, info.frame_size);
}

void test_parse_zip_data_descriptor(void) {
    uint8_t buffer[16];
    create_data_descriptor(buffer);

    struct frame_info info;
    int rc = parse_next_frame(buffer, 16, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(FRAME_ZIP_DATA_DESCRIPTOR, info.type);
    TEST_ASSERT_EQUAL(16, info.frame_size);
}

void test_parse_zstd_frame(void) {
    uint8_t buffer[64];
    size_t uncompressed_size = 1000;
    size_t frame_size = create_zstd_frame(buffer, uncompressed_size);

    struct frame_info info;
    int rc = parse_next_frame(buffer, frame_size, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(FRAME_ZSTD_COMPRESSED, info.type);
    TEST_ASSERT_EQUAL(frame_size, info.frame_size);
    TEST_ASSERT_EQUAL(uncompressed_size, info.uncompressed_size);
}

void test_parse_burst_padding(void) {
    uint8_t buffer[64];
    size_t padding_size = 32;
    size_t frame_size = create_padding_frame(buffer, padding_size);

    struct frame_info info;
    int rc = parse_next_frame(buffer, frame_size, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(FRAME_BURST_PADDING, info.type);
    TEST_ASSERT_EQUAL(frame_size, info.frame_size);
}

void test_parse_burst_start_of_part(void) {
    uint8_t buffer[32];
    uint64_t uncompressed_offset = 12345678;
    size_t frame_size = create_start_of_part_frame(buffer, uncompressed_offset);

    struct frame_info info;
    int rc = parse_next_frame(buffer, frame_size, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(FRAME_BURST_START_OF_PART, info.type);
    TEST_ASSERT_EQUAL(frame_size, info.frame_size);
    TEST_ASSERT_EQUAL(uncompressed_offset, info.start_of_part_offset);
}

void test_parse_zero_payload_burst(void) {
    uint8_t buffer[8];
    size_t frame_size = create_padding_frame(buffer, 0);

    struct frame_info info;
    int rc = parse_next_frame(buffer, frame_size, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(FRAME_BURST_PADDING, info.type);
    TEST_ASSERT_EQUAL(8, info.frame_size);
}

// =============================================================================
// Test Group: NEED_MORE_DATA Returns
// =============================================================================

void test_parse_too_few_bytes_for_magic(void) {
    uint8_t buffer[3] = {0x50, 0x4b, 0x03};  // Partial ZIP magic

    struct frame_info info;
    int rc = parse_next_frame(buffer, 3, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_NEED_MORE_DATA, rc);
}

void test_parse_zero_bytes(void) {
    uint8_t buffer[1];
    struct frame_info info;
    int rc = parse_next_frame(buffer, 0, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_NEED_MORE_DATA, rc);
}

void test_parse_one_byte(void) {
    uint8_t buffer[1] = {0x50};
    struct frame_info info;
    int rc = parse_next_frame(buffer, 1, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_NEED_MORE_DATA, rc);
}

void test_parse_partial_zip_header(void) {
    uint8_t buffer[256];
    create_zip_local_header(buffer, "test.txt");

    // Only provide magic + 5 bytes (9 bytes total, less than 30 byte header)
    struct frame_info info;
    int rc = parse_next_frame(buffer, 9, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_NEED_MORE_DATA, rc);
}

void test_parse_partial_zstd_incomplete(void) {
    uint8_t buffer[64];
    create_zstd_frame(buffer, 100);

    // Only provide magic + 1 byte (5 bytes total)
    struct frame_info info;
    int rc = parse_next_frame(buffer, 5, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_NEED_MORE_DATA, rc);
}

void test_parse_partial_burst_header(void) {
    uint8_t buffer[64];
    create_padding_frame(buffer, 16);

    // Only provide magic + 2 bytes (6 bytes, less than 8 needed for header)
    struct frame_info info;
    int rc = parse_next_frame(buffer, 6, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_NEED_MORE_DATA, rc);
}

void test_parse_partial_burst_payload(void) {
    uint8_t buffer[64];
    create_padding_frame(buffer, 32);  // Total frame size = 40 bytes

    // Provide header (8 bytes) + partial payload (10 bytes) = 18 bytes
    struct frame_info info;
    int rc = parse_next_frame(buffer, 18, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_NEED_MORE_DATA, rc);
}

void test_parse_partial_start_of_part_payload(void) {
    uint8_t buffer[32];
    create_start_of_part_frame(buffer, 1000);  // Total frame size = 24 bytes

    // Provide header (8 bytes) + partial payload (5 bytes) = 13 bytes
    struct frame_info info;
    int rc = parse_next_frame(buffer, 13, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_NEED_MORE_DATA, rc);
}

// =============================================================================
// Test Group: Error Returns
// =============================================================================

void test_parse_unknown_magic(void) {
    uint8_t buffer[64];
    uint32_t unknown_magic = 0xDEADBEEF;
    memcpy(buffer, &unknown_magic, 4);
    memset(buffer + 4, 0, 60);

    struct frame_info info;
    int rc = parse_next_frame(buffer, 64, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_FRAME, rc);
    TEST_ASSERT_EQUAL(FRAME_UNKNOWN, info.type);
}

void test_parse_zstd_without_content_size(void) {
    uint8_t buffer[64];
    size_t offset = 0;

    // Zstd magic
    uint32_t magic = ZSTD_MAGIC;
    memcpy(buffer + offset, &magic, 4);
    offset += 4;

    // Frame header descriptor: FCS_flag=0 (no content size), Single_Segment=0
    buffer[offset++] = 0x00;

    // Window descriptor (required when Single_Segment=0)
    buffer[offset++] = 0x00;

    // Block header for empty raw block (last block)
    buffer[offset++] = 0x01;
    buffer[offset++] = 0x00;
    buffer[offset++] = 0x00;

    struct frame_info info;
    int rc = parse_next_frame(buffer, offset, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_FRAME, rc);
}

void test_parse_another_unknown_magic(void) {
    uint8_t buffer[64];
    uint32_t unknown_magic = 0xCAFEBABE;
    memcpy(buffer, &unknown_magic, 4);
    memset(buffer + 4, 0, 60);

    struct frame_info info;
    int rc = parse_next_frame(buffer, 64, &info);

    TEST_ASSERT_EQUAL(STREAM_PROC_ERR_INVALID_FRAME, rc);
    TEST_ASSERT_EQUAL(FRAME_UNKNOWN, info.type);
}

// =============================================================================
// Test Group: Edge Cases
// =============================================================================

void test_parse_exactly_minimum_bytes(void) {
    uint8_t buffer[4];
    uint32_t sig = ZIP_DATA_DESCRIPTOR_SIG;
    memcpy(buffer, &sig, 4);

    // Data descriptor only needs 4 bytes to identify (magic)
    // but needs 16 bytes total - let's test the minimum for detection
    struct frame_info info;
    int rc = parse_next_frame(buffer, 4, &info);

    // Should succeed since data descriptor size is fixed at 16
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(FRAME_ZIP_DATA_DESCRIPTOR, info.type);
}

void test_parse_burst_with_non_standard_type_byte(void) {
    uint8_t buffer[32];

    // Create a BURST frame with an unknown type byte (not 0x00 or 0x01)
    uint32_t magic = BURST_MAGIC;
    uint32_t payload_size = 16;
    memcpy(buffer, &magic, 4);
    memcpy(buffer + 4, &payload_size, 4);
    buffer[8] = 0x05;  // Unknown type byte
    memset(buffer + 9, 0, 15);

    struct frame_info info;
    int rc = parse_next_frame(buffer, 24, &info);

    // Unknown type bytes should be treated as padding
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(FRAME_BURST_PADDING, info.type);
}

void test_parse_start_of_part_wrong_payload_size(void) {
    uint8_t buffer[32];

    // Create a BURST frame with type 0x01 but wrong payload size (not 16)
    uint32_t magic = BURST_MAGIC;
    uint32_t payload_size = 20;  // Should be 16 for Start-of-Part
    memcpy(buffer, &magic, 4);
    memcpy(buffer + 4, &payload_size, 4);
    buffer[8] = 0x01;  // Start-of-Part type byte
    memset(buffer + 9, 0, 19);

    struct frame_info info;
    int rc = parse_next_frame(buffer, 28, &info);

    // Wrong payload size with type 0x01 should be treated as padding
    TEST_ASSERT_EQUAL(STREAM_PROC_SUCCESS, rc);
    TEST_ASSERT_EQUAL(FRAME_BURST_PADDING, info.type);
}

// =============================================================================
// Main
// =============================================================================

int main(void) {
    UNITY_BEGIN();

    // Frame Magic Detection (Success Cases)
    RUN_TEST(test_parse_zip_local_header);
    RUN_TEST(test_parse_zip_local_header_long_filename);
    RUN_TEST(test_parse_zip_data_descriptor);
    RUN_TEST(test_parse_zstd_frame);
    RUN_TEST(test_parse_burst_padding);
    RUN_TEST(test_parse_burst_start_of_part);
    RUN_TEST(test_parse_zero_payload_burst);

    // NEED_MORE_DATA Returns
    RUN_TEST(test_parse_too_few_bytes_for_magic);
    RUN_TEST(test_parse_zero_bytes);
    RUN_TEST(test_parse_one_byte);
    RUN_TEST(test_parse_partial_zip_header);
    RUN_TEST(test_parse_partial_zstd_incomplete);
    RUN_TEST(test_parse_partial_burst_header);
    RUN_TEST(test_parse_partial_burst_payload);
    RUN_TEST(test_parse_partial_start_of_part_payload);

    // Error Returns
    RUN_TEST(test_parse_unknown_magic);
    RUN_TEST(test_parse_zstd_without_content_size);
    RUN_TEST(test_parse_another_unknown_magic);

    // Edge Cases
    RUN_TEST(test_parse_exactly_minimum_bytes);
    RUN_TEST(test_parse_burst_with_non_standard_type_byte);
    RUN_TEST(test_parse_start_of_part_wrong_payload_size);

    return UNITY_END();
}
