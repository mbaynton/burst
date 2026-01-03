/**
 * Central directory fetch utilities for large archives.
 *
 * When a BURST archive has a central directory larger than 8 MiB, we need to
 * fetch additional data beyond the initial tail buffer. This module provides
 * functions to calculate aligned fetch ranges and assemble the fetched data.
 */

#include "cd_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// AWS-dependent code is conditionally compiled
#ifdef BUILD_WITH_AWS
#include "burst_downloader.h"
#include <aws/common/allocator.h>
#include <aws/common/byte_buf.h>
#include <aws/common/condition_variable.h>
#include <aws/common/mutex.h>
#include <aws/http/request_response.h>
#include <aws/s3/s3_client.h>
#endif

int calculate_cd_fetch_ranges(
    uint64_t central_dir_offset,
    uint64_t central_dir_size,
    uint64_t part_size,
    uint64_t initial_buffer_start,
    struct cd_part_range **ranges,
    size_t *num_ranges
) {
    (void)central_dir_size;  // Used by caller for validation, not needed here

    if (!ranges || !num_ranges) {
        return -1;
    }

    *ranges = NULL;
    *num_ranges = 0;

    // If CD is entirely within initial buffer, no additional fetches needed
    if (central_dir_offset >= initial_buffer_start) {
        return 0;
    }

    // Calculate which aligned parts we need
    // First part containing central_dir_offset
    uint64_t first_part_idx = central_dir_offset / part_size;
    uint64_t first_part_start = first_part_idx * part_size;

    // We need to fetch from first_part_start up to (but not including) initial_buffer_start
    // because initial_buffer_start onwards is already in the tail buffer

    // Find last part that starts before initial_buffer_start
    // We want to cover [first_part_start, initial_buffer_start)
    if (first_part_start >= initial_buffer_start) {
        // Shouldn't happen if central_dir_offset < initial_buffer_start
        return 0;
    }

    // Count how many full parts we need
    size_t count = 0;
    for (uint64_t part_start = first_part_start;
         part_start < initial_buffer_start;
         part_start += part_size) {
        count++;
    }

    if (count == 0) {
        return 0;
    }

    // Allocate ranges array
    struct cd_part_range *result = calloc(count, sizeof(struct cd_part_range));
    if (!result) {
        return -1;
    }

    // Fill in ranges
    size_t i = 0;
    for (uint64_t part_start = first_part_start;
         part_start < initial_buffer_start && i < count;
         part_start += part_size, i++) {

        result[i].start = part_start;

        // End is either the next part boundary or initial_buffer_start - 1
        uint64_t part_end = part_start + part_size - 1;
        if (part_end >= initial_buffer_start) {
            part_end = initial_buffer_start - 1;
        }
        result[i].end = part_end;

        // Check if this range includes body data (data before central_dir_offset)
        if (part_start < central_dir_offset) {
            result[i].has_body_data = true;
            // Body data goes from part_start to central_dir_offset (exclusive)
            uint64_t body_end = central_dir_offset;
            if (body_end > result[i].end + 1) {
                body_end = result[i].end + 1;
            }
            result[i].body_data_size = body_end - part_start;
        } else {
            result[i].has_body_data = false;
            result[i].body_data_size = 0;
        }
    }

    *ranges = result;
    *num_ranges = i;

    return 0;
}

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
) {
    if (!out_cd_buffer || !out_cd_size || !out_body_segments || !out_num_body_segments) {
        return -1;
    }

    *out_cd_buffer = NULL;
    *out_cd_size = 0;
    *out_body_segments = NULL;
    *out_num_body_segments = 0;

    // Allocate contiguous CD buffer
    uint8_t *cd_buf = malloc(central_dir_size);
    if (!cd_buf) {
        fprintf(stderr, "Failed to allocate CD buffer (%llu bytes)\n",
                (unsigned long long)central_dir_size);
        return -1;
    }

    // Track where we've filled in the CD buffer
    // CD buffer covers [central_dir_offset, central_dir_offset + central_dir_size)
    uint64_t cd_end = central_dir_offset + central_dir_size;

    // Copy CD data from fetched ranges
    for (size_t i = 0; i < num_ranges; i++) {
        if (!range_buffers[i] || range_sizes[i] == 0) {
            continue;
        }

        uint64_t range_start = ranges[i].start;
        uint64_t range_end = ranges[i].end;

        // Calculate overlap with CD
        uint64_t cd_data_start = range_start;
        if (cd_data_start < central_dir_offset) {
            cd_data_start = central_dir_offset;
        }

        uint64_t cd_data_end = range_end + 1;
        if (cd_data_end > cd_end) {
            cd_data_end = cd_end;
        }

        if (cd_data_start < cd_data_end) {
            // There's CD data in this range
            size_t offset_in_range = (size_t)(cd_data_start - range_start);
            size_t offset_in_cd_buf = (size_t)(cd_data_start - central_dir_offset);
            size_t copy_size = (size_t)(cd_data_end - cd_data_start);

            if (offset_in_range + copy_size <= range_sizes[i]) {
                memcpy(cd_buf + offset_in_cd_buf,
                       range_buffers[i] + offset_in_range,
                       copy_size);
            }
        }
    }

    // Copy CD data from initial buffer
    if (initial_buffer && initial_size > 0 && initial_start < cd_end) {
        uint64_t initial_end = initial_start + initial_size;

        uint64_t cd_data_start = initial_start;
        if (cd_data_start < central_dir_offset) {
            cd_data_start = central_dir_offset;
        }

        uint64_t cd_data_end = initial_end;
        if (cd_data_end > cd_end) {
            cd_data_end = cd_end;
        }

        if (cd_data_start < cd_data_end) {
            size_t offset_in_initial = (size_t)(cd_data_start - initial_start);
            size_t offset_in_cd_buf = (size_t)(cd_data_start - central_dir_offset);
            size_t copy_size = (size_t)(cd_data_end - cd_data_start);

            if (offset_in_initial + copy_size <= initial_size) {
                memcpy(cd_buf + offset_in_cd_buf,
                       initial_buffer + offset_in_initial,
                       copy_size);
            }
        }
    }

    // Count body data segments (at most one from fetched ranges)
    size_t num_body = 0;
    for (size_t i = 0; i < num_ranges; i++) {
        if (ranges[i].has_body_data && ranges[i].body_data_size > 0) {
            num_body++;
            break;  // At most one segment from CD fetch (the first range)
        }
    }

    // Allocate body segments array if needed
    struct body_data_segment *body_segs = NULL;
    if (num_body > 0) {
        body_segs = calloc(num_body, sizeof(struct body_data_segment));
        if (!body_segs) {
            free(cd_buf);
            return -1;
        }

        // Fill in body segment from first range with body data
        for (size_t i = 0; i < num_ranges; i++) {
            if (ranges[i].has_body_data && ranges[i].body_data_size > 0) {
                body_segs[0].data = range_buffers[i];  // Points into range buffer
                body_segs[0].size = ranges[i].body_data_size;
                body_segs[0].archive_offset = ranges[i].start;
                break;
            }
        }
    }

    *out_cd_buffer = cd_buf;
    *out_cd_size = central_dir_size;
    *out_body_segments = body_segs;
    *out_num_body_segments = num_body;

    return 0;
}

int add_tail_buffer_segment(
    struct body_data_segment **body_segments,
    size_t *num_body_segments,
    const uint8_t *initial_buffer,
    size_t initial_size,
    uint64_t initial_start,
    uint64_t central_dir_offset,
    uint64_t part_size
) {
    (void)part_size;  // Not currently used, but may be useful for future optimizations

    if (!body_segments || !num_body_segments || !initial_buffer) {
        return -1;
    }

    // Check if there's body data in the tail buffer
    // Body data is from initial_start to central_dir_offset
    if (initial_start >= central_dir_offset) {
        // No body data in tail buffer
        return 0;
    }

    // Calculate body data extent in tail buffer
    uint64_t body_data_end = central_dir_offset;
    if (body_data_end > initial_start + initial_size) {
        body_data_end = initial_start + initial_size;
    }

    size_t body_size = (size_t)(body_data_end - initial_start);
    if (body_size == 0) {
        return 0;
    }

    // Expand body_segments array
    size_t new_count = *num_body_segments + 1;
    struct body_data_segment *new_segs = realloc(*body_segments,
                                                   new_count * sizeof(struct body_data_segment));
    if (!new_segs) {
        return -1;
    }

    // Add the tail buffer segment
    new_segs[new_count - 1].data = (uint8_t *)initial_buffer;  // Points into initial buffer
    new_segs[new_count - 1].size = body_size;
    new_segs[new_count - 1].archive_offset = initial_start;

    *body_segments = new_segs;
    *num_body_segments = new_count;

    return 0;
}

void free_body_segments(struct body_data_segment *segments, size_t num_segments) {
    (void)num_segments;  // Segments point into external buffers, don't free data

    if (segments) {
        free(segments);
    }
}

// ============================================================================
// Concurrent CD Range Fetch (AWS-dependent)
// ============================================================================
#ifdef BUILD_WITH_AWS

// Context for a single range fetch
struct cd_range_fetch_context {
    struct burst_downloader *downloader;
    const struct cd_part_range *range;
    size_t range_index;

    // Result
    uint8_t *buffer;
    size_t buffer_size;
    size_t buffer_capacity;

    // Error tracking
    int error_code;
    char error_message[256];

    // Meta request handle
    struct aws_s3_meta_request *meta_request;

    // Coordinator reference
    struct cd_fetch_coordinator *coordinator;
};

// Coordinator for concurrent range fetches
struct cd_fetch_coordinator {
    struct aws_mutex mutex;
    struct aws_condition_variable cv;

    // Configuration
    size_t max_concurrent;
    struct burst_downloader *downloader;

    // Range tracking
    const struct cd_part_range *ranges;
    size_t total_ranges;
    size_t next_range_to_start;
    size_t ranges_in_flight;
    size_t ranges_completed;

    // Results
    uint8_t **buffers;
    size_t *sizes;

    // Per-range contexts
    struct cd_range_fetch_context **contexts;

    // Error handling
    bool cancel_requested;
    int first_error_code;
    char first_error_message[256];
};

// Forward declarations
static int start_range_fetch_async(struct cd_fetch_coordinator *coord, size_t range_index);

// Body callback - accumulate response data
static int cd_range_body_callback(
    struct aws_s3_meta_request *meta_request,
    const struct aws_byte_cursor *body,
    uint64_t range_start,
    void *user_data
) {
    (void)meta_request;
    (void)range_start;

    struct cd_range_fetch_context *ctx = user_data;

    // Expand buffer if needed
    size_t new_size = ctx->buffer_size + body->len;
    if (new_size > ctx->buffer_capacity) {
        size_t new_capacity = ctx->buffer_capacity == 0 ? 4096 : ctx->buffer_capacity * 2;
        while (new_capacity < new_size) {
            new_capacity *= 2;
        }

        uint8_t *new_buffer = realloc(ctx->buffer, new_capacity);
        if (!new_buffer) {
            ctx->error_code = -1;
            snprintf(ctx->error_message, sizeof(ctx->error_message),
                    "Failed to allocate buffer (%zu bytes)", new_capacity);
            return AWS_OP_ERR;
        }

        ctx->buffer = new_buffer;
        ctx->buffer_capacity = new_capacity;
    }

    // Append data
    memcpy(ctx->buffer + ctx->buffer_size, body->ptr, body->len);
    ctx->buffer_size += body->len;

    return AWS_OP_SUCCESS;
}

// Headers callback - check HTTP status
static int cd_range_headers_callback(
    struct aws_s3_meta_request *meta_request,
    const struct aws_http_headers *headers,
    int response_status,
    void *user_data
) {
    (void)meta_request;
    (void)headers;

    struct cd_range_fetch_context *ctx = user_data;

    if (response_status < 200 || response_status >= 300) {
        ctx->error_code = -1;
        snprintf(ctx->error_message, sizeof(ctx->error_message),
                "HTTP error: status %d", response_status);
    }

    return AWS_OP_SUCCESS;
}

// Finish callback
static void cd_range_finish_callback(
    struct aws_s3_meta_request *meta_request,
    const struct aws_s3_meta_request_result *result,
    void *user_data
) {
    (void)meta_request;

    struct cd_range_fetch_context *ctx = user_data;
    struct cd_fetch_coordinator *coord = ctx->coordinator;

    // Record error if any
    if (result->error_code != AWS_ERROR_SUCCESS && ctx->error_code == 0) {
        ctx->error_code = result->error_code;
        snprintf(ctx->error_message, sizeof(ctx->error_message),
                "S3 request failed: %s", aws_error_debug_str(result->error_code));
    }

    aws_mutex_lock(&coord->mutex);

    coord->ranges_in_flight--;

    if (ctx->error_code != 0) {
        // Error occurred
        if (!coord->cancel_requested) {
            coord->cancel_requested = true;
            coord->first_error_code = ctx->error_code;
            strncpy(coord->first_error_message, ctx->error_message,
                    sizeof(coord->first_error_message) - 1);
            coord->first_error_message[sizeof(coord->first_error_message) - 1] = '\0';

            // Cancel other in-flight requests
            for (size_t i = 0; i < coord->total_ranges; i++) {
                if (coord->contexts[i] &&
                    coord->contexts[i] != ctx &&
                    coord->contexts[i]->meta_request) {
                    aws_s3_meta_request_cancel(coord->contexts[i]->meta_request);
                }
            }
        }
    } else {
        // Success - store result
        coord->buffers[ctx->range_index] = ctx->buffer;
        coord->sizes[ctx->range_index] = ctx->buffer_size;
        ctx->buffer = NULL;  // Transfer ownership
        ctx->buffer_capacity = 0;
        coord->ranges_completed++;
    }

    // Start next range if available
    if (!coord->cancel_requested && coord->next_range_to_start < coord->total_ranges) {
        size_t next = coord->next_range_to_start++;
        coord->ranges_in_flight++;
        aws_mutex_unlock(&coord->mutex);

        if (start_range_fetch_async(coord, next) != 0) {
            aws_mutex_lock(&coord->mutex);
            coord->ranges_in_flight--;
            if (!coord->cancel_requested) {
                coord->cancel_requested = true;
                coord->first_error_code = -1;
                snprintf(coord->first_error_message, sizeof(coord->first_error_message),
                         "Failed to start range %zu fetch", next);
            }
            if (coord->ranges_in_flight == 0) {
                aws_condition_variable_notify_one(&coord->cv);
            }
            aws_mutex_unlock(&coord->mutex);
        }
        return;
    }

    // Signal if all done
    if (coord->ranges_in_flight == 0) {
        aws_condition_variable_notify_one(&coord->cv);
    }

    aws_mutex_unlock(&coord->mutex);
}

static int start_range_fetch_async(struct cd_fetch_coordinator *coord, size_t range_index) {
    struct burst_downloader *downloader = coord->downloader;
    const struct cd_part_range *range = &coord->ranges[range_index];

    // Create context
    struct cd_range_fetch_context *ctx = calloc(1, sizeof(struct cd_range_fetch_context));
    if (!ctx) {
        return -1;
    }

    ctx->downloader = downloader;
    ctx->range = range;
    ctx->range_index = range_index;
    ctx->coordinator = coord;
    ctx->error_code = 0;
    ctx->error_message[0] = '\0';

    coord->contexts[range_index] = ctx;

    // Build HTTP request
    struct aws_http_message *message = aws_http_message_new_request(downloader->allocator);
    if (!message) {
        free(ctx);
        coord->contexts[range_index] = NULL;
        return -1;
    }

    aws_http_message_set_request_method(message, aws_http_method_get);

    // Set path
    struct aws_byte_buf path_buf;
    aws_byte_buf_init(&path_buf, downloader->allocator, strlen(downloader->key) + 2);
    aws_byte_buf_append_byte_dynamic(&path_buf, '/');
    struct aws_byte_cursor key_cursor = aws_byte_cursor_from_c_str(downloader->key);
    aws_byte_buf_append_dynamic(&path_buf, &key_cursor);
    struct aws_byte_cursor path_cursor = aws_byte_cursor_from_buf(&path_buf);
    aws_http_message_set_request_path(message, path_cursor);

    // Set Host header
    char host_value[256];
    snprintf(host_value, sizeof(host_value), "%s.s3.%s.amazonaws.com",
             downloader->bucket, downloader->region);
    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_c_str("Host"),
        .value = aws_byte_cursor_from_c_str(host_value),
    };
    aws_http_message_add_header(message, host_header);

    // Set Range header
    char range_value[128];
    snprintf(range_value, sizeof(range_value), "bytes=%llu-%llu",
             (unsigned long long)range->start, (unsigned long long)range->end);
    struct aws_http_header range_header = {
        .name = aws_byte_cursor_from_c_str("Range"),
        .value = aws_byte_cursor_from_c_str(range_value),
    };
    aws_http_message_add_header(message, range_header);

    // Create meta request
    struct aws_s3_meta_request_options options = {
        .type = AWS_S3_META_REQUEST_TYPE_GET_OBJECT,
        .message = message,
        .user_data = ctx,
        .headers_callback = cd_range_headers_callback,
        .body_callback = cd_range_body_callback,
        .finish_callback = cd_range_finish_callback,
    };

    ctx->meta_request = aws_s3_client_make_meta_request(downloader->s3_client, &options);
    aws_http_message_release(message);
    aws_byte_buf_clean_up(&path_buf);

    if (!ctx->meta_request) {
        free(ctx);
        coord->contexts[range_index] = NULL;
        return -1;
    }

    return 0;
}

int burst_downloader_fetch_cd_ranges(
    struct burst_downloader *downloader,
    const struct cd_part_range *ranges,
    size_t num_ranges,
    size_t max_concurrent,
    uint8_t ***out_buffers,
    size_t **out_sizes
) {
    if (!downloader || !ranges || num_ranges == 0 || !out_buffers || !out_sizes) {
        return -1;
    }

    *out_buffers = NULL;
    *out_sizes = NULL;

    // Allocate result arrays
    uint8_t **buffers = calloc(num_ranges, sizeof(uint8_t *));
    size_t *sizes = calloc(num_ranges, sizeof(size_t));
    struct cd_range_fetch_context **contexts = calloc(num_ranges, sizeof(struct cd_range_fetch_context *));

    if (!buffers || !sizes || !contexts) {
        free(buffers);
        free(sizes);
        free(contexts);
        return -1;
    }

    // Initialize coordinator
    struct cd_fetch_coordinator coord = {
        .max_concurrent = max_concurrent,
        .downloader = downloader,
        .ranges = ranges,
        .total_ranges = num_ranges,
        .next_range_to_start = 0,
        .ranges_in_flight = 0,
        .ranges_completed = 0,
        .buffers = buffers,
        .sizes = sizes,
        .contexts = contexts,
        .cancel_requested = false,
        .first_error_code = 0,
    };
    coord.first_error_message[0] = '\0';

    aws_mutex_init(&coord.mutex);
    aws_condition_variable_init(&coord.cv);

    // Start initial batch
    aws_mutex_lock(&coord.mutex);
    while (coord.ranges_in_flight < coord.max_concurrent &&
           coord.next_range_to_start < coord.total_ranges) {
        size_t idx = coord.next_range_to_start++;
        coord.ranges_in_flight++;

        aws_mutex_unlock(&coord.mutex);

        printf("Fetching CD range %zu/%zu (bytes %llu-%llu)...\n",
               idx + 1, num_ranges,
               (unsigned long long)ranges[idx].start,
               (unsigned long long)ranges[idx].end);

        if (start_range_fetch_async(&coord, idx) != 0) {
            aws_mutex_lock(&coord.mutex);
            coord.ranges_in_flight--;
            coord.cancel_requested = true;
            coord.first_error_code = -1;
            snprintf(coord.first_error_message, sizeof(coord.first_error_message),
                     "Failed to start range %zu fetch", idx);
            break;
        }

        aws_mutex_lock(&coord.mutex);
    }

    // Wait for all to complete
    while (coord.ranges_in_flight > 0) {
        aws_condition_variable_wait(&coord.cv, &coord.mutex);
    }
    aws_mutex_unlock(&coord.mutex);

    // Check for errors
    int result = coord.first_error_code;
    if (result != 0) {
        fprintf(stderr, "Error fetching CD ranges: %s\n", coord.first_error_message);
    }

    // Cleanup contexts
    for (size_t i = 0; i < num_ranges; i++) {
        if (contexts[i]) {
            if (contexts[i]->meta_request) {
                aws_s3_meta_request_release(contexts[i]->meta_request);
            }
            if (contexts[i]->buffer) {
                free(contexts[i]->buffer);
            }
            free(contexts[i]);
        }
    }
    free(contexts);

    aws_mutex_clean_up(&coord.mutex);
    aws_condition_variable_clean_up(&coord.cv);

    if (result != 0) {
        // Free any buffers that were allocated
        for (size_t i = 0; i < num_ranges; i++) {
            if (buffers[i]) {
                free(buffers[i]);
            }
        }
        free(buffers);
        free(sizes);
        return -1;
    }

    *out_buffers = buffers;
    *out_sizes = sizes;
    return 0;
}

#endif // BUILD_WITH_AWS
