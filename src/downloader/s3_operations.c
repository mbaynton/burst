#include "burst_downloader.h"
#include "s3_client.h"
#include "stream_processor.h"

#include <aws/common/byte_buf.h>
#include <aws/common/condition_variable.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/http/request_response.h>
#include <aws/s3/s3_client.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Context structure for async GET requests
struct get_request_context {
    struct burst_downloader *downloader;

    // Synchronization
    struct aws_mutex mutex;
    struct aws_condition_variable condition_variable;
    bool request_complete;

    // Response data
    uint8_t *buffer;
    size_t buffer_size;
    size_t buffer_capacity;

    // Response metadata
    uint64_t content_length;
    uint64_t range_start;
    uint64_t range_end;
    uint64_t total_size;

    // Error tracking
    int error_code;
    char error_message[256];

    // Meta request handle
    struct aws_s3_meta_request *meta_request;
};

// Initialize request context
static struct get_request_context *get_request_context_new(struct burst_downloader *downloader) {
    struct get_request_context *ctx =
        aws_mem_calloc(downloader->allocator, 1, sizeof(struct get_request_context));

    if (!ctx) {
        return NULL;
    }

    ctx->downloader = downloader;
    aws_mutex_init(&ctx->mutex);
    aws_condition_variable_init(&ctx->condition_variable);
    ctx->request_complete = false;
    ctx->buffer = NULL;
    ctx->buffer_size = 0;
    ctx->buffer_capacity = 0;
    ctx->error_code = 0;
    ctx->error_message[0] = '\0';

    return ctx;
}

// Clean up request context
static void get_request_context_destroy(struct get_request_context *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->buffer) {
        aws_mem_release(ctx->downloader->allocator, ctx->buffer);
    }

    aws_condition_variable_clean_up(&ctx->condition_variable);
    aws_mutex_clean_up(&ctx->mutex);
    aws_mem_release(ctx->downloader->allocator, ctx);
}

// Callback: Parse response headers
static int s3_get_headers_callback(
    struct aws_s3_meta_request *meta_request,
    const struct aws_http_headers *headers,
    int response_status,
    void *user_data
) {
    (void)meta_request;
    struct get_request_context *ctx = user_data;

    // Check HTTP status
    if (response_status < 200 || response_status >= 300) {
        snprintf(ctx->error_message, sizeof(ctx->error_message),
                "HTTP error: status code %d", response_status);
        ctx->error_code = -1;
        return AWS_OP_SUCCESS;  // Continue to get error body
    }

    // Parse Content-Length header
    struct aws_byte_cursor content_length_name = aws_byte_cursor_from_c_str("Content-Length");
    struct aws_byte_cursor content_length_value;
    if (aws_http_headers_get(headers, content_length_name, &content_length_value) == AWS_OP_SUCCESS) {
        char value_str[64];
        size_t copy_len = content_length_value.len < sizeof(value_str) - 1
                         ? content_length_value.len
                         : sizeof(value_str) - 1;
        memcpy(value_str, content_length_value.ptr, copy_len);
        value_str[copy_len] = '\0';
        ctx->content_length = strtoull(value_str, NULL, 10);
    }

    // Parse Content-Range header (if present)
    // Format: "bytes START-END/TOTAL" or "bytes */TOTAL"
    struct aws_byte_cursor content_range_name = aws_byte_cursor_from_c_str("Content-Range");
    struct aws_byte_cursor content_range_value;
    if (aws_http_headers_get(headers, content_range_name, &content_range_value) == AWS_OP_SUCCESS) {
        char range_str[128];
        size_t copy_len = content_range_value.len < sizeof(range_str) - 1
                         ? content_range_value.len
                         : sizeof(range_str) - 1;
        memcpy(range_str, content_range_value.ptr, copy_len);
        range_str[copy_len] = '\0';

        // Parse: "bytes START-END/TOTAL"
        if (sscanf(range_str, "bytes %llu-%llu/%llu",
                   (unsigned long long *)&ctx->range_start,
                   (unsigned long long *)&ctx->range_end,
                   (unsigned long long *)&ctx->total_size) == 3) {
            // Successfully parsed range
        } else if (sscanf(range_str, "bytes */%llu",
                          (unsigned long long *)&ctx->total_size) == 1) {
            // Unsatisfiable range (416 response) - just got total size
            ctx->range_start = 0;
            ctx->range_end = 0;
        }
    }

    return AWS_OP_SUCCESS;
}

// Callback: Accumulate response body data
static int s3_get_body_callback(
    struct aws_s3_meta_request *meta_request,
    const struct aws_byte_cursor *body,
    uint64_t range_start,
    void *user_data
) {
    (void)meta_request;
    (void)range_start;

    struct get_request_context *ctx = user_data;

    // Allocate or expand buffer as needed
    size_t new_size = ctx->buffer_size + body->len;
    if (new_size > ctx->buffer_capacity) {
        size_t new_capacity = ctx->buffer_capacity == 0 ? 4096 : ctx->buffer_capacity * 2;
        while (new_capacity < new_size) {
            new_capacity *= 2;
        }

        void *buffer_ptr = ctx->buffer;
        if (aws_mem_realloc(ctx->downloader->allocator, &buffer_ptr, ctx->buffer_capacity, new_capacity) != AWS_OP_SUCCESS) {
            snprintf(ctx->error_message, sizeof(ctx->error_message),
                    "Failed to allocate buffer (%zu bytes)", new_capacity);
            ctx->error_code = -1;
            return AWS_OP_ERR;
        }

        ctx->buffer = buffer_ptr;
        ctx->buffer_capacity = new_capacity;
    }

    // Append data to buffer
    memcpy(ctx->buffer + ctx->buffer_size, body->ptr, body->len);
    ctx->buffer_size += body->len;

    return AWS_OP_SUCCESS;
}

// Callback: Request completion
static void s3_get_finish_callback(
    struct aws_s3_meta_request *meta_request,
    const struct aws_s3_meta_request_result *result,
    void *user_data
) {
    (void)meta_request;

    struct get_request_context *ctx = user_data;

    aws_mutex_lock(&ctx->mutex);

    if (result->error_code != AWS_ERROR_SUCCESS) {
        ctx->error_code = result->error_code;
        snprintf(ctx->error_message, sizeof(ctx->error_message),
                "S3 request failed: %s", aws_error_debug_str(result->error_code));
    }

    ctx->request_complete = true;
    aws_condition_variable_notify_one(&ctx->condition_variable);
    aws_mutex_unlock(&ctx->mutex);
}

// Wait for request to complete (with timeout)
static bool wait_for_completion(struct get_request_context *ctx, uint64_t timeout_ns) {
    aws_mutex_lock(&ctx->mutex);

    bool success = true;
    if (!ctx->request_complete) {
        if (aws_condition_variable_wait_for(&ctx->condition_variable, &ctx->mutex, timeout_ns) != AWS_OP_SUCCESS) {
            success = false;  // Timeout
        }
    }

    aws_mutex_unlock(&ctx->mutex);
    return success && ctx->request_complete;
}

// Get object size using HEAD request or negative range
int burst_downloader_get_object_size(struct burst_downloader *downloader) {
    if (!downloader || !downloader->s3_client) {
        fprintf(stderr, "Error: Invalid downloader\n");
        return -1;
    }

    // Create request context
    struct get_request_context *ctx = get_request_context_new(downloader);
    if (!ctx) {
        fprintf(stderr, "Error: Failed to allocate request context\n");
        return -1;
    }

    // Build HTTP message for HEAD request
    struct aws_http_message *message = aws_http_message_new_request(downloader->allocator);
    if (!message) {
        fprintf(stderr, "Error: Failed to create HTTP message\n");
        get_request_context_destroy(ctx);
        return -1;
    }

    // Set method to HEAD
    aws_http_message_set_request_method(message, aws_http_method_head);

    // Set path: /KEY
    struct aws_byte_buf path_buf;
    aws_byte_buf_init(&path_buf, downloader->allocator, strlen(downloader->key) + 2);
    aws_byte_buf_append_byte_dynamic(&path_buf, '/');
    struct aws_byte_cursor key_cursor = aws_byte_cursor_from_c_str(downloader->key);
    aws_byte_buf_append_dynamic(&path_buf, &key_cursor);
    struct aws_byte_cursor path_cursor = aws_byte_cursor_from_buf(&path_buf);
    aws_http_message_set_request_path(message, path_cursor);

    // Set Host header: BUCKET.s3.REGION.amazonaws.com
    char host_value[256];
    snprintf(host_value, sizeof(host_value), "%s.s3.%s.amazonaws.com",
             downloader->bucket, downloader->region);
    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_c_str("Host"),
        .value = aws_byte_cursor_from_c_str(host_value),
    };
    aws_http_message_add_header(message, host_header);

    // Create meta request options for HEAD
    struct aws_s3_meta_request_options request_options = {
        .type = AWS_S3_META_REQUEST_TYPE_DEFAULT,
        .operation_name = aws_byte_cursor_from_c_str("HeadObject"),
        .message = message,
        .user_data = ctx,
        .headers_callback = s3_get_headers_callback,
        .finish_callback = s3_get_finish_callback,
    };

    // Make the request
    ctx->meta_request = aws_s3_client_make_meta_request(downloader->s3_client, &request_options);
    aws_http_message_release(message);
    aws_byte_buf_clean_up(&path_buf);

    if (!ctx->meta_request) {
        fprintf(stderr, "Error: Failed to create meta request: %s\n",
                aws_error_debug_str(aws_last_error()));
        get_request_context_destroy(ctx);
        return -1;
    }

    // Wait for completion (60 second timeout)
    uint64_t timeout_ns = 60 * 1000 * 1000 * 1000ULL;
    if (!wait_for_completion(ctx, timeout_ns)) {
        fprintf(stderr, "Error: HEAD request timed out\n");
        aws_s3_meta_request_release(ctx->meta_request);
        get_request_context_destroy(ctx);
        return -1;
    }

    // Check for errors
    if (ctx->error_code != 0) {
        fprintf(stderr, "Error: %s\n", ctx->error_message);
        aws_s3_meta_request_release(ctx->meta_request);
        get_request_context_destroy(ctx);
        return -1;
    }

    // Store object size (from Content-Length or Content-Range)
    downloader->object_size = ctx->total_size > 0 ? ctx->total_size : ctx->content_length;

    // Clean up
    aws_s3_meta_request_release(ctx->meta_request);
    get_request_context_destroy(ctx);

    return 0;
}

// Test range GET - download arbitrary byte range
int burst_downloader_test_range_get(
    struct burst_downloader *downloader,
    uint64_t start,
    uint64_t end,
    uint8_t **out_buffer,
    size_t *out_size
) {
    if (!downloader || !downloader->s3_client || !out_buffer || !out_size) {
        fprintf(stderr, "Error: Invalid parameters\n");
        return -1;
    }

    // Create request context
    struct get_request_context *ctx = get_request_context_new(downloader);
    if (!ctx) {
        fprintf(stderr, "Error: Failed to allocate request context\n");
        return -1;
    }

    // Build HTTP message for GET request
    struct aws_http_message *message = aws_http_message_new_request(downloader->allocator);
    if (!message) {
        fprintf(stderr, "Error: Failed to create HTTP message\n");
        get_request_context_destroy(ctx);
        return -1;
    }

    // Set method to GET
    aws_http_message_set_request_method(message, aws_http_method_get);

    // Set path: /KEY
    struct aws_byte_buf path_buf;
    aws_byte_buf_init(&path_buf, downloader->allocator, strlen(downloader->key) + 2);
    aws_byte_buf_append_byte_dynamic(&path_buf, '/');
    struct aws_byte_cursor key_cursor = aws_byte_cursor_from_c_str(downloader->key);
    aws_byte_buf_append_dynamic(&path_buf, &key_cursor);
    struct aws_byte_cursor path_cursor = aws_byte_cursor_from_buf(&path_buf);
    aws_http_message_set_request_path(message, path_cursor);

    // Set Host header: BUCKET.s3.REGION.amazonaws.com
    char host_value[256];
    snprintf(host_value, sizeof(host_value), "%s.s3.%s.amazonaws.com",
             downloader->bucket, downloader->region);
    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_c_str("Host"),
        .value = aws_byte_cursor_from_c_str(host_value),
    };
    aws_http_message_add_header(message, host_header);

    // Set Range header: bytes=START-END
    char range_value[128];
    snprintf(range_value, sizeof(range_value), "bytes=%llu-%llu",
             (unsigned long long)start, (unsigned long long)end);
    struct aws_http_header range_header = {
        .name = aws_byte_cursor_from_c_str("Range"),
        .value = aws_byte_cursor_from_c_str(range_value),
    };
    aws_http_message_add_header(message, range_header);

    // Create meta request options for GET
    struct aws_s3_meta_request_options request_options = {
        .type = AWS_S3_META_REQUEST_TYPE_GET_OBJECT,
        .message = message,
        .user_data = ctx,
        .headers_callback = s3_get_headers_callback,
        .body_callback = s3_get_body_callback,
        .finish_callback = s3_get_finish_callback,
    };

    // Make the request
    ctx->meta_request = aws_s3_client_make_meta_request(downloader->s3_client, &request_options);
    aws_http_message_release(message);
    aws_byte_buf_clean_up(&path_buf);

    if (!ctx->meta_request) {
        fprintf(stderr, "Error: Failed to create meta request: %s\n",
                aws_error_debug_str(aws_last_error()));
        get_request_context_destroy(ctx);
        return -1;
    }

    // Wait for completion (60 second timeout)
    uint64_t timeout_ns = 60 * 1000 * 1000 * 1000ULL;
    if (!wait_for_completion(ctx, timeout_ns)) {
        fprintf(stderr, "Error: GET request timed out\n");
        aws_s3_meta_request_release(ctx->meta_request);
        get_request_context_destroy(ctx);
        return -1;
    }

    // Check for errors
    if (ctx->error_code != 0) {
        fprintf(stderr, "Error: %s\n", ctx->error_message);
        aws_s3_meta_request_release(ctx->meta_request);
        get_request_context_destroy(ctx);
        return -1;
    }

    // Return buffer to caller (they must free it with aws_mem_release)
    *out_buffer = ctx->buffer;
    *out_size = ctx->buffer_size;

    // Don't free the buffer - caller owns it now
    ctx->buffer = NULL;
    ctx->buffer_capacity = 0;

    // Clean up
    aws_s3_meta_request_release(ctx->meta_request);
    get_request_context_destroy(ctx);

    return 0;
}

// ============================================================================
// Phase 4: Central Directory Fetch and Part Streaming
// ============================================================================

// Part size constant (8 MiB)
#define PART_SIZE (8ULL * 1024 * 1024)

// Fetch central directory part using suffix-length Range header
int burst_downloader_fetch_cd_part(
    struct burst_downloader *downloader,
    uint8_t **out_buffer,
    size_t *out_size,
    uint64_t *out_start_offset,
    uint64_t *out_total_size
) {
    if (!downloader || !downloader->s3_client || !out_buffer || !out_size ||
        !out_start_offset || !out_total_size) {
        fprintf(stderr, "Error: Invalid parameters\n");
        return -1;
    }

    // Create request context
    struct get_request_context *ctx = get_request_context_new(downloader);
    if (!ctx) {
        fprintf(stderr, "Error: Failed to allocate request context\n");
        return -1;
    }

    // Build HTTP message for GET request
    struct aws_http_message *message = aws_http_message_new_request(downloader->allocator);
    if (!message) {
        fprintf(stderr, "Error: Failed to create HTTP message\n");
        get_request_context_destroy(ctx);
        return -1;
    }

    // Set method to GET
    aws_http_message_set_request_method(message, aws_http_method_get);

    // Set path: /KEY
    struct aws_byte_buf path_buf;
    aws_byte_buf_init(&path_buf, downloader->allocator, strlen(downloader->key) + 2);
    aws_byte_buf_append_byte_dynamic(&path_buf, '/');
    struct aws_byte_cursor key_cursor = aws_byte_cursor_from_c_str(downloader->key);
    aws_byte_buf_append_dynamic(&path_buf, &key_cursor);
    struct aws_byte_cursor path_cursor = aws_byte_cursor_from_buf(&path_buf);
    aws_http_message_set_request_path(message, path_cursor);

    // Set Host header: BUCKET.s3.REGION.amazonaws.com
    char host_value[256];
    snprintf(host_value, sizeof(host_value), "%s.s3.%s.amazonaws.com",
             downloader->bucket, downloader->region);
    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_c_str("Host"),
        .value = aws_byte_cursor_from_c_str(host_value),
    };
    aws_http_message_add_header(message, host_header);

    // Set Range header using suffix-length syntax: bytes=-8388608 (last 8 MiB)
    struct aws_http_header range_header = {
        .name = aws_byte_cursor_from_c_str("Range"),
        .value = aws_byte_cursor_from_c_str("bytes=-8388608"),
    };
    aws_http_message_add_header(message, range_header);

    // Create meta request options for GET
    struct aws_s3_meta_request_options request_options = {
        .type = AWS_S3_META_REQUEST_TYPE_GET_OBJECT,
        .message = message,
        .user_data = ctx,
        .headers_callback = s3_get_headers_callback,
        .body_callback = s3_get_body_callback,
        .finish_callback = s3_get_finish_callback,
    };

    // Make the request
    ctx->meta_request = aws_s3_client_make_meta_request(downloader->s3_client, &request_options);
    aws_http_message_release(message);
    aws_byte_buf_clean_up(&path_buf);

    if (!ctx->meta_request) {
        fprintf(stderr, "Error: Failed to create meta request: %s\n",
                aws_error_debug_str(aws_last_error()));
        get_request_context_destroy(ctx);
        return -1;
    }

    // Wait for completion (120 second timeout for potentially large download)
    uint64_t timeout_ns = 120ULL * 1000 * 1000 * 1000;
    if (!wait_for_completion(ctx, timeout_ns)) {
        fprintf(stderr, "Error: CD fetch request timed out\n");
        aws_s3_meta_request_release(ctx->meta_request);
        get_request_context_destroy(ctx);
        return -1;
    }

    // Check for errors
    if (ctx->error_code != 0) {
        fprintf(stderr, "Error: %s\n", ctx->error_message);
        aws_s3_meta_request_release(ctx->meta_request);
        get_request_context_destroy(ctx);
        return -1;
    }

    // Return results to caller
    *out_buffer = ctx->buffer;
    *out_size = ctx->buffer_size;
    *out_start_offset = ctx->range_start;
    *out_total_size = ctx->total_size;

    // Don't free the buffer - caller owns it now
    ctx->buffer = NULL;
    ctx->buffer_capacity = 0;

    // Clean up
    aws_s3_meta_request_release(ctx->meta_request);
    get_request_context_destroy(ctx);

    return 0;
}

// ============================================================================
// Streaming Part Download
// ============================================================================

// Context structure for streaming downloads
struct stream_part_context {
    struct burst_downloader *downloader;
    struct part_processor_state *processor;

    // Synchronization
    struct aws_mutex mutex;
    struct aws_condition_variable condition_variable;
    bool request_complete;

    // Error tracking
    int error_code;
    char error_message[256];

    // Meta request handle
    struct aws_s3_meta_request *meta_request;
};

// Initialize stream context
static struct stream_part_context *stream_part_context_new(
    struct burst_downloader *downloader,
    struct part_processor_state *processor
) {
    struct stream_part_context *ctx =
        aws_mem_calloc(downloader->allocator, 1, sizeof(struct stream_part_context));

    if (!ctx) {
        return NULL;
    }

    ctx->downloader = downloader;
    ctx->processor = processor;
    aws_mutex_init(&ctx->mutex);
    aws_condition_variable_init(&ctx->condition_variable);
    ctx->request_complete = false;
    ctx->error_code = 0;
    ctx->error_message[0] = '\0';

    return ctx;
}

// Clean up stream context
static void stream_part_context_destroy(struct stream_part_context *ctx) {
    if (!ctx) {
        return;
    }

    aws_condition_variable_clean_up(&ctx->condition_variable);
    aws_mutex_clean_up(&ctx->mutex);
    aws_mem_release(ctx->downloader->allocator, ctx);
}

// Streaming body callback - feeds chunks directly to stream processor
static int s3_stream_body_callback(
    struct aws_s3_meta_request *meta_request,
    const struct aws_byte_cursor *body,
    uint64_t range_start,
    void *user_data
) {
    (void)meta_request;
    (void)range_start;

    struct stream_part_context *ctx = user_data;

    // Feed chunk directly to stream processor
    int rc = part_processor_process_data(ctx->processor, body->ptr, body->len);
    if (rc != STREAM_PROC_SUCCESS) {
        ctx->error_code = rc;
        snprintf(ctx->error_message, sizeof(ctx->error_message),
                 "Stream processor error: %s", part_processor_get_error(ctx->processor));
        return AWS_OP_ERR;  // Abort request
    }

    return AWS_OP_SUCCESS;
}

// Streaming headers callback - just check HTTP status
static int s3_stream_headers_callback(
    struct aws_s3_meta_request *meta_request,
    const struct aws_http_headers *headers,
    int response_status,
    void *user_data
) {
    (void)meta_request;
    (void)headers;

    struct stream_part_context *ctx = user_data;

    // Check HTTP status
    if (response_status < 200 || response_status >= 300) {
        snprintf(ctx->error_message, sizeof(ctx->error_message),
                "HTTP error: status code %d", response_status);
        ctx->error_code = -1;
    }

    return AWS_OP_SUCCESS;
}

// Streaming finish callback
static void s3_stream_finish_callback(
    struct aws_s3_meta_request *meta_request,
    const struct aws_s3_meta_request_result *result,
    void *user_data
) {
    (void)meta_request;

    struct stream_part_context *ctx = user_data;

    aws_mutex_lock(&ctx->mutex);

    if (result->error_code != AWS_ERROR_SUCCESS && ctx->error_code == 0) {
        ctx->error_code = result->error_code;
        snprintf(ctx->error_message, sizeof(ctx->error_message),
                "S3 request failed: %s", aws_error_debug_str(result->error_code));
    }

    ctx->request_complete = true;
    aws_condition_variable_notify_one(&ctx->condition_variable);
    aws_mutex_unlock(&ctx->mutex);
}

// Wait for streaming completion (with timeout)
static bool wait_for_stream_completion(struct stream_part_context *ctx, uint64_t timeout_ns) {
    aws_mutex_lock(&ctx->mutex);

    bool success = true;
    if (!ctx->request_complete) {
        if (aws_condition_variable_wait_for(&ctx->condition_variable, &ctx->mutex, timeout_ns) != AWS_OP_SUCCESS) {
            success = false;  // Timeout
        }
    }

    aws_mutex_unlock(&ctx->mutex);
    return success && ctx->request_complete;
}

// Stream a single 8 MiB part through the stream processor
int burst_downloader_stream_part(
    struct burst_downloader *downloader,
    uint32_t part_index,
    struct part_processor_state *processor
) {
    if (!downloader || !downloader->s3_client || !processor) {
        fprintf(stderr, "Error: Invalid parameters\n");
        return -1;
    }

    // Calculate byte range for this part
    uint64_t start = (uint64_t)part_index * PART_SIZE;
    uint64_t end = start + PART_SIZE - 1;

    // Create stream context
    struct stream_part_context *ctx = stream_part_context_new(downloader, processor);
    if (!ctx) {
        fprintf(stderr, "Error: Failed to allocate stream context\n");
        return -1;
    }

    // Build HTTP message for GET request
    struct aws_http_message *message = aws_http_message_new_request(downloader->allocator);
    if (!message) {
        fprintf(stderr, "Error: Failed to create HTTP message\n");
        stream_part_context_destroy(ctx);
        return -1;
    }

    // Set method to GET
    aws_http_message_set_request_method(message, aws_http_method_get);

    // Set path: /KEY
    struct aws_byte_buf path_buf;
    aws_byte_buf_init(&path_buf, downloader->allocator, strlen(downloader->key) + 2);
    aws_byte_buf_append_byte_dynamic(&path_buf, '/');
    struct aws_byte_cursor key_cursor = aws_byte_cursor_from_c_str(downloader->key);
    aws_byte_buf_append_dynamic(&path_buf, &key_cursor);
    struct aws_byte_cursor path_cursor = aws_byte_cursor_from_buf(&path_buf);
    aws_http_message_set_request_path(message, path_cursor);

    // Set Host header: BUCKET.s3.REGION.amazonaws.com
    char host_value[256];
    snprintf(host_value, sizeof(host_value), "%s.s3.%s.amazonaws.com",
             downloader->bucket, downloader->region);
    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_c_str("Host"),
        .value = aws_byte_cursor_from_c_str(host_value),
    };
    aws_http_message_add_header(message, host_header);

    // Set Range header: bytes=START-END
    char range_value[128];
    snprintf(range_value, sizeof(range_value), "bytes=%llu-%llu",
             (unsigned long long)start, (unsigned long long)end);
    struct aws_http_header range_header = {
        .name = aws_byte_cursor_from_c_str("Range"),
        .value = aws_byte_cursor_from_c_str(range_value),
    };
    aws_http_message_add_header(message, range_header);

    // Create meta request options for GET with streaming callbacks
    struct aws_s3_meta_request_options request_options = {
        .type = AWS_S3_META_REQUEST_TYPE_GET_OBJECT,
        .message = message,
        .user_data = ctx,
        .headers_callback = s3_stream_headers_callback,
        .body_callback = s3_stream_body_callback,
        .finish_callback = s3_stream_finish_callback,
    };

    // Make the request
    ctx->meta_request = aws_s3_client_make_meta_request(downloader->s3_client, &request_options);
    aws_http_message_release(message);
    aws_byte_buf_clean_up(&path_buf);

    if (!ctx->meta_request) {
        fprintf(stderr, "Error: Failed to create meta request: %s\n",
                aws_error_debug_str(aws_last_error()));
        stream_part_context_destroy(ctx);
        return -1;
    }

    // Wait for completion (120 second timeout for 8 MiB part)
    uint64_t timeout_ns = 120ULL * 1000 * 1000 * 1000;
    if (!wait_for_stream_completion(ctx, timeout_ns)) {
        fprintf(stderr, "Error: Stream request timed out\n");
        aws_s3_meta_request_release(ctx->meta_request);
        stream_part_context_destroy(ctx);
        return -1;
    }

    // Check for errors
    int result = 0;
    if (ctx->error_code != 0) {
        fprintf(stderr, "Error: %s\n", ctx->error_message);
        result = ctx->error_code;
    }

    // Clean up
    aws_s3_meta_request_release(ctx->meta_request);
    stream_part_context_destroy(ctx);

    return result;
}
