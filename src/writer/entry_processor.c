/*
 * Entry Processor - Process individual file system entries for archiving
 *
 * This module handles the processing of files, directories, and symlinks
 * for addition to BURST archives.
 */
#include "entry_processor.h"
#include "burst_writer.h"
#include "zip_structures.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <zlib.h>

/*
 * Build a local file header for a regular file.
 * Files use Zstandard compression (or STORE for empty files) and data descriptors.
 */
static struct zip_local_header* build_local_file_header(const char *filename, bool is_empty,
                                                         uint32_t uid, uint32_t gid,
                                                         int *lfh_len_out) {
    time_t now = time(NULL);
    uint16_t mod_time, mod_date;
    dos_datetime_from_time_t(now, &mod_time, &mod_date);

    uint8_t extra_field[16];
    size_t extra_field_len = build_unix_extra_field(extra_field, sizeof(extra_field), uid, gid);

    size_t filename_len = strlen(filename);
    size_t total_size = sizeof(struct zip_local_header) + filename_len + extra_field_len;

    struct zip_local_header *lfh = malloc(total_size);
    if (!lfh) {
        return NULL;
    }

    memset(lfh, 0, sizeof(struct zip_local_header));
    lfh->signature = ZIP_LOCAL_FILE_HEADER_SIG;
    lfh->flags = ZIP_FLAG_DATA_DESCRIPTOR;

    if (is_empty) {
        lfh->version_needed = ZIP_VERSION_STORE;
        lfh->compression_method = ZIP_METHOD_STORE;
    } else {
        lfh->version_needed = ZIP_VERSION_ZSTD;
        lfh->compression_method = ZIP_METHOD_ZSTD;
    }

    lfh->last_mod_time = mod_time;
    lfh->last_mod_date = mod_date;
    lfh->crc32 = 0;
    lfh->compressed_size = 0;
    lfh->uncompressed_size = 0;
    lfh->filename_length = filename_len;
    lfh->extra_field_length = extra_field_len;

    uint8_t *ptr = (uint8_t*)lfh + sizeof(struct zip_local_header);
    memcpy(ptr, filename, filename_len);
    ptr += filename_len;

    if (extra_field_len > 0) {
        memcpy(ptr, extra_field, extra_field_len);
    }

    *lfh_len_out = total_size;
    return lfh;
}

/*
 * Build a local file header for a symlink.
 * Symlinks use STORE method and have pre-computed CRC32 and sizes.
 */
static struct zip_local_header* build_symlink_local_file_header(const char *filename,
                                                                  const char *target,
                                                                  size_t target_len,
                                                                  uint32_t uid, uint32_t gid,
                                                                  int *lfh_len_out) {
    time_t now = time(NULL);
    uint16_t mod_time, mod_date;
    dos_datetime_from_time_t(now, &mod_time, &mod_date);

    uint8_t extra_field[16];
    size_t extra_field_len = build_unix_extra_field(extra_field, sizeof(extra_field), uid, gid);

    size_t filename_len = strlen(filename);
    size_t total_size = sizeof(struct zip_local_header) + filename_len + extra_field_len;

    struct zip_local_header *lfh = malloc(total_size);
    if (!lfh) {
        return NULL;
    }

    uint32_t target_crc = crc32(0, (const uint8_t *)target, target_len);

    memset(lfh, 0, sizeof(struct zip_local_header));
    lfh->signature = ZIP_LOCAL_FILE_HEADER_SIG;
    lfh->version_needed = ZIP_VERSION_STORE;
    lfh->flags = 0;
    lfh->compression_method = ZIP_METHOD_STORE;
    lfh->last_mod_time = mod_time;
    lfh->last_mod_date = mod_date;
    lfh->crc32 = target_crc;
    lfh->compressed_size = target_len;
    lfh->uncompressed_size = target_len;
    lfh->filename_length = filename_len;
    lfh->extra_field_length = extra_field_len;

    uint8_t *ptr = (uint8_t*)lfh + sizeof(struct zip_local_header);
    memcpy(ptr, filename, filename_len);
    ptr += filename_len;

    if (extra_field_len > 0) {
        memcpy(ptr, extra_field, extra_field_len);
    }

    *lfh_len_out = total_size;
    return lfh;
}

/*
 * Build a local file header for a directory.
 * Directories use STORE method with zero sizes and CRC.
 */
static struct zip_local_header* build_directory_local_file_header(
    const char *dirname,
    uint32_t uid,
    uint32_t gid,
    time_t mtime,
    int *lfh_len_out)
{
    size_t dirname_len = strlen(dirname);
    if (dirname_len == 0 || dirname[dirname_len - 1] != '/') {
        fprintf(stderr, "Error: Directory name must end with '/': %s\n", dirname);
        return NULL;
    }

    uint16_t mod_time, mod_date;
    dos_datetime_from_time_t(mtime, &mod_time, &mod_date);

    uint8_t extra_field[16];
    size_t extra_field_len = build_unix_extra_field(extra_field, sizeof(extra_field), uid, gid);

    size_t total_size = sizeof(struct zip_local_header) + dirname_len + extra_field_len;

    struct zip_local_header *lfh = malloc(total_size);
    if (!lfh) {
        return NULL;
    }

    memset(lfh, 0, sizeof(struct zip_local_header));
    lfh->signature = ZIP_LOCAL_FILE_HEADER_SIG;
    lfh->version_needed = ZIP_VERSION_STORE;
    lfh->flags = 0;
    lfh->compression_method = ZIP_METHOD_STORE;
    lfh->last_mod_time = mod_time;
    lfh->last_mod_date = mod_date;
    lfh->crc32 = 0;
    lfh->compressed_size = 0;
    lfh->uncompressed_size = 0;
    lfh->filename_length = dirname_len;
    lfh->extra_field_length = extra_field_len;

    uint8_t *ptr = (uint8_t*)lfh + sizeof(struct zip_local_header);
    memcpy(ptr, dirname, dirname_len);
    ptr += dirname_len;

    if (extra_field_len > 0) {
        memcpy(ptr, extra_field, extra_field_len);
    }

    *lfh_len_out = total_size;
    return lfh;
}

int process_entry(struct burst_writer *writer,
                  const char *input_path,
                  const char *archive_name,
                  const char *symlink_target,
                  const struct stat *file_stat,
                  bool is_dir) {
    int success = 0;

    if (is_dir) {
        // Handle directory
        int lfh_len = 0;
        struct zip_local_header *lfh = build_directory_local_file_header(
            archive_name,
            file_stat->st_uid,
            file_stat->st_gid,
            file_stat->st_mtime,
            &lfh_len);

        if (!lfh) {
            fprintf(stderr, "Failed to build directory local file header\n");
            return 0;
        }

        if (burst_writer_add_directory(writer, lfh, lfh_len,
                                       file_stat->st_mode,
                                       file_stat->st_uid,
                                       file_stat->st_gid) == 0) {
            success = 1;
        } else {
            fprintf(stderr, "Failed to add directory: %s\n", input_path);
        }

        free(lfh);  // Single cleanup point - no double-free
    } else if (S_ISLNK(file_stat->st_mode)) {
        // Handle symlink
        size_t target_len = strlen(symlink_target);

        int lfh_len = 0;
        struct zip_local_header *lfh = build_symlink_local_file_header(
            archive_name, symlink_target, target_len,
            file_stat->st_uid, file_stat->st_gid, &lfh_len);

        if (!lfh) {
            fprintf(stderr, "Failed to build symlink local file header\n");
            return 0;
        }

        if (burst_writer_add_symlink(writer, lfh, lfh_len, symlink_target, target_len,
                                      file_stat->st_mode, file_stat->st_uid, file_stat->st_gid) == 0) {
            success = 1;
        } else {
            fprintf(stderr, "Failed to add symlink: %s\n", input_path);
        }

        free(lfh);  // Single cleanup point - no double-free
    } else {
        // Handle regular file
        bool is_empty = (file_stat->st_size == 0);

        FILE *input = fopen(input_path, "rb");
        if (!input) {
            fprintf(stderr, "Failed to open file: %s\n", input_path);
            perror("fopen");
            return 0;
        }

        int lfh_len = 0;
        struct zip_local_header *lfh = build_local_file_header(archive_name, is_empty,
                                                                file_stat->st_uid, file_stat->st_gid,
                                                                &lfh_len);
        if (!lfh) {
            fprintf(stderr, "Failed to build local file header\n");
            fclose(input);
            return 0;
        }

        if (burst_writer_add_file(writer, input, lfh, lfh_len, is_empty,
                                  file_stat->st_mode, file_stat->st_uid, file_stat->st_gid) == 0) {
            success = 1;
        } else {
            fprintf(stderr, "Failed to add file: %s\n", input_path);
        }

        // Single cleanup point - no double-free or double-close
        free(lfh);
        fclose(input);
    }

    return success;
}
