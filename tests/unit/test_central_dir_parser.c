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
    // Since we now support ZIP64, the parser will attempt to parse it.
    // However, this test data doesn't contain a valid ZIP64 EOCD record,
    // so parsing will fail with truncated error (the locator points to garbage).
    uint8_t buffer[200];
    memcpy(buffer, minimal_zip, sizeof(minimal_zip));

    // Insert ZIP64 EOCD Locator 20 bytes before EOCD
    // EOCD is at offset 102, so locator starts at 82
    // Locator: signature (4) + disk_with_eocd64 (4) + eocd64_offset (8) + total_disks (4) = 20 bytes
    buffer[82] = 0x50;  // signature
    buffer[83] = 0x4b;
    buffer[84] = 0x06;
    buffer[85] = 0x07;
    buffer[86] = 0x00;  // disk_with_eocd64
    buffer[87] = 0x00;
    buffer[88] = 0x00;
    buffer[89] = 0x00;
    buffer[90] = 0x00;  // eocd64_offset (pointing to offset 0, which is invalid)
    buffer[91] = 0x00;
    buffer[92] = 0x00;
    buffer[93] = 0x00;
    buffer[94] = 0x00;
    buffer[95] = 0x00;
    buffer[96] = 0x00;
    buffer[97] = 0x00;
    buffer[98] = 0x01;  // total_disks
    buffer[99] = 0x00;
    buffer[100] = 0x00;
    buffer[101] = 0x00;

    struct central_dir_parse_result result;
    int rc = central_dir_parse(buffer, sizeof(minimal_zip),
                               sizeof(minimal_zip), BURST_BASE_PART_SIZE, &result);

    // ZIP64 is detected, but parsing fails because the eocd64_offset (0) points
    // to data that doesn't have a valid ZIP64 EOCD signature
    TEST_ASSERT_TRUE(result.is_zip64);
    // The parse will fail because offset 0 contains LFH signature, not ZIP64 EOCD signature
    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_ERR_INVALID_SIGNATURE, rc);
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
// BURST EOCD Comment Tests
// =============================================================================

/**
 * Test: EOCD with no comment returns 0 for first_cdfh_offset_in_tail.
 */
void test_eocd_no_burst_comment(void) {
    uint64_t cd_offset, cd_size;
    bool is_zip64;
    uint32_t first_cdfh_offset;
    char error_msg[256];

    int rc = central_dir_parse_eocd_only(minimal_zip, sizeof(minimal_zip),
                                          sizeof(minimal_zip),
                                          &cd_offset, &cd_size, NULL, &is_zip64,
                                          &first_cdfh_offset, error_msg);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_UINT32(0, first_cdfh_offset);  // No BURST comment
}

/**
 * Test: EOCD with valid BURST comment extracts first_cdfh_offset_in_tail.
 */
void test_eocd_with_burst_comment(void) {
    // Create a buffer with EOCD followed by a BURST comment
    uint8_t buffer[sizeof(minimal_zip) + BURST_EOCD_COMMENT_SIZE];
    memcpy(buffer, minimal_zip, sizeof(minimal_zip));

    // Fix the comment length field in EOCD
    // EOCD starts at offset 102, comment length is at offset +20 from EOCD start
    buffer[102 + 20] = BURST_EOCD_COMMENT_SIZE;
    buffer[102 + 21] = 0x00;

    // Add BURST comment:
    // Magic "BRST" (0x54535242 little-endian), Version 1, Offset as uint24
    uint8_t *comment = buffer + sizeof(minimal_zip);
    comment[0] = 0x42;  // 'B' = 0x42
    comment[1] = 0x52;  // 'R' = 0x52
    comment[2] = 0x53;  // 'S' = 0x53
    comment[3] = 0x54;  // 'T' = 0x54
    comment[4] = BURST_EOCD_COMMENT_VERSION;  // Version 1
    // Offset = 0x123456 (little-endian uint24)
    comment[5] = 0x56;
    comment[6] = 0x34;
    comment[7] = 0x12;

    uint64_t cd_offset, cd_size;
    bool is_zip64;
    uint32_t first_cdfh_offset;
    char error_msg[256];

    int rc = central_dir_parse_eocd_only(buffer, sizeof(buffer),
                                          sizeof(buffer),
                                          &cd_offset, &cd_size, NULL, &is_zip64,
                                          &first_cdfh_offset, error_msg);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_UINT32(0x123456, first_cdfh_offset);  // BURST comment parsed
}

/**
 * Test: EOCD with invalid BURST magic returns 0 for first_cdfh_offset_in_tail.
 */
void test_eocd_with_non_burst_comment(void) {
    // Create a buffer with EOCD followed by a non-BURST 8-byte comment
    uint8_t buffer[sizeof(minimal_zip) + 8];
    memcpy(buffer, minimal_zip, sizeof(minimal_zip));

    // Fix the comment length field in EOCD
    buffer[102 + 20] = 8;
    buffer[102 + 21] = 0;

    // Add non-BURST comment
    memset(buffer + sizeof(minimal_zip), 'X', 8);

    uint64_t cd_offset, cd_size;
    bool is_zip64;
    uint32_t first_cdfh_offset;
    char error_msg[256];

    int rc = central_dir_parse_eocd_only(buffer, sizeof(buffer),
                                          sizeof(buffer),
                                          &cd_offset, &cd_size, NULL, &is_zip64,
                                          &first_cdfh_offset, error_msg);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_UINT32(0, first_cdfh_offset);  // Non-BURST comment
}

/**
 * Test: EOCD with wrong BURST version returns 0 for first_cdfh_offset_in_tail.
 */
void test_eocd_with_wrong_burst_version(void) {
    uint8_t buffer[sizeof(minimal_zip) + BURST_EOCD_COMMENT_SIZE];
    memcpy(buffer, minimal_zip, sizeof(minimal_zip));

    buffer[102 + 20] = BURST_EOCD_COMMENT_SIZE;
    buffer[102 + 21] = 0;

    uint8_t *comment = buffer + sizeof(minimal_zip);
    comment[0] = 0x42;  // 'B'
    comment[1] = 0x52;  // 'R'
    comment[2] = 0x53;  // 'S'
    comment[3] = 0x54;  // 'T'
    comment[4] = 99;    // Wrong version
    comment[5] = 0x56;
    comment[6] = 0x34;
    comment[7] = 0x12;

    uint64_t cd_offset, cd_size;
    bool is_zip64;
    uint32_t first_cdfh_offset;
    char error_msg[256];

    int rc = central_dir_parse_eocd_only(buffer, sizeof(buffer),
                                          sizeof(buffer),
                                          &cd_offset, &cd_size, NULL, &is_zip64,
                                          &first_cdfh_offset, error_msg);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_UINT32(0, first_cdfh_offset);  // Wrong version
}

/**
 * Test: EOCD with BURST comment indicating no CDFH in tail (sentinel value).
 */
void test_eocd_with_burst_comment_no_cdfh(void) {
    uint8_t buffer[sizeof(minimal_zip) + BURST_EOCD_COMMENT_SIZE];
    memcpy(buffer, minimal_zip, sizeof(minimal_zip));

    buffer[102 + 20] = BURST_EOCD_COMMENT_SIZE;
    buffer[102 + 21] = 0;

    uint8_t *comment = buffer + sizeof(minimal_zip);
    comment[0] = 0x42;  // 'B'
    comment[1] = 0x52;  // 'R'
    comment[2] = 0x53;  // 'S'
    comment[3] = 0x54;  // 'T'
    comment[4] = BURST_EOCD_COMMENT_VERSION;
    // Sentinel value: 0xFFFFFF means no complete CDFH in tail
    comment[5] = 0xFF;
    comment[6] = 0xFF;
    comment[7] = 0xFF;

    uint64_t cd_offset, cd_size;
    bool is_zip64;
    uint32_t first_cdfh_offset;
    char error_msg[256];

    int rc = central_dir_parse_eocd_only(buffer, sizeof(buffer),
                                          sizeof(buffer),
                                          &cd_offset, &cd_size, NULL, &is_zip64,
                                          &first_cdfh_offset, error_msg);

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_UINT32(BURST_EOCD_NO_CDFH_IN_TAIL, first_cdfh_offset);
}

// =============================================================================
// Partial CD Parsing Tests
// =============================================================================

/**
 * Test: Parse partial CD from a simulated tail buffer.
 *
 * Simulates a large archive where the tail buffer contains only a portion
 * of the central directory starting from an offset indicated by BURST comment.
 */
void test_partial_cd_parse_basic(void) {
    // Build a simulated tail buffer containing partial CD data
    // We'll pretend:
    // - Archive size: 100 MiB
    // - CD starts at offset 70 MiB
    // - Tail buffer starts at offset 92 MiB (last 8 MiB)
    // - First complete CDFH in tail is at offset 0 within tail (at the start of buffer)
    //
    // The first_cdfh_offset is now relative to TAIL START (not CD start), so it's 0.

    uint64_t archive_size = 100ULL * 1024 * 1024;
    uint64_t cd_offset = 70ULL * 1024 * 1024;
    uint64_t tail_start = 92ULL * 1024 * 1024;
    uint32_t first_cdfh_offset_in_tail = 0;  // CDFH at start of tail buffer

    // Create a small buffer with one CDFH entry
    // The CDFH will be at the start of our "buffer"
    uint8_t buffer[128];
    memset(buffer, 0, sizeof(buffer));

    // Central directory header for file "test.txt"
    uint8_t *cdfh = buffer;
    cdfh[0] = 0x50; cdfh[1] = 0x4b; cdfh[2] = 0x01; cdfh[3] = 0x02;  // signature
    // version made by = 20
    cdfh[4] = 0x14; cdfh[5] = 0x00;
    // version needed = 10
    cdfh[6] = 0x0a; cdfh[7] = 0x00;
    // flags = 0
    cdfh[8] = 0x00; cdfh[9] = 0x00;
    // compression = 0
    cdfh[10] = 0x00; cdfh[11] = 0x00;
    // mod time/date = 0
    cdfh[12] = 0x00; cdfh[13] = 0x00;
    cdfh[14] = 0x00; cdfh[15] = 0x00;
    // crc32 = 0x12345678
    cdfh[16] = 0x78; cdfh[17] = 0x56; cdfh[18] = 0x34; cdfh[19] = 0x12;
    // compressed size = 1000
    uint32_t comp_size = 1000;
    memcpy(cdfh + 20, &comp_size, 4);
    // uncompressed size = 1000
    memcpy(cdfh + 24, &comp_size, 4);
    // filename length = 8 ("test.txt")
    cdfh[28] = 8; cdfh[29] = 0;
    // extra field length = 0
    cdfh[30] = 0; cdfh[31] = 0;
    // file comment length = 0
    cdfh[32] = 0; cdfh[33] = 0;
    // disk number start = 0
    cdfh[34] = 0; cdfh[35] = 0;
    // internal attributes = 0
    cdfh[36] = 0; cdfh[37] = 0;
    // external attributes = 0
    cdfh[38] = 0; cdfh[39] = 0; cdfh[40] = 0; cdfh[41] = 0;
    // local header offset = 80 MiB (in part 10)
    uint32_t local_offset = 80UL * 1024 * 1024;
    memcpy(cdfh + 42, &local_offset, 4);
    // filename
    memcpy(cdfh + 46, "test.txt", 8);

    // Now call partial parse
    struct central_dir_parse_result result;
    int rc = central_dir_parse_partial(
        buffer, sizeof(buffer),
        tail_start,                 // buffer starts at this archive offset
        cd_offset,                  // CD starts at this archive offset
        first_cdfh_offset_in_tail,  // First complete CDFH offset from TAIL START
        archive_size,
        BURST_BASE_PART_SIZE,
        false,                      // not ZIP64
        &result
    );

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_size_t(1, result.num_files);
    TEST_ASSERT_EQUAL_STRING("test.txt", result.files[0].filename);
    TEST_ASSERT_EQUAL_UINT32(0x12345678, result.files[0].crc32);
    TEST_ASSERT_EQUAL_UINT64(local_offset, result.files[0].local_header_offset);
    // Part index: 80 MiB / 8 MiB = 10
    TEST_ASSERT_EQUAL_UINT32(10, result.files[0].part_index);

    central_dir_parse_result_free(&result);
}

/**
 * Test: Partial CD parse with non-zero offset within tail buffer.
 *
 * This test verifies the key semantic: first_cdfh_offset is relative to
 * TAIL START (buffer_start_offset), not CD start. The CDFH is placed at
 * offset 1024 within the buffer, and the test verifies it's found correctly.
 */
void test_partial_cd_parse_nonzero_offset(void) {
    // Scenario:
    // - Archive size: 100 MiB
    // - CD starts at offset 70 MiB
    // - Tail buffer starts at offset 92 MiB (last 8 MiB)
    // - First complete CDFH is at offset 1024 within tail buffer
    //
    // The first_cdfh_offset (1024) is relative to tail start, NOT CD start.
    // Old (buggy) code would have used offset = 22 MiB (tail_start - cd_start)
    // which wouldn't fit in 24 bits for large CDs.

    uint64_t archive_size = 100ULL * 1024 * 1024;
    uint64_t cd_offset = 70ULL * 1024 * 1024;
    uint64_t tail_start = 92ULL * 1024 * 1024;
    uint32_t first_cdfh_offset_in_tail = 1024;  // 1 KiB into tail buffer

    // Create a buffer with CDFH at offset 1024
    uint8_t buffer[2048];
    memset(buffer, 0, sizeof(buffer));

    // Central directory header for file "offset_test.txt" at offset 1024
    uint8_t *cdfh = buffer + 1024;
    cdfh[0] = 0x50; cdfh[1] = 0x4b; cdfh[2] = 0x01; cdfh[3] = 0x02;  // signature
    cdfh[4] = 0x14; cdfh[5] = 0x00;  // version made by
    cdfh[6] = 0x0a; cdfh[7] = 0x00;  // version needed
    cdfh[8] = 0x00; cdfh[9] = 0x00;  // flags
    cdfh[10] = 0x00; cdfh[11] = 0x00;  // compression
    cdfh[12] = 0x00; cdfh[13] = 0x00;  // mod time
    cdfh[14] = 0x00; cdfh[15] = 0x00;  // mod date
    // crc32 = 0xDEADBEEF
    cdfh[16] = 0xEF; cdfh[17] = 0xBE; cdfh[18] = 0xAD; cdfh[19] = 0xDE;
    // compressed size = 500
    uint32_t comp_size = 500;
    memcpy(cdfh + 20, &comp_size, 4);
    memcpy(cdfh + 24, &comp_size, 4);
    // filename length = 15 ("offset_test.txt")
    cdfh[28] = 15; cdfh[29] = 0;
    cdfh[30] = 0; cdfh[31] = 0;  // extra field length
    cdfh[32] = 0; cdfh[33] = 0;  // comment length
    cdfh[34] = 0; cdfh[35] = 0;  // disk number
    cdfh[36] = 0; cdfh[37] = 0;  // internal attrs
    cdfh[38] = 0; cdfh[39] = 0; cdfh[40] = 0; cdfh[41] = 0;  // external attrs
    // local header offset = 85 MiB
    uint32_t local_offset = 85UL * 1024 * 1024;
    memcpy(cdfh + 42, &local_offset, 4);
    memcpy(cdfh + 46, "offset_test.txt", 15);

    struct central_dir_parse_result result;
    int rc = central_dir_parse_partial(
        buffer, sizeof(buffer),
        tail_start,                 // buffer starts at this archive offset
        cd_offset,                  // CD starts at this archive offset
        first_cdfh_offset_in_tail,  // 1024 bytes into tail (NOT 22 MiB from CD start!)
        archive_size,
        BURST_BASE_PART_SIZE,
        false,
        &result
    );

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_SUCCESS, rc);
    TEST_ASSERT_EQUAL_size_t(1, result.num_files);
    TEST_ASSERT_EQUAL_STRING("offset_test.txt", result.files[0].filename);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, result.files[0].crc32);
    TEST_ASSERT_EQUAL_UINT64(local_offset, result.files[0].local_header_offset);
    // Part index: 85 MiB / 8 MiB = 10 (integer division)
    TEST_ASSERT_EQUAL_UINT32(10, result.files[0].part_index);

    central_dir_parse_result_free(&result);
}

/**
 * Test: Partial parse with empty buffer returns error.
 */
void test_partial_cd_parse_empty_buffer(void) {
    struct central_dir_parse_result result;
    int rc = central_dir_parse_partial(
        NULL, 0,
        0, 0, 0,
        100, BURST_BASE_PART_SIZE,
        false, &result
    );

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER, rc);
}

/**
 * Test: Partial parse with first_cdfh_offset beyond buffer returns error.
 */
void test_partial_cd_parse_offset_beyond_buffer(void) {
    uint8_t buffer[64];
    memset(buffer, 0, sizeof(buffer));

    struct central_dir_parse_result result;
    int rc = central_dir_parse_partial(
        buffer, sizeof(buffer),
        100,              // buffer_start_offset
        0,                // central_dir_offset
        200,              // first_cdfh_offset (beyond buffer)
        200,              // archive_size
        BURST_BASE_PART_SIZE,
        false, &result
    );

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_ERR_TRUNCATED, rc);
}

/**
 * Test: Partial parse with invalid part_size returns error.
 */
void test_partial_cd_parse_invalid_part_size(void) {
    uint8_t buffer[64];
    memset(buffer, 0, sizeof(buffer));

    struct central_dir_parse_result result;
    int rc = central_dir_parse_partial(
        buffer, sizeof(buffer),
        0, 0, 0,
        100,
        1024 * 1024,  // 1 MiB - not a valid part size (must be multiple of 8 MiB)
        false, &result
    );

    TEST_ASSERT_EQUAL_INT(CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER, rc);
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

    // BURST EOCD comment tests
    RUN_TEST(test_eocd_no_burst_comment);
    RUN_TEST(test_eocd_with_burst_comment);
    RUN_TEST(test_eocd_with_non_burst_comment);
    RUN_TEST(test_eocd_with_wrong_burst_version);
    RUN_TEST(test_eocd_with_burst_comment_no_cdfh);

    // Partial CD parsing tests
    RUN_TEST(test_partial_cd_parse_basic);
    RUN_TEST(test_partial_cd_parse_nonzero_offset);
    RUN_TEST(test_partial_cd_parse_empty_buffer);
    RUN_TEST(test_partial_cd_parse_offset_beyond_buffer);
    RUN_TEST(test_partial_cd_parse_invalid_part_size);

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
