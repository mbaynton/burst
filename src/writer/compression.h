#ifndef BURST_COMPRESSION_H
#define BURST_COMPRESSION_H

#include <stddef.h>
#include <stdint.h>

// Compression result
struct compression_result {
    size_t compressed_size;
    int error;
    const char* error_message;
};

// Compress a single chunk (mockable interface)
struct compression_result compress_chunk(
    uint8_t* output_buffer,
    size_t output_capacity,
    const uint8_t* input_buffer,
    size_t input_size,
    int compression_level);

// Verify frame has content size (mockable interface)
int verify_frame_content_size(
    const uint8_t* compressed_data,
    size_t compressed_size,
    size_t expected_uncompressed_size);

#endif // BURST_COMPRESSION_H
