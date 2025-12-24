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
// next_lfh_len: Size of next file's local header, or 0 if this is the last file
int burst_writer_add_file(struct burst_writer *writer,
                          FILE *input_file,
                          struct zip_local_header *lfh,
                          int lfh_len,
                          int next_lfh_len);
int burst_writer_finalize(struct burst_writer *writer);

// Internal functions
int burst_writer_flush(struct burst_writer *writer);
int burst_writer_write(struct burst_writer *writer, const void *data, size_t len);

void burst_writer_print_stats(const struct burst_writer *writer);

#endif // BURST_WRITER_H
