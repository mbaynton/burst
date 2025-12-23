#include "unity.h"
#include <zstd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

void setUp(void) {
}

void tearDown(void) {
}

// Test that ZSTD_compress() produces frames with content size in headers
// This is critical for BTRFS_IOC_ENCODED_WRITE downloader implementation
void test_zstd_frame_has_content_size_small(void) {
    // Allocate 1 KiB input buffer
    uint8_t input[1024];
    memset(input, 'A', sizeof(input));

    // Allocate output buffer
    size_t max_compressed = ZSTD_compressBound(sizeof(input));
    uint8_t *output = malloc(max_compressed);
    TEST_ASSERT_NOT_NULL(output);

    // Compress using ZSTD_compress() (matches burst_writer.c:242)
    size_t compressed_size = ZSTD_compress(output, max_compressed, input, sizeof(input), 3);
    TEST_ASSERT_FALSE(ZSTD_isError(compressed_size));

    // Verify frame has content size (matches burst_writer.c:256)
    unsigned long long content_size = ZSTD_getFrameContentSize(output, compressed_size);
    TEST_ASSERT_NOT_EQUAL(ZSTD_CONTENTSIZE_UNKNOWN, content_size);
    TEST_ASSERT_NOT_EQUAL(ZSTD_CONTENTSIZE_ERROR, content_size);
    TEST_ASSERT_EQUAL(1024, content_size);

    free(output);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_zstd_frame_has_content_size_small);
    return UNITY_END();
}
