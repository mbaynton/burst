#include "central_dir_parser.h"
#include "zip_structures.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

// ZIP64 signatures and constants
#define ZIP64_EOCD_LOCATOR_SIG 0x07064b50
#define ZIP64_EOCD_SIG 0x06064b50

/**
 * Parse Unix extra field (0x7875) to extract uid/gid.
 *
 * @param extra_field    Pointer to extra field data (after fixed header)
 * @param extra_len      Length of extra field data
 * @param uid            Output: Unix user ID
 * @param gid            Output: Unix group ID
 * @return true if 0x7875 field was found and parsed, false otherwise
 */
static bool parse_unix_extra_field(const uint8_t *extra_field, uint16_t extra_len,
                                   uint32_t *uid, uint32_t *gid) {
    const uint8_t *ptr = extra_field;
    const uint8_t *end = extra_field + extra_len;

    while (ptr + 4 <= end) {
        // Read header ID (2 bytes) and data size (2 bytes)
        uint16_t header_id;
        uint16_t data_size;
        memcpy(&header_id, ptr, sizeof(uint16_t));
        memcpy(&data_size, ptr + 2, sizeof(uint16_t));
        ptr += 4;

        // Check if this is the Unix extra field (0x7875)
        if (header_id == ZIP_EXTRA_UNIX_7875_ID) {
            // Verify we have enough data
            if (ptr + data_size > end || data_size < 3) {
                return false;
            }

            // Parse 0x7875 format:
            // Version (1 byte)
            // UIDSize (1 byte)
            // UID (UIDSize bytes)
            // GIDSize (1 byte)
            // GID (GIDSize bytes)
            uint8_t version = ptr[0];
            if (version != 1) {
                return false;  // Unknown version
            }

            uint8_t uid_size = ptr[1];
            if (ptr + 2 + uid_size + 1 > end) {
                return false;
            }

            // Read UID (little-endian, variable size)
            *uid = 0;
            for (uint8_t i = 0; i < uid_size && i < 4; i++) {
                *uid |= ((uint32_t)ptr[2 + i]) << (i * 8);
            }

            uint8_t gid_size = ptr[2 + uid_size];
            if (ptr + 2 + uid_size + 1 + gid_size > end) {
                return false;
            }

            // Read GID (little-endian, variable size)
            *gid = 0;
            for (uint8_t i = 0; i < gid_size && i < 4; i++) {
                *gid |= ((uint32_t)ptr[3 + uid_size + i]) << (i * 8);
            }

            return true;
        }

        // Skip to next extra field entry
        if (ptr + data_size > end) {
            break;
        }
        ptr += data_size;
    }

    return false;
}

/**
 * Parse ZIP64 extended information extra field (0x0001) from central directory.
 *
 * The ZIP64 extra field contains 64-bit values for fields that overflow 32-bit.
 * Fields appear in order: uncompressed, compressed, offset, disk_start.
 * Only fields whose 32-bit counterpart is 0xFFFFFFFF are present.
 *
 * @param extra_field    Pointer to extra field data
 * @param extra_len      Length of extra field data
 * @param header         Central directory header (to check which fields overflow)
 * @param compressed_size    Output: 64-bit compressed size (unchanged if not present)
 * @param uncompressed_size  Output: 64-bit uncompressed size (unchanged if not present)
 * @param local_header_offset Output: 64-bit offset (unchanged if not present)
 * @return true if ZIP64 extra field was found, false otherwise
 */
static bool parse_zip64_extra_field(const uint8_t *extra_field, uint16_t extra_len,
                                    const struct zip_central_header *header,
                                    uint64_t *compressed_size,
                                    uint64_t *uncompressed_size,
                                    uint64_t *local_header_offset) {
    const uint8_t *ptr = extra_field;
    const uint8_t *end = extra_field + extra_len;

    while (ptr + 4 <= end) {
        // Read header ID (2 bytes) and data size (2 bytes)
        uint16_t header_id;
        uint16_t data_size;
        memcpy(&header_id, ptr, sizeof(uint16_t));
        memcpy(&data_size, ptr + 2, sizeof(uint16_t));
        ptr += 4;

        // Check if this is the ZIP64 extra field (0x0001)
        if (header_id == ZIP_EXTRA_ZIP64_ID) {
            // Verify we have enough data
            if (ptr + data_size > end) {
                return false;
            }

            // Parse ZIP64 values in order:
            // 1. Original Size (if uncompressed_size == 0xFFFFFFFF)
            // 2. Compressed Size (if compressed_size == 0xFFFFFFFF)
            // 3. Relative Header Offset (if local_header_offset == 0xFFFFFFFF)
            // 4. Disk Start Number (if disk_number_start == 0xFFFF) - not used
            const uint8_t *data_ptr = ptr;
            const uint8_t *data_end = ptr + data_size;

            if (header->uncompressed_size == 0xFFFFFFFF) {
                if (data_ptr + 8 > data_end) return false;
                memcpy(uncompressed_size, data_ptr, sizeof(uint64_t));
                data_ptr += 8;
            }

            if (header->compressed_size == 0xFFFFFFFF) {
                if (data_ptr + 8 > data_end) return false;
                memcpy(compressed_size, data_ptr, sizeof(uint64_t));
                data_ptr += 8;
            }

            if (header->local_header_offset == 0xFFFFFFFF) {
                if (data_ptr + 8 > data_end) return false;
                memcpy(local_header_offset, data_ptr, sizeof(uint64_t));
                // data_ptr += 8;  // Not needed, last field we care about
            }

            return true;
        }

        // Skip to next extra field entry
        if (ptr + data_size > end) {
            break;
        }
        ptr += data_size;
    }

    return false;
}

/**
 * Parse ZIP64 End of Central Directory record.
 *
 * @param buffer         Buffer containing ZIP64 EOCD
 * @param buffer_size    Size of buffer
 * @param eocd64_offset  Offset of ZIP64 EOCD within buffer
 * @param cd_offset      Output: offset of central directory in archive
 * @param num_entries    Output: number of entries (64-bit)
 * @param cd_size        Output: size of central directory (64-bit)
 * @return Error code
 */
static int parse_zip64_eocd(const uint8_t *buffer, size_t buffer_size,
                            size_t eocd64_offset,
                            uint64_t *cd_offset, uint64_t *num_entries,
                            uint64_t *cd_size) {
    // ZIP64 EOCD is 56 bytes minimum
    if (eocd64_offset + sizeof(struct zip64_end_central_dir) > buffer_size) {
        return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
    }

    const struct zip64_end_central_dir *eocd64 =
        (const struct zip64_end_central_dir *)(buffer + eocd64_offset);

    // Verify signature
    if (eocd64->signature != ZIP64_EOCD_SIG) {
        return CENTRAL_DIR_PARSE_ERR_INVALID_SIGNATURE;
    }

    // Extract metadata
    *cd_offset = eocd64->central_dir_offset;
    *num_entries = eocd64->num_entries_total;
    *cd_size = eocd64->central_dir_size;

    return CENTRAL_DIR_PARSE_SUCCESS;
}

/**
 * Search backwards from buffer end for EOCD signature.
 * Also detects ZIP64 archives and returns locator offset.
 *
 * @param buffer           Buffer to search
 * @param buffer_size      Size of buffer
 * @param eocd_offset      Output: offset of EOCD within buffer
 * @param is_zip64         Output: whether ZIP64 locator was detected
 * @param locator_offset   Output: offset of ZIP64 locator within buffer (if ZIP64)
 * @return Error code
 */
static int find_eocd(const uint8_t *buffer, size_t buffer_size,
                     size_t *eocd_offset, bool *is_zip64, size_t *locator_offset) {
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
                uint32_t loc_sig;
                memcpy(&loc_sig, search_ptr - 20, sizeof(loc_sig));
                if (loc_sig == ZIP64_EOCD_LOCATOR_SIG) {
                    *is_zip64 = true;
                    *locator_offset = *eocd_offset - 20;
                    return CENTRAL_DIR_PARSE_SUCCESS;  // Now we support ZIP64!
                }
            }

            *is_zip64 = false;
            *locator_offset = 0;
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
 * @param num_entries  Number of entries to parse (64-bit for ZIP64 support)
 * @param cd_size      Size of central directory (64-bit for ZIP64 support)
 * @param part_size    Part size in bytes
 * @param files        Output: array of file metadata
 * @param num_files    Output: number of files parsed
 * @return Error code
 */
static int parse_central_directory(
    const uint8_t *buffer, size_t buffer_size,
    uint64_t cd_offset, uint64_t buffer_offset,
    uint64_t num_entries, uint64_t cd_size,
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

    uint64_t actual_entries = 0;
    for (uint64_t i = 0; i < num_entries; i++) {
        // Check if we've reached the end of the CD
        if (ptr >= cd_end) {
            // Reached end of CD before num_entries - this is OK if num_entries was estimated
            break;
        }

        // Verify we have enough space for fixed header
        if (ptr + sizeof(struct zip_central_header) > cd_end) {
            // Reached end of CD with partial header - this is OK if we've parsed at least one entry
            if (actual_entries > 0) {
                break;  // Gracefully stop parsing
            }
            // Cleanup and return error - no entries parsed
            free(file_array);
            return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
        }

        const struct zip_central_header *header =
            (const struct zip_central_header *)ptr;

        // Verify signature
        if (header->signature != ZIP_CENTRAL_DIR_HEADER_SIG) {
            // Invalid signature - might have reached end of CD or corrupted data
            // If we've already parsed entries successfully, assume we've hit end-of-CD
            if (actual_entries > 0) {
                break;  // Gracefully stop parsing
            }
            // First entry has bad signature - this is an error
            free(file_array);
            return CENTRAL_DIR_PARSE_ERR_INVALID_SIGNATURE;
        }
        actual_entries++;

        // Extract metadata (initial 32-bit values, may be overwritten by ZIP64)
        file_array[i].local_header_offset = header->local_header_offset;
        file_array[i].compressed_size = header->compressed_size;
        file_array[i].uncompressed_size = header->uncompressed_size;
        file_array[i].crc32 = header->crc32;
        file_array[i].compression_method = header->compression_method;
        file_array[i].uses_zip64_descriptor = false;  // Will be set if ZIP64 extra field found

        // Extract Unix mode from external_file_attributes
        // Unix stores mode in upper 16 bits of external_file_attributes
        // Check if version_made_by indicates Unix (upper byte == 3)
        uint8_t made_by_os = (header->version_made_by >> 8) & 0xFF;
        if (made_by_os == 3) {  // Unix
            file_array[i].unix_mode = header->external_file_attributes >> 16;
            file_array[i].has_unix_mode = true;

            // Check if this is a symlink (S_IFLNK = 0120000)
            file_array[i].is_symlink = ((file_array[i].unix_mode & S_IFMT) == S_IFLNK);
        } else {
            file_array[i].unix_mode = 0;
            file_array[i].has_unix_mode = false;
            file_array[i].is_symlink = false;
        }

        // Initialize extra field info (will be parsed below)
        file_array[i].uid = 0;
        file_array[i].gid = 0;
        file_array[i].has_unix_extra = false;

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

        // Parse extra fields
        if (header->extra_field_length > 0) {
            const uint8_t *extra_field_ptr = ptr + header->filename_length;

            // Parse Unix uid/gid (0x7875)
            file_array[i].has_unix_extra = parse_unix_extra_field(
                extra_field_ptr, header->extra_field_length,
                &file_array[i].uid, &file_array[i].gid);

            // Parse ZIP64 extra field (0x0001)
            // The presence of ZIP64 extra field indicates the file uses ZIP64 data descriptor
            file_array[i].uses_zip64_descriptor = parse_zip64_extra_field(
                extra_field_ptr, header->extra_field_length, header,
                &file_array[i].compressed_size,
                &file_array[i].uncompressed_size,
                &file_array[i].local_header_offset);
        }

        // Calculate part index (must be done after ZIP64 parsing updates local_header_offset)
        file_array[i].part_index = (uint32_t)(file_array[i].local_header_offset / part_size);

        // Skip past variable-length fields
        ptr += variable_len;
    }

    *files = file_array;
    *num_files = actual_entries;

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

int central_dir_parse_eocd_only(const uint8_t *buffer, size_t buffer_size,
                                 uint64_t archive_size,
                                 uint64_t *central_dir_offset,
                                 uint64_t *central_dir_size,
                                 uint64_t *num_entries,
                                 bool *is_zip64,
                                 char *error_msg) {
    // Validate inputs
    if (!buffer || buffer_size == 0 || !central_dir_offset || !central_dir_size ||
        !is_zip64 || !error_msg) {
        if (error_msg) {
            snprintf(error_msg, 256, "Invalid parameters");
        }
        return CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER;
    }

    // Find EOCD
    size_t eocd_offset;
    size_t locator_offset = 0;
    *is_zip64 = false;
    int rc = find_eocd(buffer, buffer_size, &eocd_offset, is_zip64, &locator_offset);
    if (rc != CENTRAL_DIR_PARSE_SUCCESS) {
        if (rc == CENTRAL_DIR_PARSE_ERR_NO_EOCD) {
            snprintf(error_msg, 256, "No End of Central Directory signature found in buffer");
        }
        return rc;
    }

    // Parse EOCD (ZIP64 or standard)
    uint64_t cd_offset;
    uint64_t num_entries_64;
    uint64_t cd_size_64;

    if (*is_zip64) {
        // Parse ZIP64 EOCD locator to find ZIP64 EOCD offset
        if (locator_offset + sizeof(struct zip64_end_central_dir_locator) > buffer_size) {
            snprintf(error_msg, 256, "ZIP64 EOCD Locator truncated at offset %zu", locator_offset);
            return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
        }

        const struct zip64_end_central_dir_locator *locator =
            (const struct zip64_end_central_dir_locator *)(buffer + locator_offset);

        // The locator contains the absolute archive offset of ZIP64 EOCD
        uint64_t eocd64_archive_offset = locator->eocd64_offset;

        // Calculate buffer's position within the archive
        uint64_t buffer_offset = archive_size - buffer_size;

        // Check if ZIP64 EOCD is within our buffer
        if (eocd64_archive_offset < buffer_offset) {
            snprintf(error_msg, 256,
                    "ZIP64 EOCD at offset %llu is outside buffer (buffer starts at %llu)",
                    (unsigned long long)eocd64_archive_offset,
                    (unsigned long long)buffer_offset);
            return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
        }

        size_t eocd64_buffer_offset = (size_t)(eocd64_archive_offset - buffer_offset);

        rc = parse_zip64_eocd(buffer, buffer_size, eocd64_buffer_offset,
                              &cd_offset, &num_entries_64, &cd_size_64);
        if (rc != CENTRAL_DIR_PARSE_SUCCESS) {
            snprintf(error_msg, 256, "Failed to parse ZIP64 EOCD at offset %zu",
                    eocd64_buffer_offset);
            return rc;
        }
    } else {
        // Parse standard EOCD
        uint32_t num_entries_32, cd_size_32;
        rc = parse_eocd(buffer, buffer_size, eocd_offset,
                        &cd_offset, &num_entries_32, &cd_size_32);
        if (rc != CENTRAL_DIR_PARSE_SUCCESS) {
            snprintf(error_msg, 256, "Failed to parse EOCD at offset %zu", eocd_offset);
            return rc;
        }
        cd_size_64 = cd_size_32;
    }

    // Validate that CD extent is within archive bounds
    if (cd_offset >= archive_size) {
        snprintf(error_msg, 256,
                "Central directory offset %llu is beyond archive size %llu (corrupted archive)",
                (unsigned long long)cd_offset, (unsigned long long)archive_size);
        return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
    }
    if (cd_offset + cd_size_64 > archive_size) {
        snprintf(error_msg, 256,
                "Central directory extends beyond archive (offset %llu + size %llu > archive size %llu)",
                (unsigned long long)cd_offset, (unsigned long long)cd_size_64,
                (unsigned long long)archive_size);
        return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
    }

    *central_dir_offset = cd_offset;
    *central_dir_size = cd_size_64;
    if (num_entries) {
        *num_entries = num_entries_64;
    }
    error_msg[0] = '\0';

    return CENTRAL_DIR_PARSE_SUCCESS;
}

int central_dir_parse(const uint8_t *buffer, size_t buffer_size,
                      uint64_t archive_size,
                      uint64_t part_size,
                      struct central_dir_parse_result *result) {
    // Initialize result structure first
    if (result) {
        memset(result, 0, sizeof(*result));
    }

    // Validate inputs early
    if (!buffer || buffer_size == 0 || !result) {
        if (result) {
            result->error_code = CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER;
            snprintf(result->error_message, sizeof(result->error_message),
                    "Invalid parameters: buffer=%p size=%zu result=%p",
                    (void*)buffer, buffer_size, (void*)result);
        }
        return CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER;
    }

    // Parse EOCD first to get CD location
    uint64_t cd_offset, cd_size;
    bool is_zip64;
    char error_msg[256];

    int rc = central_dir_parse_eocd_only(buffer, buffer_size, archive_size,
                                          &cd_offset, &cd_size, NULL, &is_zip64, error_msg);
    if (rc != CENTRAL_DIR_PARSE_SUCCESS) {
        result->error_code = rc;
        result->is_zip64 = is_zip64;
        snprintf(result->error_message, sizeof(result->error_message), "%s", error_msg);
        return rc;
    }

    // Record is_zip64 status from EOCD
    result->is_zip64 = is_zip64;

    // Calculate pointer to CD within buffer
    uint64_t buffer_start = archive_size - buffer_size;
    if (cd_offset < buffer_start) {
        result->error_code = CENTRAL_DIR_PARSE_ERR_TRUNCATED;
        snprintf(result->error_message, sizeof(result->error_message),
                "CD at offset %llu is before buffer start %llu",
                (unsigned long long)cd_offset, (unsigned long long)buffer_start);
        return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
    }

    size_t cd_offset_in_buffer = (size_t)(cd_offset - buffer_start);
    const uint8_t *cd_data = buffer + cd_offset_in_buffer;
    size_t cd_data_size = buffer_size - cd_offset_in_buffer;

    return central_dir_parse_from_cd_buffer(cd_data, cd_data_size, cd_offset, cd_size,
                                             archive_size, part_size, is_zip64, result);
}

int central_dir_parse_from_cd_buffer(const uint8_t *cd_buffer, size_t cd_buffer_size,
                                      uint64_t cd_offset, uint64_t cd_size,
                                      uint64_t archive_size, uint64_t part_size,
                                      bool is_zip64,
                                      struct central_dir_parse_result *result) {
    // Initialize result structure
    if (result) {
        memset(result, 0, sizeof(*result));
    }

    // Validate inputs
    if (!cd_buffer || cd_buffer_size == 0 || !result) {
        if (result) {
            result->error_code = CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER;
            snprintf(result->error_message, sizeof(result->error_message),
                    "Invalid parameters: cd_buffer=%p size=%zu result=%p",
                    (void*)cd_buffer, cd_buffer_size, (void*)result);
        }
        return CENTRAL_DIR_PARSE_ERR_INVALID_BUFFER;
    }

    // Validate that we have enough data for the CD
    if (cd_buffer_size < cd_size) {
        result->error_code = CENTRAL_DIR_PARSE_ERR_TRUNCATED;
        snprintf(result->error_message, sizeof(result->error_message),
                "CD buffer too small: have %zu bytes, need %llu bytes",
                cd_buffer_size, (unsigned long long)cd_size);
        return CENTRAL_DIR_PARSE_ERR_TRUNCATED;
    }

    result->is_zip64 = is_zip64;
    result->central_dir_offset = cd_offset;
    result->central_dir_size = cd_size;

    // For the assembled buffer case, the buffer starts at cd_offset
    // and contains exactly the CD data. We pass buffer_offset = cd_offset
    // so parse_central_directory calculates cd_start_in_buffer = 0.
    //
    // We don't have num_entries from this call path, so we pass a large value
    // and let the parser stop when it hits cd_size or an invalid signature.
    // This works because parse_central_directory validates entry count dynamically.
    uint64_t estimated_entries = cd_size / 46;  // Minimum entry size is ~46 bytes
    if (estimated_entries == 0) {
        estimated_entries = 1;
    }

    int rc = parse_central_directory(cd_buffer, cd_buffer_size,
                                     cd_offset, cd_offset,  // buffer_offset == cd_offset
                                     estimated_entries, cd_size, part_size,
                                     &result->files, &result->num_files);
    if (rc != CENTRAL_DIR_PARSE_SUCCESS) {
        result->error_code = rc;
        snprintf(result->error_message, sizeof(result->error_message),
                "Failed to parse central directory at offset %llu",
                (unsigned long long)cd_offset);
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
