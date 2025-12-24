#include "burst_writer.h"
#include "zip_structures.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>

// Build a local file header for a given filename
// Returns allocated buffer containing header + filename, caller must free
// Sets *lfh_len_out to total size
static struct zip_local_header* build_local_file_header(const char *filename, int *lfh_len_out) {
    // Get current time
    time_t now = time(NULL);
    uint16_t mod_time, mod_date;
    dos_datetime_from_time_t(now, &mod_time, &mod_date);

    // Calculate sizes
    size_t filename_len = strlen(filename);
    size_t total_size = sizeof(struct zip_local_header) + filename_len;

    // Allocate buffer for header + filename
    struct zip_local_header *lfh = malloc(total_size);
    if (!lfh) {
        return NULL;
    }

    // Fill in header
    memset(lfh, 0, sizeof(struct zip_local_header));
    lfh->signature = ZIP_LOCAL_FILE_HEADER_SIG;
    lfh->version_needed = ZIP_VERSION_ZSTD;  // Phase 3: Always Zstandard
    lfh->flags = ZIP_FLAG_DATA_DESCRIPTOR;
    lfh->compression_method = ZIP_METHOD_ZSTD;
    lfh->last_mod_time = mod_time;
    lfh->last_mod_date = mod_date;
    lfh->crc32 = 0;  // Will be in data descriptor
    lfh->compressed_size = 0;  // Will be in data descriptor
    lfh->uncompressed_size = 0;  // Will be in data descriptor
    lfh->filename_length = filename_len;
    lfh->extra_field_length = 0;

    // Copy filename immediately after header
    memcpy((uint8_t*)lfh + sizeof(struct zip_local_header), filename, filename_len);

    *lfh_len_out = total_size;
    return lfh;
}

static void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS] -o OUTPUT_FILE INPUT_FILE...\n", program_name);
    printf("\nCreate a BURST-optimized ZIP archive.\n");
    printf("\nOptions:\n");
    printf("  -o, --output FILE     Output archive file (required)\n");
    printf("  -l, --level LEVEL     Zstandard compression level (-15 to 22, default: 3)\n");
    printf("                        Use 0 for uncompressed STORE method\n");
    printf("  -h, --help            Show this help message\n");
    printf("\nPhase 2: Zstandard compression with 128 KiB frames.\n");
    printf("Later phases will add 8 MiB alignment and ZIP64 support.\n");
}

int main(int argc, char **argv) {
    const char *output_path = NULL;
    int compression_level = 3;

    // Parse command-line options
    static struct option long_options[] = {
        {"output", required_argument, 0, 'o'},
        {"level", required_argument, 0, 'l'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "o:l:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'o':
                output_path = optarg;
                break;
            case 'l':
                compression_level = atoi(optarg);
                if (compression_level < -15 || compression_level > 22) {
                    fprintf(stderr, "Error: Compression level must be between -15 and 22\n");
                    return 1;
                }
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Check if output file and input files are provided
    if (!output_path) {
        fprintf(stderr, "Error: Output file required (-o)\n");
        print_usage(argv[0]);
        return 1;
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: At least one input file required\n");
        print_usage(argv[0]);
        return 1;
    }

    // Open output file
    FILE *output = fopen(output_path, "wb");
    if (!output) {
        perror("Failed to open output file");
        return 1;
    }

    // Create BURST writer
    printf("Creating BURST archive: %s\n", output_path);
    if (compression_level == 0) {
        printf("Compression level: 0 (using STORE method - uncompressed)\n");
    } else {
        printf("Compression level: %d (using Zstandard compression)\n", compression_level);
    }
    printf("\n");

    struct burst_writer *writer = burst_writer_create(output, compression_level);
    if (!writer) {
        fprintf(stderr, "Failed to create BURST writer\n");
        fclose(output);
        return 1;
    }

    // Add each input file
    int num_added = 0;
    for (int i = optind; i < argc; i++) {
        const char *input_path = argv[i];

        // Use basename for ZIP entry name
        const char *filename = strrchr(input_path, '/');
        filename = filename ? filename + 1 : input_path;

        // Open the file
        FILE *input = fopen(input_path, "rb");
        if (!input) {
            fprintf(stderr, "Failed to open file: %s\n", input_path);
            perror("fopen");
            continue;
        }

        // Build local file header for current file
        int lfh_len = 0;
        struct zip_local_header *lfh = build_local_file_header(filename, &lfh_len);
        if (!lfh) {
            fprintf(stderr, "Failed to build local file header\n");
            fclose(input);
            continue;
        }

        // Calculate next file's local header size
        int next_lfh_len = 0;
        if (i + 1 < argc) {  // Not the last file
            // Build next header just to get its size, then free it
            const char *next_input_path = argv[i + 1];
            const char *next_filename = strrchr(next_input_path, '/');
            next_filename = next_filename ? next_filename + 1 : next_input_path;

            struct zip_local_header *next_lfh = build_local_file_header(next_filename, &next_lfh_len);
            if (next_lfh) {
                free(next_lfh);
            }
            // If building next header failed, next_lfh_len remains 0 (safe fallback)
        }

        // Add the file
        if (burst_writer_add_file(writer, input, lfh, lfh_len, next_lfh_len) != 0) {
            fprintf(stderr, "Failed to add file: %s\n", input_path);
            free(lfh);
            fclose(input);
            // Continue with other files
        } else {
            num_added++;
        }

        // Clean up (writer has copied what it needs)
        free(lfh);
        fclose(input);
    }

    if (num_added == 0) {
        fprintf(stderr, "Error: No files were added to archive\n");
        burst_writer_destroy(writer);
        fclose(output);
        return 1;
    }

    // Finalize archive
    printf("\nFinalizing archive...\n");
    if (burst_writer_finalize(writer) != 0) {
        fprintf(stderr, "Failed to finalize archive\n");
        burst_writer_destroy(writer);
        fclose(output);
        return 1;
    }

    // Print statistics
    burst_writer_print_stats(writer);

    // Cleanup
    burst_writer_destroy(writer);
    fclose(output);

    printf("\nArchive created successfully: %s\n", output_path);
    printf("\nTest with: 7zz x %s\n", output_path);

    return 0;
}
