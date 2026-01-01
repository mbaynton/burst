#include "burst_writer.h"
#include "zip_structures.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

// Dynamic array of file paths for directory recursion
struct file_list {
    char **paths;      // Array of file paths (full paths)
    char **names;      // Array of archive names (relative paths)
    size_t count;
    size_t capacity;
};

static struct file_list *file_list_create(void) {
    struct file_list *list = malloc(sizeof(struct file_list));
    if (!list) return NULL;
    list->capacity = 64;
    list->count = 0;
    list->paths = malloc(list->capacity * sizeof(char *));
    list->names = malloc(list->capacity * sizeof(char *));
    if (!list->paths || !list->names) {
        free(list->paths);
        free(list->names);
        free(list);
        return NULL;
    }
    return list;
}

static void file_list_destroy(struct file_list *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->paths[i]);
        free(list->names[i]);
    }
    free(list->paths);
    free(list->names);
    free(list);
}

static int file_list_add(struct file_list *list, const char *path, const char *name) {
    if (list->count >= list->capacity) {
        size_t new_capacity = list->capacity * 2;
        char **new_paths = realloc(list->paths, new_capacity * sizeof(char *));
        char **new_names = realloc(list->names, new_capacity * sizeof(char *));
        if (!new_paths || !new_names) {
            return -1;
        }
        list->paths = new_paths;
        list->names = new_names;
        list->capacity = new_capacity;
    }
    list->paths[list->count] = strdup(path);
    list->names[list->count] = strdup(name);
    if (!list->paths[list->count] || !list->names[list->count]) {
        free(list->paths[list->count]);
        free(list->names[list->count]);
        return -1;
    }
    list->count++;
    return 0;
}

// Check if path is a directory
static int is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

// Check if path is a regular file
static int is_regular_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISREG(st.st_mode);
}

// Recursively collect files from a directory
// base_dir: the original directory path (for calculating relative names)
// current_dir: the current directory being scanned
// prefix: the relative path prefix for archive names
static int collect_files_recursive(struct file_list *list,
                                   const char *current_dir,
                                   const char *prefix) {
    DIR *dir = opendir(current_dir);
    if (!dir) {
        fprintf(stderr, "Warning: Cannot open directory: %s (%s)\n",
                current_dir, strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Build full path
        size_t path_len = strlen(current_dir) + 1 + strlen(entry->d_name) + 1;
        char *full_path = malloc(path_len);
        if (!full_path) {
            closedir(dir);
            return -1;
        }
        snprintf(full_path, path_len, "%s/%s", current_dir, entry->d_name);

        // Build archive name (relative path)
        size_t name_len = (prefix ? strlen(prefix) + 1 : 0) + strlen(entry->d_name) + 1;
        char *archive_name = malloc(name_len);
        if (!archive_name) {
            free(full_path);
            closedir(dir);
            return -1;
        }
        if (prefix && prefix[0]) {
            snprintf(archive_name, name_len, "%s/%s", prefix, entry->d_name);
        } else {
            snprintf(archive_name, name_len, "%s", entry->d_name);
        }

        if (is_directory(full_path)) {
            // Recurse into subdirectory
            int rc = collect_files_recursive(list, full_path, archive_name);
            free(full_path);
            free(archive_name);
            if (rc != 0) {
                closedir(dir);
                return rc;
            }
        } else if (is_regular_file(full_path)) {
            // Add regular file to list
            if (file_list_add(list, full_path, archive_name) != 0) {
                free(full_path);
                free(archive_name);
                closedir(dir);
                return -1;
            }
            free(full_path);
            free(archive_name);
        } else {
            // Skip non-regular files (symlinks, devices, etc.)
            free(full_path);
            free(archive_name);
        }
    }

    closedir(dir);
    return 0;
}

// Build a local file header for a given filename
// Returns allocated buffer containing header + filename, caller must free
// Sets *lfh_len_out to total size
// If is_empty is true, uses STORE method instead of Zstandard
static struct zip_local_header* build_local_file_header(const char *filename, bool is_empty, int *lfh_len_out) {
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
    lfh->flags = ZIP_FLAG_DATA_DESCRIPTOR;

    // Empty files use STORE method (no compression)
    if (is_empty) {
        lfh->version_needed = ZIP_VERSION_STORE;
        lfh->compression_method = ZIP_METHOD_STORE;
    } else {
        lfh->version_needed = ZIP_VERSION_ZSTD;
        lfh->compression_method = ZIP_METHOD_ZSTD;
    }

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
    printf("Usage: %s [OPTIONS] -o OUTPUT_FILE INPUT...\n", program_name);
    printf("\nCreate a BURST-optimized ZIP archive.\n");
    printf("\nInput:\n");
    printf("  INPUT can be one or more files, or a single directory.\n");
    printf("  If a directory is given, all files are recursively added.\n");
    printf("  Directory mode does not allow mixing with individual files.\n");
    printf("\nOptions:\n");
    printf("  -o, --output FILE     Output archive file (required)\n");
    printf("  -l, --level LEVEL     Zstandard compression level (-15 to 22, default: 3)\n");
    printf("                        Use 0 for uncompressed STORE method\n");
    printf("  -h, --help            Show this help message\n");
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
        fprintf(stderr, "Error: At least one input file or directory required\n");
        print_usage(argv[0]);
        return 1;
    }

    // Build file list - handle directory vs individual files
    struct file_list *files = file_list_create();
    if (!files) {
        fprintf(stderr, "Error: Failed to allocate file list\n");
        return 1;
    }

    const char *first_input = argv[optind];
    if (is_directory(first_input)) {
        // Directory mode: only one argument allowed
        if (optind + 1 < argc) {
            fprintf(stderr, "Error: When input is a directory, no other inputs are allowed\n");
            file_list_destroy(files);
            return 1;
        }

        printf("Scanning directory: %s\n", first_input);
        if (collect_files_recursive(files, first_input, "") != 0) {
            fprintf(stderr, "Error: Failed to scan directory\n");
            file_list_destroy(files);
            return 1;
        }

        if (files->count == 0) {
            fprintf(stderr, "Error: No files found in directory\n");
            file_list_destroy(files);
            return 1;
        }
        printf("Found %zu files\n\n", files->count);
    } else {
        // Individual files mode
        for (int i = optind; i < argc; i++) {
            const char *input_path = argv[i];

            if (is_directory(input_path)) {
                fprintf(stderr, "Error: Cannot mix directories with individual files: %s\n", input_path);
                file_list_destroy(files);
                return 1;
            }

            if (!is_regular_file(input_path)) {
                fprintf(stderr, "Warning: Skipping non-regular file: %s\n", input_path);
                continue;
            }

            // Use basename for ZIP entry name
            const char *filename = strrchr(input_path, '/');
            filename = filename ? filename + 1 : input_path;

            if (file_list_add(files, input_path, filename) != 0) {
                fprintf(stderr, "Error: Failed to add file to list\n");
                file_list_destroy(files);
                return 1;
            }
        }

        if (files->count == 0) {
            fprintf(stderr, "Error: No valid input files\n");
            file_list_destroy(files);
            return 1;
        }
    }

    // Open output file
    FILE *output = fopen(output_path, "wb");
    if (!output) {
        perror("Failed to open output file");
        file_list_destroy(files);
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
        file_list_destroy(files);
        return 1;
    }

    // Add each file from the list
    int num_added = 0;
    for (size_t i = 0; i < files->count; i++) {
        const char *input_path = files->paths[i];
        const char *archive_name = files->names[i];

        // Check file size to detect empty files
        struct stat file_stat;
        if (stat(input_path, &file_stat) != 0) {
            fprintf(stderr, "Failed to stat file: %s\n", input_path);
            perror("stat");
            continue;
        }
        bool is_empty = (file_stat.st_size == 0);

        // Open the file
        FILE *input = fopen(input_path, "rb");
        if (!input) {
            fprintf(stderr, "Failed to open file: %s\n", input_path);
            perror("fopen");
            continue;
        }

        // Build local file header for current file
        int lfh_len = 0;
        struct zip_local_header *lfh = build_local_file_header(archive_name, is_empty, &lfh_len);
        if (!lfh) {
            fprintf(stderr, "Failed to build local file header\n");
            fclose(input);
            continue;
        }

        // Add the file
        if (burst_writer_add_file(writer, input, lfh, lfh_len, is_empty) != 0) {
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
        file_list_destroy(files);
        return 1;
    }

    // Finalize archive
    printf("\nFinalizing archive...\n");
    if (burst_writer_finalize(writer) != 0) {
        fprintf(stderr, "Failed to finalize archive\n");
        burst_writer_destroy(writer);
        fclose(output);
        file_list_destroy(files);
        return 1;
    }

    // Print statistics
    burst_writer_print_stats(writer);

    // Cleanup
    burst_writer_destroy(writer);
    fclose(output);
    file_list_destroy(files);

    printf("\nArchive created successfully: %s\n", output_path);
    printf("\nTest with: 7zz x %s\n", output_path);

    return 0;
}
