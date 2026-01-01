/**
 * Unit tests for parts_to_download calculation.
 *
 * Tests the logic that determines whether the final part should be
 * downloaded from S3 or processed from the CD buffer.
 */

#include "unity.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MiB (1024 * 1024)

// Import the function under test - this is defined in s3_operations.c
// but we declare it here to avoid linking against AWS SDK
extern void calculate_parts_to_download(
    size_t num_parts,
    uint64_t part_size,
    uint64_t cd_start,
    size_t *parts_to_download,
    bool *process_final_from_buffer
);

void setUp(void) {
    // Nothing to set up
}

void tearDown(void) {
    // Nothing to tear down
}

// =============================================================================
// Test Cases: 8 MiB part size (default)
// =============================================================================

// 10 MiB archive, 8 MiB parts -> 2 parts
// CD buffer starts at 2 MiB (10 - 8 = 2)
// Final part starts at 8 MiB >= 2 MiB -> process from buffer
void test_8mib_parts_10mib_archive(void) {
    size_t parts_to_download;
    bool process_final_from_buffer;

    size_t num_parts = 2;                    // ceil(10 MiB / 8 MiB)
    uint64_t part_size = 8 * MiB;
    uint64_t cd_start = 2 * MiB;             // 10 MiB - 8 MiB

    calculate_parts_to_download(num_parts, part_size, cd_start,
                                &parts_to_download, &process_final_from_buffer);

    TEST_ASSERT_EQUAL_size_t(1, parts_to_download);  // Download part 0 only
    TEST_ASSERT_TRUE(process_final_from_buffer);
}

// 20 MiB archive, 8 MiB parts -> 3 parts
// CD buffer starts at 12 MiB (20 - 8 = 12)
// Final part starts at 16 MiB >= 12 MiB -> process from buffer
void test_8mib_parts_20mib_archive(void) {
    size_t parts_to_download;
    bool process_final_from_buffer;

    size_t num_parts = 3;                    // ceil(20 MiB / 8 MiB)
    uint64_t part_size = 8 * MiB;
    uint64_t cd_start = 12 * MiB;            // 20 MiB - 8 MiB

    calculate_parts_to_download(num_parts, part_size, cd_start,
                                &parts_to_download, &process_final_from_buffer);

    TEST_ASSERT_EQUAL_size_t(2, parts_to_download);  // Download parts 0, 1
    TEST_ASSERT_TRUE(process_final_from_buffer);
}

// =============================================================================
// Test Cases: 16 MiB part size
// =============================================================================

// 10 MiB archive, 16 MiB parts -> 1 part
// CD buffer starts at 2 MiB (10 - 8 = 2)
// Final part starts at 0 MiB < 2 MiB -> must download from S3
void test_16mib_parts_10mib_archive(void) {
    size_t parts_to_download;
    bool process_final_from_buffer;

    size_t num_parts = 1;                    // ceil(10 MiB / 16 MiB)
    uint64_t part_size = 16 * MiB;
    uint64_t cd_start = 2 * MiB;             // 10 MiB - 8 MiB

    calculate_parts_to_download(num_parts, part_size, cd_start,
                                &parts_to_download, &process_final_from_buffer);

    TEST_ASSERT_EQUAL_size_t(1, parts_to_download);  // Must download the only part
    TEST_ASSERT_FALSE(process_final_from_buffer);
}

// 25 MiB archive, 16 MiB parts -> 2 parts
// CD buffer starts at 17 MiB (25 - 8 = 17)
// Final part starts at 16 MiB < 17 MiB -> must download from S3
void test_16mib_parts_25mib_archive(void) {
    size_t parts_to_download;
    bool process_final_from_buffer;

    size_t num_parts = 2;                    // ceil(25 MiB / 16 MiB)
    uint64_t part_size = 16 * MiB;
    uint64_t cd_start = 17 * MiB;            // 25 MiB - 8 MiB

    calculate_parts_to_download(num_parts, part_size, cd_start,
                                &parts_to_download, &process_final_from_buffer);

    TEST_ASSERT_EQUAL_size_t(2, parts_to_download);  // Must download both parts
    TEST_ASSERT_FALSE(process_final_from_buffer);
}

// 61 MiB archive, 16 MiB parts -> 4 parts (the original bug case)
// CD buffer starts at 53 MiB (61 - 8 = 53)
// Final part starts at 48 MiB < 53 MiB -> must download from S3
// This is the exact scenario that caused the segfault before the fix
void test_16mib_parts_61mib_archive(void) {
    size_t parts_to_download;
    bool process_final_from_buffer;

    size_t num_parts = 4;                    // ceil(61 MiB / 16 MiB)
    uint64_t part_size = 16 * MiB;
    uint64_t cd_start = 53 * MiB;            // 61 MiB - 8 MiB

    calculate_parts_to_download(num_parts, part_size, cd_start,
                                &parts_to_download, &process_final_from_buffer);

    TEST_ASSERT_EQUAL_size_t(4, parts_to_download);  // Must download all 4 parts
    TEST_ASSERT_FALSE(process_final_from_buffer);
}

// 35 MiB archive, 16 MiB parts -> 3 parts
// CD buffer starts at 27 MiB (35 - 8 = 27)
// Final part starts at 32 MiB >= 27 MiB -> process from buffer
void test_16mib_parts_35mib_archive(void) {
    size_t parts_to_download;
    bool process_final_from_buffer;

    size_t num_parts = 3;                    // ceil(35 MiB / 16 MiB)
    uint64_t part_size = 16 * MiB;
    uint64_t cd_start = 27 * MiB;            // 35 MiB - 8 MiB

    calculate_parts_to_download(num_parts, part_size, cd_start,
                                &parts_to_download, &process_final_from_buffer);

    TEST_ASSERT_EQUAL_size_t(2, parts_to_download);  // Download parts 0, 1
    TEST_ASSERT_TRUE(process_final_from_buffer);
}

// =============================================================================
// Test Cases: 32 MiB part size
// =============================================================================

// 40 MiB archive, 32 MiB parts -> 2 parts
// CD buffer starts at 32 MiB (40 - 8 = 32)
// Final part starts at 32 MiB >= 32 MiB -> process from buffer (exactly on boundary)
void test_32mib_parts_40mib_archive(void) {
    size_t parts_to_download;
    bool process_final_from_buffer;

    size_t num_parts = 2;                    // ceil(40 MiB / 32 MiB)
    uint64_t part_size = 32 * MiB;
    uint64_t cd_start = 32 * MiB;            // 40 MiB - 8 MiB

    calculate_parts_to_download(num_parts, part_size, cd_start,
                                &parts_to_download, &process_final_from_buffer);

    TEST_ASSERT_EQUAL_size_t(1, parts_to_download);  // Download part 0 only
    TEST_ASSERT_TRUE(process_final_from_buffer);
}

// =============================================================================
// Test Cases: Edge cases
// =============================================================================

// Single-part archive smaller than 8 MiB
// 5 MiB archive, 8 MiB parts -> 1 part
// CD buffer starts at 0 (entire file is in buffer: max(0, 5-8) = 0)
// Final part starts at 0 MiB >= 0 MiB -> process from buffer
void test_single_part_small_archive(void) {
    size_t parts_to_download;
    bool process_final_from_buffer;

    size_t num_parts = 1;                    // ceil(5 MiB / 8 MiB)
    uint64_t part_size = 8 * MiB;
    uint64_t cd_start = 0;                   // Entire file in buffer

    calculate_parts_to_download(num_parts, part_size, cd_start,
                                &parts_to_download, &process_final_from_buffer);

    TEST_ASSERT_EQUAL_size_t(0, parts_to_download);  // Don't download anything
    TEST_ASSERT_TRUE(process_final_from_buffer);
}

// Zero parts (empty/edge case - should handle gracefully)
void test_zero_parts(void) {
    size_t parts_to_download;
    bool process_final_from_buffer;

    size_t num_parts = 0;
    uint64_t part_size = 8 * MiB;
    uint64_t cd_start = 0;

    calculate_parts_to_download(num_parts, part_size, cd_start,
                                &parts_to_download, &process_final_from_buffer);

    TEST_ASSERT_EQUAL_size_t(0, parts_to_download);
    TEST_ASSERT_FALSE(process_final_from_buffer);
}

// Final part exactly at boundary (final_part_start == cd_start)
// 24 MiB archive, 8 MiB parts -> 3 parts
// CD buffer starts at 16 MiB (24 - 8 = 16)
// Final part starts at 16 MiB == 16 MiB -> process from buffer
void test_final_part_exactly_at_boundary(void) {
    size_t parts_to_download;
    bool process_final_from_buffer;

    size_t num_parts = 3;                    // ceil(24 MiB / 8 MiB)
    uint64_t part_size = 8 * MiB;
    uint64_t cd_start = 16 * MiB;            // 24 MiB - 8 MiB

    calculate_parts_to_download(num_parts, part_size, cd_start,
                                &parts_to_download, &process_final_from_buffer);

    TEST_ASSERT_EQUAL_size_t(2, parts_to_download);  // Download parts 0, 1
    TEST_ASSERT_TRUE(process_final_from_buffer);
}

// =============================================================================
// Main
// =============================================================================

int main(void) {
    UNITY_BEGIN();

    // 8 MiB part size tests
    RUN_TEST(test_8mib_parts_10mib_archive);
    RUN_TEST(test_8mib_parts_20mib_archive);

    // 16 MiB part size tests
    RUN_TEST(test_16mib_parts_10mib_archive);
    RUN_TEST(test_16mib_parts_25mib_archive);
    RUN_TEST(test_16mib_parts_61mib_archive);
    RUN_TEST(test_16mib_parts_35mib_archive);

    // 32 MiB part size tests
    RUN_TEST(test_32mib_parts_40mib_archive);

    // Edge cases
    RUN_TEST(test_single_part_small_archive);
    RUN_TEST(test_zero_parts);
    RUN_TEST(test_final_part_exactly_at_boundary);

    return UNITY_END();
}
