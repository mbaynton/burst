/**
 * Part download calculation logic.
 *
 * This file contains the pure calculation logic for determining how many
 * parts need to be downloaded from S3 vs processed from the CD buffer.
 * It's separated from s3_operations.c to enable unit testing without
 * AWS SDK dependencies.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

void calculate_parts_to_download(
    size_t num_parts,
    uint64_t part_size,
    uint64_t cd_start,
    size_t *parts_to_download,
    bool *process_final_from_buffer
) {
    if (num_parts == 0) {
        *parts_to_download = 0;
        *process_final_from_buffer = false;
        return;
    }

    // Calculate where the final part starts
    uint64_t final_part_start = (uint64_t)(num_parts - 1) * part_size;

    // The CD buffer contains the last 8 MiB of the archive (starting at cd_start).
    // We can only process a part from the CD buffer if it starts at or after cd_start.
    // With larger part sizes (e.g., 16 MiB), the final part may start before cd_start,
    // in which case we need to download it from S3.
    if (final_part_start >= cd_start) {
        // Final part starts within or after CD buffer - process it from buffer
        *parts_to_download = num_parts - 1;
        *process_final_from_buffer = true;
    } else {
        // Final part starts before CD buffer - must download it from S3
        *parts_to_download = num_parts;
        *process_final_from_buffer = false;
    }
}
