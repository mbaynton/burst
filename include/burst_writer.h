#ifndef BURST_WRITER_H
#define BURST_WRITER_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <zstd.h>

// Constants
#define BURST_PART_SIZE (8 * 1024 * 1024)  // 8 MiB
#define BURST_FRAME_SIZE (128 * 1024)       // 128 KiB (BTRFS maximum)
#define BURST_MIN_SKIPPABLE_FRAME_SIZE 8
#define BURST_MAGIC_NUMBER 0x184D2A5B       // "BURST" marker for skippable frames

// File entry metadata
struct file_entry {
    char *filename;
    uint64_t local_header_offset;
    uint64_t compressed_start_offset;
    uint64_t uncompressed_start_offset;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint32_t crc32;
    uint16_t compression_method;
    uint16_t version_needed;
    uint16_t general_purpose_flags;
    uint16_t last_mod_time;
    uint16_t last_mod_date;
    // Unix metadata for central directory
    uint32_t unix_mode;  // Full mode including file type (S_IFREG, S_IFLNK, etc.)
    uint32_t uid;
    uint32_t gid;
    // ZIP64 tracking
    bool used_zip64_descriptor;  // True if data descriptor used 64-bit sizes
};

// BURST writer context
struct burst_writer {
    FILE *output;
    uint64_t current_offset;
    ZSTD_CCtx *zstd_ctx;
    int compression_level;

    // File tracking
    struct file_entry *files;
    size_t num_files;
    size_t files_capacity;

    // Buffering
    uint8_t *write_buffer;
    size_t buffer_size;
    size_t buffer_used;

    // Statistics
    uint64_t total_uncompressed;
    uint64_t total_compressed;
    uint64_t padding_bytes;

    // Phase 3: Alignment tracking
    uint64_t current_uncompressed_offset;  // Track uncompressed position within current file
};

// Forward declaration
struct zip_local_header;

// Writer API
struct burst_writer* burst_writer_create(FILE *output, int compression_level);
void burst_writer_destroy(struct burst_writer *writer);

// Add a file to the archive
// input_file: Open file handle to read from (caller must close)
// lfh: Fully-constructed local file header (caller allocates)
// lfh_len: Total size of local file header including filename and extra fields
// is_header_only: True for empty files or symlinks (no compressed data frames)
//                 When true, ensures proper alignment by inserting padding LFH if needed
// unix_mode: Unix file mode (permissions + file type) for central directory
// uid: Unix user ID for extra field
// gid: Unix group ID for extra field
int burst_writer_add_file(struct burst_writer *writer,
                          FILE *input_file,
                          struct zip_local_header *lfh,
                          int lfh_len,
                          bool is_header_only,
                          uint32_t unix_mode,
                          uint32_t uid,
                          uint32_t gid);

// Add a symlink to the archive
// lfh: Fully-constructed local file header (with STORE method, CRC32 and sizes pre-filled)
//      The LFH flags should NOT have bit 3 set (no data descriptor)
// lfh_len: Total size of local file header including filename and extra fields
// target: Symlink target path (NOT null-terminated in archive)
// target_len: Length of target path
// unix_mode: Unix mode (should have S_IFLNK set, e.g., 0120755)
// uid: Unix user ID for extra field
// gid: Unix group ID for extra field
int burst_writer_add_symlink(struct burst_writer *writer,
                              struct zip_local_header *lfh,
                              int lfh_len,
                              const char *target,
                              size_t target_len,
                              uint32_t unix_mode,
                              uint32_t uid,
                              uint32_t gid);

// Add a directory to the archive
// lfh: Fully-constructed local file header (with STORE method, zero sizes, trailing slash)
//      The LFH flags should NOT have bit 3 set (no data descriptor)
// lfh_len: Total size of local file header including filename and extra fields
// unix_mode: Unix mode (MUST have S_IFDIR set, e.g., 040755)
// uid: Unix user ID for extra field
// gid: Unix group ID for extra field
int burst_writer_add_directory(struct burst_writer *writer,
                                struct zip_local_header *lfh,
                                int lfh_len,
                                uint32_t unix_mode,
                                uint32_t uid,
                                uint32_t gid);

int burst_writer_finalize(struct burst_writer *writer);

// Internal functions
int burst_writer_flush(struct burst_writer *writer);
int burst_writer_write(struct burst_writer *writer, const void *data, size_t len);

void burst_writer_print_stats(const struct burst_writer *writer);

#endif // BURST_WRITER_H
