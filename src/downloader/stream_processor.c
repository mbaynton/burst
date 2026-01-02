#include "stream_processor.h"
#include "frame_parser.h"
#include "btrfs_writer.h"
#include "central_dir_parser.h"
#include "zip_structures.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>

// Initial frame buffer capacity (will grow as needed)
#define INITIAL_FRAME_BUFFER_CAPACITY (256 * 1024)

// BURST archives have Start-of-Part frames at 8 MiB boundaries
#define BURST_BASE_ALIGNMENT (8 * 1024 * 1024)

// Forward declarations for internal functions
static int handle_start_of_part_frame(struct part_processor_state *state,
                                      const uint8_t *frame_data, size_t frame_size);
static int handle_local_header(struct part_processor_state *state,
                               const uint8_t *header_data, size_t available_len,
                               size_t *bytes_consumed);
static int handle_zstd_frame(struct part_processor_state *state,
                             const uint8_t *frame_data, size_t compressed_size,
                             uint64_t uncompressed_size);
static int handle_data_descriptor(struct part_processor_state *state,
                                  const uint8_t *descriptor_data);
static int open_output_file(struct part_processor_state *state,
                            struct file_metadata *file_meta);
static int close_output_file(struct part_processor_state *state);
static int ensure_directory_exists(const char *path);


struct part_processor_state *part_processor_create(
    uint32_t part_index,
    struct central_dir_parse_result *cd_result,
    const char *output_dir,
    uint64_t part_size)
{
    if (cd_result == NULL || output_dir == NULL) {
        return NULL;
    }

    if (part_index >= cd_result->num_parts) {
        return NULL;
    }

    struct part_processor_state *state = calloc(1, sizeof(struct part_processor_state));
    if (state == NULL) {
        return NULL;
    }

    state->part_index = part_index;
    state->part_size = part_size;
    state->part_start_offset = (uint64_t)part_index * part_size;
    state->cd_result = cd_result;
    state->output_dir = output_dir;

    // Allocate frame buffer
    state->frame_buffer = malloc(INITIAL_FRAME_BUFFER_CAPACITY);
    if (state->frame_buffer == NULL) {
        free(state);
        return NULL;
    }
    state->frame_buffer_capacity = INITIAL_FRAME_BUFFER_CAPACITY;
    state->frame_buffer_used = 0;

    state->next_entry_idx = 0;
    state->bytes_processed = 0;
    state->state = STATE_INIT;
    state->current_file = NULL;
    state->error_code = 0;
    state->error_message[0] = '\0';

    return state;
}

void part_processor_destroy(struct part_processor_state *state)
{
    if (state == NULL) {
        return;
    }

    // Close any open file
    if (state->current_file != NULL) {
        if (state->current_file->fd >= 0) {
            close(state->current_file->fd);
        }
        free(state->current_file->symlink_buffer);
        free(state->current_file->filename);
        free(state->current_file);
    }

    free(state->frame_buffer);
    free(state);
}

const char *part_processor_get_error(const struct part_processor_state *state)
{
    if (state == NULL) {
        return "NULL state";
    }
    return state->error_message;
}

int part_processor_process_data(
    struct part_processor_state *state,
    const uint8_t *data,
    size_t data_len)
{
    if (state == NULL || (data == NULL && data_len > 0)) {
        return STREAM_PROC_ERR_INVALID_ARGS;
    }

    if (state->state == STATE_ERROR) {
        return state->error_code;
    }

    if (state->state == STATE_DONE) {
        return STREAM_PROC_SUCCESS;
    }

    // Combine buffered data with new data
    const uint8_t *work_buffer;
    size_t work_len;
    bool using_frame_buffer = false;

    if (state->frame_buffer_used > 0) {
        // Need to combine buffered data with new data
        size_t total_len = state->frame_buffer_used + data_len;
        if (total_len > state->frame_buffer_capacity) {
            // Grow buffer
            size_t new_capacity = state->frame_buffer_capacity * 2;
            while (new_capacity < total_len) {
                new_capacity *= 2;
            }
            uint8_t *new_buffer = realloc(state->frame_buffer, new_capacity);
            if (new_buffer == NULL) {
                snprintf(state->error_message, sizeof(state->error_message),
                         "Failed to grow frame buffer to %zu bytes", new_capacity);
                state->state = STATE_ERROR;
                state->error_code = STREAM_PROC_ERR_MEMORY;
                return STREAM_PROC_ERR_MEMORY;
            }
            state->frame_buffer = new_buffer;
            state->frame_buffer_capacity = new_capacity;
        }

        // Append new data to buffer
        memcpy(state->frame_buffer + state->frame_buffer_used, data, data_len);
        state->frame_buffer_used += data_len;

        work_buffer = state->frame_buffer;
        work_len = state->frame_buffer_used;
        using_frame_buffer = true;
    } else {
        work_buffer = data;
        work_len = data_len;
    }

    size_t offset = 0;

    while (offset < work_len) {
        size_t remaining = work_len - offset;
        const uint8_t *ptr = work_buffer + offset;

        // Handle state machine
        switch (state->state) {
        case STATE_INIT: {
            // Check if there's a continuing file for this part
            struct part_files *part = &state->cd_result->parts[state->part_index];
            if (part->continuing_file != NULL) {
                state->state = STATE_CONTINUING_FILE;
            } else {
                state->state = STATE_EXPECT_LOCAL_HEADER;
            }
            continue;  // Re-enter loop with new state
        }

        case STATE_CONTINUING_FILE: {
            // Expect Start-of-Part frame at offset 0
            struct frame_info info;
            int rc = parse_next_frame(ptr, remaining, &info);
            if (rc == STREAM_PROC_NEED_MORE_DATA) {
                goto buffer_remaining;
            }
            if (rc != STREAM_PROC_SUCCESS) {
                snprintf(state->error_message, sizeof(state->error_message),
                         "Failed to parse Start-of-Part frame at part %u",
                         state->part_index);
                state->state = STATE_ERROR;
                state->error_code = rc;
                return rc;
            }

            if (info.type != FRAME_BURST_START_OF_PART) {
                snprintf(state->error_message, sizeof(state->error_message),
                         "Expected Start-of-Part frame at part %u, got type %d",
                         state->part_index, info.type);
                state->state = STATE_ERROR;
                state->error_code = STREAM_PROC_ERR_INVALID_FRAME;
                return STREAM_PROC_ERR_INVALID_FRAME;
            }

            // Open continuing file with offset from Start-of-Part frame
            rc = handle_start_of_part_frame(state, ptr, info.frame_size);
            if (rc != STREAM_PROC_SUCCESS) {
                return rc;
            }

            offset += info.frame_size;
            state->bytes_processed += info.frame_size;
            state->state = STATE_PROCESSING_FRAMES;
            break;
        }

        case STATE_EXPECT_LOCAL_HEADER: {
            // Check if we've processed all files for this part
            struct part_files *part = &state->cd_result->parts[state->part_index];
            if (state->next_entry_idx >= part->num_entries) {
                state->state = STATE_DONE;
                continue;
            }

            // Parse frame to identify what's next
            struct frame_info info;
            int rc = parse_next_frame(ptr, remaining, &info);
            if (rc == STREAM_PROC_NEED_MORE_DATA) {
                goto buffer_remaining;
            }
            if (rc != STREAM_PROC_SUCCESS) {
                snprintf(state->error_message, sizeof(state->error_message),
                         "Failed to parse frame at offset %zu", offset);
                state->state = STATE_ERROR;
                state->error_code = rc;
                return rc;
            }

            // Skip padding frames
            if (info.type == FRAME_BURST_PADDING) {
                offset += info.frame_size;
                state->bytes_processed += info.frame_size;
                continue;
            }

            // Should be local header
            if (info.type != FRAME_ZIP_LOCAL_HEADER) {
                snprintf(state->error_message, sizeof(state->error_message),
                         "Expected local header, got type %d", info.type);
                state->state = STATE_ERROR;
                state->error_code = STREAM_PROC_ERR_INVALID_FRAME;
                return STREAM_PROC_ERR_INVALID_FRAME;
            }

            // Parse local header
            size_t bytes_consumed;
            rc = handle_local_header(state, ptr, remaining, &bytes_consumed);
            if (rc == STREAM_PROC_NEED_MORE_DATA) {
                goto buffer_remaining;
            }
            if (rc != STREAM_PROC_SUCCESS) {
                return rc;
            }

            offset += bytes_consumed;
            state->bytes_processed += bytes_consumed;
            state->next_entry_idx++;

            // Symlinks use STATE_READING_SYMLINK to read raw content
            if (state->current_file && state->current_file->is_symlink) {
                state->state = STATE_READING_SYMLINK;
            } else {
                state->state = STATE_PROCESSING_FRAMES;
            }
            break;
        }

        case STATE_PROCESSING_FRAMES: {
            struct frame_info info;
            int rc = parse_next_frame(ptr, remaining, &info);
            if (rc == STREAM_PROC_NEED_MORE_DATA) {
                goto buffer_remaining;
            }
            if (rc != STREAM_PROC_SUCCESS) {
                snprintf(state->error_message, sizeof(state->error_message),
                         "Failed to parse frame at offset %zu", offset);
                state->state = STATE_ERROR;
                state->error_code = rc;
                return rc;
            }

            switch (info.type) {
            case FRAME_ZSTD_COMPRESSED:
                // Check if we have the full frame
                if (remaining < info.frame_size) {
                    goto buffer_remaining;
                }
                rc = handle_zstd_frame(state, ptr, info.frame_size, info.uncompressed_size);
                if (rc != STREAM_PROC_SUCCESS) {
                    return rc;
                }
                offset += info.frame_size;
                state->bytes_processed += info.frame_size;
                break;

            case FRAME_BURST_PADDING:
                // Skip padding frame
                offset += info.frame_size;
                state->bytes_processed += info.frame_size;
                break;

            case FRAME_BURST_START_OF_PART: {
                // Start-of-Part frames appear at 8 MiB boundaries in BURST archives.
                // When downloading with part sizes larger than 8 MiB, we encounter
                // these frames within a single download part.
                //
                // Check if this frame is at a valid 8 MiB boundary within the archive.
                uint64_t archive_offset = state->part_start_offset + state->bytes_processed;
                if (archive_offset % BURST_BASE_ALIGNMENT != 0) {
                    snprintf(state->error_message, sizeof(state->error_message),
                             "Start-of-Part frame at non-aligned offset %llu",
                             (unsigned long long)archive_offset);
                    state->state = STATE_ERROR;
                    state->error_code = STREAM_PROC_ERR_INVALID_FRAME;
                    return STREAM_PROC_ERR_INVALID_FRAME;
                }

                // Valid Start-of-Part frame - update current file's write position
                // using the uncompressed offset from the frame
                rc = handle_start_of_part_frame(state, ptr, info.frame_size);
                if (rc != STREAM_PROC_SUCCESS) {
                    return rc;
                }
                offset += info.frame_size;
                state->bytes_processed += info.frame_size;
                break;
            }

            case FRAME_ZIP_DATA_DESCRIPTOR: {
                // End of current file
                // Determine descriptor size based on ZIP64 status from central directory
                size_t descriptor_size;
                if (state->current_file && state->current_file->uses_zip64_descriptor) {
                    descriptor_size = sizeof(struct zip_data_descriptor_zip64);  // 24 bytes
                } else {
                    descriptor_size = sizeof(struct zip_data_descriptor);  // 16 bytes
                }

                rc = handle_data_descriptor(state, ptr);
                if (rc != STREAM_PROC_SUCCESS) {
                    return rc;
                }
                offset += descriptor_size;
                state->bytes_processed += descriptor_size;
                state->state = STATE_EXPECT_LOCAL_HEADER;
                break;
            }

            case FRAME_ZIP_LOCAL_HEADER:
                // Next file started without data descriptor
                // Close current file and process new header
                rc = close_output_file(state);
                if (rc != STREAM_PROC_SUCCESS) {
                    return rc;
                }
                state->state = STATE_EXPECT_LOCAL_HEADER;
                // Don't advance offset - re-parse in new state
                break;

            default:
                snprintf(state->error_message, sizeof(state->error_message),
                         "Unknown frame type %d", info.type);
                state->state = STATE_ERROR;
                state->error_code = STREAM_PROC_ERR_INVALID_FRAME;
                return STREAM_PROC_ERR_INVALID_FRAME;
            }
            break;
        }

        case STATE_READING_SYMLINK: {
            // Read raw symlink content (STORE method - no compression)
            if (state->current_file == NULL || state->current_file->symlink_buffer == NULL) {
                snprintf(state->error_message, sizeof(state->error_message),
                         "Reading symlink content but no buffer allocated");
                state->state = STATE_ERROR;
                state->error_code = STREAM_PROC_ERR_INVALID_FRAME;
                return STREAM_PROC_ERR_INVALID_FRAME;
            }

            // Calculate how many bytes we still need
            size_t bytes_needed = state->current_file->expected_total_size -
                                  state->current_file->symlink_bytes_read;
            size_t bytes_to_copy = (remaining < bytes_needed) ? remaining : bytes_needed;

            // Copy raw content to symlink buffer
            memcpy(state->current_file->symlink_buffer + state->current_file->symlink_bytes_read,
                   ptr, bytes_to_copy);
            state->current_file->symlink_bytes_read += bytes_to_copy;
            offset += bytes_to_copy;
            state->bytes_processed += bytes_to_copy;

            // Check if we've read all symlink content
            if (state->current_file->symlink_bytes_read >= state->current_file->expected_total_size) {
                // Close symlink (creates the actual symlink)
                int rc = close_output_file(state);
                if (rc != STREAM_PROC_SUCCESS) {
                    return rc;
                }
                // Symlinks don't have data descriptors, so go directly to next file
                state->state = STATE_EXPECT_LOCAL_HEADER;
            }
            break;
        }

        case STATE_DONE:
            // Consume remaining bytes silently (should be central directory or beyond)
            offset = work_len;
            break;

        case STATE_ERROR:
            return state->error_code;
        }
    }

    // Successfully processed all data
    if (using_frame_buffer) {
        state->frame_buffer_used = 0;
    }
    return STREAM_PROC_SUCCESS;

buffer_remaining:
    // Buffer remaining data for next call
    if (using_frame_buffer) {
        // Already in frame buffer, just update used count
        memmove(state->frame_buffer, work_buffer + offset, work_len - offset);
        state->frame_buffer_used = work_len - offset;
    } else {
        // Copy remaining data to frame buffer
        size_t to_buffer = work_len - offset;
        if (to_buffer > state->frame_buffer_capacity) {
            size_t new_capacity = state->frame_buffer_capacity;
            while (new_capacity < to_buffer) {
                new_capacity *= 2;
            }
            uint8_t *new_buffer = realloc(state->frame_buffer, new_capacity);
            if (new_buffer == NULL) {
                snprintf(state->error_message, sizeof(state->error_message),
                         "Failed to grow frame buffer");
                state->state = STATE_ERROR;
                state->error_code = STREAM_PROC_ERR_MEMORY;
                return STREAM_PROC_ERR_MEMORY;
            }
            state->frame_buffer = new_buffer;
            state->frame_buffer_capacity = new_capacity;
        }
        memcpy(state->frame_buffer, work_buffer + offset, to_buffer);
        state->frame_buffer_used = to_buffer;
    }

    return STREAM_PROC_SUCCESS;
}

int part_processor_finalize(struct part_processor_state *state)
{
    if (state == NULL) {
        return STREAM_PROC_ERR_INVALID_ARGS;
    }

    // Close any open file
    if (state->current_file != NULL) {
        int rc = close_output_file(state);
        if (rc != STREAM_PROC_SUCCESS) {
            return rc;
        }
    }

    // Check for unexpected buffered data
    if (state->frame_buffer_used > 0) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Unexpected %zu bytes remaining in buffer at end of part",
                 state->frame_buffer_used);
        state->state = STATE_ERROR;
        state->error_code = STREAM_PROC_ERR_UNEXPECTED_EOF;
        return STREAM_PROC_ERR_UNEXPECTED_EOF;
    }

    state->state = STATE_DONE;
    return STREAM_PROC_SUCCESS;
}

static int handle_start_of_part_frame(struct part_processor_state *state,
                                      const uint8_t *frame_data, size_t frame_size)
{
    (void)frame_size;  // Already validated

    // Extract uncompressed offset from frame
    uint64_t uncompressed_offset;
    memcpy(&uncompressed_offset, frame_data + 9, sizeof(uint64_t));

    // If we already have a file open (mid-part Start-of-Part frame), just update
    // the uncompressed offset. This happens when downloading with part sizes
    // larger than 8 MiB - Start-of-Part frames appear at each 8 MiB boundary.
    if (state->current_file != NULL) {
        state->current_file->uncompressed_offset = uncompressed_offset;
        return STREAM_PROC_SUCCESS;
    }

    // No file open yet - this is a continuing file at the start of a downloaded part.
    // Get the continuing file metadata from the part map.
    struct part_files *part = &state->cd_result->parts[state->part_index];
    struct file_metadata *file_meta = part->continuing_file;

    if (file_meta == NULL) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Start-of-Part frame but no continuing file for part %u",
                 state->part_index);
        state->state = STATE_ERROR;
        state->error_code = STREAM_PROC_ERR_INVALID_FRAME;
        return STREAM_PROC_ERR_INVALID_FRAME;
    }

    // Open the file for the continuing portion
    int rc = open_output_file(state, file_meta);
    if (rc != STREAM_PROC_SUCCESS) {
        return rc;
    }

    // Set uncompressed offset from Start-of-Part frame
    state->current_file->uncompressed_offset = uncompressed_offset;

    return STREAM_PROC_SUCCESS;
}

static int handle_local_header(struct part_processor_state *state,
                               const uint8_t *header_data, size_t available_len,
                               size_t *bytes_consumed)
{
    if (available_len < sizeof(struct zip_local_header)) {
        return STREAM_PROC_NEED_MORE_DATA;
    }

    const struct zip_local_header *lfh = (const struct zip_local_header *)header_data;
    size_t header_size = sizeof(struct zip_local_header) +
                        lfh->filename_length + lfh->extra_field_length;

    if (available_len < header_size) {
        return STREAM_PROC_NEED_MORE_DATA;
    }

    // Get file metadata from central directory
    struct part_files *part = &state->cd_result->parts[state->part_index];
    if (state->next_entry_idx >= part->num_entries) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Local header found but no more entries expected");
        state->state = STATE_ERROR;
        state->error_code = STREAM_PROC_ERR_INVALID_FRAME;
        return STREAM_PROC_ERR_INVALID_FRAME;
    }

    struct part_file_entry *entry = &part->entries[state->next_entry_idx];
    struct file_metadata *file_meta = &state->cd_result->files[entry->file_index];

    // Open output file
    int rc = open_output_file(state, file_meta);
    if (rc != STREAM_PROC_SUCCESS) {
        return rc;
    }

    // New file starts at offset 0
    state->current_file->uncompressed_offset = 0;

    *bytes_consumed = header_size;
    return STREAM_PROC_SUCCESS;
}

static int handle_zstd_frame(struct part_processor_state *state,
                             const uint8_t *frame_data, size_t compressed_size,
                             uint64_t uncompressed_size)
{
    if (state->current_file == NULL || state->current_file->fd < 0) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Zstd frame without open output file");
        state->state = STATE_ERROR;
        state->error_code = STREAM_PROC_ERR_INVALID_FRAME;
        return STREAM_PROC_ERR_INVALID_FRAME;
    }

    // Write frame to BTRFS
    int rc = do_write_encoded(
        state->current_file->fd,
        frame_data,
        compressed_size,
        uncompressed_size,
        state->current_file->uncompressed_offset);

    if (rc != BTRFS_WRITER_SUCCESS) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "BTRFS write failed: %d", rc);
        state->state = STATE_ERROR;
        state->error_code = STREAM_PROC_ERR_BTRFS_WRITE;
        return STREAM_PROC_ERR_BTRFS_WRITE;
    }

    // Advance uncompressed offset
    state->current_file->uncompressed_offset += uncompressed_size;

    return STREAM_PROC_SUCCESS;
}

static int handle_data_descriptor(struct part_processor_state *state,
                                  const uint8_t *descriptor_data)
{
    const struct zip_data_descriptor *desc = (const struct zip_data_descriptor *)descriptor_data;

    // Validate CRC if we have it
    if (state->current_file != NULL) {
        // Note: CRC validation would require tracking CRC during decompression
        // For now, just log and close the file
        (void)desc;  // Suppress unused warning
    }

    return close_output_file(state);
}

static int open_output_file(struct part_processor_state *state,
                            struct file_metadata *file_meta)
{
    // Close any existing file
    if (state->current_file != NULL) {
        int rc = close_output_file(state);
        if (rc != STREAM_PROC_SUCCESS) {
            return rc;
        }
    }

    // Allocate file context
    state->current_file = calloc(1, sizeof(struct file_context));
    if (state->current_file == NULL) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Failed to allocate file context");
        state->state = STATE_ERROR;
        state->error_code = STREAM_PROC_ERR_MEMORY;
        return STREAM_PROC_ERR_MEMORY;
    }

    // Build full output path
    size_t path_len = strlen(state->output_dir) + 1 + strlen(file_meta->filename) + 1;
    state->current_file->filename = malloc(path_len);
    if (state->current_file->filename == NULL) {
        free(state->current_file);
        state->current_file = NULL;
        snprintf(state->error_message, sizeof(state->error_message),
                 "Failed to allocate filename");
        state->state = STATE_ERROR;
        state->error_code = STREAM_PROC_ERR_MEMORY;
        return STREAM_PROC_ERR_MEMORY;
    }
    snprintf(state->current_file->filename, path_len, "%s/%s",
             state->output_dir, file_meta->filename);

    // Ensure directory exists
    int rc = ensure_directory_exists(state->current_file->filename);
    if (rc != 0) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Failed to create directory for %s", state->current_file->filename);
        free(state->current_file->filename);
        free(state->current_file);
        state->current_file = NULL;
        state->state = STATE_ERROR;
        state->error_code = STREAM_PROC_ERR_IO;
        return STREAM_PROC_ERR_IO;
    }

    // Store expected values from central directory
    state->current_file->expected_total_size = file_meta->uncompressed_size;
    state->current_file->expected_crc32 = file_meta->crc32;

    // Copy Unix metadata for permission restoration
    state->current_file->unix_mode = file_meta->unix_mode;
    state->current_file->uid = file_meta->uid;
    state->current_file->gid = file_meta->gid;
    state->current_file->has_unix_mode = file_meta->has_unix_mode;
    state->current_file->has_unix_extra = file_meta->has_unix_extra;
    state->current_file->is_symlink = file_meta->is_symlink;

    // Copy ZIP64 tracking from central directory
    state->current_file->uses_zip64_descriptor = file_meta->uses_zip64_descriptor;

    // Symlinks: allocate buffer for target path instead of opening file
    if (file_meta->is_symlink) {
        state->current_file->fd = -1;  // No file descriptor for symlinks
        state->current_file->symlink_buffer_size = file_meta->uncompressed_size + 1;  // +1 for null terminator
        state->current_file->symlink_buffer = malloc(state->current_file->symlink_buffer_size);
        if (state->current_file->symlink_buffer == NULL) {
            snprintf(state->error_message, sizeof(state->error_message),
                     "Failed to allocate symlink buffer for %s", state->current_file->filename);
            free(state->current_file->filename);
            free(state->current_file);
            state->current_file = NULL;
            state->state = STATE_ERROR;
            state->error_code = STREAM_PROC_ERR_MEMORY;
            return STREAM_PROC_ERR_MEMORY;
        }
        state->current_file->symlink_bytes_read = 0;
        return STREAM_PROC_SUCCESS;
    }

    // Regular file: open for writing
    // Never use O_TRUNC - with concurrent part processing, parts may complete
    // out of order, so we must not truncate data written by other parts
    state->current_file->fd = open(state->current_file->filename,
                                   O_WRONLY | O_CREAT, 0644);
    if (state->current_file->fd < 0) {
        snprintf(state->error_message, sizeof(state->error_message),
                 "Failed to open %s: %s", state->current_file->filename, strerror(errno));
        free(state->current_file->filename);
        free(state->current_file);
        state->current_file = NULL;
        state->state = STATE_ERROR;
        state->error_code = STREAM_PROC_ERR_IO;
        return STREAM_PROC_ERR_IO;
    }

    return STREAM_PROC_SUCCESS;
}

static int close_output_file(struct part_processor_state *state)
{
    if (state->current_file == NULL) {
        return STREAM_PROC_SUCCESS;
    }

    // Handle symlinks: create symlink from buffered target path
    if (state->current_file->is_symlink && state->current_file->symlink_buffer != NULL) {
        // Null-terminate the target path
        state->current_file->symlink_buffer[state->current_file->symlink_bytes_read] = '\0';

        // Remove existing symlink/file if it exists
        unlink(state->current_file->filename);

        // Create symlink
        if (symlink((char *)state->current_file->symlink_buffer, state->current_file->filename) != 0) {
            // Log but don't fail (symlink may already exist from another part)
            fprintf(stderr, "Warning: failed to create symlink %s -> %s: %s\n",
                    state->current_file->filename,
                    (char *)state->current_file->symlink_buffer,
                    strerror(errno));
        }

        // Apply ownership to symlink if running as root (using lchown, not fchown)
        if (state->current_file->has_unix_extra) {
            if (geteuid() == 0) {
                if (lchown(state->current_file->filename, state->current_file->uid, state->current_file->gid) != 0) {
                    fprintf(stderr, "Warning: failed to set ownership on symlink %s: %s\n",
                            state->current_file->filename, strerror(errno));
                }
            }
        }

        // Free symlink buffer
        free(state->current_file->symlink_buffer);
        state->current_file->symlink_buffer = NULL;

        free(state->current_file->filename);
        free(state->current_file);
        state->current_file = NULL;

        return STREAM_PROC_SUCCESS;
    }

    if (state->current_file->fd >= 0) {
        // Truncate file to expected final size (from central directory).
        // This handles pre-existing files that may be larger than expected,
        // and is safe with concurrent parts since all parts truncate to the
        // same final size (multiple truncates to same size are idempotent).
        if (ftruncate(state->current_file->fd, (off_t)state->current_file->expected_total_size) != 0) {
            // Log but don't fail
            fprintf(stderr, "Warning: failed to truncate %s: %s\n",
                    state->current_file->filename, strerror(errno));
        }

        // Apply Unix permissions if available
        if (state->current_file->has_unix_mode) {
            // Extract permission bits only (lower 12 bits: rwx + setuid/setgid/sticky)
            mode_t mode = state->current_file->unix_mode & 07777;
            if (fchmod(state->current_file->fd, mode) != 0) {
                // Log but don't fail
                fprintf(stderr, "Warning: failed to set permissions on %s: %s\n",
                        state->current_file->filename, strerror(errno));
            }
        }

        // Apply Unix ownership if available and running as root
        if (state->current_file->has_unix_extra) {
            if (geteuid() == 0) {
                if (fchown(state->current_file->fd, state->current_file->uid, state->current_file->gid) != 0) {
                    // Log error when running as root (shouldn't fail)
                    fprintf(stderr, "Warning: failed to set ownership on %s: %s\n",
                            state->current_file->filename, strerror(errno));
                }
            }
            // Silently skip chown when not running as root
        }

        close(state->current_file->fd);
        state->current_file->fd = -1;
    }

    // Free any allocated symlink buffer (in case of error cleanup)
    free(state->current_file->symlink_buffer);

    free(state->current_file->filename);
    free(state->current_file);
    state->current_file = NULL;

    return STREAM_PROC_SUCCESS;
}

// Ensure all directories in the path exist
static int ensure_directory_exists(const char *path)
{
    // Make a copy since we'll modify it
    char *path_copy = strdup(path);
    if (path_copy == NULL) {
        return -1;
    }

    // Get directory portion
    char *dir = dirname(path_copy);

    // Build path component by component
    char *p = dir;
    while (*p == '/') p++;  // Skip leading slashes

    while (*p) {
        // Find next slash
        char *slash = strchr(p, '/');
        if (slash) {
            *slash = '\0';
        }

        // Try to create directory
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            // Check if it exists as a directory
            struct stat st;
            if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
                free(path_copy);
                return -1;
            }
        }

        if (slash) {
            *slash = '/';
            p = slash + 1;
        } else {
            break;
        }
    }

    free(path_copy);
    return 0;
}
