/**
 * Unit tests for cd_fetch.c - Central directory fetch utilities.
 */

#include "unity.h"
#include "cd_fetch.h"
#include <stdlib.h>
#include <string.h>

#define MiB (1024 * 1024)

void setUp(void) {
}

void tearDown(void) {
}

/**
 * Test: Small CD that fits entirely in initial buffer.
 * No additional ranges should be needed.
 */
void test_calculate_ranges_small_cd(void) {
    struct cd_part_range *ranges = NULL;
    size_t num_ranges = 0;

    // Archive: 20 MiB total
    // Initial buffer starts at 12 MiB (last 8 MiB)
    // CD at offset 15 MiB (entirely within initial buffer)
    uint64_t central_dir_offset = 15 * MiB;
    uint64_t central_dir_size = 2 * MiB;
    uint64_t part_size = 8 * MiB;
    uint64_t initial_buffer_start = 12 * MiB;

    int rc = calculate_cd_fetch_ranges(central_dir_offset, central_dir_size,
                                        part_size, initial_buffer_start,
                                        &ranges, &num_ranges);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_size_t(0, num_ranges);
    TEST_ASSERT_NULL(ranges);
}

/**
 * Test: CD that needs one additional aligned part.
 */
void test_calculate_ranges_one_part(void) {
    struct cd_part_range *ranges = NULL;
    size_t num_ranges = 0;

    // Archive: 30 MiB total
    // Initial buffer starts at 22 MiB (last 8 MiB)
    // CD at offset 18 MiB, size 10 MiB
    // Need to fetch part starting at 16 MiB (aligned to 8 MiB)
    uint64_t central_dir_offset = 18 * MiB;
    uint64_t central_dir_size = 10 * MiB;
    uint64_t part_size = 8 * MiB;
    uint64_t initial_buffer_start = 22 * MiB;

    int rc = calculate_cd_fetch_ranges(central_dir_offset, central_dir_size,
                                        part_size, initial_buffer_start,
                                        &ranges, &num_ranges);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_size_t(1, num_ranges);
    TEST_ASSERT_NOT_NULL(ranges);

    // Range should be [16 MiB, 22 MiB - 1] (before initial buffer)
    TEST_ASSERT_EQUAL_UINT64(16 * MiB, ranges[0].start);
    TEST_ASSERT_EQUAL_UINT64(22 * MiB - 1, ranges[0].end);

    // Body data: 16 MiB to 18 MiB (2 MiB of body data)
    TEST_ASSERT_TRUE(ranges[0].has_body_data);
    TEST_ASSERT_EQUAL_UINT64(2 * MiB, ranges[0].body_data_size);

    free(ranges);
}

/**
 * Test: CD that needs multiple additional parts.
 */
void test_calculate_ranges_multiple_parts(void) {
    struct cd_part_range *ranges = NULL;
    size_t num_ranges = 0;

    // Archive: 100 MiB total
    // Initial buffer starts at 92 MiB (last 8 MiB)
    // CD at offset 60 MiB, size 38 MiB
    // Need parts starting at: 56, 64, 72, 80, 88
    uint64_t central_dir_offset = 60 * MiB;
    uint64_t central_dir_size = 38 * MiB;
    uint64_t part_size = 8 * MiB;
    uint64_t initial_buffer_start = 92 * MiB;

    int rc = calculate_cd_fetch_ranges(central_dir_offset, central_dir_size,
                                        part_size, initial_buffer_start,
                                        &ranges, &num_ranges);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_size_t(5, num_ranges);
    TEST_ASSERT_NOT_NULL(ranges);

    // First range: 56-63 MiB (contains 4 MiB body data: 56-60)
    TEST_ASSERT_EQUAL_UINT64(56 * MiB, ranges[0].start);
    TEST_ASSERT_EQUAL_UINT64(64 * MiB - 1, ranges[0].end);
    TEST_ASSERT_TRUE(ranges[0].has_body_data);
    TEST_ASSERT_EQUAL_UINT64(4 * MiB, ranges[0].body_data_size);

    // Second range: 64-71 MiB (no body data)
    TEST_ASSERT_EQUAL_UINT64(64 * MiB, ranges[1].start);
    TEST_ASSERT_EQUAL_UINT64(72 * MiB - 1, ranges[1].end);
    TEST_ASSERT_FALSE(ranges[1].has_body_data);
    TEST_ASSERT_EQUAL_UINT64(0, ranges[1].body_data_size);

    // Third range: 72-79 MiB (no body data)
    TEST_ASSERT_EQUAL_UINT64(72 * MiB, ranges[2].start);
    TEST_ASSERT_EQUAL_UINT64(80 * MiB - 1, ranges[2].end);
    TEST_ASSERT_FALSE(ranges[2].has_body_data);

    // Fourth range: 80-87 MiB (no body data)
    TEST_ASSERT_EQUAL_UINT64(80 * MiB, ranges[3].start);
    TEST_ASSERT_EQUAL_UINT64(88 * MiB - 1, ranges[3].end);
    TEST_ASSERT_FALSE(ranges[3].has_body_data);

    // Fifth range: 88-91 MiB (ends at initial_buffer_start - 1)
    TEST_ASSERT_EQUAL_UINT64(88 * MiB, ranges[4].start);
    TEST_ASSERT_EQUAL_UINT64(92 * MiB - 1, ranges[4].end);
    TEST_ASSERT_FALSE(ranges[4].has_body_data);

    free(ranges);
}

/**
 * Test: CD starts exactly on part boundary.
 */
void test_calculate_ranges_exact_alignment(void) {
    struct cd_part_range *ranges = NULL;
    size_t num_ranges = 0;

    // CD starts exactly at 16 MiB boundary
    // Initial buffer starts at 24 MiB
    uint64_t central_dir_offset = 16 * MiB;
    uint64_t central_dir_size = 12 * MiB;
    uint64_t part_size = 8 * MiB;
    uint64_t initial_buffer_start = 24 * MiB;

    int rc = calculate_cd_fetch_ranges(central_dir_offset, central_dir_size,
                                        part_size, initial_buffer_start,
                                        &ranges, &num_ranges);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_size_t(1, num_ranges);
    TEST_ASSERT_NOT_NULL(ranges);

    // Range starts exactly at CD offset (which is part-aligned)
    TEST_ASSERT_EQUAL_UINT64(16 * MiB, ranges[0].start);
    TEST_ASSERT_EQUAL_UINT64(24 * MiB - 1, ranges[0].end);
    TEST_ASSERT_FALSE(ranges[0].has_body_data);  // No body data before CD

    free(ranges);
}

/**
 * Test: assemble_cd_buffer with data from multiple ranges.
 */
void test_assemble_buffer_from_ranges(void) {
    // Simulate fetched range with CD data
    uint8_t range_data[64];
    for (int i = 0; i < 64; i++) {
        range_data[i] = (uint8_t)i;
    }

    uint8_t initial_data[64];
    for (int i = 0; i < 64; i++) {
        initial_data[i] = (uint8_t)(64 + i);
    }

    // Setup: CD at offset 32, size 64
    // Initial buffer at offset 64, size 64
    // Range buffer at offset 0, size 64 (covers body 0-32 and CD 32-64)
    struct cd_part_range ranges[1] = {
        { .start = 0, .end = 63, .has_body_data = true, .body_data_size = 32 }
    };
    uint8_t *range_buffers[1] = { range_data };
    size_t range_sizes[1] = { 64 };

    uint8_t *cd_buffer = NULL;
    size_t cd_size = 0;
    struct body_data_segment *body_segments = NULL;
    size_t num_body_segments = 0;

    int rc = assemble_cd_buffer(
        initial_data, 64, 64,          // initial buffer
        ranges, range_buffers, range_sizes, 1,  // fetched ranges
        32, 64,                         // CD offset and size
        &cd_buffer, &cd_size,
        &body_segments, &num_body_segments
    );

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(cd_buffer);
    TEST_ASSERT_EQUAL_size_t(64, cd_size);

    // CD buffer should contain:
    // - Bytes 32-63 from range_data (indices 32-63)
    // - Bytes 64-95 from initial_data (indices 0-31)
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_UINT8(32 + i, cd_buffer[i]);  // From range
    }
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_UINT8(64 + i, cd_buffer[32 + i]);  // From initial
    }

    // Should have one body segment (from range)
    TEST_ASSERT_EQUAL_size_t(1, num_body_segments);
    TEST_ASSERT_NOT_NULL(body_segments);
    TEST_ASSERT_EQUAL_PTR(range_data, body_segments[0].data);
    TEST_ASSERT_EQUAL_size_t(32, body_segments[0].size);
    TEST_ASSERT_EQUAL_UINT64(0, body_segments[0].archive_offset);

    free(cd_buffer);
    free(body_segments);
}

/**
 * Test: add_tail_buffer_segment when tail has body data.
 */
void test_add_tail_buffer_segment_with_body_data(void) {
    uint8_t buffer[128];
    for (int i = 0; i < 128; i++) {
        buffer[i] = (uint8_t)i;
    }

    struct body_data_segment *segments = NULL;
    size_t num_segments = 0;

    // Tail buffer starts at 100, size 128
    // CD starts at 150, so body data is 100-150 (50 bytes)
    int rc = add_tail_buffer_segment(&segments, &num_segments,
                                      buffer, 128, 100, 150, 8 * MiB);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_size_t(1, num_segments);
    TEST_ASSERT_NOT_NULL(segments);

    TEST_ASSERT_EQUAL_PTR(buffer, segments[0].data);
    TEST_ASSERT_EQUAL_size_t(50, segments[0].size);
    TEST_ASSERT_EQUAL_UINT64(100, segments[0].archive_offset);

    free(segments);
}

/**
 * Test: add_tail_buffer_segment when tail has no body data.
 */
void test_add_tail_buffer_segment_no_body_data(void) {
    uint8_t buffer[128] = {0};  // Initialize to avoid warning
    struct body_data_segment *segments = NULL;
    size_t num_segments = 0;

    // Tail buffer starts at 200, but CD starts at 100 (before tail)
    // No body data in this tail buffer
    int rc = add_tail_buffer_segment(&segments, &num_segments,
                                      buffer, 128, 200, 100, 8 * MiB);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_size_t(0, num_segments);
    TEST_ASSERT_NULL(segments);
}

/**
 * Test: add_tail_buffer_segment appends to existing segments.
 */
void test_add_tail_buffer_segment_appends(void) {
    uint8_t buffer1[64];
    uint8_t buffer2[64];

    // Start with one existing segment
    struct body_data_segment *segments = malloc(sizeof(struct body_data_segment));
    segments[0].data = buffer1;
    segments[0].size = 64;
    segments[0].archive_offset = 0;
    size_t num_segments = 1;

    // Add tail buffer segment
    int rc = add_tail_buffer_segment(&segments, &num_segments,
                                      buffer2, 64, 100, 150, 8 * MiB);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_size_t(2, num_segments);
    TEST_ASSERT_NOT_NULL(segments);

    // Original segment preserved
    TEST_ASSERT_EQUAL_PTR(buffer1, segments[0].data);
    TEST_ASSERT_EQUAL_size_t(64, segments[0].size);
    TEST_ASSERT_EQUAL_UINT64(0, segments[0].archive_offset);

    // New segment added
    TEST_ASSERT_EQUAL_PTR(buffer2, segments[1].data);
    TEST_ASSERT_EQUAL_size_t(50, segments[1].size);
    TEST_ASSERT_EQUAL_UINT64(100, segments[1].archive_offset);

    free(segments);
}

/**
 * Test: free_body_segments handles NULL.
 */
void test_free_body_segments_null(void) {
    // Should not crash
    free_body_segments(NULL, 0);
}

/**
 * Test: Real-world scenario - 30 MiB CD with 8 MiB part size.
 * Based on the original bug report example.
 */
void test_calculate_ranges_real_world_30mib_cd(void) {
    struct cd_part_range *ranges = NULL;
    size_t num_ranges = 0;

    // 4 GB archive with 30.75 MiB CD
    // Initial 8 MiB buffer
    // CD starts at ~3.96 GB
    uint64_t archive_size = 4ULL * 1024 * MiB;  // 4 GiB
    uint64_t central_dir_size = 31539200;  // ~30.08 MiB
    uint64_t central_dir_offset = archive_size - central_dir_size - 22;  // 22 bytes for EOCD
    uint64_t part_size = 8 * MiB;
    uint64_t initial_buffer_start = archive_size - 8 * MiB;

    int rc = calculate_cd_fetch_ranges(central_dir_offset, central_dir_size,
                                        part_size, initial_buffer_start,
                                        &ranges, &num_ranges);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(ranges);

    // Should need 3 additional parts (CD extends ~22.75 MiB before initial buffer)
    // Part boundaries before initial_buffer_start that CD crosses:
    // archive_size - 8 MiB (initial), then 16 MiB, 24 MiB, 32 MiB back
    TEST_ASSERT_TRUE(num_ranges >= 2);

    // First range should have body data (aligned start before CD offset)
    // The amount of body data depends on alignment

    free(ranges);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_calculate_ranges_small_cd);
    RUN_TEST(test_calculate_ranges_one_part);
    RUN_TEST(test_calculate_ranges_multiple_parts);
    RUN_TEST(test_calculate_ranges_exact_alignment);
    RUN_TEST(test_assemble_buffer_from_ranges);
    RUN_TEST(test_add_tail_buffer_segment_with_body_data);
    RUN_TEST(test_add_tail_buffer_segment_no_body_data);
    RUN_TEST(test_add_tail_buffer_segment_appends);
    RUN_TEST(test_free_body_segments_null);
    RUN_TEST(test_calculate_ranges_real_world_30mib_cd);

    return UNITY_END();
}
