/*
 * Unit tests for static helper functions in burst_writer.c
 *
 * Since the helper functions are static, we include the source file directly
 * to gain access to them for testing.
 */

#include "unity.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Include the source file to access static functions
#include "../../src/writer/burst_writer.c"

// Test fixtures
static FILE *test_output = NULL;
static struct burst_writer *writer = NULL;

void setUp(void) {
    test_output = tmpfile();
    TEST_ASSERT_NOT_NULL(test_output);
    writer = burst_writer_create(test_output, 3);
    TEST_ASSERT_NOT_NULL(writer);
}

void tearDown(void) {
    if (writer) {
        burst_writer_destroy(writer);
        writer = NULL;
    }
    if (test_output) {
        fclose(test_output);
        test_output = NULL;
    }
}

// Helper to create a minimal LFH with a given filename
static void create_test_lfh(uint8_t *buffer, const char *filename, struct zip_local_header **lfh_out, int *lfh_len_out) {
    struct zip_local_header *lfh = (struct zip_local_header *)buffer;
    memset(lfh, 0, sizeof(struct zip_local_header));

    lfh->signature = ZIP_LOCAL_FILE_HEADER_SIG;
    lfh->version_needed = 63;
    lfh->flags = 0x0008;
    lfh->compression_method = ZIP_METHOD_ZSTD;
    lfh->last_mod_time = 0x1234;
    lfh->last_mod_date = 0x5678;
    lfh->filename_length = strlen(filename);
    lfh->extra_field_length = 0;

    // Copy filename after header
    memcpy(buffer + sizeof(struct zip_local_header), filename, strlen(filename));

    *lfh_out = lfh;
    *lfh_len_out = sizeof(struct zip_local_header) + strlen(filename);
}

// =============================================================================
// ensure_file_capacity() Tests
// =============================================================================

void test_ensure_capacity_no_expansion_needed(void) {
    // Writer starts with capacity 16, num_files 0
    TEST_ASSERT_EQUAL(16, writer->files_capacity);
    writer->num_files = 5;

    int result = ensure_file_capacity(writer);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(16, writer->files_capacity);  // No change
}

void test_ensure_capacity_expansion_triggered(void) {
    // Fill to capacity
    writer->num_files = 16;
    TEST_ASSERT_EQUAL(16, writer->files_capacity);

    int result = ensure_file_capacity(writer);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(32, writer->files_capacity);  // Doubled
}

void test_ensure_capacity_multiple_expansions(void) {
    // First expansion: 16 -> 32
    writer->num_files = 16;
    TEST_ASSERT_EQUAL(0, ensure_file_capacity(writer));
    TEST_ASSERT_EQUAL(32, writer->files_capacity);

    // Second expansion: 32 -> 64
    writer->num_files = 32;
    TEST_ASSERT_EQUAL(0, ensure_file_capacity(writer));
    TEST_ASSERT_EQUAL(64, writer->files_capacity);
}

void test_ensure_capacity_at_boundary(void) {
    // At exactly capacity - 1, should not expand
    writer->num_files = 15;
    TEST_ASSERT_EQUAL(0, ensure_file_capacity(writer));
    TEST_ASSERT_EQUAL(16, writer->files_capacity);

    // At exactly capacity, should expand
    writer->num_files = 16;
    TEST_ASSERT_EQUAL(0, ensure_file_capacity(writer));
    TEST_ASSERT_EQUAL(32, writer->files_capacity);
}

// =============================================================================
// allocate_file_entry() Tests
// =============================================================================

void test_allocate_entry_success(void) {
    uint8_t buffer[512];
    struct zip_local_header *lfh;
    int lfh_len;
    create_test_lfh(buffer, "test.txt", &lfh, &lfh_len);

    struct file_entry *entry = allocate_file_entry(writer, lfh);

    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_NOT_NULL(entry->filename);
    TEST_ASSERT_EQUAL_STRING("test.txt", entry->filename);

    // Cleanup
    free(entry->filename);
}

void test_allocate_entry_long_filename(void) {
    uint8_t buffer[512];
    char long_name[256];
    memset(long_name, 'a', 255);
    long_name[255] = '\0';

    struct zip_local_header *lfh;
    int lfh_len;
    create_test_lfh(buffer, long_name, &lfh, &lfh_len);

    struct file_entry *entry = allocate_file_entry(writer, lfh);

    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_NOT_NULL(entry->filename);
    TEST_ASSERT_EQUAL(255, strlen(entry->filename));
    TEST_ASSERT_EQUAL_STRING(long_name, entry->filename);

    free(entry->filename);
}

void test_allocate_entry_empty_filename(void) {
    uint8_t buffer[512];
    struct zip_local_header *lfh = (struct zip_local_header *)buffer;
    memset(lfh, 0, sizeof(struct zip_local_header));
    lfh->signature = ZIP_LOCAL_FILE_HEADER_SIG;
    lfh->filename_length = 0;

    struct file_entry *entry = allocate_file_entry(writer, lfh);

    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_NOT_NULL(entry->filename);
    TEST_ASSERT_EQUAL_STRING("", entry->filename);

    free(entry->filename);
}

void test_allocate_entry_zeroes_struct(void) {
    uint8_t buffer[512];
    struct zip_local_header *lfh;
    int lfh_len;
    create_test_lfh(buffer, "test.txt", &lfh, &lfh_len);

    // Pre-fill the entry slot with garbage
    struct file_entry *entry_slot = &writer->files[writer->num_files];
    memset(entry_slot, 0xFF, sizeof(struct file_entry));

    struct file_entry *entry = allocate_file_entry(writer, lfh);

    TEST_ASSERT_NOT_NULL(entry);
    // Check that fields are zeroed (except filename which is allocated)
    TEST_ASSERT_EQUAL(0, entry->local_header_offset);
    TEST_ASSERT_EQUAL(0, entry->compressed_size);
    TEST_ASSERT_EQUAL(0, entry->uncompressed_size);
    TEST_ASSERT_EQUAL(0, entry->crc32);

    free(entry->filename);
}

void test_allocate_entry_path_with_directories(void) {
    uint8_t buffer[512];
    struct zip_local_header *lfh;
    int lfh_len;
    create_test_lfh(buffer, "path/to/file.txt", &lfh, &lfh_len);

    struct file_entry *entry = allocate_file_entry(writer, lfh);

    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_STRING("path/to/file.txt", entry->filename);

    free(entry->filename);
}

// =============================================================================
// populate_entry_metadata() Tests
// =============================================================================

void test_populate_metadata_all_fields(void) {
    uint8_t buffer[512];
    struct zip_local_header *lfh;
    int lfh_len;
    create_test_lfh(buffer, "test.txt", &lfh, &lfh_len);

    // Set specific values in LFH
    lfh->compression_method = ZIP_METHOD_ZSTD;
    lfh->version_needed = 63;
    lfh->flags = 0x0008;
    lfh->last_mod_time = 0xABCD;
    lfh->last_mod_date = 0x1234;

    struct file_entry entry;
    memset(&entry, 0, sizeof(entry));

    populate_entry_metadata(&entry, lfh, 12345, 0755, 1000, 1001);

    TEST_ASSERT_EQUAL(12345, entry.local_header_offset);
    TEST_ASSERT_EQUAL(ZIP_METHOD_ZSTD, entry.compression_method);
    TEST_ASSERT_EQUAL(63, entry.version_needed);
    TEST_ASSERT_EQUAL(0x0008, entry.general_purpose_flags);
    TEST_ASSERT_EQUAL(0xABCD, entry.last_mod_time);
    TEST_ASSERT_EQUAL(0x1234, entry.last_mod_date);
    TEST_ASSERT_EQUAL(0755, entry.unix_mode);
    TEST_ASSERT_EQUAL(1000, entry.uid);
    TEST_ASSERT_EQUAL(1001, entry.gid);
}

void test_populate_metadata_unix_mode(void) {
    uint8_t buffer[512];
    struct zip_local_header *lfh;
    int lfh_len;
    create_test_lfh(buffer, "test.txt", &lfh, &lfh_len);

    struct file_entry entry;
    memset(&entry, 0, sizeof(entry));

    // Test various Unix modes
    populate_entry_metadata(&entry, lfh, 0, 0755, 0, 0);
    TEST_ASSERT_EQUAL(0755, entry.unix_mode);

    populate_entry_metadata(&entry, lfh, 0, 0644, 0, 0);
    TEST_ASSERT_EQUAL(0644, entry.unix_mode);

    populate_entry_metadata(&entry, lfh, 0, 0777, 0, 0);
    TEST_ASSERT_EQUAL(0777, entry.unix_mode);

    // Directory mode
    populate_entry_metadata(&entry, lfh, 0, 040755, 0, 0);
    TEST_ASSERT_EQUAL(040755, entry.unix_mode);
}

void test_populate_metadata_uid_gid(void) {
    uint8_t buffer[512];
    struct zip_local_header *lfh;
    int lfh_len;
    create_test_lfh(buffer, "test.txt", &lfh, &lfh_len);

    struct file_entry entry;
    memset(&entry, 0, sizeof(entry));

    populate_entry_metadata(&entry, lfh, 0, 0, 1000, 1000);
    TEST_ASSERT_EQUAL(1000, entry.uid);
    TEST_ASSERT_EQUAL(1000, entry.gid);

    populate_entry_metadata(&entry, lfh, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL(0, entry.uid);
    TEST_ASSERT_EQUAL(0, entry.gid);

    populate_entry_metadata(&entry, lfh, 0, 0, 65534, 65534);
    TEST_ASSERT_EQUAL(65534, entry.uid);
    TEST_ASSERT_EQUAL(65534, entry.gid);
}

void test_populate_metadata_offset(void) {
    uint8_t buffer[512];
    struct zip_local_header *lfh;
    int lfh_len;
    create_test_lfh(buffer, "test.txt", &lfh, &lfh_len);

    struct file_entry entry;
    memset(&entry, 0, sizeof(entry));

    // Test various offsets
    populate_entry_metadata(&entry, lfh, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL(0, entry.local_header_offset);

    populate_entry_metadata(&entry, lfh, 8388608, 0, 0, 0);  // 8 MiB
    TEST_ASSERT_EQUAL(8388608, entry.local_header_offset);

    populate_entry_metadata(&entry, lfh, 0xFFFFFFFFFFFFFFFFULL, 0, 0, 0);  // Max uint64
    TEST_ASSERT_EQUAL(0xFFFFFFFFFFFFFFFFULL, entry.local_header_offset);
}

// =============================================================================
// check_alignment_and_pad() Tests - Basic Functionality
// =============================================================================

void test_alignment_no_pad_plenty_of_space(void) {
    // Position at start of archive, lots of space to boundary
    writer->current_offset = 0;
    writer->buffer_used = 100;

    // Directory: lfh=50, content=0, no descriptor
    // Space needed: 50 + 0 + 44 = 94 bytes
    // Space available: 8388608 - 100 = 8388508 bytes
    int result = check_alignment_and_pad(writer, 50, 0, false);

    TEST_ASSERT_EQUAL(0, result);
    // Position should be unchanged (no padding written)
    TEST_ASSERT_EQUAL(100, alignment_get_write_position(writer));
}

void test_alignment_no_pad_exactly_fits(void) {
    // Position so that entry exactly fits
    // Space needed for directory: lfh(50) + content(0) + min_padding(44) = 94
    size_t space_needed = 50 + 0 + PADDING_LFH_MIN_SIZE;
    writer->current_offset = BURST_PART_SIZE - space_needed;
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, 50, 0, false);

    TEST_ASSERT_EQUAL(0, result);
    // No padding should have been written
    TEST_ASSERT_EQUAL(BURST_PART_SIZE - space_needed, alignment_get_write_position(writer));
}

void test_alignment_pad_needed_no_room(void) {
    // Position so close to boundary that entry won't fit
    // Space needed for directory: 50 + 0 + 44 = 94 bytes
    // Give only 50 bytes
    writer->current_offset = BURST_PART_SIZE - 50;
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, 50, 0, false);

    TEST_ASSERT_EQUAL(0, result);
    // Padding should have been written, advancing to boundary
    uint64_t pos_after = alignment_get_write_position(writer);
    TEST_ASSERT_EQUAL(BURST_PART_SIZE, pos_after);
}

// =============================================================================
// check_alignment_and_pad() Tests - Entry Type Variations
// =============================================================================

void test_alignment_file_with_descriptor(void) {
    // File entry: adds ZIP64 descriptor (24 bytes) to space calculation
    // Space needed: lfh(100) + content(0) + descriptor(24) + min_padding(44) = 168
    size_t lfh_len = 100;
    size_t space_needed = lfh_len + sizeof(struct zip_data_descriptor_zip64) + PADDING_LFH_MIN_SIZE;

    // Position where exactly space_needed is available - should NOT pad
    writer->current_offset = BURST_PART_SIZE - space_needed;
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, lfh_len, 0, true);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(BURST_PART_SIZE - space_needed, alignment_get_write_position(writer));

    // Now position where one less byte is available - should pad
    writer->current_offset = BURST_PART_SIZE - space_needed + 1;
    writer->buffer_used = 0;

    result = check_alignment_and_pad(writer, lfh_len, 0, true);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(BURST_PART_SIZE, alignment_get_write_position(writer));
}

void test_alignment_symlink_with_content(void) {
    // Symlink: has content (target path), no descriptor
    // Space needed: lfh(80) + content(30) + min_padding(44) = 154
    size_t lfh_len = 80;
    size_t content_size = 30;
    size_t space_needed = lfh_len + content_size + PADDING_LFH_MIN_SIZE;

    writer->current_offset = BURST_PART_SIZE - space_needed;
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, lfh_len, content_size, false);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(BURST_PART_SIZE - space_needed, alignment_get_write_position(writer));
}

void test_alignment_directory_minimal(void) {
    // Directory: no content, no descriptor
    // Space needed: lfh(60) + content(0) + min_padding(44) = 104
    size_t lfh_len = 60;
    size_t space_needed = lfh_len + PADDING_LFH_MIN_SIZE;

    writer->current_offset = BURST_PART_SIZE - space_needed;
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, lfh_len, 0, false);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(BURST_PART_SIZE - space_needed, alignment_get_write_position(writer));
}

// =============================================================================
// check_alignment_and_pad() Tests - Boundary Edge Cases
// =============================================================================

void test_alignment_at_boundary_start(void) {
    // Exactly at first boundary (8 MiB)
    writer->current_offset = BURST_PART_SIZE;
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, 50, 0, false);

    TEST_ASSERT_EQUAL(0, result);
    // Should use next boundary (16 MiB), plenty of space
    TEST_ASSERT_EQUAL(BURST_PART_SIZE, alignment_get_write_position(writer));
}

void test_alignment_one_byte_before_boundary(void) {
    // 1 byte before boundary - can't write padding (min 44 bytes)
    // This is an edge case: write_padding_lfh requires at least PADDING_LFH_MIN_SIZE (44) bytes
    // In practice, this shouldn't happen because we always check alignment before writing
    // Test that the function correctly returns an error in this case
    writer->current_offset = BURST_PART_SIZE - 1;
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, 50, 0, false);

    // This returns -1 because write_padding_lfh fails with only 1 byte
    TEST_ASSERT_EQUAL(-1, result);
}

void test_alignment_min_padding_size_exact(void) {
    // Position where exactly PADDING_LFH_MIN_SIZE bytes remain
    writer->current_offset = BURST_PART_SIZE - PADDING_LFH_MIN_SIZE;
    writer->buffer_used = 0;

    // Request smallest possible entry (lfh=44, no content, no descriptor)
    // Space needed: 44 + 0 + 44 = 88 bytes, but only 44 available
    int result = check_alignment_and_pad(writer, PADDING_LFH_MIN_SIZE, 0, false);

    TEST_ASSERT_EQUAL(0, result);
    // Should have padded to boundary
    TEST_ASSERT_EQUAL(BURST_PART_SIZE, alignment_get_write_position(writer));
}

void test_alignment_near_second_boundary(void) {
    // Near the 16 MiB boundary
    writer->current_offset = 2 * BURST_PART_SIZE - 50;
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, 100, 0, false);

    TEST_ASSERT_EQUAL(0, result);
    // Should have padded to 16 MiB boundary
    TEST_ASSERT_EQUAL(2 * BURST_PART_SIZE, alignment_get_write_position(writer));
}

void test_alignment_near_third_boundary(void) {
    // Near the 24 MiB boundary
    writer->current_offset = 3 * BURST_PART_SIZE - 50;
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, 100, 0, false);

    TEST_ASSERT_EQUAL(0, result);
    // Should have padded to 24 MiB boundary
    TEST_ASSERT_EQUAL(3 * BURST_PART_SIZE, alignment_get_write_position(writer));
}

// =============================================================================
// check_alignment_and_pad() Tests - Space Calculation Verification
// =============================================================================

void test_alignment_space_calc_file(void) {
    // File: lfh(100) + content(0) + descriptor(24) + min_padding(44) = 168
    size_t lfh_len = 100;
    size_t expected_space = 100 + 24 + 44;

    // Position where exactly expected_space is available
    writer->current_offset = BURST_PART_SIZE - expected_space;
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, lfh_len, 0, true);
    TEST_ASSERT_EQUAL(0, result);
    // Should NOT have padded
    TEST_ASSERT_EQUAL(BURST_PART_SIZE - expected_space, alignment_get_write_position(writer));

    // Position where expected_space - 1 is available
    writer->current_offset = BURST_PART_SIZE - expected_space + 1;
    writer->buffer_used = 0;

    result = check_alignment_and_pad(writer, lfh_len, 0, true);
    TEST_ASSERT_EQUAL(0, result);
    // Should have padded
    TEST_ASSERT_EQUAL(BURST_PART_SIZE, alignment_get_write_position(writer));
}

void test_alignment_space_calc_symlink(void) {
    // Symlink: lfh(80) + target(30) + min_padding(44) = 154
    size_t lfh_len = 80;
    size_t content_size = 30;
    size_t expected_space = 80 + 30 + 44;

    writer->current_offset = BURST_PART_SIZE - expected_space;
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, lfh_len, content_size, false);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(BURST_PART_SIZE - expected_space, alignment_get_write_position(writer));
}

void test_alignment_space_calc_dir(void) {
    // Directory: lfh(60) + content(0) + min_padding(44) = 104
    size_t lfh_len = 60;
    size_t expected_space = 60 + 44;

    writer->current_offset = BURST_PART_SIZE - expected_space;
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, lfh_len, 0, false);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(BURST_PART_SIZE - expected_space, alignment_get_write_position(writer));
}

// =============================================================================
// check_alignment_and_pad() Tests - Alignment Rule Compliance
// =============================================================================

void test_alignment_rule_padding_fills_to_boundary(void) {
    // Test that padding always advances to exact boundary
    // Try various positions before boundary
    // Note: Minimum space needed for padding is PADDING_LFH_MIN_SIZE (44 bytes)
    uint64_t test_positions[] = {
        BURST_PART_SIZE - 50,      // Just above minimum
        BURST_PART_SIZE - 100,
        BURST_PART_SIZE - 200,
        BURST_PART_SIZE - PADDING_LFH_MIN_SIZE,  // Exact minimum
        BURST_PART_SIZE - PADDING_LFH_MIN_SIZE - 10,
    };

    for (size_t i = 0; i < sizeof(test_positions) / sizeof(test_positions[0]); i++) {
        // Reset writer for each test
        tearDown();
        setUp();

        writer->current_offset = test_positions[i];
        writer->buffer_used = 0;

        // Force padding by requesting more space than available
        // Large LFH ensures we need padding
        int result = check_alignment_and_pad(writer, 500, 0, true);
        TEST_ASSERT_EQUAL(0, result);

        uint64_t pos_after = alignment_get_write_position(writer);
        TEST_ASSERT_EQUAL_MESSAGE(BURST_PART_SIZE, pos_after,
            "After padding, position should be at boundary");
    }
}

void test_alignment_rule_position_after_pad_is_boundary_aligned(void) {
    // After padding, the write position must be at an 8 MiB boundary
    // Need at least PADDING_LFH_MIN_SIZE (44) bytes for padding to work
    writer->current_offset = BURST_PART_SIZE - 60;  // 60 bytes before boundary
    writer->buffer_used = 0;

    // Request entry that won't fit (needs 200 bytes total)
    int result = check_alignment_and_pad(writer, 100, 50, true);
    TEST_ASSERT_EQUAL(0, result);

    uint64_t pos = alignment_get_write_position(writer);
    TEST_ASSERT_EQUAL(0, pos % BURST_PART_SIZE);  // Must be boundary-aligned
}

void test_alignment_large_lfh_near_boundary(void) {
    // Very large LFH (long filename) near boundary
    size_t large_lfh = 300;  // Large filename

    writer->current_offset = BURST_PART_SIZE - 100;  // Only 100 bytes available
    writer->buffer_used = 0;

    int result = check_alignment_and_pad(writer, large_lfh, 0, false);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(BURST_PART_SIZE, alignment_get_write_position(writer));
}

void test_alignment_buffer_used_affects_position(void) {
    // Test that buffer_used is correctly included in position calculation
    writer->current_offset = BURST_PART_SIZE - 1000;
    writer->buffer_used = 900;  // 100 bytes to boundary

    // Space needed: 50 + 0 + 44 = 94, but only 100 available
    int result = check_alignment_and_pad(writer, 50, 0, false);

    TEST_ASSERT_EQUAL(0, result);
    // Should NOT have padded (100 > 94)
    TEST_ASSERT_EQUAL(BURST_PART_SIZE - 100, alignment_get_write_position(writer));
}

void test_alignment_buffer_used_triggers_padding(void) {
    // Test that buffer_used can push us over the threshold
    writer->current_offset = BURST_PART_SIZE - 1000;
    writer->buffer_used = 950;  // Only 50 bytes to boundary

    // Space needed: 100 + 0 + 44 = 144, but only 50 available
    int result = check_alignment_and_pad(writer, 100, 0, false);

    TEST_ASSERT_EQUAL(0, result);
    // Should have padded to boundary
    TEST_ASSERT_EQUAL(BURST_PART_SIZE, alignment_get_write_position(writer));
}

// =============================================================================
// Test Runner
// =============================================================================

int main(void) {
    UNITY_BEGIN();

    // ensure_file_capacity() tests
    RUN_TEST(test_ensure_capacity_no_expansion_needed);
    RUN_TEST(test_ensure_capacity_expansion_triggered);
    RUN_TEST(test_ensure_capacity_multiple_expansions);
    RUN_TEST(test_ensure_capacity_at_boundary);

    // allocate_file_entry() tests
    RUN_TEST(test_allocate_entry_success);
    RUN_TEST(test_allocate_entry_long_filename);
    RUN_TEST(test_allocate_entry_empty_filename);
    RUN_TEST(test_allocate_entry_zeroes_struct);
    RUN_TEST(test_allocate_entry_path_with_directories);

    // populate_entry_metadata() tests
    RUN_TEST(test_populate_metadata_all_fields);
    RUN_TEST(test_populate_metadata_unix_mode);
    RUN_TEST(test_populate_metadata_uid_gid);
    RUN_TEST(test_populate_metadata_offset);

    // check_alignment_and_pad() - Basic functionality
    RUN_TEST(test_alignment_no_pad_plenty_of_space);
    RUN_TEST(test_alignment_no_pad_exactly_fits);
    RUN_TEST(test_alignment_pad_needed_no_room);

    // check_alignment_and_pad() - Entry type variations
    RUN_TEST(test_alignment_file_with_descriptor);
    RUN_TEST(test_alignment_symlink_with_content);
    RUN_TEST(test_alignment_directory_minimal);

    // check_alignment_and_pad() - Boundary edge cases
    RUN_TEST(test_alignment_at_boundary_start);
    RUN_TEST(test_alignment_one_byte_before_boundary);
    RUN_TEST(test_alignment_min_padding_size_exact);
    RUN_TEST(test_alignment_near_second_boundary);
    RUN_TEST(test_alignment_near_third_boundary);

    // check_alignment_and_pad() - Space calculation verification
    RUN_TEST(test_alignment_space_calc_file);
    RUN_TEST(test_alignment_space_calc_symlink);
    RUN_TEST(test_alignment_space_calc_dir);

    // check_alignment_and_pad() - Alignment Rule compliance
    RUN_TEST(test_alignment_rule_padding_fills_to_boundary);
    RUN_TEST(test_alignment_rule_position_after_pad_is_boundary_aligned);
    RUN_TEST(test_alignment_large_lfh_near_boundary);
    RUN_TEST(test_alignment_buffer_used_affects_position);
    RUN_TEST(test_alignment_buffer_used_triggers_padding);

    return UNITY_END();
}
