#ifndef STREAM_PROCESSOR_H
#define STREAM_PROCESSOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Forward declaration
struct central_dir_parse_result;
struct file_metadata;

/**
 * @file stream_processor.h
 * @brief Stream processor for BURST archive downloader.
 *
 * This module implements sequential frame traversal for processing 8 MiB parts
 * downloaded from S3. It parses ZIP local headers, detects Zstandard frames
 * and BURST skippable frames, and calls the BTRFS writer for each compressed
 * frame.
 *
 * ## Design
 *
 * The stream processor is designed for streaming operation where data arrives
 * in arbitrary-sized chunks from S3 callbacks. It buffers partial frames that
 * span callback boundaries and maintains state between calls.
 *
 * ## Usage
 *
 * ```c
 * // Create processor for a specific part
 * struct part_processor_state *state = part_processor_create(
 *     part_index, cd_result, "/output/dir");
 *
 * // Feed data as it arrives from S3
 * while (data_available) {
 *     int rc = part_processor_process_data(state, data, len);
 *     if (rc != 0) { handle_error(); }
 * }
 *
 * // Finalize when part download completes
 * part_processor_finalize(state);
 * part_processor_destroy(state);
 * ```
 */

// Return codes
#define STREAM_PROC_SUCCESS 0
#define STREAM_PROC_SKIPPED_PADDING 1  // Successfully skipped padding LFH (don't increment entry index)
#define STREAM_PROC_ERR_INVALID_ARGS -1
#define STREAM_PROC_ERR_MEMORY -2
#define STREAM_PROC_ERR_INVALID_FRAME -3
#define STREAM_PROC_ERR_IO -4
#define STREAM_PROC_ERR_BTRFS_WRITE -5
#define STREAM_PROC_ERR_UNEXPECTED_EOF -6
#define STREAM_PROC_NEED_MORE_DATA -7

// Magic numbers for frame detection
#define ZSTD_MAGIC_NUMBER 0xFD2FB528
#define BURST_SKIPPABLE_MAGIC 0x184D2A5B

// BURST skippable frame type bytes
#define BURST_TYPE_PADDING 0x00
#define BURST_TYPE_START_OF_PART 0x01

// State machine states
enum processor_state {
    STATE_INIT,                 // Initial state, checking for continuing file
    STATE_CONTINUING_FILE,      // Processing continuation from previous part
    STATE_EXPECT_LOCAL_HEADER,  // Expecting ZIP local file header
    STATE_PROCESSING_FRAMES,    // Processing Zstd/skippable frames
    STATE_READING_SYMLINK,      // Reading raw symlink content (STORE method)
    STATE_DONE,                 // Part processing complete
    STATE_ERROR                 // Error state
};

/**
 * Context for a file being written.
 * Tracks file descriptor and write progress.
 */
struct file_context {
    char *filename;             // Full path to output file (allocated)
    int fd;                     // File descriptor (or -1 if not open)
    uint64_t uncompressed_offset;   // Next write position for BTRFS_IOC_ENCODED_WRITE
                                    // Initialized from Start-of-Part frame for continuing files,
                                    // or 0 for new files. Incremented by each frame's uncompressed size.
    uint64_t expected_total_size;   // From central directory, for validation
    uint32_t expected_crc32;        // From central directory, for validation

    // Unix metadata for permission restoration
    uint32_t unix_mode;         // Unix mode bits (permissions + file type)
    uint32_t uid;               // Unix user ID
    uint32_t gid;               // Unix group ID
    bool has_unix_mode;         // True if unix_mode should be applied
    bool has_unix_extra;        // True if uid/gid should be applied
    bool is_symlink;            // True if this is a symlink (handled differently)
    bool is_directory;          // True if this is a directory entry (filename ends with '/')

    // Symlink content buffer (for STORE-method symlinks)
    uint8_t *symlink_buffer;    // Buffer for symlink target path
    size_t symlink_buffer_size; // Allocated size
    size_t symlink_bytes_read;  // Bytes read so far

    // ZIP64 tracking
    bool uses_zip64_descriptor; // True if data descriptor is 24 bytes (ZIP64), false if 16 bytes
};

/**
 * State for processing a single part.
 * Handles frame boundary spanning and maintains context.
 */
struct part_processor_state {
    // Part identification
    uint32_t part_index;
    uint64_t part_size;             // Part size in bytes
    uint64_t part_start_offset;     // Archive offset where this part starts

    // Central directory metadata (borrowed reference, not owned)
    struct central_dir_parse_result *cd_result;
    const char *output_dir;

    // Current file being processed
    struct file_context *current_file;

    // Frame buffer for handling partial frames across callbacks
    uint8_t *frame_buffer;
    size_t frame_buffer_capacity;
    size_t frame_buffer_used;

    // Position tracking
    size_t next_entry_idx;          // Index into parts[part_index].entries[]
    uint64_t bytes_processed;       // Bytes processed in this part so far

    // State machine
    enum processor_state state;

    // Error tracking
    int error_code;
    char error_message[256];
};

/**
 * Information about a detected frame.
 * Used internally during frame traversal.
 */
struct frame_info {
    enum {
        FRAME_ZSTD_COMPRESSED,
        FRAME_BURST_PADDING,
        FRAME_BURST_START_OF_PART,
        FRAME_ZIP_LOCAL_HEADER,
        FRAME_ZIP_DATA_DESCRIPTOR,
        FRAME_UNKNOWN
    } type;

    size_t frame_size;              // Total size including header
    uint64_t uncompressed_size;     // For Zstd frames: uncompressed data size
    uint64_t start_of_part_offset;  // For Start-of-Part frames: uncompressed offset
};

/**
 * Create a part processor state.
 *
 * @param part_index Part number to process (0-indexed)
 * @param cd_result Central directory parse result (borrowed, not owned)
 * @param output_dir Base directory for extracted files
 * @param part_size Part size in bytes
 * @return Allocated state, or NULL on error
 */
struct part_processor_state *part_processor_create(
    uint32_t part_index,
    struct central_dir_parse_result *cd_result,
    const char *output_dir,
    uint64_t part_size);

/**
 * Process incoming data from S3 callback.
 *
 * This function handles partial frames by buffering. Call it repeatedly
 * as data arrives from S3. The processor will consume as much data as
 * possible and buffer any partial frames at the end.
 *
 * @param state Processor state
 * @param data Incoming data chunk
 * @param data_len Length of data
 * @return STREAM_PROC_SUCCESS on success, negative error code on failure
 */
int part_processor_process_data(
    struct part_processor_state *state,
    const uint8_t *data,
    size_t data_len);

/**
 * Finalize part processing.
 *
 * Call after all data for the part has been processed. This ensures all
 * frames have been processed and files are properly closed.
 *
 * @param state Processor state
 * @return STREAM_PROC_SUCCESS on success, negative error code on failure
 */
int part_processor_finalize(struct part_processor_state *state);

/**
 * Clean up and free processor state.
 *
 * Closes any open files and frees all allocated memory.
 *
 * @param state Processor state (can be NULL)
 */
void part_processor_destroy(struct part_processor_state *state);

/**
 * Get error message from processor state.
 *
 * @param state Processor state
 * @return Error message string (valid until state is destroyed)
 */
const char *part_processor_get_error(const struct part_processor_state *state);

#endif // STREAM_PROCESSOR_H
