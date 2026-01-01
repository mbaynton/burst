#include "central_dir_parser.h"
#include "zip_structures.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ZIP64 End of Central Directory Locator signature
#define ZIP64_EOCD_LOCATOR_SIG 0x07064b50

/**
 * Search backwards from buffer end for EOCD signature.
 * Also detects ZIP64 archives.
 *
 * @param buffer       Buffer to search
 * @param buffer_size  Size of buffer
 * @param eocd_offset  Output: offset of EOCD within buffer
 * @param is_zip64     Output: whether ZIP64 locator was detected
 * @return Error code
 */
static int find_eocd(const uint8_t *buffer, size_t buffer_size,
                     size_t *eocd_offset, bool *is_zip64) {
    if (buffer_size < sizeof(struct zip_end_central_dir)) {
        return CENTRAL_DIR_PARSE_ERR_NO_EOCD;
    }

    // Search backwards for EOCD signature
    // EOCD can have a comment up to 64KB, but we search the entire buffer
    const uint8_t *search_end = buffer;
    const uint8_t *search_ptr = buffer + buffer_size - sizeof(struct zip_end_central_dir);

    while (search_ptr >= search_end) {
        uint32_t sig;
        memcpy(&sig, search_ptr, sizeof(sig));

        if (sig == ZIP_END_CENTRAL_DIR_SIG) {
            *eocd_offset = search_ptr - buffer;

            // Check for ZIP64 locator 20 bytes before EOCD
            if (*eocd_offset >= 20) {
                uint32_t locator_sig;
                memcpy(&locator_sig, search_ptr - 20, sizeof(locator_sig));
                if (locator_sig == ZIP64_EOCD_LOCATOR_SIG) {
                    *is_zip64 = true;
                    return CENTRAL_DIR_PARSE_ERR_ZIP64_UNSUPPORTED;
                }
            }

            *is_zip64 = false;
            return CENTRAL_DIR_PARSE_SUCCESS;
        }
        search_ptr--;
    }

    return CENTRAL_DIR_PARSE_ERR_NO_EOCD;
}

/**
 * Parse the EOCD structure.
 *
 * @param buffer       Buffer containing EOCD
 * @param buffer_size  Size of buffer
 * @param eocd_offset  Offset of EOCD within buffer
 * @param cd_offset    Output: offset of central directory in archive
 * @param num_entries  Output: number of entries in central directory
 * @param cd_size      Output: size of central directory
 * @return Error code
 */
static int parse_eocd(const uint8_t *buffer, size_t buffer_size,
                      size_t eocd_offset,
                      uint64_t *cd_offset, uint32_t *num_entries,
                      uint32_t *cd_size) {
    // Verify we have enough bytes for full EOCD
    if (eocd_offset + sizeof(struct zip_end_central_dir) > buffer_size) {
        return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
    }

    const struct zip_end_central_dir *eocd =
        (const struct zip_end_central_dir *)(buffer + eocd_offset);

    // Verify signature
    if (eocd->signature != ZIP_END_CENTRAL_DIR_SIG) {
        return CENTRAL_DIR_PARSE_ERR_INVALID_SIGNATURE;
    }

    // Extract metadata
    *cd_offset = eocd->central_dir_offset;
    *num_entries = eocd->num_entries_total;
    *cd_size = eocd->central_dir_size;

    return CENTRAL_DIR_PARSE_SUCCESS;
}

/**
 * Parse all central directory entries.
 *
 * @param buffer       Buffer containing central directory
 * @param buffer_size  Size of buffer
 * @param cd_offset    Offset of central directory in archive (absolute)
 * @param buffer_offset Offset within the archive that buffer[0] represents
 * @param num_entries  Number of entries to parse
 * @param cd_size      Size of central directory
 * @param part_size    Part size in bytes
 * @param files        Output: array of file metadata
 * @param num_files    Output: number of files parsed
 * @return Error code
 */
static int parse_central_directory(
    const uint8_t *buffer, size_t buffer_size,
    uint64_t cd_offset, uint64_t buffer_offset,
    uint32_t num_entries, uint32_t cd_size,
    uint64_t part_size,
    struct file_metadata **files, size_t *num_files)
{
    // Calculate where CD starts within our buffer
    if (cd_offset < buffer_offset) {
        // Central directory is not in our buffer
        return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
    }
    size_t cd_start_in_buffer = (size_t)(cd_offset - buffer_offset);

    if (cd_start_in_buffer + cd_size > buffer_size) {
        return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
    }

    // Allocate array for file metadata
    struct file_metadata *file_array = calloc(num_entries, sizeof(struct file_metadata));
    if (!file_array) {
        return CENTRAL_DIR_PARSE_ERR_MEMORY;
    }

    const uint8_t *ptr = buffer + cd_start_in_buffer;
    const uint8_t *cd_end = ptr + cd_size;

    for (uint32_t i = 0; i < num_entries; i++) {
        // Verify we have enough space for fixed header
        if (ptr + sizeof(struct zip_central_header) > cd_end) {
            // Cleanup and return error
            for (size_t j = 0; j < i; j++) {
                free(file_array[j].filename);
            }
            free(file_array);
            return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
        }

        const struct zip_central_header *header =
            (const struct zip_central_header *)ptr;

        // Verify signature
        if (header->signature != ZIP_CENTRAL_DIR_HEADER_SIG) {
            // Cleanup and return error
            for (size_t j = 0; j < i; j++) {
                free(file_array[j].filename);
            }
            free(file_array);
            return CENTRAL_DIR_PARSE_ERR_INVALID_SIGNATURE;
        }

        // Extract metadata
        file_array[i].local_header_offset = header->local_header_offset;
        file_array[i].compressed_size = header->compressed_size;
        file_array[i].uncompressed_size = header->uncompressed_size;
        file_array[i].crc32 = header->crc32;
        file_array[i].compression_method = header->compression_method;

        // Calculate part index
        file_array[i].part_index = (uint32_t)(header->local_header_offset / part_size);

        // Move past fixed header
        ptr += sizeof(struct zip_central_header);

        // Verify we have enough space for variable-length fields
        size_t variable_len = header->filename_length +
                              header->extra_field_length +
                              header->file_comment_length;
        if (ptr + variable_len > cd_end) {
            // Cleanup and return error
            for (size_t j = 0; j < i; j++) {
                free(file_array[j].filename);
            }
            free(file_array);
            return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
        }

        // Allocate and copy filename (null-terminated)
        file_array[i].filename = malloc(header->filename_length + 1);
        if (!file_array[i].filename) {
            // Cleanup and return error
            for (size_t j = 0; j < i; j++) {
                free(file_array[j].filename);
            }
            free(file_array);
            return CENTRAL_DIR_PARSE_ERR_MEMORY;
        }
        memcpy(file_array[i].filename, ptr, header->filename_length);
        file_array[i].filename[header->filename_length] = '\0';

        // Skip past variable-length fields
        ptr += variable_len;
    }

    *files = file_array;
    *num_files = num_entries;

    return CENTRAL_DIR_PARSE_SUCCESS;
}

/**
 * Comparison function for sorting part_file_entry by offset_in_part.
 */
static int compare_part_file_entries(const void *a, const void *b) {
    const struct part_file_entry *entry_a = (const struct part_file_entry *)a;
    const struct part_file_entry *entry_b = (const struct part_file_entry *)b;

    if (entry_a->offset_in_part < entry_b->offset_in_part) {
        return -1;
    } else if (entry_a->offset_in_part > entry_b->offset_in_part) {
        return 1;
    }
    return 0;
}

/**
 * Build the part-to-files mapping.
 *
 * @param files        Array of file metadata (non-const because we store pointers to elements)
 * @param num_files    Number of files
 * @param archive_size Total archive size
 * @param part_size    Part size in bytes
 * @param parts        Output: array of part_files structures
 * @param num_parts    Output: number of parts
 * @return Error code
 */
static int build_part_map(
    struct file_metadata *files, size_t num_files,
    uint64_t archive_size,
    uint64_t part_size,
    struct part_files **parts_out, size_t *num_parts_out)
{
    // Calculate number of parts (round up)
    size_t num_parts = (size_t)((archive_size + part_size - 1) / part_size);
    if (num_parts == 0) {
        num_parts = 1;  // At least one part for empty archives
    }

    // Allocate parts array
    struct part_files *parts = calloc(num_parts, sizeof(struct part_files));
    if (!parts) {
        return CENTRAL_DIR_PARSE_ERR_MEMORY;
    }

    // Initialize all parts with NULL continuing_file
    for (size_t i = 0; i < num_parts; i++) {
        parts[i].continuing_file = NULL;
    }

    // First pass: count how many files start in each part
    size_t *counts = calloc(num_parts, sizeof(size_t));
    if (!counts) {
        free(parts);
        return CENTRAL_DIR_PARSE_ERR_MEMORY;
    }

    for (size_t i = 0; i < num_files; i++) {
        uint32_t part_idx = files[i].part_index;
        if (part_idx < num_parts) {
            counts[part_idx]++;
        }
    }

    // Allocate entry arrays for each part
    for (size_t i = 0; i < num_parts; i++) {
        if (counts[i] > 0) {
            parts[i].entries = calloc(counts[i], sizeof(struct part_file_entry));
            if (!parts[i].entries) {
                // Cleanup
                for (size_t j = 0; j < i; j++) {
                    free(parts[j].entries);
                }
                free(parts);
                free(counts);
                return CENTRAL_DIR_PARSE_ERR_MEMORY;
            }
        }
    }

    // Reset counts for second pass
    memset(counts, 0, num_parts * sizeof(size_t));

    // Second pass: populate entries
    for (size_t i = 0; i < num_files; i++) {
        uint32_t part_idx = files[i].part_index;
        if (part_idx < num_parts) {
            size_t entry_idx = counts[part_idx]++;
            parts[part_idx].entries[entry_idx].file_index = i;
            parts[part_idx].entries[entry_idx].offset_in_part =
                files[i].local_header_offset % part_size;
        }
    }

    // Set num_entries for each part
    for (size_t i = 0; i < num_parts; i++) {
        parts[i].num_entries = counts[i];
    }

    free(counts);

    // Sort entries in each part by offset_in_part
    for (size_t i = 0; i < num_parts; i++) {
        if (parts[i].num_entries > 1) {
            qsort(parts[i].entries, parts[i].num_entries,
                  sizeof(struct part_file_entry), compare_part_file_entries);
        }
    }

    // Determine continuing_file for each part
    // A file continues into part N if its data extends beyond the part boundary
    for (size_t part_idx = 1; part_idx < num_parts; part_idx++) {
        uint64_t part_start = (uint64_t)part_idx * part_size;

        // Search all files to find one that spans into this part
        for (size_t i = 0; i < num_files; i++) {
            uint64_t file_start = files[i].local_header_offset;
            // Estimate end of file data: local header + compressed data + data descriptor
            // Local header size is at least 30 bytes plus filename length
            // We estimate conservatively; the file spans if its start is before part_start
            // and its data extends past part_start
            uint64_t file_data_end = file_start + 30 + files[i].compressed_size + 16;

            if (file_start < part_start && file_data_end > part_start) {
                // This file continues into this part
                parts[part_idx].continuing_file = &files[i];
                break;
            }
        }
    }

    *parts_out = parts;
    *num_parts_out = num_parts;

    return CENTRAL_DIR_PARSE_SUCCESS;
}

int central_dir_parse(const uint8_t *buffer, size_t buffer_size,
                      uint64_t archive_size,
                      uint64_t part_size,
                      struct central_dir_parse_result *result) {
    // Initialize result structure
    if (result) {
        memset(result, 0, sizeof(*result));
    }

    // Validate inputs
    if (!buffer || buffer_size == 0 || !result) {
        if (result) {
            result->error_code = CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER;
            snprintf(result->error_message, sizeof(result->error_message),
                    "Invalid parameters: buffer=%p size=%zu result=%p",
                    (void*)buffer, buffer_size, (void*)result);
        }
        return CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER;
    }

    // Find EOCD
    size_t eocd_offset;
    bool is_zip64 = false;
    int rc = find_eocd(buffer, buffer_size, &eocd_offset, &is_zip64);
    if (rc != CENTRAL_DIR_PARSE_SUCCESS) {
        result->error_code = rc;
        result->is_zip64 = is_zip64;
        if (rc == CENTRAL_DIR_PARSE_ERR_NO_EOCD) {
            snprintf(result->error_message, sizeof(result->error_message),
                    "No End of Central Directory signature found in buffer");
        } else if (rc == CENTRAL_DIR_PARSE_ERR_ZIP64_UNSUPPORTED) {
            snprintf(result->error_message, sizeof(result->error_message),
                    "ZIP64 archives are not yet supported");
        }
        return rc;
    }

    // Parse EOCD
    uint64_t cd_offset;
    uint32_t num_entries, cd_size;
    rc = parse_eocd(buffer, buffer_size, eocd_offset,
                    &cd_offset, &num_entries, &cd_size);
    if (rc != CENTRAL_DIR_PARSE_SUCCESS) {
        result->error_code = rc;
        snprintf(result->error_message, sizeof(result->error_message),
                "Failed to parse EOCD at offset %zu", eocd_offset);
        return rc;
    }

    result->central_dir_offset = cd_offset;
    result->central_dir_size = cd_size;

    // Calculate buffer's position within the archive
    // buffer contains the last buffer_size bytes of the archive
    uint64_t buffer_offset = archive_size - buffer_size;

    // Parse central directory
    rc = parse_central_directory(buffer, buffer_size, cd_offset, buffer_offset,
                                 num_entries, cd_size, part_size,
                                 &result->files, &result->num_files);
    if (rc != CENTRAL_DIR_PARSE_SUCCESS) {
        result->error_code = rc;
        snprintf(result->error_message, sizeof(result->error_message),
                "Failed to parse central directory: %u entries expected at offset %llu",
                num_entries, (unsigned long long)cd_offset);
        return rc;
    }

    // Build part mapping
    rc = build_part_map(result->files, result->num_files, archive_size, part_size,
                        &result->parts, &result->num_parts);
    if (rc != CENTRAL_DIR_PARSE_SUCCESS) {
        // Cleanup files on error
        for (size_t i = 0; i < result->num_files; i++) {
            free(result->files[i].filename);
        }
        free(result->files);
        result->files = NULL;
        result->num_files = 0;

        result->error_code = rc;
        snprintf(result->error_message, sizeof(result->error_message),
                "Failed to build part mapping");
        return rc;
    }

    result->error_code = CENTRAL_DIR_PARSE_SUCCESS;
    return CENTRAL_DIR_PARSE_SUCCESS;
}

void central_dir_parse_result_free(struct central_dir_parse_result *result) {
    if (!result) {
        return;
    }

    // Free each filename
    for (size_t i = 0; i < result->num_files; i++) {
        free(result->files[i].filename);
    }

    // Free files array
    free(result->files);

    // Free each parts[i].entries array
    for (size_t i = 0; i < result->num_parts; i++) {
        free(result->parts[i].entries);
    }

    // Free parts array
    free(result->parts);

    // Zero out structure
    memset(result, 0, sizeof(*result));
}
