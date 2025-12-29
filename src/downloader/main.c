#include "burst_downloader.h"
#include "s3_client.h"
#include "central_dir_parser.h"
#include "stream_processor.h"

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

int burst_downloader_extract(struct burst_downloader *downloader) {
    if (!downloader) {
        fprintf(stderr, "Error: NULL downloader\n");
        return -1;
    }

    int result = -1;
    struct central_dir_parse_result cd_result = {0};
    uint8_t *cd_buffer = NULL;
    size_t cd_size = 0;
    uint64_t cd_start = 0;
    uint64_t object_size = 0;

    // 1. Fetch central directory part (gets object size from Content-Range header)
    printf("Fetching central directory...\n");
    if (burst_downloader_fetch_cd_part(downloader, &cd_buffer, &cd_size, &cd_start, &object_size) != 0) {
        fprintf(stderr, "Failed to fetch central directory\n");
        goto cleanup;
    }
    printf("Object size: %llu bytes (fetched %zu bytes starting at offset %llu)\n",
           (unsigned long long)object_size, cd_size, (unsigned long long)cd_start);

    // 2. Parse central directory
    printf("Parsing central directory...\n");
    int parse_rc = central_dir_parse(cd_buffer, cd_size, object_size, &cd_result);
    if (parse_rc != CENTRAL_DIR_PARSE_SUCCESS) {
        fprintf(stderr, "Failed to parse central directory: %s\n", cd_result.error_message);
        goto cleanup;
    }
    printf("Found %zu files in %zu parts\n", cd_result.num_files, cd_result.num_parts);

    // 3. Process parts 0 to num_parts-2 (all except final part)
    for (uint32_t part_idx = 0; part_idx + 1 < cd_result.num_parts; part_idx++) {
        printf("Processing part %u/%zu...\n", part_idx + 1, cd_result.num_parts);

        struct part_processor_state *processor =
            part_processor_create(part_idx, &cd_result, downloader->output_dir);
        if (!processor) {
            fprintf(stderr, "Failed to create processor for part %u\n", part_idx);
            goto cleanup;
        }

        int stream_rc = burst_downloader_stream_part(downloader, part_idx, processor);
        if (stream_rc != 0) {
            fprintf(stderr, "Failed to stream part %u: %s\n",
                    part_idx, part_processor_get_error(processor));
            part_processor_destroy(processor);
            goto cleanup;
        }

        int finalize_rc = part_processor_finalize(processor);
        if (finalize_rc != STREAM_PROC_SUCCESS) {
            fprintf(stderr, "Failed to finalize part %u: %s\n",
                    part_idx, part_processor_get_error(processor));
            part_processor_destroy(processor);
            goto cleanup;
        }

        part_processor_destroy(processor);
    }

    // 4. Process final part from cd_buffer (if num_parts > 0)
    if (cd_result.num_parts > 0) {
        uint32_t final_idx = cd_result.num_parts - 1;
        printf("Processing final part %u/%zu from buffer...\n",
               final_idx + 1, cd_result.num_parts);

        struct part_processor_state *processor =
            part_processor_create(final_idx, &cd_result, downloader->output_dir);
        if (!processor) {
            fprintf(stderr, "Failed to create processor for final part\n");
            goto cleanup;
        }

        // Calculate data portion of final part
        uint64_t final_part_start = (uint64_t)final_idx * PART_SIZE;
        uint64_t data_end = cd_result.central_dir_offset;

        if (data_end > final_part_start) {
            size_t offset_in_buffer = final_part_start - cd_start;
            size_t data_size = data_end - final_part_start;

            int proc_rc = part_processor_process_data(
                processor, cd_buffer + offset_in_buffer, data_size);
            if (proc_rc != STREAM_PROC_SUCCESS) {
                fprintf(stderr, "Failed to process final part: %s\n",
                        part_processor_get_error(processor));
                part_processor_destroy(processor);
                goto cleanup;
            }
        }

        int finalize_rc = part_processor_finalize(processor);
        if (finalize_rc != STREAM_PROC_SUCCESS) {
            fprintf(stderr, "Failed to finalize final part: %s\n",
                    part_processor_get_error(processor));
            part_processor_destroy(processor);
            goto cleanup;
        }

        part_processor_destroy(processor);
    }

    printf("\nExtraction complete! %zu files extracted.\n", cd_result.num_files);
    result = 0;

cleanup:
    central_dir_parse_result_free(&cd_result);
    if (cd_buffer) {
        aws_mem_release(downloader->allocator, cd_buffer);
    }
    return result;
}

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

    printf("BURST Downloader\n");
    printf("================\n");
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

    printf("S3 client initialized.\n\n");

    // Run extraction
    int result = burst_downloader_extract(downloader);

    // Clean up
    burst_downloader_destroy(downloader);

    return result == 0 ? 0 : 1;
}
