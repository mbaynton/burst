#ifndef ALIGNMENT_H
#define ALIGNMENT_H

#include <stdint.h>
#include <stdbool.h>
#include "burst_writer.h"

// Alignment decision actions
enum alignment_action {
    ALIGNMENT_WRITE_FRAME,           // Write frame immediately
    ALIGNMENT_PAD_THEN_FRAME,        // Pad to boundary, then write frame
    ALIGNMENT_PAD_THEN_METADATA,     // Pad to boundary, write metadata, then frame
    ALIGNMENT_WRITE_FRAME_THEN_METADATA  // Write frame (fills to boundary), then metadata
};

// Result of alignment decision
struct alignment_decision {
    enum alignment_action action;
    size_t padding_size;             // Size of padding frame payload (if needed)
    uint64_t next_boundary;          // Next 8 MiB boundary offset
    bool at_file_end;                // True if this is the last frame of file
};

// Make alignment decision before writing a frame
struct alignment_decision alignment_decide(
    uint64_t current_offset,
    size_t frame_size,
    bool at_file_end);

// Write skippable padding frame
int alignment_write_padding_frame(struct burst_writer *writer, size_t padding_size);

// Write Start-of-Part metadata frame
int alignment_write_start_of_part_frame(struct burst_writer *writer,
                                        uint64_t uncompressed_offset);

// Calculate current write position (offset + buffered)
uint64_t alignment_get_write_position(struct burst_writer *writer);

// Calculate next 8 MiB boundary
uint64_t alignment_next_boundary(uint64_t current_offset);

#endif // ALIGNMENT_H
