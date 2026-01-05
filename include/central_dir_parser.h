#ifndef CENTRAL_DIR_PARSER_H
#define CENTRAL_DIR_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @file central_dir_parser.h
 * @brief ZIP central directory parser for BURST archive downloader.
 *
 * This module parses the central directory from a buffer containing the end of
 * a ZIP file (fetched via S3 range GET) and builds data structures optimized
 * for concurrent 8 MiB part downloads.
 *
 * ## Usage by Stream Processor (Phase 3)
 *
 * When downloading a BURST archive, the downloader first fetches the last 8 MiB
 * (or less for small archives) to parse the central directory. The resulting
 * `central_dir_parse_result` is then used to process each 8 MiB part concurrently.
 *
 * ### Processing a specific part
 *
 * For each part `part_idx` being downloaded:
 *
 * 1. **Check for continuing file**: If `parts[part_idx].continuing_file != NULL`,
 *    the first bytes of this part are compressed data belonging to a file that
 *    started in a previous part. The Start-of-Part metadata frame (at offset 0)
 *    contains the uncompressed offset where this data should be written. The
 *    `continuing_file` pointer provides the file's metadata (filename, expected
 *    sizes, etc.).
 *
 * 2. **Process files starting in this part**: The `parts[part_idx].entries[]`
 *    array lists all files whose local headers begin within this part, sorted
 *    by `offset_in_part` ascending. This sorting is critical: as the stream
 *    processor traverses the byte stream sequentially, each local header
 *    encountered corresponds to the next entry in the list.
 *
 * 3. **Access file metadata**: Each `part_file_entry` contains a `file_index`
 *    that indexes into the `files[]` array. Use this to get the full metadata:
 *    ```c
 *    struct part_file_entry *entry = &result->parts[part_idx].entries[i];
 *    struct file_metadata *file = &result->files[entry->file_index];
 *    // Now use file->filename, file->compressed_size, etc.
 *    ```
 *
 * ### Why local headers are still parsed
 *
 * Although file metadata comes from the central directory (via `files[]`), the
 * stream processor still parses local headers to:
 * - Skip variable-length fields (filename, extra field) to reach compressed data
 * - The local header's filename_length and extra_field_length determine the
 *   offset to the actual compressed data bytes
 *
 * ### Example traversal pseudocode
 *
 * ```c
 * void process_part(struct central_dir_parse_result *result, uint32_t part_idx,
 *                   uint8_t *part_data, size_t part_size) {
 *     struct part_files *part = &result->parts[part_idx];
 *     size_t offset = 0;
 *     size_t entry_idx = 0;
 *
 *     // Handle continuing file from previous part
 *     if (part->continuing_file != NULL) {
 *         // Parse Start-of-Part frame at offset 0 for uncompressed_offset
 *         // Write subsequent Zstd frames to continuing_file at that offset
 *         // Advance offset past the continuing file's data
 *     }
 *
 *     // Process files starting in this part
 *     while (entry_idx < part->num_entries) {
 *         struct part_file_entry *entry = &part->entries[entry_idx];
 *         struct file_metadata *file = &result->files[entry->file_index];
 *
 *         // Skip any padding frames to reach entry->offset_in_part
 *         // Parse local header at entry->offset_in_part
 *         // Extract and write Zstd frames using file->compressed_size as guide
 *         entry_idx++;
 *     }
 * }
 * ```
 */

// Parse result codes
#define CENTRAL_DIR_PARSE_SUCCESS 0
#define CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER -1
#define CENTRAL_DIR_PARSE_ERR_NO_EOCD -2
#define CENTRAL_DIR_PARSE_ERR_TRUNCATED -3
#define CENTRAL_DIR_PARSE_ERR_INVALID_SIGNATURE -4
#define CENTRAL_DIR_PARSE_ERR_MEMORY -5
#define CENTRAL_DIR_PARSE_ERR_ZIP64_UNSUPPORTED -6

// Base part size constant (8 MiB) - used for BURST archive alignment
#define BURST_BASE_PART_SIZE (8 * 1024 * 1024)

// BURST EOCD comment format constants (must match writer definitions)
// Format: Magic(4) + Version(1) + Offset(3) = 8 bytes
// Note: The offset is relative to TAIL START (archive_size - 8 MiB), NOT CD start
#define BURST_EOCD_COMMENT_MAGIC 0x54535242  // "BRST" in little-endian
#define BURST_EOCD_COMMENT_VERSION 1
#define BURST_EOCD_COMMENT_SIZE 8
#define BURST_EOCD_NO_CDFH_IN_TAIL 0xFFFFFF  // Sentinel: no complete CDFH in tail

/**
 * File metadata extracted from the central directory.
 *
 * Each entry corresponds to one file in the archive. The `part_index` field
 * indicates which 8 MiB part contains this file's local header.
 */
struct file_metadata {
    char *filename;                    // Allocated, caller must free via cleanup
    uint64_t local_header_offset;      // Offset in archive where local header starts
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint32_t crc32;
    uint16_t compression_method;
    uint32_t part_index;               // Derived: local_header_offset / 8MiB

    // Unix metadata from external_file_attributes and extra fields
    uint32_t unix_mode;                // Unix mode bits (from external_file_attributes >> 16)
    uint32_t uid;                      // Unix user ID (from 0x7875 extra field)
    uint32_t gid;                      // Unix group ID (from 0x7875 extra field)
    bool has_unix_mode;                // True if unix_mode was extracted
    bool has_unix_extra;               // True if uid/gid were extracted from extra field
    bool is_symlink;                   // True if (unix_mode & S_IFMT) == S_IFLNK

    // ZIP64 tracking
    bool uses_zip64_descriptor;        // True if ZIP64 extra field present (data descriptor is 24 bytes)
};

/**
 * Entry in the part-to-files mapping.
 *
 * Maps a file to its position within a specific 8 MiB part. The `file_index`
 * references the full metadata in the `files[]` array of the parse result.
 */
struct part_file_entry {
    size_t file_index;                 // Index into files array
    uint64_t offset_in_part;           // Where this file's local header starts within the part
};

/**
 * Files associated with a single 8 MiB part.
 *
 * This structure enables efficient processing of each part during concurrent
 * downloads. The `entries` array is sorted by `offset_in_part` so the stream
 * processor can match local headers to file metadata as it traverses sequentially.
 *
 * The `continuing_file` pointer handles files that span part boundaries: if a
 * file's compressed data extends from a previous part into this one, the stream
 * processor needs to know which file it belongs to (for the output filename and
 * to track the uncompressed offset from the Start-of-Part metadata frame).
 */
struct part_files {
    struct part_file_entry *entries;   // Sorted by offset_in_part ascending
    size_t num_entries;
    struct file_metadata *continuing_file;  // File continuing from prev part, or NULL if none
};

/**
 * Complete result of parsing a ZIP central directory.
 *
 * Contains two complementary views of the archive's file list:
 *
 * 1. `files[]` - Sequential list of all files in central directory order.
 *    Use this for overall archive enumeration or when you have a file_index.
 *
 * 2. `parts[]` - Reverse mapping from part index to files. Use this when
 *    processing a specific 8 MiB part to know which files start there and
 *    whether a file continues from the previous part.
 *
 * Memory is allocated by `central_dir_parse()` and must be freed by calling
 * `central_dir_parse_result_free()`.
 */
struct central_dir_parse_result {
    /** File metadata array, indexed by file order in central directory. */
    struct file_metadata *files;
    size_t num_files;

    /**
     * Reverse mapping: part_index -> files in that part.
     * Array of `num_parts` elements, indexed by part_index (0 to num_parts-1).
     * Use `parts[part_idx]` to get information about a specific 8 MiB part.
     */
    struct part_files *parts;
    size_t num_parts;

    uint64_t central_dir_offset;       // Offset where central directory starts
    uint64_t central_dir_size;         // Size of central directory
    bool is_zip64;                     // Whether ZIP64 structures were detected
    int error_code;                    // Error code (0 = success)
    char error_message[256];           // Human-readable error message
};

/**
 * Parse only the EOCD structures to determine central directory location and size.
 * This is a lightweight operation that doesn't parse individual CD entries.
 * Used to detect if additional data needs to be fetched for large central directories.
 *
 * @param buffer              Buffer containing end of ZIP file (must include EOCD)
 * @param buffer_size         Size of buffer in bytes
 * @param archive_size        Total size of the archive
 * @param central_dir_offset  Output: offset where CD starts in archive
 * @param central_dir_size    Output: size of CD in bytes
 * @param num_entries         Output: number of entries in CD (can be NULL if not needed)
 * @param is_zip64            Output: whether this is a ZIP64 archive
 * @param first_cdfh_offset_in_tail  Output: offset from TAIL START to first complete CDFH
 *                            within the last 8 MiB of archive (can be NULL if not needed).
 *                            Value 0 = entire CD fits in tail (no partial CD scenario).
 *                            Value 0xFFFFFF = no complete CDFH in tail.
 *                            Other = byte offset from tail start (archive_size - 8 MiB) to
 *                            the first complete CDFH. Always < 8 MiB.
 *                            Only valid if BURST EOCD comment is present (otherwise 0).
 * @param error_msg           Output: error message buffer (must be at least 256 bytes)
 * @return CENTRAL_DIR_PARSE_SUCCESS on success, or error code on failure
 */
int central_dir_parse_eocd_only(const uint8_t *buffer, size_t buffer_size,
                                 uint64_t archive_size,
                                 uint64_t *central_dir_offset,
                                 uint64_t *central_dir_size,
                                 uint64_t *num_entries,
                                 bool *is_zip64,
                                 uint32_t *first_cdfh_offset_in_tail,
                                 char *error_msg);

/**
 * Parse the central directory from a buffer containing the end of a ZIP file.
 *
 * Convenience wrapper that calls central_dir_parse_eocd_only() then
 * central_dir_parse_from_cd_buffer(). Use this when the buffer contains
 * both the CD and EOCD (i.e., for archives where the CD fits in the tail buffer).
 *
 * @param buffer       Buffer containing end of ZIP file (must include EOCD and CD)
 * @param buffer_size  Size of buffer in bytes
 * @param archive_size Total size of the archive
 * @param part_size    Part size in bytes (must be multiple of 8 MiB)
 * @param result       Output structure to populate with parsed data
 * @return CENTRAL_DIR_PARSE_SUCCESS on success, or error code on failure
 */
int central_dir_parse(const uint8_t *buffer, size_t buffer_size,
                      uint64_t archive_size,
                      uint64_t part_size,
                      struct central_dir_parse_result *result);

/**
 * Parse central directory from a buffer containing the CD entries.
 *
 * The caller must first call central_dir_parse_eocd_only() to get the
 * cd_offset, cd_size, and is_zip64 values, then pass a buffer pointing
 * to the start of the CD data.
 *
 * @param cd_buffer        Buffer containing the central directory entries
 * @param cd_buffer_size   Size of the CD buffer in bytes
 * @param cd_offset        Offset where CD starts in archive (from EOCD)
 * @param cd_size          Size of CD in bytes (from EOCD)
 * @param archive_size     Total size of the archive
 * @param part_size        Part size in bytes (must be multiple of 8 MiB)
 * @param is_zip64         Whether this is a ZIP64 archive
 * @param result           Output structure to populate with parsed data
 * @return CENTRAL_DIR_PARSE_SUCCESS on success, or error code on failure
 */
int central_dir_parse_from_cd_buffer(const uint8_t *cd_buffer, size_t cd_buffer_size,
                                      uint64_t cd_offset, uint64_t cd_size,
                                      uint64_t archive_size, uint64_t part_size,
                                      bool is_zip64,
                                      struct central_dir_parse_result *result);

/**
 * Parse available CDFH entries from a partial central directory buffer.
 *
 * This function is used when the CD extends beyond the tail buffer and we want
 * to start processing files before the full CD is fetched. It parses whatever
 * CDFH entries are complete in the buffer, starting from the offset specified
 * by the BURST EOCD comment.
 *
 * @param buffer              Tail buffer (last 8 MiB of archive)
 * @param buffer_size         Size of tail buffer in bytes
 * @param buffer_start_offset Absolute offset of buffer start in archive (= archive_size - 8 MiB)
 * @param central_dir_offset  Offset where CD starts in archive (from EOCD)
 * @param first_cdfh_offset   Offset from TAIL START to first complete CDFH in buffer
 *                            (from BURST EOCD comment). This is relative to buffer_start_offset.
 * @param archive_size        Total archive size
 * @param part_size           Part size in bytes (must be multiple of 8 MiB)
 * @param is_zip64            Whether this is a ZIP64 archive
 * @param result              Output structure to populate with parsed data
 * @return CENTRAL_DIR_PARSE_SUCCESS on success, or error code on failure
 *
 * Note: Unlike central_dir_parse(), this function may return with fewer entries
 * than actually exist in the archive. The caller should expect num_files to be
 * smaller than the total archive file count.
 */
int central_dir_parse_partial(const uint8_t *buffer, size_t buffer_size,
                               uint64_t buffer_start_offset,
                               uint64_t central_dir_offset,
                               uint32_t first_cdfh_offset,
                               uint64_t archive_size,
                               uint64_t part_size,
                               bool is_zip64,
                               struct central_dir_parse_result *result);

/**
 * Free resources allocated by central_dir_parse().
 *
 * @param result Result structure to free (can be NULL)
 */
void central_dir_parse_result_free(struct central_dir_parse_result *result);

#endif // CENTRAL_DIR_PARSER_H
