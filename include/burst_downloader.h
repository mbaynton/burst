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

#endif // BURST_DOWNLOADER_H
