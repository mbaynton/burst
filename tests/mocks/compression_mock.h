// Mockable version of compression.h for testing
#ifndef COMPRESSION_MOCK_H
#define COMPRESSION_MOCK_H

#include <stddef.h>
#include <stdint.h>

struct compression_result {
    size_t compressed_size;
    int error;
    const char* error_message;
};

struct compression_result compress_chunk(
    uint8_t* output_buffer,
    size_t output_capacity,
    const uint8_t* input_buffer,
    size_t input_size,
    int compression_level);

int verify_frame_content_size(
    const uint8_t* compressed_data,
    size_t compressed_size,
    size_t expected_uncompressed_size);

#endif // COMPRESSION_MOCK_H
