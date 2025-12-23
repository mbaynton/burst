#include "burst_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS] -o OUTPUT_FILE INPUT_FILE...\n", program_name);
    printf("\nCreate a BURST-optimized ZIP archive.\n");
    printf("\nOptions:\n");
    printf("  -o, --output FILE     Output archive file (required)\n");
    printf("  -l, --level LEVEL     Zstandard compression level (-15 to 22, default: 3)\n");
    printf("  -h, --help            Show this help message\n");
    printf("\nPhase 1: Creates uncompressed ZIP archives for testing.\n");
    printf("Later phases will add Zstandard compression and 8 MiB alignment.\n");
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
    printf("Compression level: %d (Phase 1: using STORE method)\n", compression_level);
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

        if (burst_writer_add_file(writer, filename, input_path) != 0) {
            fprintf(stderr, "Failed to add file: %s\n", input_path);
            // Continue with other files
        } else {
            num_added++;
        }
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
    printf("\nTest with: unzip -t %s\n", output_path);

    return 0;
}
