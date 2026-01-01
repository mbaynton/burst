#include "burst_writer.h"
#include "zip_structures.h"
#include "compression.h"
#include "alignment.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define INITIAL_FILES_CAPACITY 16
#define WRITE_BUFFER_SIZE (64 * 1024)  // 64 KiB write buffer

struct burst_writer* burst_writer_create(FILE *output, int compression_level) {
    if (!output) {
        return NULL;
    }

    struct burst_writer *writer = calloc(1, sizeof(struct burst_writer));
    if (!writer) {
        return NULL;
    }

    writer->output = output;
    writer->current_offset = 0;
    writer->compression_level = compression_level;

    // Allocate file tracking array
    writer->files_capacity = INITIAL_FILES_CAPACITY;
    writer->files = calloc(writer->files_capacity, sizeof(struct file_entry));
    if (!writer->files) {
        free(writer);
        return NULL;
    }

    // Allocate write buffer
    writer->buffer_size = WRITE_BUFFER_SIZE;
    writer->write_buffer = malloc(writer->buffer_size);
    if (!writer->write_buffer) {
        free(writer->files);
        free(writer);
        return NULL;
    }

    // Create Zstandard compression context (will be used in Phase 2)
    writer->zstd_ctx = ZSTD_createCCtx();
    if (!writer->zstd_ctx) {
        free(writer->write_buffer);
        free(writer->files);
        free(writer);
        return NULL;
    }

    return writer;
}

void burst_writer_destroy(struct burst_writer *writer) {
    if (!writer) {
        return;
    }

    // Free file entries
    if (writer->files) {
        for (size_t i = 0; i < writer->num_files; i++) {
            free(writer->files[i].filename);
        }
        free(writer->files);
    }

    // Free write buffer
    free(writer->write_buffer);

    // Free Zstandard context
    if (writer->zstd_ctx) {
        ZSTD_freeCCtx(writer->zstd_ctx);
    }

    free(writer);
}

int burst_writer_write(struct burst_writer *writer, const void *data, size_t len) {
    if (!writer || !data) {
        return -1;
    }

    const uint8_t *src = data;
    size_t remaining = len;

    while (remaining > 0) {
        size_t available = writer->buffer_size - writer->buffer_used;
        size_t to_copy = (remaining < available) ? remaining : available;

        memcpy(writer->write_buffer + writer->buffer_used, src, to_copy);
        writer->buffer_used += to_copy;
        src += to_copy;
        remaining -= to_copy;

        // Flush if buffer is full
        if (writer->buffer_used == writer->buffer_size) {
            if (burst_writer_flush(writer) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int burst_writer_flush(struct burst_writer *writer) {
    if (!writer || writer->buffer_used == 0) {
        return 0;
    }

    size_t written = fwrite(writer->write_buffer, 1, writer->buffer_used, writer->output);
    if (written != writer->buffer_used) {
        fprintf(stderr, "Failed to write to output: %s\n", strerror(errno));
        return -1;
    }

    writer->current_offset += writer->buffer_used;
    writer->buffer_used = 0;

    return 0;
}

/*
burst_writer_add_file adds a file to the BURST archive.
It may write a number of structures to the output in the process:
- Local File Header (from caller)
- Zstandard frames representing some compressed file data
- Zip Data Descriptor
- Zstandard skippable padding frames
- Zstandard Start-of-Part metadata frames
- Unlisted padding Local File Header (for alignment of header-only files)

It is responsible for ensuring that it does not write any type of data
other than a Start-of-Part frame or a Local File Header at an 8MiB part boundary,
and for ensuring that sufficient free space to the next boundary exists for a minimal
local file header.
*/
int burst_writer_add_file(struct burst_writer *writer,
                          FILE *input_file,
                          struct zip_local_header *lfh,
                          int lfh_len,
                          bool is_header_only,
                          uint32_t unix_mode,
                          uint32_t uid,
                          uint32_t gid) {
    if (!writer || !input_file || !lfh || lfh_len <= 0) {
        return -1;
    }

    // Get file size
    long file_size = 0;
    if (!is_header_only) {
        if (fseek(input_file, 0, SEEK_END) != 0) {
            fprintf(stderr, "Failed to seek input file: %s\n", strerror(errno));
            return -1;
        }
        if (file_size < 0) {
            fprintf(stderr, "Failed to get file size: %s\n", strerror(errno));
            return -1;
        }
        rewind(input_file);
    }

    // Expand file tracking array if needed
    if (writer->num_files >= writer->files_capacity) {
        size_t new_capacity = writer->files_capacity * 2;
        struct file_entry *new_files = realloc(writer->files, new_capacity * sizeof(struct file_entry));
        if (!new_files) {
            fprintf(stderr, "Failed to expand file tracking array\n");
            return -1;
        }
        writer->files = new_files;
        writer->files_capacity = new_capacity;
    }

    // Initialize file entry
    struct file_entry *entry = &writer->files[writer->num_files];
    memset(entry, 0, sizeof(struct file_entry));

    // Extract filename from local file header for central directory entry
    const char *filename = (const char *)((uint8_t *)lfh + sizeof(struct zip_local_header));
    entry->filename = strndup(filename, lfh->filename_length);
    if (!entry->filename) {
        return -1;
    }

    // Check if we need to insert a padding LFH before writing this file's LFH
    // to ensure proper alignment. This ensures we always have room for the current
    // file's LFH + data descriptor + at least a minimum padding LFH for the next file.
    {
        uint64_t write_pos = alignment_get_write_position(writer);
        uint64_t next_boundary = alignment_next_boundary(write_pos);
        uint64_t space_until_boundary = next_boundary - write_pos;

        // Space needed: current LFH + data descriptor + minimum padding LFH
        // The data descriptor is 16 bytes (even for STORE method, for consistency)
        // It's possible we could further optimize this in future, for example by computing
        // space needed more precisely based on the data that will follow this file's LFH and
        // padding options for the data type following this file's LFH.
        const size_t data_descriptor_size = sizeof(struct zip_data_descriptor);
        size_t space_needed = (size_t)lfh_len + data_descriptor_size + PADDING_LFH_MIN_SIZE;

        if (space_until_boundary < space_needed) {
            // Not enough space - write a padding LFH to advance to boundary
            if (write_padding_lfh(writer, (size_t)space_until_boundary) != 0) {
                free(entry->filename);
                return -1;
            }
        }
    }

    entry->local_header_offset = writer->current_offset + writer->buffer_used;

    // Copy header fields from caller-provided local file header
    entry->compression_method = lfh->compression_method;
    entry->version_needed = lfh->version_needed;
    entry->general_purpose_flags = lfh->flags;
    entry->last_mod_time = lfh->last_mod_time;
    entry->last_mod_date = lfh->last_mod_date;

    // Store Unix metadata for central directory
    entry->unix_mode = unix_mode;
    entry->uid = uid;
    entry->gid = gid;

    // Write the pre-constructed local file header directly
    if (burst_writer_write(writer, lfh, lfh_len) < 0) {
        free(entry->filename);
        return -1;
    }

    entry->compressed_start_offset = writer->current_offset + writer->buffer_used;
    entry->uncompressed_start_offset = 0;  // Will be set by alignment logic in Phase 3

    // Read and compress file data
    uint32_t crc = 0;
    uint64_t total_uncompressed = 0;

    // ZSTD method: compress in 128 KiB chunks
    #define ZSTD_CHUNK_SIZE (128 * 1024)  // 128 KiB chunks (BTRFS maximum)
    uint8_t *input_buffer = malloc(ZSTD_CHUNK_SIZE);
    uint8_t *output_buffer = malloc(ZSTD_compressBound(ZSTD_CHUNK_SIZE));

    if (!input_buffer || !output_buffer) {
        fprintf(stderr, "Failed to allocate compression buffers\n");
        free(input_buffer);
        free(output_buffer);
        free(entry->filename);
        return -1;
    }

    size_t bytes_read;

    // Handle header-only files (empty files with STORE method, symlinks)
    // These have no data to compress, so skip directly to the data descriptor
    if (is_header_only) {
        // For STORE method empty files: no data bytes at all
        // The CRC is 0, sizes are 0, and we just write the data descriptor
        free(input_buffer);
        free(output_buffer);
        goto write_descriptor;
    }

    // Otherwise this is a regular and non-empty file, so start writing compressed zstandard frames.

    while ((bytes_read = fread(input_buffer, 1, ZSTD_CHUNK_SIZE, input_file)) > 0) {
        // Compute CRC32 of uncompressed data
        crc = crc32(crc, input_buffer, bytes_read);
        total_uncompressed += bytes_read;

        // Compress chunk using mockable API
        struct compression_result comp_result = compress_chunk(
            output_buffer, ZSTD_compressBound(ZSTD_CHUNK_SIZE),
            input_buffer, bytes_read,
            writer->compression_level);

        if (comp_result.error) {
            fprintf(stderr, "Zstandard compression error: %s\n",
                    comp_result.error_message);
            free(input_buffer);
            free(output_buffer);
            free(entry->filename);
            return -1;
        }

        // Verify frame header contains content size (debug builds only)
#ifdef DEBUG
        if (verify_frame_content_size(output_buffer, comp_result.compressed_size,
                                       bytes_read) != 0) {
            free(input_buffer);
            free(output_buffer);
            free(entry->filename);
            return -1;
        }
#endif

        // Phase 3: Check alignment before writing frame
        uint64_t write_pos = alignment_get_write_position(writer);
        bool at_eof = (bytes_read < ZSTD_CHUNK_SIZE) || feof(input_file);

        struct alignment_decision decision = alignment_decide(
            write_pos,
            comp_result.compressed_size,
            at_eof
        );

        // Execute alignment actions
        if (decision.action == ALIGNMENT_PAD_THEN_FRAME) {
            // Write padding to reach boundary
            if (alignment_write_padding_frame(writer, decision.padding_size) != 0) {
                free(input_buffer);
                free(output_buffer);
                free(entry->filename);
                return -1;
            }
        } else if (decision.action == ALIGNMENT_PAD_THEN_METADATA) {
            // Write padding, then Start-of-Part metadata
            if (alignment_write_padding_frame(writer, decision.padding_size) != 0) {
                free(input_buffer);
                free(output_buffer);
                free(entry->filename);
                return -1;
            }

            // Write Start-of-Part frame with current uncompressed offset
            if (alignment_write_start_of_part_frame(writer, total_uncompressed - bytes_read) != 0) {
                free(input_buffer);
                free(output_buffer);
                free(entry->filename);
                return -1;
            }
        }

        // Write compressed frame
        if (burst_writer_write(writer, output_buffer, comp_result.compressed_size) < 0) {
            free(input_buffer);
            free(output_buffer);
            free(entry->filename);
            return -1;
        }

        // Handle exact-fit mid-file case: write Start-of-Part at boundary
        if (decision.action == ALIGNMENT_WRITE_FRAME_THEN_METADATA) {
            if (alignment_write_start_of_part_frame(writer, total_uncompressed) != 0) {
                free(input_buffer);
                free(output_buffer);
                free(entry->filename);
                return -1;
            }
        }
    } // file fully written out. Caller responsible for closing.

    free(input_buffer);
    free(output_buffer);

    if (ferror(input_file)) {
        fprintf(stderr, "Error reading input file: %s\n", strerror(errno));
        free(entry->filename);
        return -1;
    }

    // Calculate actual compressed size (includes padding and metadata frames)
    // This is the total bytes written between local header and data descriptor
    uint64_t current_pos = writer->current_offset + writer->buffer_used;
    uint64_t total_compressed = current_pos - entry->compressed_start_offset;

    // Check if compression achieved size reduction
    // Note: We always require Zstandard for alignment; STORE method not supported
    if (total_compressed >= total_uncompressed) {
        printf("Warning: Compressed size (%lu) >= uncompressed (%lu) for %s\n",
               (unsigned long)total_compressed, (unsigned long)total_uncompressed, filename);
    }

    // Store sizes and CRC
    entry->compressed_size = total_compressed;
    entry->uncompressed_size = total_uncompressed;
    entry->crc32 = crc;

    // Check if minimum padding LFH would fit after data descriptor before boundary.
    // We must check BEFORE writing the data descriptor, because we cannot
    // insert Zstandard skippable frames between the descriptor and next local header
    // (that space is outside any ZIP file entry).
    // Note: This check is skipped for header-only files as they have no compressed data
    // where we could insert skippable frames.
    uint64_t write_pos = alignment_get_write_position(writer);
    uint64_t next_boundary = alignment_next_boundary(write_pos);
    uint64_t space_until_boundary = next_boundary - write_pos;

    // Space needed: data descriptor (16 bytes) + minimum padding LFH (44 bytes)
    // The next file's entry check will insert a padding LFH if its actual header is larger
    const size_t descriptor_size = sizeof(struct zip_data_descriptor);
    size_t space_needed = descriptor_size + PADDING_LFH_MIN_SIZE;

    // If insufficient space, pad current file to boundary so descriptor + next header
    // will be at/after boundary
    if (space_until_boundary < space_needed + BURST_MIN_SKIPPABLE_FRAME_SIZE) {
        // Not enough space - pad to boundary within current file's compressed data
        size_t padding_size = (size_t)(space_until_boundary - 8);  // Exclude frame header

        if (alignment_write_padding_frame(writer, padding_size) != 0) {
            free(entry->filename);
            return -1;
        }

        // Write Start-of-Part frame at boundary indicating where data descriptor
        // and next file will begin. Use current file's final uncompressed offset.
        if (alignment_write_start_of_part_frame(writer, total_uncompressed) != 0) {
            free(entry->filename);
            return -1;
        }
    }

write_descriptor:
    // Write data descriptor
    if (write_data_descriptor(writer, entry->crc32, entry->compressed_size, entry->uncompressed_size) != 0) {
        free(entry->filename);
        return -1;
    }

    // Update statistics
    writer->total_uncompressed += entry->uncompressed_size;
    writer->total_compressed += entry->compressed_size;
    writer->num_files++;

    printf("Added file: %s (%lu bytes)\n", filename, (unsigned long)entry->uncompressed_size);

    return 0;
}

/*
burst_writer_add_symlink adds a symbolic link to the BURST archive.
Unlike burst_writer_add_file, symlinks:
- Use STORE method (no compression)
- Have known content size upfront (target path length)
- Store CRC32 and sizes directly in LFH (no data descriptor, bit 3 NOT set)
- Content is the symlink target path (no null terminator in archive)

Alignment: Since we know the exact size upfront, we check if:
  lfh_len + target_len + PADDING_LFH_MIN_SIZE fits before boundary.
If not, we write a padding LFH to advance to boundary first.
*/
int burst_writer_add_symlink(struct burst_writer *writer,
                              struct zip_local_header *lfh,
                              int lfh_len,
                              const char *target,
                              size_t target_len,
                              uint32_t unix_mode,
                              uint32_t uid,
                              uint32_t gid) {
    if (!writer || !lfh || lfh_len <= 0 || !target || target_len == 0) {
        return -1;
    }

    // Expand file tracking array if needed
    if (writer->num_files >= writer->files_capacity) {
        size_t new_capacity = writer->files_capacity * 2;
        struct file_entry *new_files = realloc(writer->files, new_capacity * sizeof(struct file_entry));
        if (!new_files) {
            fprintf(stderr, "Failed to expand file tracking array\n");
            return -1;
        }
        writer->files = new_files;
        writer->files_capacity = new_capacity;
    }

    // Initialize file entry
    struct file_entry *entry = &writer->files[writer->num_files];
    memset(entry, 0, sizeof(struct file_entry));

    // Extract filename from local file header
    const char *filename = (const char *)((uint8_t *)lfh + sizeof(struct zip_local_header));
    entry->filename = strndup(filename, lfh->filename_length);
    if (!entry->filename) {
        return -1;
    }

    // Check alignment: lfh + target + minimum padding LFH for next file
    // (No data descriptor for symlinks - sizes known upfront)
    {
        uint64_t write_pos = alignment_get_write_position(writer);
        uint64_t next_boundary = alignment_next_boundary(write_pos);
        uint64_t space_until_boundary = next_boundary - write_pos;

        size_t space_needed = (size_t)lfh_len + target_len + PADDING_LFH_MIN_SIZE;

        if (space_until_boundary < space_needed) {
            // Not enough space - write a padding LFH to advance to boundary
            if (write_padding_lfh(writer, (size_t)space_until_boundary) != 0) {
                free(entry->filename);
                return -1;
            }
        }
    }

    entry->local_header_offset = writer->current_offset + writer->buffer_used;

    // Copy header fields from caller-provided local file header
    entry->compression_method = lfh->compression_method;
    entry->version_needed = lfh->version_needed;
    entry->general_purpose_flags = lfh->flags;
    entry->last_mod_time = lfh->last_mod_time;
    entry->last_mod_date = lfh->last_mod_date;

    // Symlinks have content, so CRC and sizes are in LFH (pre-computed by caller)
    entry->crc32 = lfh->crc32;
    entry->compressed_size = target_len;  // STORE method: compressed = uncompressed
    entry->uncompressed_size = target_len;

    // Store Unix metadata for central directory
    entry->unix_mode = unix_mode;
    entry->uid = uid;
    entry->gid = gid;

    // Write the pre-constructed local file header
    if (burst_writer_write(writer, lfh, lfh_len) < 0) {
        free(entry->filename);
        return -1;
    }

    // Write symlink target as file content (no compression)
    if (burst_writer_write(writer, target, target_len) < 0) {
        free(entry->filename);
        return -1;
    }

    // No data descriptor for symlinks - sizes are in the LFH

    // Update statistics
    writer->total_uncompressed += entry->uncompressed_size;
    writer->total_compressed += entry->compressed_size;
    writer->num_files++;

    printf("Added symlink: %s -> %.*s\n", entry->filename, (int)target_len, target);

    return 0;
}

int burst_writer_finalize(struct burst_writer *writer) {
    if (!writer) {
        return -1;
    }

    // Flush any remaining buffered data
    if (burst_writer_flush(writer) != 0) {
        return -1;
    }

    // Write central directory
    uint64_t central_dir_start = writer->current_offset;
    if (write_central_directory(writer) != 0) {
        return -1;
    }

    // Write end of central directory record
    if (write_end_central_directory(writer, central_dir_start) != 0) {
        return -1;
    }

    // Final flush
    if (burst_writer_flush(writer) != 0) {
        return -1;
    }

    return 0;
}

void burst_writer_print_stats(const struct burst_writer *writer) {
    if (!writer) {
        return;
    }

    printf("\nBURST Archive Statistics:\n");
    printf("  Files: %zu\n", writer->num_files);
    printf("  Total uncompressed: %lu bytes\n", (unsigned long)writer->total_uncompressed);
    printf("  Total compressed: %lu bytes\n", (unsigned long)writer->total_compressed);
    if (writer->total_uncompressed > 0) {
        double ratio = 100.0 * writer->total_compressed / writer->total_uncompressed;
        printf("  Compression ratio: %.1f%%\n", ratio);
    }
    printf("  Padding bytes: %lu\n", (unsigned long)writer->padding_bytes);
    printf("  Final size: %lu bytes\n", (unsigned long)writer->current_offset);
}
