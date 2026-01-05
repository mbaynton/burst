#include "burst_downloader.h"
#include "s3_client.h"
#include "central_dir_parser.h"
#include "stream_processor.h"
#include "cd_fetch.h"
#include "profiling.h"

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
    printf("  -c, --connections NUM     Max concurrent connections (0=auto, max: 256)\n");
    printf("  -n, --max-concurrent-parts NUM\n");
    printf("                            Max concurrent part downloads (1-128, default: 8)\n");
    printf("  -s, --part-size NUM       Part size in MiB (8-64, must be multiple of 8,\n");
    printf("                            default: 8)\n");
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
    size_t max_concurrent_parts,
    uint64_t part_size,
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
    downloader->max_concurrent_parts = max_concurrent_parts;
    downloader->part_size = part_size;
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

/**
 * Process a single-part archive entirely from the initial buffer.
 * Used when the entire archive fits within the 8 MiB tail buffer.
 */
static int process_single_part_archive(
    struct burst_downloader *downloader,
    struct central_dir_parse_result *cd_result,
    uint8_t *buffer,
    size_t buffer_size
) {
    printf("Processing single part from buffer...\n");

    struct part_processor_state *processor =
        part_processor_create(0, cd_result, downloader->output_dir,
                              downloader->part_size);
    if (!processor) {
        fprintf(stderr, "Failed to create processor for single part\n");
        return -1;
    }

    uint64_t data_end = cd_result->central_dir_offset;
    if (data_end > buffer_size) {
        data_end = buffer_size;
    }

    if (data_end > 0) {
        int proc_rc = part_processor_process_data(processor, buffer, data_end);
        if (proc_rc != STREAM_PROC_SUCCESS) {
            fprintf(stderr, "Failed to process single part: %s\n",
                    part_processor_get_error(processor));
            part_processor_destroy(processor);
            return -1;
        }
    }

    int finalize_rc = part_processor_finalize(processor);
    part_processor_destroy(processor);

    return finalize_rc == STREAM_PROC_SUCCESS ? 0 : -1;
}

int burst_downloader_extract(struct burst_downloader *downloader) {
    if (!downloader) {
        fprintf(stderr, "Error: NULL downloader\n");
        return -1;
    }

    int result = -1;
    struct central_dir_parse_result cd_result = {0};
    uint8_t *initial_buffer = NULL;
    size_t initial_size = 0;
    uint64_t initial_start = 0;
    uint64_t object_size = 0;

    // Additional buffers for large CD support
    uint8_t *assembled_cd_buffer = NULL;
    size_t assembled_cd_size = 0;
    struct cd_part_range *cd_ranges = NULL;
    size_t num_cd_ranges = 0;
    uint8_t **range_buffers = NULL;
    size_t *range_sizes = NULL;
    struct body_data_segment *body_segments = NULL;
    size_t num_body_segments = 0;

    // The buffer we'll use for CD parsing (either initial or assembled)
    uint8_t *cd_buffer = NULL;
    size_t cd_buffer_size = 0;

    // 1. Fetch initial tail buffer (last 8 MiB)
    printf("Fetching tail buffer...\n");
    if (burst_downloader_fetch_cd_part(downloader, &initial_buffer, &initial_size,
                                        &initial_start, &object_size) != 0) {
        fprintf(stderr, "Failed to fetch tail buffer\n");
        goto cleanup;
    }
    printf("Object size: %llu bytes (fetched %zu bytes starting at offset %llu)\n",
           (unsigned long long)object_size, initial_size, (unsigned long long)initial_start);

    // 2. Parse EOCD only to determine CD extent
    uint64_t central_dir_offset = 0;
    uint64_t central_dir_size = 0;
    uint64_t num_entries = 0;
    bool is_zip64 = false;
    uint32_t first_cdfh_offset_in_tail = 0;
    char eocd_error[256] = {0};
    bool used_assembled_buffer = false;

    printf("Parsing EOCD to determine central directory extent...\n");
    int eocd_rc = central_dir_parse_eocd_only(initial_buffer, initial_size, object_size,
                                               &central_dir_offset, &central_dir_size,
                                               &num_entries, &is_zip64,
                                               &first_cdfh_offset_in_tail, eocd_error);
    if (eocd_rc != CENTRAL_DIR_PARSE_SUCCESS) {
        fprintf(stderr, "Failed to parse EOCD: %s\n", eocd_error);
        goto cleanup;
    }

    printf("Central directory: offset=%llu size=%llu (%s)\n",
           (unsigned long long)central_dir_offset,
           (unsigned long long)central_dir_size,
           is_zip64 ? "ZIP64" : "standard");

    // 3. Check if we need additional fetches for large CD
    if (central_dir_offset < initial_start) {
        // CD extends before our initial buffer - need to fetch more
        printf("Central directory extends before tail buffer (need %.2f MiB more)\n",
               (double)(initial_start - central_dir_offset) / (1024 * 1024));

        // Calculate aligned ranges to fetch
        if (calculate_cd_fetch_ranges(central_dir_offset, central_dir_size,
                                       downloader->part_size, initial_start,
                                       &cd_ranges, &num_cd_ranges) != 0) {
            fprintf(stderr, "Failed to calculate CD fetch ranges\n");
            goto cleanup;
        }

        // Check for BURST EOCD comment - enables hybrid optimization path
        if (first_cdfh_offset_in_tail != 0 &&
            first_cdfh_offset_in_tail != BURST_EOCD_NO_CDFH_IN_TAIL &&
            num_cd_ranges > 0) {
            // Parse partial CD from tail buffer
            printf("Using partial CD optimization (first CDFH at offset %u in CD)...\n",
                   first_cdfh_offset_in_tail);

            struct central_dir_parse_result partial_cd = {0};
            int partial_rc = central_dir_parse_partial(
                initial_buffer, initial_size,
                initial_start, central_dir_offset,
                first_cdfh_offset_in_tail,
                object_size, downloader->part_size,
                is_zip64, &partial_cd);

            if (partial_rc == CENTRAL_DIR_PARSE_SUCCESS && partial_cd.num_files > 0) {
                printf("Partial CD: %zu files parsed from tail buffer\n", partial_cd.num_files);

                // Create hybrid coordinator for parallel CD fetch + part downloads
                struct hybrid_download_coordinator *coord =
                    hybrid_coordinator_create(downloader, &partial_cd,
                                              cd_ranges, num_cd_ranges,
                                              initial_buffer, initial_size,
                                              initial_start, object_size, is_zip64);

                if (coord) {
                    printf("Starting hybrid download (CD fetch + early parts)...\n");
                    result = hybrid_coordinator_run(coord);

                    hybrid_coordinator_destroy(coord);
                    central_dir_parse_result_free(&partial_cd);

                    if (result == 0) {
                        printf("\nExtraction complete!\n");
                    }

                    // Skip to cleanup - hybrid coordinator handles everything
                    goto cleanup;
                } else {
                    fprintf(stderr, "Failed to create hybrid coordinator, falling back to sequential path\n");
                    central_dir_parse_result_free(&partial_cd);
                }
            } else {
                if (partial_rc != CENTRAL_DIR_PARSE_SUCCESS) {
                    printf("Partial CD parse failed (error %d), using sequential path\n", partial_rc);
                } else {
                    printf("No files in partial CD, using sequential path\n");
                }
                central_dir_parse_result_free(&partial_cd);
            }
        }

        // Sequential path: fetch all CD ranges first, then download parts
        if (num_cd_ranges > 0) {
            printf("Fetching %zu additional range(s) for central directory...\n", num_cd_ranges);

            // Fetch ranges concurrently
            if (burst_downloader_fetch_cd_ranges(downloader, cd_ranges, num_cd_ranges,
                                                  downloader->max_concurrent_parts,
                                                  &range_buffers, &range_sizes) != 0) {
                fprintf(stderr, "Failed to fetch CD ranges\n");
                goto cleanup;
            }

            // Assemble into contiguous buffer
            if (assemble_cd_buffer(initial_buffer, initial_size, initial_start,
                                    cd_ranges, range_buffers, range_sizes, num_cd_ranges,
                                    central_dir_offset, central_dir_size,
                                    &assembled_cd_buffer, &assembled_cd_size,
                                    &body_segments, &num_body_segments) != 0) {
                fprintf(stderr, "Failed to assemble CD buffer\n");
                goto cleanup;
            }

            // Use assembled buffer for CD parsing
            cd_buffer = assembled_cd_buffer;
            cd_buffer_size = assembled_cd_size;
            used_assembled_buffer = true;

            printf("Assembled CD buffer: %zu bytes\n", cd_buffer_size);
        }
    }

    // If we didn't assemble a buffer, use the initial buffer
    if (cd_buffer == NULL) {
        cd_buffer = initial_buffer;
        cd_buffer_size = initial_size;
    }

    // 4. Parse full central directory
    printf("Parsing central directory...\n");

    // Calculate pointer to start of CD within whatever buffer we're using
    const uint8_t *cd_data;
    size_t cd_data_size;
    if (used_assembled_buffer) {
        // Assembled buffer starts exactly at central_dir_offset
        cd_data = cd_buffer;
        cd_data_size = cd_buffer_size;
    } else {
        // Initial buffer starts at initial_start, CD is at central_dir_offset
        size_t cd_offset_in_buffer = (size_t)(central_dir_offset - initial_start);
        cd_data = cd_buffer + cd_offset_in_buffer;
        cd_data_size = cd_buffer_size - cd_offset_in_buffer;
    }

    int parse_rc = central_dir_parse_from_cd_buffer(cd_data, cd_data_size,
                                                     central_dir_offset, central_dir_size,
                                                     object_size, downloader->part_size,
                                                     is_zip64, &cd_result);
    if (parse_rc != CENTRAL_DIR_PARSE_SUCCESS) {
        fprintf(stderr, "Failed to parse central directory: %s\n", cd_result.error_message);
        goto cleanup;
    }
    printf("Found %zu files in %zu parts\n", cd_result.num_files, cd_result.num_parts);

    // 5. Handle small archives (single part) separately
    if (cd_result.num_parts <= 1) {
        result = process_single_part_archive(downloader, &cd_result,
                                              initial_buffer, initial_size);
        if (result == 0) {
            printf("\nExtraction complete! %zu files extracted.\n", cd_result.num_files);
        }
        goto cleanup;
    }

    // 6. Add tail buffer body segment
    // The tail buffer may contain body data from initial_start to central_dir_offset
    if (add_tail_buffer_segment(&body_segments, &num_body_segments,
                                 initial_buffer, initial_size, initial_start,
                                 cd_result.central_dir_offset,
                                 downloader->part_size) != 0) {
        fprintf(stderr, "Failed to add tail buffer segment\n");
        goto cleanup;
    }

    if (num_body_segments > 0) {
        printf("Using %zu pre-fetched body segment(s)\n", num_body_segments);
    }

    // 7. Extract using concurrent part downloads
    printf("Extracting with up to %zu concurrent parts...\n", downloader->max_concurrent_parts);
    result = burst_downloader_extract_concurrent(
        downloader, &cd_result, body_segments, num_body_segments);

    if (result == 0) {
        printf("\nExtraction complete! %zu files extracted.\n", cd_result.num_files);
    }

#ifdef BURST_PROFILE
    burst_profile_finalize();
    printf("\n");
    burst_profile_print_stats();

    // Write JSON to output directory
    char json_path[1024];
    snprintf(json_path, sizeof(json_path), "%s/burst_profile.json", downloader->output_dir);
    if (burst_profile_write_json(json_path) == 0) {
        printf("\nProfile data written to: %s\n", json_path);
    }
#endif

cleanup:
    central_dir_parse_result_free(&cd_result);

    // Free body segments array (but not data pointers - they point into other buffers)
    free_body_segments(body_segments, num_body_segments);

    // Free assembled CD buffer if allocated
    if (assembled_cd_buffer) {
        free(assembled_cd_buffer);
    }

    // Free range buffers
    if (range_buffers) {
        for (size_t i = 0; i < num_cd_ranges; i++) {
            if (range_buffers[i]) {
                free(range_buffers[i]);
            }
        }
        free(range_buffers);
    }
    if (range_sizes) {
        free(range_sizes);
    }
    if (cd_ranges) {
        free(cd_ranges);
    }

    // Free initial buffer
    if (initial_buffer) {
        aws_mem_release(downloader->allocator, initial_buffer);
    }

    return result;
}

int main(int argc, char **argv) {
#ifdef BURST_PROFILE
    burst_profile_init();
#endif

    const char *bucket = NULL;
    const char *key = NULL;
    const char *region = NULL;
    const char *output_dir = NULL;
    const char *profile = NULL;
    size_t max_connections = 0;
    size_t max_concurrent_parts = 8;
    uint64_t part_size = 8 * 1024 * 1024;  // Default 8 MiB

    // Parse command-line options
    static struct option long_options[] = {
        {"bucket", required_argument, 0, 'b'},
        {"key", required_argument, 0, 'k'},
        {"region", required_argument, 0, 'r'},
        {"output-dir", required_argument, 0, 'o'},
        {"connections", required_argument, 0, 'c'},
        {"max-concurrent-parts", required_argument, 0, 'n'},
        {"part-size", required_argument, 0, 's'},
        {"profile", required_argument, 0, 'p'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "b:k:r:o:c:n:s:p:h", long_options, NULL)) != -1) {
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
                if (max_connections > 256) {
                    fprintf(stderr, "Error: Connections must be 0-256 (0=auto)\n");
                    return 1;
                }
                break;
            case 'n':
                max_concurrent_parts = atoi(optarg);
                if (max_concurrent_parts < 1 || max_concurrent_parts > 128) {
                    fprintf(stderr, "Error: Max concurrent parts must be between 1 and 128\n");
                    return 1;
                }
                break;
            case 's': {
                int part_size_mib = atoi(optarg);
                if (part_size_mib < 8 || part_size_mib > 64 || (part_size_mib % 8) != 0) {
                    fprintf(stderr, "Error: Part size must be a multiple of 8 between 8 and 64\n");
                    return 1;
                }
                part_size = (uint64_t)part_size_mib * 1024 * 1024;
                break;
            }
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
    printf("Concurrent Parts: %zu\n", max_concurrent_parts);
    printf("Part Size:   %llu MiB\n", (unsigned long long)(part_size / (1024 * 1024)));
    printf("\n");

    // Profile resolution: CLI arg > AWS_PROFILE env > NULL (defaults to "default")
    if (!profile) {
        profile = getenv("AWS_PROFILE");
    }

    // Create downloader
    printf("Initializing AWS S3 client...\n");
    struct burst_downloader *downloader = burst_downloader_create(
        bucket, key, region, output_dir, max_connections, max_concurrent_parts,
        part_size, profile
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
