#ifndef CD_FETCH_H
#define CD_FETCH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @file cd_fetch.h
 * @brief Central directory fetch utilities for large archives.
 *
 * When a BURST archive has a central directory larger than 8 MiB, we need to
 * fetch additional data beyond the initial tail buffer. This module provides
 * functions to calculate aligned fetch ranges and assemble the fetched data.
 */

// Forward declaration
struct burst_downloader;

/**
 * Structure representing a part-aligned range to fetch.
 *
 * Ranges are aligned to part_size boundaries from the start of the archive
 * because S3 performs better with aligned range requests.
 */
struct cd_part_range {
    uint64_t start;           /**< Part-aligned start offset in archive */
    uint64_t end;             /**< End offset (inclusive) */
    bool has_body_data;       /**< True if range includes zip body data before CD */
    uint64_t body_data_size;  /**< Bytes of body data before central_dir_offset */
};

/**
 * Body data segment that was pre-fetched.
 *
 * When fetching aligned parts for the CD, we may also fetch zip body data.
 * This structure tracks such pre-fetched body data for later processing
 * during extraction.
 */
struct body_data_segment {
    uint8_t *data;            /**< Pointer to body data (not owned, points into a buffer) */
    size_t size;              /**< Size of body data in bytes */
    uint64_t archive_offset;  /**< Where this data starts in the archive */
};

/**
 * Calculate which part-aligned ranges need to be fetched to cover the central directory.
 *
 * Ranges are aligned to part_size boundaries from the start of the archive.
 * Only ranges that are not already covered by the initial buffer are returned.
 *
 * @param central_dir_offset    Start of central directory in archive
 * @param central_dir_size      Size of central directory
 * @param part_size             Part size in bytes (8-64 MiB, multiple of 8 MiB)
 * @param initial_buffer_start  Archive offset where initial 8 MiB buffer starts
 * @param ranges                Output: array of ranges to fetch (caller must free)
 * @param num_ranges            Output: number of ranges
 * @return 0 on success, -1 on error
 */
int calculate_cd_fetch_ranges(
    uint64_t central_dir_offset,
    uint64_t central_dir_size,
    uint64_t part_size,
    uint64_t initial_buffer_start,
    struct cd_part_range **ranges,
    size_t *num_ranges
);

/**
 * Assemble fetched range buffers and initial buffer into a contiguous CD buffer.
 *
 * This function merges data from multiple sources:
 * - The initial tail buffer (last 8 MiB)
 * - Additional fetched range buffers
 *
 * It extracts:
 * - A contiguous buffer containing the complete central directory
 * - Any body data that was included in the fetched ranges
 *
 * @param initial_buffer        Initial 8 MiB tail buffer
 * @param initial_size          Size of initial buffer
 * @param initial_start         Archive offset where initial buffer starts
 * @param ranges                Array of fetched part ranges
 * @param range_buffers         Array of buffers from fetched ranges
 * @param range_sizes           Array of sizes for each fetched buffer
 * @param num_ranges            Number of ranges
 * @param central_dir_offset    Start of CD in archive
 * @param central_dir_size      Size of CD
 * @param out_cd_buffer         Output: contiguous CD buffer (caller must free)
 * @param out_cd_size           Output: size of CD buffer
 * @param out_body_segments     Output: array of body data segments (caller must free array)
 * @param out_num_body_segments Output: number of body segments
 * @return 0 on success, -1 on error
 */
int assemble_cd_buffer(
    const uint8_t *initial_buffer,
    size_t initial_size,
    uint64_t initial_start,
    const struct cd_part_range *ranges,
    uint8_t **range_buffers,
    size_t *range_sizes,
    size_t num_ranges,
    uint64_t central_dir_offset,
    uint64_t central_dir_size,
    uint8_t **out_cd_buffer,
    size_t *out_cd_size,
    struct body_data_segment **out_body_segments,
    size_t *out_num_body_segments
);

/**
 * Fetch multiple aligned ranges concurrently from S3.
 *
 * @param downloader       Initialized downloader
 * @param ranges           Array of ranges to fetch
 * @param num_ranges       Number of ranges
 * @param max_concurrent   Maximum concurrent requests
 * @param out_buffers      Output: array of buffers (caller must free each and array)
 * @param out_sizes        Output: array of buffer sizes (caller must free)
 * @return 0 on success, -1 on error
 */
int burst_downloader_fetch_cd_ranges(
    struct burst_downloader *downloader,
    const struct cd_part_range *ranges,
    size_t num_ranges,
    size_t max_concurrent,
    uint8_t ***out_buffers,
    size_t **out_sizes
);

/**
 * Add a body segment from the tail buffer.
 *
 * The tail buffer may contain body data from initial_start to central_dir_offset.
 * This function adds that segment to the body_segments array if applicable.
 *
 * @param body_segments         In/out: array of body segments (may be reallocated)
 * @param num_body_segments     In/out: number of segments
 * @param initial_buffer        Initial tail buffer
 * @param initial_size          Size of initial buffer
 * @param initial_start         Archive offset where initial buffer starts
 * @param central_dir_offset    Where central directory starts
 * @param part_size             Part size in bytes
 * @return 0 on success, -1 on error
 */
int add_tail_buffer_segment(
    struct body_data_segment **body_segments,
    size_t *num_body_segments,
    const uint8_t *initial_buffer,
    size_t initial_size,
    uint64_t initial_start,
    uint64_t central_dir_offset,
    uint64_t part_size
);

/**
 * Free body segments array.
 *
 * Note: This frees the array and any owned data buffers, but does NOT free
 * segment data that points into external buffers.
 *
 * @param segments      Array of body segments
 * @param num_segments  Number of segments
 */
void free_body_segments(struct body_data_segment *segments, size_t num_segments);

#endif // CD_FETCH_H
