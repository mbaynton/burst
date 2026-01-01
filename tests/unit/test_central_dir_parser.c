/**
 * Unit tests for central directory parser.
 *
 * Tests use hand-crafted byte arrays representing minimal valid ZIP structures.
 */

#include "unity.h"
#include "central_dir_parser.h"
#include <string.h>
#include <stdlib.h>

void setUp(void) {
    // Nothing to set up
}

void tearDown(void) {
    // Nothing to tear down
}

// =============================================================================
// Test Data: Minimal valid ZIP file with one empty file "a.txt"
// =============================================================================

// Structure:
// - Local file header (30 bytes + 5 bytes filename = 35 bytes)
// - No file data (empty file, stored)
// - Data descriptor (16 bytes)
// - Central directory header (46 bytes + 5 bytes filename = 51 bytes)
// - End of central directory (22 bytes)
// Total: 35 + 16 + 51 + 22 = 124 bytes

static const uint8_t minimal_zip[] = {
    // Local file header (offset 0)
    0x50, 0x4b, 0x03, 0x04,  // signature
    0x0a, 0x00,              // version needed (1.0)
    0x08, 0x00,              // flags (data descriptor)
    0x00, 0x00,              // compression method (store)
    0x00, 0x00,              // last mod time
    0x00, 0x00,              // last mod date
    0x00, 0x00, 0x00, 0x00,  // crc32 (in data descriptor)
    0x00, 0x00, 0x00, 0x00,  // compressed size (in data descriptor)
    0x00, 0x00, 0x00, 0x00,  // uncompressed size (in data descriptor)
    0x05, 0x00,              // filename length (5)
    0x00, 0x00,              // extra field length (0)
    'a', '.', 't', 'x', 't', // filename

    // Data descriptor (offset 35)
    0x50, 0x4b, 0x07, 0x08,  // signature
    0x00, 0x00, 0x00, 0x00,  // crc32
    0x00, 0x00, 0x00, 0x00,  // compressed size
    0x00, 0x00, 0x00, 0x00,  // uncompressed size

    // Central directory header (offset 51)
    0x50, 0x4b, 0x01, 0x02,  // signature
    0x14, 0x00,              // version made by
    0x0a, 0x00,              // version needed
    0x08, 0x00,              // flags (data descriptor)
    0x00, 0x00,              // compression method (store)
    0x00, 0x00,              // last mod time
    0x00, 0x00,              // last mod date
    0x00, 0x00, 0x00, 0x00,  // crc32
    0x00, 0x00, 0x00, 0x00,  // compressed size
    0x00, 0x00, 0x00, 0x00,  // uncompressed size
    0x05, 0x00,              // filename length
    0x00, 0x00,              // extra field length
    0x00, 0x00,              // file comment length
    0x00, 0x00,              // disk number start
    0x00, 0x00,              // internal file attributes
    0x00, 0x00, 0x00, 0x00,  // external file attributes
    0x00, 0x00, 0x00, 0x00,  // local header offset
    'a', '.', 't', 'x', 't', // filename

    // End of central directory (offset 102)
    0x50, 0x4b, 0x05, 0x06,  // signature
    0x00, 0x00,              // disk number
    0x00, 0x00,              // disk with CD
    0x01, 0x00,              // entries on this disk
    0x01, 0x00,              // total entries
    0x33, 0x00, 0x00, 0x00,  // CD size (51 bytes)
    0x33, 0x00, 0x00, 0x00,  // CD offset (51)
    0x00, 0x00,              // comment length
};

// =============================================================================
// Test Data: ZIP with two files
// =============================================================================

// Two files: "a.txt" (10 bytes compressed) and "b.txt" (20 bytes compressed)
// Simplified structure for testing part mapping

static const uint8_t two_file_zip[] = {
    // Local file header 1 (offset 0)
    0x50, 0x4b, 0x03, 0x04,
    0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  // crc32
    0x0a, 0x00, 0x00, 0x00,  // compressed size = 10
    0x0a, 0x00, 0x00, 0x00,  // uncompressed size = 10
    0x05, 0x00, 0x00, 0x00,  // filename length = 5, extra = 0
    'a', '.', 't', 'x', 't',

    // File data 1 (10 bytes of zeros)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // Local file header 2 (offset 45)
    0x50, 0x4b, 0x03, 0x04,
    0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  // crc32
    0x14, 0x00, 0x00, 0x00,  // compressed size = 20
    0x14, 0x00, 0x00, 0x00,  // uncompressed size = 20
    0x05, 0x00, 0x00, 0x00,  // filename length = 5, extra = 0
    'b', '.', 't', 'x', 't',

    // File data 2 (20 bytes of zeros)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // Central directory header 1 (offset 100)
    0x50, 0x4b, 0x01, 0x02,
    0x14, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0xab, 0xcd, 0xef, 0x12,  // crc32 = 0x12efcdab
    0x0a, 0x00, 0x00, 0x00,  // compressed size = 10
    0x0a, 0x00, 0x00, 0x00,  // uncompressed size = 10
    0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  // local header offset = 0
    'a', '.', 't', 'x', 't',

    // Central directory header 2 (offset 151)
    0x50, 0x4b, 0x01, 0x02,
    0x14, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x34, 0x56, 0x78, 0x9a,  // crc32 = 0x9a785634
    0x14, 0x00, 0x00, 0x00,  // compressed size = 20
    0x14, 0x00, 0x00, 0x00,  // uncompressed size = 20
    0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x2d, 0x00, 0x00, 0x00,  // local header offset = 45
    'b', '.', 't', 'x', 't',

    // End of central directory (offset 202)
    0x50, 0x4b, 0x05, 0x06,
    0x00, 0x00, 0x00, 0x00,
    0x02, 0x00,              // entries on this disk = 2
    0x02, 0x00,              // total entries = 2
    0x66, 0x00, 0x00, 0x00,  // CD size = 102
    0x64, 0x00, 0x00, 0x00,  // CD offset = 100
    0x00, 0x00,
};

// =============================================================================
// EOCD Tests
// =============================================================================

void test_find_eocd_at_end(void) {
    struct central_dir_parse_result result;
    int rc = central_dir_parse(minimal_zip, sizeof(minimal_zip),
                               sizeof(minimal_zip), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, result.error_code);
    TEST_ASSERT_EQUAL_UINT64(51, result.central_dir_offset);
    TEST_ASSERT_FALSE(result.is_zip64);

    central_dir_parse_result_free(&result);
}

void test_find_eocd_with_comment(void) {
    // Create a buffer with EOCD followed by a 10-byte comment
    uint8_t buffer[sizeof(minimal_zip) + 10];
    memcpy(buffer, minimal_zip, sizeof(minimal_zip));

    // Fix the comment length field in EOCD
    // EOCD starts at offset 102, comment length is at offset +20 from EOCD start
    buffer[102 + 20] = 0x0a;  // comment length = 10
    buffer[102 + 21] = 0x00;

    // Add comment bytes
    memset(buffer + sizeof(minimal_zip), 'X', 10);

    struct central_dir_parse_result result;
    int rc = central_dir_parse(buffer, sizeof(buffer), sizeof(buffer), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_UINT64(51, result.central_dir_offset);

    central_dir_parse_result_free(&result);
}

void test_find_eocd_not_found(void) {
    uint8_t buffer[100];
    memset(buffer, 0, sizeof(buffer));

    struct central_dir_parse_result result;
    int rc = central_dir_parse(buffer, sizeof(buffer), sizeof(buffer), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_ERR_NO_EOCD, rc);
    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_ERR_NO_EOCD, result.error_code);
}

void test_find_eocd_zip64_detected(void) {
    // Create buffer with ZIP64 EOCD Locator before standard EOCD
    uint8_t buffer[200];
    memcpy(buffer, minimal_zip, sizeof(minimal_zip));

    // Insert ZIP64 EOCD Locator signature 20 bytes before EOCD
    // EOCD is at offset 102, so locator signature at 82
    buffer[82] = 0x50;
    buffer[83] = 0x4b;
    buffer[84] = 0x06;
    buffer[85] = 0x07;

    struct central_dir_parse_result result;
    int rc = central_dir_parse(buffer, sizeof(minimal_zip),
                               sizeof(minimal_zip), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_ERR_ZIP64_UNSUPPORTED, rc);
    TEST_ASSERT_TRUE(result.is_zip64);
}

// =============================================================================
// Central Directory Tests
// =============================================================================

void test_parse_single_file(void) {
    struct central_dir_parse_result result;
    int rc = central_dir_parse(minimal_zip, sizeof(minimal_zip),
                               sizeof(minimal_zip), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_size_t(1, result.num_files);
    TEST_ASSERT_NOT_NULL(result.files);
    TEST_ASSERT_EQUAL_STRING("a.txt", result.files[0].filename);
    TEST_ASSERT_EQUAL_UINT64(0, result.files[0].local_header_offset);
    TEST_ASSERT_EQUAL_UINT64(0, result.files[0].compressed_size);
    TEST_ASSERT_EQUAL_UINT64(0, result.files[0].uncompressed_size);

    central_dir_parse_result_free(&result);
}

void test_parse_multiple_files(void) {
    struct central_dir_parse_result result;
    int rc = central_dir_parse(two_file_zip, sizeof(two_file_zip),
                               sizeof(two_file_zip), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_size_t(2, result.num_files);

    TEST_ASSERT_EQUAL_STRING("a.txt", result.files[0].filename);
    TEST_ASSERT_EQUAL_UINT64(0, result.files[0].local_header_offset);
    TEST_ASSERT_EQUAL_UINT64(10, result.files[0].compressed_size);
    TEST_ASSERT_EQUAL_UINT32(0x12efcdab, result.files[0].crc32);

    TEST_ASSERT_EQUAL_STRING("b.txt", result.files[1].filename);
    TEST_ASSERT_EQUAL_UINT64(45, result.files[1].local_header_offset);
    TEST_ASSERT_EQUAL_UINT64(20, result.files[1].compressed_size);
    TEST_ASSERT_EQUAL_UINT32(0x9a785634, result.files[1].crc32);

    central_dir_parse_result_free(&result);
}

void test_parse_empty_archive(void) {
    // EOCD only, no files
    static const uint8_t empty_zip[] = {
        // End of central directory
        0x50, 0x4b, 0x05, 0x06,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,              // entries = 0
        0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,  // CD size = 0
        0x00, 0x00, 0x00, 0x00,  // CD offset = 0
        0x00, 0x00,
    };

    struct central_dir_parse_result result;
    int rc = central_dir_parse(empty_zip, sizeof(empty_zip),
                               sizeof(empty_zip), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_size_t(0, result.num_files);

    central_dir_parse_result_free(&result);
}

void test_parse_truncated(void) {
    // Cut off the central directory mid-way
    uint8_t buffer[80];
    memcpy(buffer, minimal_zip, sizeof(buffer));

    // Adjust EOCD to point to offset that's beyond our truncated buffer
    struct central_dir_parse_result result;
    int rc = central_dir_parse(buffer, sizeof(buffer), sizeof(buffer), BURST_BASE_PART_SIZE, &result);

    // Should fail because we don't have the complete central directory
    TEST_ASSERT_NOT_EQUAL(CENTRAL_DIR_PARSE_SUCCESS, rc);

    central_dir_parse_result_free(&result);
}

// =============================================================================
// Part Index and Mapping Tests
// =============================================================================

void test_part_index_calculation(void) {
    struct central_dir_parse_result result;
    int rc = central_dir_parse(minimal_zip, sizeof(minimal_zip),
                               sizeof(minimal_zip), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    // File at offset 0 should be in part 0
    TEST_ASSERT_EQUAL_UINT32(0, result.files[0].part_index);

    central_dir_parse_result_free(&result);
}

void test_part_index_at_boundary(void) {
    // Create a fake ZIP where a file starts at exactly 8 MiB
    // We'll just modify the local_header_offset in the central dir

    uint8_t buffer[sizeof(two_file_zip)];
    memcpy(buffer, two_file_zip, sizeof(buffer));

    // Modify second file's local_header_offset to be exactly 8 MiB
    // Central directory header 2 starts at offset 151
    // local_header_offset is at offset +42 from CD header start
    uint32_t offset_8mib = 8 * 1024 * 1024;
    memcpy(buffer + 151 + 42, &offset_8mib, sizeof(offset_8mib));

    // We need to also update the EOCD's central_dir_offset to be consistent
    // with the fake archive size. The buffer represents the tail of a 16 MiB archive.
    // In the real archive, the CD would start at (16 MiB - size of buffer) + 100
    // where 100 is the offset within our buffer.
    // Actually, let's keep it simple: the archive is sizeof(buffer) bytes,
    // and we just verify the part_index calculation is correct.
    // The buffer represents the last sizeof(buffer) bytes of a larger archive.

    // For this test, let's use archive_size = buffer_size so offsets work correctly
    // and just verify that the part_index is calculated correctly from local_header_offset
    struct central_dir_parse_result result;
    int rc = central_dir_parse(buffer, sizeof(buffer), sizeof(buffer), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_UINT32(0, result.files[0].part_index);
    // File at 8 MiB should be in part 1 (8MiB / 8MiB = 1)
    TEST_ASSERT_EQUAL_UINT32(1, result.files[1].part_index);

    central_dir_parse_result_free(&result);
}

void test_part_map_single_part(void) {
    struct central_dir_parse_result result;
    int rc = central_dir_parse(minimal_zip, sizeof(minimal_zip),
                               sizeof(minimal_zip), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_size_t(1, result.num_parts);
    TEST_ASSERT_NOT_NULL(result.parts);
    TEST_ASSERT_EQUAL_size_t(1, result.parts[0].num_entries);
    TEST_ASSERT_EQUAL_size_t(0, result.parts[0].entries[0].file_index);
    TEST_ASSERT_EQUAL_UINT64(0, result.parts[0].entries[0].offset_in_part);
    TEST_ASSERT_NULL(result.parts[0].continuing_file);

    central_dir_parse_result_free(&result);
}

void test_part_map_multiple_files_same_part(void) {
    struct central_dir_parse_result result;
    int rc = central_dir_parse(two_file_zip, sizeof(two_file_zip),
                               sizeof(two_file_zip), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_size_t(1, result.num_parts);
    TEST_ASSERT_EQUAL_size_t(2, result.parts[0].num_entries);

    // Both files should be in part 0
    // They should be sorted by offset
    TEST_ASSERT_EQUAL_size_t(0, result.parts[0].entries[0].file_index);
    TEST_ASSERT_EQUAL_UINT64(0, result.parts[0].entries[0].offset_in_part);
    TEST_ASSERT_EQUAL_size_t(1, result.parts[0].entries[1].file_index);
    TEST_ASSERT_EQUAL_UINT64(45, result.parts[0].entries[1].offset_in_part);

    central_dir_parse_result_free(&result);
}

void test_entries_sorted_by_offset(void) {
    // Create a ZIP where central directory order differs from archive order
    // We'll swap the order of central directory headers

    uint8_t buffer[sizeof(two_file_zip)];
    memcpy(buffer, two_file_zip, sizeof(buffer));

    // Swap the two central directory entries
    // CD header 1 is at offset 100 (51 bytes)
    // CD header 2 is at offset 151 (51 bytes)

    uint8_t cd1[51], cd2[51];
    memcpy(cd1, buffer + 100, 51);
    memcpy(cd2, buffer + 151, 51);
    memcpy(buffer + 100, cd2, 51);
    memcpy(buffer + 151, cd1, 51);

    struct central_dir_parse_result result;
    int rc = central_dir_parse(buffer, sizeof(buffer), sizeof(buffer), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);

    // Files in result.files are in central directory order (now b.txt, a.txt)
    TEST_ASSERT_EQUAL_STRING("b.txt", result.files[0].filename);
    TEST_ASSERT_EQUAL_STRING("a.txt", result.files[1].filename);

    // But entries in part map should still be sorted by offset
    // a.txt (file_index=1) is at offset 0
    // b.txt (file_index=0) is at offset 45
    TEST_ASSERT_EQUAL_size_t(1, result.parts[0].entries[0].file_index);  // a.txt at offset 0
    TEST_ASSERT_EQUAL_UINT64(0, result.parts[0].entries[0].offset_in_part);
    TEST_ASSERT_EQUAL_size_t(0, result.parts[0].entries[1].file_index);  // b.txt at offset 45
    TEST_ASSERT_EQUAL_UINT64(45, result.parts[0].entries[1].offset_in_part);

    central_dir_parse_result_free(&result);
}

void test_continuing_file_detection(void) {
    // Test the continuing_file pointer logic.
    //
    // Scenario: A 10 MiB archive where:
    // - File 0 starts at offset 0, compressed_size = 9 MiB (spans into part 1)
    // - File 1 starts at offset 8 MiB + 1000 (in part 1)
    // - Central directory is at offset 10 MiB - 124 bytes (in the tail buffer)
    //
    // We simulate fetching the last 124 bytes of the archive (just the CD + EOCD).

    // Archive layout:
    // [0, 8 MiB): Part 0 - file 0 local header + start of file 0 data
    // [8 MiB, 10 MiB): Part 1 - rest of file 0 data + file 1 + CD + EOCD
    //
    // Our buffer contains only the CD + EOCD (the tail).
    // archive_size = 10 MiB
    // buffer_size = size of CD + EOCD

    // Build a buffer with just CD + EOCD (102 bytes CD + 22 bytes EOCD = 124 bytes)
    uint8_t buffer[124];

    // Central directory header 1 for file 0 (51 bytes)
    uint8_t *cd1 = buffer;
    memset(cd1, 0, 51);
    // Signature
    cd1[0] = 0x50; cd1[1] = 0x4b; cd1[2] = 0x01; cd1[3] = 0x02;
    // local_header_offset = 0 (at byte offset 42)
    uint32_t offset0 = 0;
    memcpy(cd1 + 42, &offset0, 4);
    // compressed_size = 9 MiB (at byte offset 20)
    uint32_t size0 = 9 * 1024 * 1024;
    memcpy(cd1 + 20, &size0, 4);
    // uncompressed_size = 9 MiB (at byte offset 24)
    memcpy(cd1 + 24, &size0, 4);
    // filename_length = 5 (at byte offset 28)
    cd1[28] = 5;
    // Filename
    memcpy(cd1 + 46, "a.txt", 5);

    // Central directory header 2 for file 1 (51 bytes)
    uint8_t *cd2 = buffer + 51;
    memset(cd2, 0, 51);
    // Signature
    cd2[0] = 0x50; cd2[1] = 0x4b; cd2[2] = 0x01; cd2[3] = 0x02;
    // local_header_offset = 8 MiB + 1000 (at byte offset 42)
    uint32_t offset1 = 8 * 1024 * 1024 + 1000;
    memcpy(cd2 + 42, &offset1, 4);
    // compressed_size = 100 (at byte offset 20)
    uint32_t size1 = 100;
    memcpy(cd2 + 20, &size1, 4);
    // uncompressed_size = 100 (at byte offset 24)
    memcpy(cd2 + 24, &size1, 4);
    // filename_length = 5 (at byte offset 28)
    cd2[28] = 5;
    // Filename
    memcpy(cd2 + 46, "b.txt", 5);

    // End of central directory (22 bytes)
    uint8_t *eocd = buffer + 102;
    memset(eocd, 0, 22);
    // Signature
    eocd[0] = 0x50; eocd[1] = 0x4b; eocd[2] = 0x05; eocd[3] = 0x06;
    // num_entries_this_disk = 2 (at byte offset 8)
    eocd[8] = 2;
    // num_entries_total = 2 (at byte offset 10)
    eocd[10] = 2;
    // central_dir_size = 102 (at byte offset 12)
    uint32_t cd_size = 102;
    memcpy(eocd + 12, &cd_size, 4);
    // central_dir_offset = 10 MiB - 124 bytes = where CD starts in archive
    uint64_t archive_size = 10 * 1024 * 1024;
    uint32_t cd_offset = (uint32_t)(archive_size - sizeof(buffer));
    memcpy(eocd + 16, &cd_offset, 4);

    struct central_dir_parse_result result;
    int rc = central_dir_parse(buffer, sizeof(buffer), archive_size, BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);

    // Should have 2 parts (10 MiB / 8 MiB = 2, rounded up)
    TEST_ASSERT_EQUAL_size_t(2, result.num_parts);

    // Part 0: file 0 starts here
    TEST_ASSERT_EQUAL_size_t(1, result.parts[0].num_entries);
    TEST_ASSERT_NULL(result.parts[0].continuing_file);

    // Part 1: file 1 starts here, file 0 continues from part 0
    TEST_ASSERT_EQUAL_size_t(1, result.parts[1].num_entries);
    TEST_ASSERT_NOT_NULL(result.parts[1].continuing_file);
    TEST_ASSERT_EQUAL_STRING("a.txt", result.parts[1].continuing_file->filename);

    central_dir_parse_result_free(&result);
}

void test_no_continuing_file(void) {
    struct central_dir_parse_result result;
    int rc = central_dir_parse(minimal_zip, sizeof(minimal_zip),
                               sizeof(minimal_zip), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_NULL(result.parts[0].continuing_file);

    central_dir_parse_result_free(&result);
}

// =============================================================================
// Error Handling Tests
// =============================================================================

void test_null_buffer(void) {
    struct central_dir_parse_result result;
    int rc = central_dir_parse(NULL, 100, 100, BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER, rc);
}

void test_zero_size(void) {
    struct central_dir_parse_result result;
    uint8_t buffer[1] = {0};
    int rc = central_dir_parse(buffer, 0, 0, BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER, rc);
}

void test_null_result(void) {
    int rc = central_dir_parse(minimal_zip, sizeof(minimal_zip),
                               sizeof(minimal_zip), BURST_BASE_PART_SIZE, NULL);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER, rc);
}

void test_invalid_cd_signature(void) {
    uint8_t buffer[sizeof(minimal_zip)];
    memcpy(buffer, minimal_zip, sizeof(buffer));

    // Corrupt the central directory signature
    buffer[51] = 0x00;

    struct central_dir_parse_result result;
    int rc = central_dir_parse(buffer, sizeof(buffer), sizeof(buffer), BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_ERR_INVALID_SIGNATURE, rc);

    central_dir_parse_result_free(&result);
}

// =============================================================================
// Cleanup Tests
// =============================================================================

void test_free_null_result(void) {
    // Should not crash
    central_dir_parse_result_free(NULL);
}

void test_free_empty_result(void) {
    struct central_dir_parse_result result;
    memset(&result, 0, sizeof(result));

    // Should not crash
    central_dir_parse_result_free(&result);
}

void test_double_free_protection(void) {
    struct central_dir_parse_result result;
    int rc = central_dir_parse(minimal_zip, sizeof(minimal_zip),
                               sizeof(minimal_zip), BURST_BASE_PART_SIZE, &result);
    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);

    central_dir_parse_result_free(&result);

    // After free, structure should be zeroed
    TEST_ASSERT_NULL(result.files);
    TEST_ASSERT_NULL(result.parts);
    TEST_ASSERT_EQUAL_size_t(0, result.num_files);
    TEST_ASSERT_EQUAL_size_t(0, result.num_parts);

    // Second free should be safe
    central_dir_parse_result_free(&result);
}

// =============================================================================
// Main
// =============================================================================

int main(void) {
    UNITY_BEGIN();

    // EOCD tests
    RUN_TEST(test_find_eocd_at_end);
    RUN_TEST(test_find_eocd_with_comment);
    RUN_TEST(test_find_eocd_not_found);
    RUN_TEST(test_find_eocd_zip64_detected);

    // Central directory tests
    RUN_TEST(test_parse_single_file);
    RUN_TEST(test_parse_multiple_files);
    RUN_TEST(test_parse_empty_archive);
    RUN_TEST(test_parse_truncated);

    // Part index and mapping tests
    RUN_TEST(test_part_index_calculation);
    RUN_TEST(test_part_index_at_boundary);
    RUN_TEST(test_part_map_single_part);
    RUN_TEST(test_part_map_multiple_files_same_part);
    RUN_TEST(test_entries_sorted_by_offset);
    RUN_TEST(test_continuing_file_detection);
    RUN_TEST(test_no_continuing_file);

    // Error handling tests
    RUN_TEST(test_null_buffer);
    RUN_TEST(test_zero_size);
    RUN_TEST(test_null_result);
    RUN_TEST(test_invalid_cd_signature);

    // Cleanup tests
    RUN_TEST(test_free_null_result);
    RUN_TEST(test_free_empty_result);
    RUN_TEST(test_double_free_protection);

    return UNITY_END();
}
