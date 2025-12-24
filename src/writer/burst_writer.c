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
    writer->current_part = 0;
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

int burst_writer_add_file(struct burst_writer *writer,
                          FILE *input_file,
                          struct zip_local_header *lfh,
                          int lfh_len,
                          int next_lfh_len) {
    if (!writer || !input_file || !lfh || lfh_len <= 0) {
        return -1;
    }

    // Get file size
    if (fseek(input_file, 0, SEEK_END) != 0) {
        fprintf(stderr, "Failed to seek input file: %s\n", strerror(errno));
        return -1;
    }
    long file_size = ftell(input_file);
    if (file_size < 0) {
        fprintf(stderr, "Failed to get file size: %s\n", strerror(errno));
        return -1;
    }
    rewind(input_file);

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

    entry->local_header_offset = writer->current_offset + writer->buffer_used;

    // Copy header fields from caller-provided local file header
    entry->compression_method = lfh->compression_method;
    entry->version_needed = lfh->version_needed;
    entry->general_purpose_flags = lfh->flags;
    entry->last_mod_time = lfh->last_mod_time;
    entry->last_mod_date = lfh->last_mod_date;

    // Write the pre-constructed local file header directly
    if (burst_writer_write(writer, lfh, lfh_len) != 0) {
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
    bool needs_descriptor_alignment = false;

    // Handle empty files: write a valid empty Zstandard frame
    // This is required for 7-Zip compatibility with Zstandard method
    if (file_size == 0) {
        // Valid empty Zstandard frame: 13 bytes
        // Magic (4) + frame header (5) + checksum (4)
        const uint8_t empty_zstd_frame[] = {
            0x28, 0xB5, 0x2F, 0xFD,  // Magic number
            0x24, 0x00, 0x01, 0x00, 0x00,  // Frame header (FCS=0, single segment, no checksum flag but checksum present)
            0x99, 0xE9, 0xD8, 0x51   // XXH64 checksum of empty data
        };

        if (burst_writer_write(writer, empty_zstd_frame, sizeof(empty_zstd_frame)) != 0) {
            free(input_buffer);
            free(output_buffer);
            free(entry->filename);
            return -1;
        }
    }

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
            at_eof,
            false  // Not a new file (we're mid-file)
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
            writer->padding_bytes += decision.padding_size + 8;
        } else if (decision.action == ALIGNMENT_PAD_THEN_METADATA) {
            // Write padding, then Start-of-Part metadata
            if (alignment_write_padding_frame(writer, decision.padding_size) != 0) {
                free(input_buffer);
                free(output_buffer);
                free(entry->filename);
                return -1;
            }
            writer->padding_bytes += decision.padding_size + 8;

            // Write Start-of-Part frame with current uncompressed offset
            if (alignment_write_start_of_part_frame(writer, total_uncompressed - bytes_read) != 0) {
                free(input_buffer);
                free(output_buffer);
                free(entry->filename);
                return -1;
            }
            writer->padding_bytes += 24;
        }

        // If data descriptor won't fit before boundary, mark for later handling
        if (at_eof && decision.descriptor_after_boundary) {
            needs_descriptor_alignment = true;
        }

        // Write compressed frame
        if (burst_writer_write(writer, output_buffer, comp_result.compressed_size) != 0) {
            free(input_buffer);
            free(output_buffer);
            free(entry->filename);
            return -1;
        }
    }

    free(input_buffer);
    free(output_buffer);

    // Phase 3: Handle data descriptor alignment for both empty and non-empty files
    bool needs_alignment_for_descriptor = false;

    if (total_uncompressed == 0) {
        // Empty file - check if 16-byte descriptor needs alignment
        // Empty files skip the compression loop, so we check here
        uint64_t write_pos = alignment_get_write_position(writer);
        uint64_t next_boundary = alignment_next_boundary(write_pos);
        uint64_t space_until_boundary = next_boundary - write_pos;
        const size_t descriptor_size = 16;  // sizeof(struct zip_data_descriptor)

        // Need padding if descriptor + min skippable frame won't fit
        if (space_until_boundary < descriptor_size + BURST_MIN_SKIPPABLE_FRAME_SIZE) {
            needs_alignment_for_descriptor = true;
        }
    } else if (needs_descriptor_alignment) {
        // Non-empty file with descriptor_after_boundary flag set in compression loop
        needs_alignment_for_descriptor = true;
    }

    // Write alignment padding if needed
    if (needs_alignment_for_descriptor) {
        uint64_t write_pos = alignment_get_write_position(writer);
        uint64_t space_until_boundary = alignment_next_boundary(write_pos) - write_pos;

        // Pad to boundary
        size_t padding_size = (size_t)(space_until_boundary - 8);
        if (alignment_write_padding_frame(writer, padding_size) != 0) {
            free(entry->filename);
            return -1;
        }
        writer->padding_bytes += padding_size + 8;

        // Write Start-of-Part frame at boundary
        // For empty files, uncompressed_offset is 0
        if (alignment_write_start_of_part_frame(writer, total_uncompressed) != 0) {
            free(entry->filename);
            return -1;
        }
        writer->padding_bytes += 24;
    }

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
    // Note: Phase 3 requires Zstandard for alignment; STORE method not supported
    if (total_compressed >= total_uncompressed) {
        printf("Warning: Compressed size (%lu) >= uncompressed (%lu) for %s\n",
               (unsigned long)total_compressed, (unsigned long)total_uncompressed, filename);
    }

    // Store sizes and CRC
    entry->compressed_size = total_compressed;
    entry->uncompressed_size = total_uncompressed;
    entry->crc32 = crc;

    // Phase 3: Check if next file's local header would fit before boundary
    // We must check BEFORE writing the data descriptor, because we cannot
    // insert Zstandard skippable frames between the descriptor and next local header
    // (that space is outside any ZIP file entry).
    if (next_lfh_len > 0) {  // Skip check if this is the last file
        uint64_t write_pos = alignment_get_write_position(writer);
        uint64_t next_boundary = alignment_next_boundary(write_pos);
        uint64_t space_until_boundary = next_boundary - write_pos;

        // Space needed: data descriptor (16 bytes) + next local header (actual size)
        const size_t descriptor_size = 16;  // sizeof(struct zip_data_descriptor)
        size_t space_needed = descriptor_size + next_lfh_len;

        // If insufficient space, pad current file to boundary so descriptor + next header
        // will be at/after boundary
        if (space_until_boundary < space_needed + BURST_MIN_SKIPPABLE_FRAME_SIZE) {
            // Not enough space - pad to boundary within current file's compressed data
            size_t padding_size = (size_t)(space_until_boundary - 8);  // Exclude frame header

            if (alignment_write_padding_frame(writer, padding_size) != 0) {
                free(entry->filename);
                return -1;
            }
            writer->padding_bytes += padding_size + 8;

            // Write Start-of-Part frame at boundary indicating where data descriptor
            // and next file will begin. Use current file's final uncompressed offset.
            if (alignment_write_start_of_part_frame(writer, total_uncompressed) != 0) {
                free(entry->filename);
                return -1;
            }
            writer->padding_bytes += 24;  // Start-of-Part frame size
        }
    }

    // Write data descriptor (now properly aligned)
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
    printf("  Number of 8 MiB parts: %u\n", writer->current_part + 1);
}
