#include "alignment.h"
#include "zip_structures.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Calculate next 8 MiB boundary
uint64_t alignment_next_boundary(uint64_t current_offset) {
    // Round up to next BURST_PART_SIZE boundary
    return ((current_offset / BURST_PART_SIZE) + 1) * BURST_PART_SIZE;
}

// Calculate current write position (including buffered data)
uint64_t alignment_get_write_position(struct burst_writer *writer) {
    return writer->current_offset + writer->buffer_used;
}

// Make alignment decision before writing a frame
struct alignment_decision alignment_decide(
    uint64_t current_offset,
    size_t frame_size,
    bool at_file_end)
{
    struct alignment_decision decision = {0};
    decision.next_boundary = alignment_next_boundary(current_offset);
    decision.at_file_end = at_file_end;

    // Calculate space required
    size_t space_required = frame_size;
    if (at_file_end) {
        space_required += sizeof(struct zip_data_descriptor);  // 16 bytes
    }

    uint64_t space_until_boundary = decision.next_boundary - current_offset;

    // Case 1: Exact fit (frame + optional descriptor fills exactly to boundary)
    if (space_until_boundary == space_required) {
        if (at_file_end) {
            // Descriptor ends at boundary, next LFH starts there - OK
            decision.action = ALIGNMENT_WRITE_FRAME;
        } else {
            // Frame ends exactly at boundary, more data follows
            // Need Start-of-Part metadata frame at boundary
            decision.action = ALIGNMENT_WRITE_FRAME_THEN_METADATA;
        }
        return decision;
    }

    // Case 2: Fits comfortably (space for frame + descriptor + minimum padding frame)
    size_t space_with_min_pad = space_required + BURST_MIN_SKIPPABLE_FRAME_SIZE;
    if (space_until_boundary >= space_with_min_pad) {
        decision.action = ALIGNMENT_WRITE_FRAME;
        return decision;
    }

    // Case 3: Doesn't fit - need padding to reach boundary
    decision.padding_size = (size_t)(space_until_boundary - 8);  // 8-byte header

    // Mid-file - need Start-of-Part metadata at boundary
    decision.action = ALIGNMENT_PAD_THEN_METADATA;

    return decision;
}

// Write skippable padding frame
int alignment_write_padding_frame(struct burst_writer *writer, size_t padding_size) {
    // Skippable frame format:
    // - Magic: 0x184D2A5B (4 bytes)
    // - Frame size: padding_size (4 bytes, excludes 8-byte header)
    // - Padding data: zeros (padding_size bytes)

    uint32_t magic = BURST_MAGIC_NUMBER;
    uint32_t frame_size = (uint32_t)padding_size;

    // Write magic number
    if (burst_writer_write(writer, &magic, sizeof(magic)) != 0) {
        return -1;
    }
    writer->padding_bytes += sizeof(magic);

    // Write frame size
    if (burst_writer_write(writer, &frame_size, sizeof(frame_size)) != 0) {
        return -1;
    }
    writer->padding_bytes += sizeof(frame_size);

    // Write padding (zeros)
    if (padding_size > 0) {
        uint8_t *padding = calloc(1, padding_size);
        if (!padding) {
            fprintf(stderr, "Failed to allocate padding buffer\n");
            return -1;
        }

        int result = burst_writer_write(writer, padding, padding_size);
        free(padding);

        if (result != 0) {
            return -1;
        }
        writer->padding_bytes += frame_size;
    }

    return 0;
}

// Write Start-of-Part metadata frame
int alignment_write_start_of_part_frame(struct burst_writer *writer,
                                        uint64_t uncompressed_offset) {
    // Start-of-Part frame format:
    // - Magic: 0x184D2A5B (4 bytes)
    // - Frame size: 16 bytes fixed (4 bytes)
    // - BURST info type flag: 0x01 (1 byte)
    // - Uncompressed offset: uncompressed_offset (8 bytes)
    // - Reserved/padding: 7 bytes of zeros

    uint32_t magic = BURST_MAGIC_NUMBER;
    uint32_t frame_size = 16;  // Fixed size for Start-of-Part frame
    uint8_t info_type = 0x01;  // Start-of-Part marker
    uint8_t reserved[7] = {0}; // Reserved bytes
    uint64_t bytes_written = 0;

    // Write magic number
    if (burst_writer_write(writer, &magic, sizeof(magic)) != 0) {
        return -1;
    }
    bytes_written += sizeof(magic);

    // Write frame size
    if (burst_writer_write(writer, &frame_size, sizeof(frame_size)) != 0) {
        return -1;
    }
    bytes_written += sizeof(frame_size);

    // Write info type flag
    if (burst_writer_write(writer, &info_type, sizeof(info_type)) != 0) {
        return -1;
    }
    bytes_written += sizeof(info_type);

    // Write uncompressed offset
    if (burst_writer_write(writer, &uncompressed_offset, sizeof(uncompressed_offset)) != 0) {
        return -1;
    }
    bytes_written += sizeof(uncompressed_offset);

    // Write reserved bytes
    if (burst_writer_write(writer, reserved, sizeof(reserved)) != 0) {
        return -1;
    }
    bytes_written += sizeof(reserved);

    writer->padding_bytes += bytes_written;

    return 0;
}
