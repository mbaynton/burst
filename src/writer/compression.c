#include "compression.h"
#include <zstd.h>
#include <stdio.h>

struct compression_result compress_chunk(
    uint8_t* output_buffer,
    size_t output_capacity,
    const uint8_t* input_buffer,
    size_t input_size,
    int compression_level)
{
    struct compression_result result = {0};

    result.compressed_size = ZSTD_compress(
        output_buffer, output_capacity,
        input_buffer, input_size,
        compression_level);

    if (ZSTD_isError(result.compressed_size)) {
        result.error = -1;
        result.error_message = ZSTD_getErrorName(result.compressed_size);
    }

    return result;
}

int verify_frame_content_size(
    const uint8_t* compressed_data,
    size_t compressed_size,
    size_t expected_uncompressed_size)
{
    unsigned long long frame_content_size =
        ZSTD_getFrameContentSize(compressed_data, compressed_size);

    if (frame_content_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        fprintf(stderr, "Warning: Zstandard frame missing content size\n");
        return -1;
    }

    if (frame_content_size != expected_uncompressed_size) {
        fprintf(stderr, "Error: Zstandard frame content size mismatch\n");
        return -1;
    }

    return 0;
}
