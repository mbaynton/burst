#ifndef BURST_DOWNLOADER_H
#define BURST_DOWNLOADER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct burst_downloader {
    // AWS components
    struct aws_allocator *allocator;
    struct aws_event_loop_group *event_loop_group;
    struct aws_host_resolver *host_resolver;
    struct aws_client_bootstrap *client_bootstrap;
    struct aws_credentials_provider *credentials_provider;
    struct aws_s3_client *s3_client;
    struct aws_tls_ctx *tls_ctx;  // Required for SSO provider

    // S3 object info
    char *bucket;
    char *key;
    char *region;
    uint64_t object_size;

    // Configuration
    size_t max_concurrent_connections;
    char *output_dir;
    char *profile_name;  // AWS profile name for SSO and credentials
};

// Create/destroy
struct burst_downloader *burst_downloader_create(
    const char *bucket,
    const char *key,
    const char *region,
    const char *output_dir,
    size_t max_connections,
    const char *profile_name  // Can be NULL
);
void burst_downloader_destroy(struct burst_downloader *downloader);

// Phase 1 test functions
int burst_downloader_get_object_size(struct burst_downloader *downloader);
int burst_downloader_test_range_get(
    struct burst_downloader *downloader,
    uint64_t start,
    uint64_t end,
    uint8_t **out_buffer,
    size_t *out_size
);

// Forward declarations for stream processor
struct part_processor_state;

/**
 * Extract BURST archive from S3 to local filesystem.
 * This is the main entry point for Phase 4.
 *
 * @param downloader Initialized downloader with bucket, key, region, output_dir
 * @return 0 on success, non-zero on error
 */
int burst_downloader_extract(struct burst_downloader *downloader);

/**
 * Fetch the central directory part using suffix-length Range header.
 * Sends Range: bytes=-8388608 to get last 8 MiB (or entire file if smaller).
 * Parses Content-Range response header to determine total object size.
 * Returns buffer that must be freed with aws_mem_release().
 *
 * @param downloader      Initialized downloader
 * @param out_buffer      Returns the fetched data
 * @param out_size        Returns the size of fetched data
 * @param out_start_offset Returns the archive offset where buffer starts
 * @param out_total_size  Returns the total object size (from Content-Range)
 * @return 0 on success, non-zero on error
 */
int burst_downloader_fetch_cd_part(
    struct burst_downloader *downloader,
    uint8_t **out_buffer,
    size_t *out_size,
    uint64_t *out_start_offset,
    uint64_t *out_total_size
);

/**
 * Stream a single 8 MiB part through the stream processor.
 * Downloads the part using range GET and feeds chunks directly to the processor.
 *
 * @param downloader  Initialized downloader
 * @param part_index  Index of the part to download (0-based)
 * @param processor   Initialized stream processor for this part
 * @return 0 on success, non-zero on error
 */
int burst_downloader_stream_part(
    struct burst_downloader *downloader,
    uint32_t part_index,
    struct part_processor_state *processor
);

#endif // BURST_DOWNLOADER_H
