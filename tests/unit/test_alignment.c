#include "unity.h"
#include "alignment.h"
#include "burst_writer.h"
#include "zip_structures.h"
#include <string.h>

void setUp(void) {
    // Unity setup
}

void tearDown(void) {
    // Unity teardown
}

// Test 1: Exact boundary fit
void test_exact_boundary_fit(void) {
    // Frame + descriptor exactly fills to boundary
    uint64_t current_offset = 8388608 - 1000;  // 1000 bytes before boundary
    size_t frame_size = 1000 - sizeof(struct zip_data_descriptor);  // 984 bytes

    struct alignment_decision decision = alignment_decide(
        current_offset, frame_size, true);

    TEST_ASSERT_EQUAL(ALIGNMENT_WRITE_FRAME, decision.action);
    TEST_ASSERT_FALSE(decision.descriptor_after_boundary);
    TEST_ASSERT_EQUAL(8388608, decision.next_boundary);
}

// Test 2: Frame fits with room for padding
void test_frame_fits_comfortably(void) {
    // Plenty of space before boundary
    uint64_t current_offset = 100;
    size_t frame_size = 50000;  // 50 KB frame

    struct alignment_decision decision = alignment_decide(
        current_offset, frame_size, true);

    TEST_ASSERT_EQUAL(ALIGNMENT_WRITE_FRAME, decision.action);
    TEST_ASSERT_FALSE(decision.descriptor_after_boundary);
}

// Test 3: Frame doesn't fit, at EOF
void test_frame_doesnt_fit_at_eof(void) {
    // Frame fits space, but frame + descriptor exceeds space to boundary
    uint64_t current_offset = 8388608 - 100;  // 100 bytes before boundary
    size_t frame_size = 100 - BURST_MIN_SKIPPABLE_FRAME_SIZE - sizeof(struct zip_data_descriptor) + 1; // Just over fit

    struct alignment_decision decision = alignment_decide(
        current_offset, frame_size, true);

    TEST_ASSERT_EQUAL(ALIGNMENT_PAD_THEN_METADATA, decision.action);
    TEST_ASSERT_EQUAL(100 - 8, decision.padding_size);  // Space minus header
    TEST_ASSERT_FALSE(decision.descriptor_after_boundary);
}

// Test 4: Frame doesn't fit, mid-file
void test_frame_doesnt_fit_mid_file(void) {
    // Frame exceeds space, more data coming
    uint64_t current_offset = 8388608 - 100;
    size_t frame_size = 200;

    struct alignment_decision decision = alignment_decide(
        current_offset, frame_size, false);  // NOT at EOF

    TEST_ASSERT_EQUAL(ALIGNMENT_PAD_THEN_METADATA, decision.action);
    TEST_ASSERT_EQUAL(100 - 8, decision.padding_size);
}

// Test 5: Boundary calculation
void test_boundary_calculation(void) {
    TEST_ASSERT_EQUAL(8388608, alignment_next_boundary(0));
    TEST_ASSERT_EQUAL(8388608, alignment_next_boundary(100));
    TEST_ASSERT_EQUAL(8388608, alignment_next_boundary(8388607));
    TEST_ASSERT_EQUAL(16777216, alignment_next_boundary(8388608));
    TEST_ASSERT_EQUAL(16777216, alignment_next_boundary(8388609));
    TEST_ASSERT_EQUAL(25165824, alignment_next_boundary(16777216));
}

// Test 6: Write position calculation
void test_write_position_calculation(void) {
    struct burst_writer writer = {0};
    writer.current_offset = 1000;
    writer.buffer_used = 500;

    uint64_t pos = alignment_get_write_position(&writer);
    TEST_ASSERT_EQUAL(1500, pos);
}

// Test 7: Edge case - at actual boundary
void test_at_boundary(void) {
    uint64_t current_offset = 8388608;  // Exactly at boundary
    size_t frame_size = 50000;

    struct alignment_decision decision = alignment_decide(
        current_offset, frame_size, false);

    // Should treat next boundary as target
    TEST_ASSERT_EQUAL(16777216, decision.next_boundary);
    TEST_ASSERT_EQUAL(ALIGNMENT_WRITE_FRAME, decision.action);
}

// Test 8: Minimum padding frame size
void test_minimum_padding_size(void) {
    // Test that MIN_SKIPPABLE_FRAME_SIZE (8 bytes) is correctly handled
    uint64_t current_offset = 8388608 - 16;  // 16 bytes before boundary
    size_t frame_size = 0;  // Tiny frame

    struct alignment_decision decision = alignment_decide(
        current_offset, frame_size, false);

    // Should fit (0 + 0 + 8 = 8 < 16)
    TEST_ASSERT_EQUAL(ALIGNMENT_WRITE_FRAME, decision.action);
}

// Test 9: Large frame spanning multiple chunks
void test_large_frame(void) {
    uint64_t current_offset = 100;
    size_t frame_size = 128 * 1024;  // 128 KB - BTRFS maximum

    struct alignment_decision decision = alignment_decide(
        current_offset, frame_size, false);

    // Should fit comfortably in 8 MiB part
    TEST_ASSERT_EQUAL(ALIGNMENT_WRITE_FRAME, decision.action);
    TEST_ASSERT_FALSE(decision.descriptor_after_boundary);
}

// Test 10: Empty file data descriptor near boundary
void test_empty_file_descriptor_alignment(void) {
    // Empty file: local header ends 20 bytes before boundary
    // Data descriptor (16 bytes) + min padding (8) = 24 bytes needed
    // Only 20 bytes available → needs padding
    uint64_t current_offset = 8388608 - 20;  // After local header + empty zstd frame
    uint64_t next_boundary = alignment_next_boundary(current_offset);
    uint64_t space = next_boundary - current_offset;

    TEST_ASSERT_EQUAL(20, space);
    TEST_ASSERT_TRUE(space < 16 + 8);  // Descriptor + min padding won't fit
}

// Test 11: Empty file descriptor fits comfortably
void test_empty_file_descriptor_fits(void) {
    // Empty file with plenty of space before boundary
    uint64_t current_offset = 8388608 - 100;
    uint64_t space = alignment_next_boundary(current_offset) - current_offset;

    TEST_ASSERT_EQUAL(100, space);
    TEST_ASSERT_TRUE(space >= 16 + 8);  // Descriptor fits comfortably
}

// Test 12: Multiple empty files crossing boundary
void test_multiple_empty_files_near_boundary(void) {
    // Simulate multiple empty file structures
    // Each empty file: local header (30 + filename) + empty zstd frame (13) + descriptor (16)
    // Assuming 10-char filename: 30 + 10 = 40, total = 40 + 13 + 16 = 69 bytes per empty file
    uint64_t offset = 8388608 - 200;  // 200 bytes before boundary

    // After 2 files: 200 - 138 = 62 bytes remaining
    offset += 69 * 2;  // Two complete empty files
    uint64_t space = 8388608 - offset;

    TEST_ASSERT_EQUAL(62, space);  // Enough for another file but testing boundary logic
    TEST_ASSERT_TRUE(space < 69);  // Not enough for another complete empty file
}

// Test 13: Mid-file frame fits exactly to boundary - needs Start-of-Part after
void test_exact_fit_mid_file_needs_metadata(void) {
    // Frame fits exactly to boundary, but more data follows (not at EOF)
    uint64_t current_offset = 8388608 - 100;  // 100 bytes before boundary
    size_t frame_size = 100;  // Exactly fills to boundary

    struct alignment_decision decision = alignment_decide(
        current_offset, frame_size, false);  // NOT at EOF

    // Should return WRITE_FRAME_THEN_METADATA since more data follows
    TEST_ASSERT_EQUAL(ALIGNMENT_WRITE_FRAME_THEN_METADATA, decision.action);
    TEST_ASSERT_FALSE(decision.descriptor_after_boundary);
}

// Test 14: EOF frame+descriptor fits exactly to boundary - just write frame
void test_exact_fit_eof_no_metadata(void) {
    // Frame + descriptor fits exactly to boundary at EOF
    uint64_t current_offset = 8388608 - 100;  // 100 bytes before boundary
    size_t frame_size = 100 - sizeof(struct zip_data_descriptor);  // 84 bytes

    struct alignment_decision decision = alignment_decide(
        current_offset, frame_size, true);  // At EOF

    // Should return WRITE_FRAME since next LFH will start at boundary
    TEST_ASSERT_EQUAL(ALIGNMENT_WRITE_FRAME, decision.action);
    TEST_ASSERT_FALSE(decision.descriptor_after_boundary);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_exact_boundary_fit);
    RUN_TEST(test_frame_fits_comfortably);
    RUN_TEST(test_frame_doesnt_fit_at_eof);
    RUN_TEST(test_frame_doesnt_fit_mid_file);
    RUN_TEST(test_boundary_calculation);
    RUN_TEST(test_write_position_calculation);
    RUN_TEST(test_at_boundary);
    RUN_TEST(test_minimum_padding_size);
    RUN_TEST(test_large_frame);
    RUN_TEST(test_empty_file_descriptor_alignment);
    RUN_TEST(test_empty_file_descriptor_fits);
    RUN_TEST(test_multiple_empty_files_near_boundary);
    RUN_TEST(test_exact_fit_mid_file_needs_metadata);
    RUN_TEST(test_exact_fit_eof_no_metadata);

    return UNITY_END();
}
