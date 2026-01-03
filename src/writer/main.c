#include "burst_writer.h"
#include "zip_structures.h"
#include "entry_processor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>

// Dynamic array of file paths for directory recursion
struct file_list {
    char **paths;      // Array of file paths (full paths)
    char **names;      // Array of archive names (relative paths)
    char **targets;    // Array of symlink targets (NULL for non-symlinks)
    struct stat *stats;  // Array of stat info for each file
    bool *is_directory;  // Array of directory flags
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
    list->targets = malloc(list->capacity * sizeof(char *));
    list->stats = malloc(list->capacity * sizeof(struct stat));
    list->is_directory = malloc(list->capacity * sizeof(bool));
    if (!list->paths || !list->names || !list->targets || !list->stats || !list->is_directory) {
        free(list->paths);
        free(list->names);
        free(list->targets);
        free(list->stats);
        free(list->is_directory);
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
        free(list->targets[i]);  // May be NULL, free(NULL) is safe
    }
    free(list->paths);
    free(list->names);
    free(list->targets);
    free(list->stats);
    free(list->is_directory);
    free(list);
}

// target may be NULL for non-symlinks, is_dir indicates if this is a directory entry
static int file_list_add(struct file_list *list, const char *path, const char *name,
                          const struct stat *st, const char *target, bool is_dir) {
    if (list->count >= list->capacity) {
        size_t new_capacity = list->capacity * 2;
        char **new_paths = realloc(list->paths, new_capacity * sizeof(char *));
        char **new_names = realloc(list->names, new_capacity * sizeof(char *));
        char **new_targets = realloc(list->targets, new_capacity * sizeof(char *));
        struct stat *new_stats = realloc(list->stats, new_capacity * sizeof(struct stat));
        bool *new_is_directory = realloc(list->is_directory, new_capacity * sizeof(bool));
        if (!new_paths || !new_names || !new_targets || !new_stats || !new_is_directory) {
            return -1;
        }
        list->paths = new_paths;
        list->names = new_names;
        list->targets = new_targets;
        list->stats = new_stats;
        list->is_directory = new_is_directory;
        list->capacity = new_capacity;
    }
    list->paths[list->count] = strdup(path);
    list->names[list->count] = strdup(name);
    list->targets[list->count] = target ? strdup(target) : NULL;
    if (!list->paths[list->count] || !list->names[list->count] ||
        (target && !list->targets[list->count])) {
        free(list->paths[list->count]);
        free(list->names[list->count]);
        free(list->targets[list->count]);
        return -1;
    }
    list->stats[list->count] = *st;  // Copy stat struct
    list->is_directory[list->count] = is_dir;
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

        // Use lstat to get file info (doesn't follow symlinks)
        struct stat st;
        if (lstat(full_path, &st) != 0) {
            fprintf(stderr, "Warning: Cannot stat %s (%s)\n", full_path, strerror(errno));
            free(full_path);
            continue;
        }

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

        if (S_ISDIR(st.st_mode)) {
            // Build directory archive name with trailing slash
            size_t dir_name_len = name_len + 2;  // +1 for slash, +1 for null
            char *dir_archive_name = malloc(dir_name_len);
            if (!dir_archive_name) {
                free(full_path);
                free(archive_name);
                closedir(dir);
                return -1;
            }
            snprintf(dir_archive_name, dir_name_len, "%s/", archive_name);

            // Add directory entry to list BEFORE recursing into it (pre-order)
            if (file_list_add(list, full_path, dir_archive_name, &st, NULL, true) != 0) {
                free(full_path);
                free(archive_name);
                free(dir_archive_name);
                closedir(dir);
                return -1;
            }
            free(dir_archive_name);

            // Now recurse into subdirectory
            int rc = collect_files_recursive(list, full_path, archive_name);
            free(full_path);
            free(archive_name);
            if (rc != 0) {
                closedir(dir);
                return rc;
            }
        } else if (S_ISREG(st.st_mode)) {
            // Add regular file to list
            if (file_list_add(list, full_path, archive_name, &st, NULL, false) != 0) {
                free(full_path);
                free(archive_name);
                closedir(dir);
                return -1;
            }
            free(full_path);
            free(archive_name);
        } else if (S_ISLNK(st.st_mode)) {
            // Read symlink target
            char target_buf[PATH_MAX];
            ssize_t target_len = readlink(full_path, target_buf, sizeof(target_buf) - 1);
            if (target_len < 0) {
                fprintf(stderr, "Warning: Cannot read symlink %s (%s)\n", full_path, strerror(errno));
                free(full_path);
                free(archive_name);
                continue;
            }
            target_buf[target_len] = '\0';

            // Add symlink to list
            if (file_list_add(list, full_path, archive_name, &st, target_buf, false) != 0) {
                free(full_path);
                free(archive_name);
                closedir(dir);
                return -1;
            }
            free(full_path);
            free(archive_name);
        } else {
            // Skip other file types (devices, sockets, etc.)
            free(full_path);
            free(archive_name);
        }
    }

    closedir(dir);
    return 0;
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
            fprintf(stderr, "Error: No files or directories found in directory\n");
            file_list_destroy(files);
            return 1;
        }
        printf("Found %zu entries (files and directories)\n\n", files->count);
    } else {
        // Individual files mode
        for (int i = optind; i < argc; i++) {
            const char *input_path = argv[i];

            // Use lstat to get file info (doesn't follow symlinks)
            struct stat st;
            if (lstat(input_path, &st) != 0) {
                fprintf(stderr, "Error: Cannot stat %s (%s)\n", input_path, strerror(errno));
                file_list_destroy(files);
                return 1;
            }

            if (S_ISDIR(st.st_mode)) {
                fprintf(stderr, "Error: Cannot mix directories with individual files: %s\n", input_path);
                file_list_destroy(files);
                return 1;
            }

            // Use basename for ZIP entry name
            const char *filename = strrchr(input_path, '/');
            filename = filename ? filename + 1 : input_path;

            if (S_ISLNK(st.st_mode)) {
                // Read symlink target
                char target_buf[PATH_MAX];
                ssize_t target_len = readlink(input_path, target_buf, sizeof(target_buf) - 1);
                if (target_len < 0) {
                    fprintf(stderr, "Error: Cannot read symlink %s (%s)\n", input_path, strerror(errno));
                    file_list_destroy(files);
                    return 1;
                }
                target_buf[target_len] = '\0';

                if (file_list_add(files, input_path, filename, &st, target_buf, false) != 0) {
                    fprintf(stderr, "Error: Failed to add symlink to list\n");
                    file_list_destroy(files);
                    return 1;
                }
            } else if (S_ISREG(st.st_mode)) {
                if (file_list_add(files, input_path, filename, &st, NULL, false) != 0) {
                    fprintf(stderr, "Error: Failed to add file to list\n");
                    file_list_destroy(files);
                    return 1;
                }
            } else {
                fprintf(stderr, "Warning: Skipping unsupported file type: %s\n", input_path);
                continue;
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
        if (process_entry(writer,
                          files->paths[i],
                          files->names[i],
                          files->targets[i],
                          &files->stats[i],
                          files->is_directory[i])) {
            num_added++;
        }
    }

    if (num_added == 0) {
        fprintf(stderr, "Error: No files or directories were added to archive\n");
        burst_writer_destroy(writer);
        fclose(output);
        file_list_destroy(files);
        return 1;
    }

    // Note: num_added includes directories, so empty directories are valid archives

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
