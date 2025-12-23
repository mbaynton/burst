#include "burst_writer.h"
#include "zip_structures.h"
#include "compression.h"
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

int burst_writer_add_file(struct burst_writer *writer, const char *filename, const char *input_path) {
    if (!writer || !filename || !input_path) {
        return -1;
    }

    // Open input file
    FILE *input = fopen(input_path, "rb");
    if (!input) {
        fprintf(stderr, "Failed to open input file '%s': %s\n", input_path, strerror(errno));
        return -1;
    }

    // Get file size
    if (fseek(input, 0, SEEK_END) != 0) {
        fprintf(stderr, "Failed to seek input file: %s\n", strerror(errno));
        fclose(input);
        return -1;
    }
    long file_size = ftell(input);
    if (file_size < 0) {
        fprintf(stderr, "Failed to get file size: %s\n", strerror(errno));
        fclose(input);
        return -1;
    }
    rewind(input);

    // Expand file tracking array if needed
    if (writer->num_files >= writer->files_capacity) {
        size_t new_capacity = writer->files_capacity * 2;
        struct file_entry *new_files = realloc(writer->files, new_capacity * sizeof(struct file_entry));
        if (!new_files) {
            fprintf(stderr, "Failed to expand file tracking array\n");
            fclose(input);
            return -1;
        }
        writer->files = new_files;
        writer->files_capacity = new_capacity;
    }

    // Initialize file entry
    struct file_entry *entry = &writer->files[writer->num_files];
    memset(entry, 0, sizeof(struct file_entry));

    entry->filename = strdup(filename);
    if (!entry->filename) {
        fclose(input);
        return -1;
    }

    entry->local_header_offset = writer->current_offset + writer->buffer_used;

    // Determine compression method based on compression level
    // Level 0 = STORE (uncompressed), otherwise use Zstandard
    if (writer->compression_level == 0 || file_size == 0) {
        entry->compression_method = ZIP_METHOD_STORE;
        entry->version_needed = ZIP_VERSION_STORE;
    } else {
        entry->compression_method = ZIP_METHOD_ZSTD;
        entry->version_needed = ZIP_VERSION_ZSTD;
    }
    entry->general_purpose_flags = ZIP_FLAG_DATA_DESCRIPTOR;

    // Get current time for timestamp
    time_t now = time(NULL);
    dos_datetime_from_time_t(now, &entry->last_mod_time, &entry->last_mod_date);

    // Write local file header
    if (write_local_header(writer, filename, entry->compression_method, entry->general_purpose_flags,
                          entry->last_mod_time, entry->last_mod_date) != 0) {
        free(entry->filename);
        fclose(input);
        return -1;
    }

    entry->compressed_start_offset = writer->current_offset + writer->buffer_used;
    entry->uncompressed_start_offset = 0;  // Will be set by alignment logic in Phase 3

    // Read and compress file data
    uint32_t crc = 0;
    uint64_t total_uncompressed = 0;
    uint64_t total_compressed = 0;

    if (entry->compression_method == ZIP_METHOD_STORE) {
        // STORE method: copy data uncompressed
        uint8_t buffer[8192];
        size_t bytes_read;

        while ((bytes_read = fread(buffer, 1, sizeof(buffer), input)) > 0) {
            crc = crc32(crc, buffer, bytes_read);
            if (burst_writer_write(writer, buffer, bytes_read) != 0) {
                free(entry->filename);
                fclose(input);
                return -1;
            }
            total_uncompressed += bytes_read;
            total_compressed += bytes_read;
        }
    } else {
        // ZSTD method: compress in 128 KiB chunks
        #define ZSTD_CHUNK_SIZE (128 * 1024)  // 128 KiB chunks (BTRFS maximum)
        uint8_t *input_buffer = malloc(ZSTD_CHUNK_SIZE);
        uint8_t *output_buffer = malloc(ZSTD_compressBound(ZSTD_CHUNK_SIZE));

        if (!input_buffer || !output_buffer) {
            fprintf(stderr, "Failed to allocate compression buffers\n");
            free(input_buffer);
            free(output_buffer);
            free(entry->filename);
            fclose(input);
            return -1;
        }

        size_t bytes_read;
        while ((bytes_read = fread(input_buffer, 1, ZSTD_CHUNK_SIZE, input)) > 0) {
            // Compute CRC32 of uncompressed data
            crc = crc32(crc, input_buffer, bytes_read);
            total_uncompressed += bytes_read;

            // Compress chunk using new mockable API
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
                fclose(input);
                return -1;
            }

            // Verify frame header contains content size (debug builds only)
#ifdef DEBUG
            if (verify_frame_content_size(output_buffer, comp_result.compressed_size,
                                           bytes_read) != 0) {
                free(input_buffer);
                free(output_buffer);
                free(entry->filename);
                fclose(input);
                return -1;
            }
#endif

            // Write compressed frame
            if (burst_writer_write(writer, output_buffer, comp_result.compressed_size) != 0) {
                free(input_buffer);
                free(output_buffer);
                free(entry->filename);
                fclose(input);
                return -1;
            }

            total_compressed += comp_result.compressed_size;
        }

        free(input_buffer);
        free(output_buffer);
    }

    if (ferror(input)) {
        fprintf(stderr, "Error reading input file: %s\n", strerror(errno));
        free(entry->filename);
        fclose(input);
        return -1;
    }

    fclose(input);

    // Check if compression was worthwhile
    if (entry->compression_method == ZIP_METHOD_ZSTD && total_compressed >= total_uncompressed) {
        // Compressed size is larger - should have used STORE
        // For Phase 2, we'll just accept this. Phase 3 will add proper fallback logic.
        printf("Warning: Compressed size (%lu) >= uncompressed (%lu) for %s\n",
               (unsigned long)total_compressed, (unsigned long)total_uncompressed, filename);
    }

    // Store sizes and CRC
    entry->compressed_size = total_compressed;
    entry->uncompressed_size = total_uncompressed;
    entry->crc32 = crc;

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
