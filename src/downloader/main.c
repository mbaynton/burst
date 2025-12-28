#include "burst_downloader.h"
#include "s3_client.h"

#include <aws/common/allocator.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("\nDownload and extract a BURST archive from S3.\n");
    printf("\nRequired Options:\n");
    printf("  -b, --bucket BUCKET       S3 bucket name\n");
    printf("  -k, --key KEY             S3 object key\n");
    printf("  -r, --region REGION       AWS region (e.g., us-east-1)\n");
    printf("  -o, --output-dir DIR      Output directory for extracted files\n");
    printf("\nOptional:\n");
    printf("  -c, --connections NUM     Max concurrent connections (default: 16)\n");
    printf("  -p, --profile PROFILE     AWS profile name (default: AWS_PROFILE env or 'default')\n");
    printf("  -h, --help                Show this help message\n");
    printf("\nAWS Credentials:\n");
    printf("  Uses standard AWS credential chain:\n");
    printf("    1. Environment variables (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY)\n");
    printf("    2. AWS SSO (requires 'aws sso login --profile <name>')\n");
    printf("    3. AWS config files (~/.aws/credentials, ~/.aws/config)\n");
    printf("    4. IAM role (for EC2 instances)\n");
    printf("\nPhase 1: Basic S3 GET requests with Range header support.\n");
}

// Create downloader structure and initialize AWS resources
struct burst_downloader *burst_downloader_create(
    const char *bucket,
    const char *key,
    const char *region,
    const char *output_dir,
    size_t max_connections,
    const char *profile_name
) {
    if (!bucket || !key || !region || !output_dir) {
        fprintf(stderr, "Error: All parameters required\n");
        return NULL;
    }

    struct burst_downloader *downloader = calloc(1, sizeof(struct burst_downloader));
    if (!downloader) {
        fprintf(stderr, "Error: Failed to allocate downloader\n");
        return NULL;
    }

    // Copy configuration strings
    downloader->bucket = strdup(bucket);
    downloader->key = strdup(key);
    downloader->region = strdup(region);
    downloader->output_dir = strdup(output_dir);
    downloader->profile_name = profile_name ? strdup(profile_name) : NULL;
    downloader->max_concurrent_connections = max_connections;
    downloader->object_size = 0;
    downloader->tls_ctx = NULL;

    if (!downloader->bucket || !downloader->key || !downloader->region || !downloader->output_dir) {
        fprintf(stderr, "Error: Failed to duplicate strings\n");
        burst_downloader_destroy(downloader);
        return NULL;
    }

    // Initialize S3 client
    if (s3_client_init(downloader) != 0) {
        fprintf(stderr, "Error: Failed to initialize S3 client\n");
        burst_downloader_destroy(downloader);
        return NULL;
    }

    return downloader;
}

void burst_downloader_destroy(struct burst_downloader *downloader) {
    if (!downloader) {
        return;
    }

    // Clean up S3 client first
    s3_client_cleanup(downloader);

    // Free allocated strings
    free(downloader->bucket);
    free(downloader->key);
    free(downloader->region);
    free(downloader->output_dir);
    free(downloader->profile_name);

    free(downloader);
}

// Phase 1 functions are implemented in s3_operations.c

int main(int argc, char **argv) {
    const char *bucket = NULL;
    const char *key = NULL;
    const char *region = NULL;
    const char *output_dir = NULL;
    const char *profile = NULL;
    size_t max_connections = 16;

    // Parse command-line options
    static struct option long_options[] = {
        {"bucket", required_argument, 0, 'b'},
        {"key", required_argument, 0, 'k'},
        {"region", required_argument, 0, 'r'},
        {"output-dir", required_argument, 0, 'o'},
        {"connections", required_argument, 0, 'c'},
        {"profile", required_argument, 0, 'p'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "b:k:r:o:c:p:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'b':
                bucket = optarg;
                break;
            case 'k':
                key = optarg;
                break;
            case 'r':
                region = optarg;
                break;
            case 'o':
                output_dir = optarg;
                break;
            case 'c':
                max_connections = atoi(optarg);
                if (max_connections < 1 || max_connections > 256) {
                    fprintf(stderr, "Error: Connections must be between 1 and 256\n");
                    return 1;
                }
                break;
            case 'p':
                profile = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Validate required arguments
    if (!bucket || !key || !region || !output_dir) {
        fprintf(stderr, "Error: All required arguments must be provided\n\n");
        print_usage(argv[0]);
        return 1;
    }

    printf("BURST Downloader - Phase 1 Test\n");
    printf("================================\n");
    printf("Bucket:      %s\n", bucket);
    printf("Key:         %s\n", key);
    printf("Region:      %s\n", region);
    printf("Output Dir:  %s\n", output_dir);
    printf("Connections: %zu\n", max_connections);
    printf("\n");

    // Profile resolution: CLI arg > AWS_PROFILE env > NULL (defaults to "default")
    if (!profile) {
        profile = getenv("AWS_PROFILE");
    }

    // Create downloader
    printf("Initializing AWS S3 client...\n");
    struct burst_downloader *downloader = burst_downloader_create(
        bucket, key, region, output_dir, max_connections, profile
    );

    if (!downloader) {
        fprintf(stderr, "Error: Failed to create downloader\n");
        return 1;
    }

    printf("Success! S3 client initialized.\n\n");

    // Phase 1 Test: Get object size
    printf("Phase 1 Test: Getting object size...\n");
    if (burst_downloader_get_object_size(downloader) == 0) {
        printf("✓ Object size: %llu bytes\n", (unsigned long long)downloader->object_size);
    } else {
        fprintf(stderr, "✗ Failed to get object size\n");
        burst_downloader_destroy(downloader);
        return 1;
    }

    // Phase 1 Test: Range GET (first 1024 bytes)
    printf("\nPhase 1 Test: Downloading first 1024 bytes...\n");
    uint8_t *buffer = NULL;
    size_t buffer_size = 0;
    if (burst_downloader_test_range_get(downloader, 0, 1023, &buffer, &buffer_size) == 0) {
        printf("✓ Downloaded %zu bytes\n", buffer_size);
        printf("  First 16 bytes (hex): ");
        for (size_t i = 0; i < 16 && i < buffer_size; i++) {
            printf("%02x ", buffer[i]);
        }
        printf("\n");

        // Free buffer using AWS allocator
        aws_mem_release(downloader->allocator, buffer);
    } else {
        fprintf(stderr, "✗ Failed to download range\n");
        burst_downloader_destroy(downloader);
        return 1;
    }

    printf("\n✓ Phase 1 tests completed successfully!\n");
    printf("  S3 client initialization: PASS\n");
    printf("  Object size detection: PASS\n");
    printf("  Range GET: PASS\n");

    // Clean up
    burst_downloader_destroy(downloader);

    return 0;
}
