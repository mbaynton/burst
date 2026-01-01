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
    size_t max_concurrent_parts;  // Max concurrent part downloads (default: 8)
    uint64_t part_size;  // Part size in bytes (8-64 MiB, must be multiple of 8 MiB)
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
    size_t max_concurrent_parts,  // Max concurrent part downloads (1-16, default: 8)
    uint64_t part_size,  // Part size in bytes (8-64 MiB, must be multiple of 8 MiB)
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

// Forward declaration for central directory parse result
struct central_dir_parse_result;

/**
 * Calculate how many parts need to be downloaded from S3.
 *
 * The CD buffer contains the last 8 MiB of the archive. Parts that start
 * within the CD buffer can be processed from the buffer instead of downloading.
 * With larger part sizes (e.g., 16 MiB), the final part may start before the
 * CD buffer begins, in which case it must be downloaded from S3.
 *
 * @param num_parts      Total number of parts in the archive
 * @param part_size      Size of each part in bytes
 * @param cd_start       Offset where CD buffer starts (archive_size - 8 MiB)
 * @param parts_to_download  Output: number of parts to download from S3
 * @param process_final_from_buffer  Output: whether final part comes from buffer
 */
void calculate_parts_to_download(
    size_t num_parts,
    uint64_t part_size,
    uint64_t cd_start,
    size_t *parts_to_download,
    bool *process_final_from_buffer
);

/**
 * Extract BURST archive using concurrent part downloads.
 * Downloads multiple 8 MiB parts concurrently using AWS SDK's async model.
 * Uses max_concurrent_parts from downloader config (default: 8).
 *
 * @param downloader  Initialized downloader
 * @param cd_result   Parsed central directory
 * @param cd_buffer   Buffer containing central directory data
 * @param cd_size     Size of cd_buffer
 * @param cd_start    Archive offset where cd_buffer starts
 * @return 0 on success, non-zero on error
 */
int burst_downloader_extract_concurrent(
    struct burst_downloader *downloader,
    struct central_dir_parse_result *cd_result,
    uint8_t *cd_buffer,
    size_t cd_size,
    uint64_t cd_start
);

#endif // BURST_DOWNLOADER_H
