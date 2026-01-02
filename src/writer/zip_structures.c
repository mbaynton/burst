#include "zip_structures.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// DOS date/time conversion
void dos_datetime_from_time_t(time_t t, uint16_t *time_out, uint16_t *date_out) {
    struct tm *tm = localtime(&t);
    if (!tm) {
        // Fallback to epoch
        *time_out = 0;
        *date_out = (1 << 5) | 1;  // 1980-01-01
        return;
    }

    // DOS time: bits 0-4=seconds/2, 5-10=minutes, 11-15=hours
    *time_out = ((tm->tm_hour & 0x1F) << 11) |
                ((tm->tm_min & 0x3F) << 5) |
                ((tm->tm_sec / 2) & 0x1F);

    // DOS date: bits 0-4=day, 5-8=month, 9-15=year-1980
    *date_out = (((tm->tm_year - 80) & 0x7F) << 9) |
                (((tm->tm_mon + 1) & 0x0F) << 5) |
                (tm->tm_mday & 0x1F);
}

size_t get_local_header_size(const char *filename) {
    return sizeof(struct zip_local_header) + strlen(filename);
}

size_t get_central_header_size(const char *filename) {
    return sizeof(struct zip_central_header) + strlen(filename);
}

size_t get_data_descriptor_size(uint64_t compressed_size, uint64_t uncompressed_size) {
    // Use ZIP64 descriptor if either size exceeds 32-bit limit
    if (compressed_size > 0xFFFFFFFF || uncompressed_size > 0xFFFFFFFF) {
        return sizeof(struct zip_data_descriptor_zip64);  // 24 bytes
    }
    return sizeof(struct zip_data_descriptor);  // 16 bytes
}

int write_local_header(struct burst_writer *writer, const char *filename,
                      uint16_t compression_method, uint16_t flags,
                      uint16_t last_mod_time, uint16_t last_mod_date) {
    struct zip_local_header header;
    memset(&header, 0, sizeof(header));

    header.signature = ZIP_LOCAL_FILE_HEADER_SIG;
    header.version_needed = (compression_method == ZIP_METHOD_ZSTD) ? ZIP_VERSION_ZSTD :
                           (compression_method == ZIP_METHOD_DEFLATE) ? ZIP_VERSION_DEFLATE :
                           ZIP_VERSION_STORE;
    header.flags = flags;
    header.compression_method = compression_method;
    header.last_mod_time = last_mod_time;
    header.last_mod_date = last_mod_date;

    // CRC, sizes will be in data descriptor
    header.crc32 = 0;
    header.compressed_size = 0;
    header.uncompressed_size = 0;

    header.filename_length = strlen(filename);
    header.extra_field_length = 0;

    // Write header
    if (burst_writer_write(writer, &header, sizeof(header)) != 0) {
        return -1;
    }

    // Write filename
    if (burst_writer_write(writer, filename, header.filename_length) != 0) {
        return -1;
    }

    return 0;
}

int write_data_descriptor(struct burst_writer *writer, uint32_t crc32,
                         uint64_t compressed_size, uint64_t uncompressed_size,
                         bool use_zip64) {
    if (use_zip64) {
        // Write ZIP64 data descriptor (24 bytes)
        struct zip_data_descriptor_zip64 desc;
        desc.signature = ZIP_DATA_DESCRIPTOR_SIG;
        desc.crc32 = crc32;
        desc.compressed_size = compressed_size;
        desc.uncompressed_size = uncompressed_size;
        return burst_writer_write(writer, &desc, sizeof(desc));
    } else {
        // Write standard 32-bit data descriptor (16 bytes)
        struct zip_data_descriptor desc;
        desc.signature = ZIP_DATA_DESCRIPTOR_SIG;
        desc.crc32 = crc32;
        desc.compressed_size = (uint32_t)compressed_size;
        desc.uncompressed_size = (uint32_t)uncompressed_size;
        return burst_writer_write(writer, &desc, sizeof(desc));
    }
}

int write_central_directory(struct burst_writer *writer) {
    for (size_t i = 0; i < writer->num_files; i++) {
        struct file_entry *entry = &writer->files[i];
        struct zip_central_header header;
        memset(&header, 0, sizeof(header));

        // Build extra fields for central directory
        // Buffer holds Unix extra field (15 bytes) + ZIP64 extra field (up to 28 bytes)
        uint8_t extra_field[64];
        size_t extra_field_len = 0;

        // Add Unix extra field
        extra_field_len = build_unix_extra_field(extra_field, sizeof(extra_field),
                                                 entry->uid, entry->gid);

        // Determine if ZIP64 extra field is needed for this entry
        bool need_zip64 = (entry->compressed_size > 0xFFFFFFFF) ||
                          (entry->uncompressed_size > 0xFFFFFFFF) ||
                          (entry->local_header_offset > 0xFFFFFFFF);

        // Add ZIP64 extra field if needed
        size_t zip64_len = 0;
        if (need_zip64) {
            zip64_len = build_zip64_extra_field(
                extra_field + extra_field_len,
                sizeof(extra_field) - extra_field_len,
                entry->compressed_size,
                entry->uncompressed_size,
                entry->local_header_offset);

            if (zip64_len == 0) {
                fprintf(stderr, "Failed to build ZIP64 extra field for %s\n", entry->filename);
                return -1;
            }
            extra_field_len += zip64_len;
        }

        header.signature = ZIP_CENTRAL_DIR_HEADER_SIG;
        header.version_made_by = (3 << 8) | 63;  // Unix (3) + version 6.3
        header.version_needed = entry->version_needed;
        header.flags = entry->general_purpose_flags;
        header.compression_method = entry->compression_method;
        header.last_mod_time = entry->last_mod_time;
        header.last_mod_date = entry->last_mod_date;
        header.crc32 = entry->crc32;

        // Set sizes and offset - use 0xFFFFFFFF marker if ZIP64 is needed
        if (entry->compressed_size > 0xFFFFFFFF) {
            header.compressed_size = 0xFFFFFFFF;
        } else {
            header.compressed_size = (uint32_t)entry->compressed_size;
        }

        if (entry->uncompressed_size > 0xFFFFFFFF) {
            header.uncompressed_size = 0xFFFFFFFF;
        } else {
            header.uncompressed_size = (uint32_t)entry->uncompressed_size;
        }

        if (entry->local_header_offset > 0xFFFFFFFF) {
            header.local_header_offset = 0xFFFFFFFF;
        } else {
            header.local_header_offset = (uint32_t)entry->local_header_offset;
        }

        header.filename_length = strlen(entry->filename);
        header.extra_field_length = extra_field_len;
        header.file_comment_length = 0;
        header.disk_number_start = 0;
        header.internal_file_attributes = 0;
        header.external_file_attributes = entry->unix_mode << 16;  // Unix mode in upper 16 bits

        // Write central directory header
        if (burst_writer_write(writer, &header, sizeof(header)) != 0) {
            return -1;
        }

        // Write filename
        if (burst_writer_write(writer, entry->filename, header.filename_length) != 0) {
            return -1;
        }

        // Write extra field (Unix + ZIP64 if present)
        if (extra_field_len > 0) {
            if (burst_writer_write(writer, extra_field, extra_field_len) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int write_zip64_end_central_directory(struct burst_writer *writer,
                                      uint64_t central_dir_start,
                                      uint64_t central_dir_size) {
    struct zip64_end_central_dir eocd64;
    memset(&eocd64, 0, sizeof(eocd64));

    eocd64.signature = ZIP_ZIP64_END_CENTRAL_DIR_SIG;
    eocd64.eocd64_size = sizeof(eocd64) - 12;  // Size of remaining structure (44 bytes)
    eocd64.version_made_by = (3 << 8) | ZIP_VERSION_ZSTD;  // Unix (3) + version 6.3
    eocd64.version_needed = ZIP_VERSION_ZSTD;
    eocd64.disk_number = 0;
    eocd64.disk_with_cd = 0;
    eocd64.num_entries_this_disk = writer->num_files;
    eocd64.num_entries_total = writer->num_files;
    eocd64.central_dir_size = central_dir_size;
    eocd64.central_dir_offset = central_dir_start;

    return burst_writer_write(writer, &eocd64, sizeof(eocd64));
}

int write_zip64_end_central_directory_locator(struct burst_writer *writer,
                                              uint64_t eocd64_offset) {
    struct zip64_end_central_dir_locator locator;
    memset(&locator, 0, sizeof(locator));

    locator.signature = ZIP_ZIP64_END_CENTRAL_DIR_LOCATOR_SIG;
    locator.disk_with_eocd64 = 0;
    locator.eocd64_offset = eocd64_offset;
    locator.total_disks = 1;

    return burst_writer_write(writer, &locator, sizeof(locator));
}

int write_end_central_directory(struct burst_writer *writer,
                                uint64_t central_dir_start,
                                uint64_t central_dir_size) {
    struct zip_end_central_dir eocd;
    memset(&eocd, 0, sizeof(eocd));

    eocd.signature = ZIP_END_CENTRAL_DIR_SIG;
    eocd.disk_number = 0;
    eocd.disk_with_cd = 0;

    // Use actual values when they fit in 32-bit fields, otherwise 0xFFFF/0xFFFFFFFF
    // The ZIP64 EOCD always has the authoritative values, but we include actual
    // values here for compatibility with tools that check both
    eocd.num_entries_this_disk = (writer->num_files > 0xFFFE) ? 0xFFFF : (uint16_t)writer->num_files;
    eocd.num_entries_total = (writer->num_files > 0xFFFE) ? 0xFFFF : (uint16_t)writer->num_files;
    eocd.central_dir_size = (central_dir_size > 0xFFFFFFFE) ? 0xFFFFFFFF : (uint32_t)central_dir_size;
    eocd.central_dir_offset = (central_dir_start > 0xFFFFFFFE) ? 0xFFFFFFFF : (uint32_t)central_dir_start;
    eocd.comment_length = 0;

    return burst_writer_write(writer, &eocd, sizeof(eocd));
}

int write_padding_lfh(struct burst_writer *writer, size_t target_size) {
    // Padding LFH is used to fill space to an 8 MiB boundary when adding
    // header-only files (empty files, symlinks). It is NOT added to the
    // central directory, so extractors will ignore it.
    //
    // Structure:
    //   Local File Header (30 bytes)
    //   Filename ".burst_padding" (14 bytes)
    //   Extra field (variable length zeros)
    //   No data, no data descriptor (all values known upfront)

    if (target_size < PADDING_LFH_MIN_SIZE) {
        fprintf(stderr, "Padding LFH target_size %zu too small (min %d)\n",
                target_size, PADDING_LFH_MIN_SIZE);
        return -1;
    }

    // Calculate extra field length to reach target_size
    uint16_t extra_field_len = (uint16_t)(target_size - sizeof(struct zip_local_header)
                                          - PADDING_LFH_FILENAME_LEN);

    // Get current time for timestamp
    time_t now = time(NULL);
    uint16_t mod_time, mod_date;
    dos_datetime_from_time_t(now, &mod_time, &mod_date);

    // Build the local file header
    struct zip_local_header header;
    memset(&header, 0, sizeof(header));

    header.signature = ZIP_LOCAL_FILE_HEADER_SIG;
    header.version_needed = ZIP_VERSION_STORE;
    header.flags = 0;  // No data descriptor
    header.compression_method = ZIP_METHOD_STORE;
    header.last_mod_time = mod_time;
    header.last_mod_date = mod_date;
    header.crc32 = 0;
    header.compressed_size = 0;
    header.uncompressed_size = 0;
    header.filename_length = PADDING_LFH_FILENAME_LEN;
    header.extra_field_length = extra_field_len;

    // Write header
    if (burst_writer_write(writer, &header, sizeof(header)) != 0) {
        return -1;
    }

    // Write filename
    if (burst_writer_write(writer, PADDING_LFH_FILENAME, PADDING_LFH_FILENAME_LEN) != 0) {
        return -1;
    }

    // Write extra field (zeros)
    if (extra_field_len > 0) {
        uint8_t *extra = calloc(1, extra_field_len);
        if (!extra) {
            fprintf(stderr, "Failed to allocate padding LFH extra field\n");
            return -1;
        }

        int result = burst_writer_write(writer, extra, extra_field_len);
        free(extra);

        if (result != 0) {
            return -1;
        }
    }

    // Track padding bytes
    writer->padding_bytes += target_size;

    return 0;
}

size_t build_unix_extra_field(uint8_t *buffer, size_t buffer_size, uint32_t uid, uint32_t gid) {
    // Info-ZIP Unix extra field (0x7875) format:
    //   Header ID:  0x7875 (2 bytes)
    //   TSize:      Total data size (2 bytes)
    //   Version:    1 (1 byte)
    //   UIDSize:    Size of UID field (1 byte)
    //   UID:        User ID (UIDSize bytes, little-endian)
    //   GIDSize:    Size of GID field (1 byte)
    //   GID:        Group ID (GIDSize bytes, little-endian)
    //
    // We always use 4-byte uid/gid for simplicity (covers all practical values)
    // Total size: 2 + 2 + 1 + 1 + 4 + 1 + 4 = 15 bytes

    const size_t extra_field_size = 15;
    if (buffer_size < extra_field_size) {
        return 0;
    }

    size_t offset = 0;

    // Header ID (0x7875) - little-endian
    buffer[offset++] = ZIP_EXTRA_UNIX_7875_ID & 0xFF;
    buffer[offset++] = (ZIP_EXTRA_UNIX_7875_ID >> 8) & 0xFF;

    // TSize: data size after this field (1 + 1 + 4 + 1 + 4 = 11 bytes)
    uint16_t tsize = 11;
    buffer[offset++] = tsize & 0xFF;
    buffer[offset++] = (tsize >> 8) & 0xFF;

    // Version: 1
    buffer[offset++] = 1;

    // UIDSize: 4 bytes
    buffer[offset++] = 4;

    // UID: 4 bytes, little-endian
    buffer[offset++] = uid & 0xFF;
    buffer[offset++] = (uid >> 8) & 0xFF;
    buffer[offset++] = (uid >> 16) & 0xFF;
    buffer[offset++] = (uid >> 24) & 0xFF;

    // GIDSize: 4 bytes
    buffer[offset++] = 4;

    // GID: 4 bytes, little-endian
    buffer[offset++] = gid & 0xFF;
    buffer[offset++] = (gid >> 8) & 0xFF;
    buffer[offset++] = (gid >> 16) & 0xFF;
    buffer[offset++] = (gid >> 24) & 0xFF;

    return extra_field_size;
}

size_t build_zip64_extra_field(uint8_t *buffer, size_t buffer_size,
                               uint64_t compressed_size,
                               uint64_t uncompressed_size,
                               uint64_t local_header_offset) {
    // ZIP64 extended information extra field (0x0001) format:
    //   Header ID:  0x0001 (2 bytes)
    //   Data Size:  Size of following data (2 bytes)
    //   Uncompressed Size: (8 bytes) - only if original field is 0xFFFFFFFF
    //   Compressed Size:   (8 bytes) - only if original field is 0xFFFFFFFF
    //   Local Header Offset: (8 bytes) - only if original field is 0xFFFFFFFF
    //   Disk Start Number: (4 bytes) - only if original field is 0xFFFF (not used in BURST)
    //
    // Fields must appear in the order listed above, but only include those that overflow.

    // Determine which fields need to be included
    bool need_uncompressed = (uncompressed_size > 0xFFFFFFFF);
    bool need_compressed = (compressed_size > 0xFFFFFFFF);
    bool need_offset = (local_header_offset > 0xFFFFFFFF);

    // If no fields overflow, no ZIP64 extra field is needed
    if (!need_uncompressed && !need_compressed && !need_offset) {
        return 0;
    }

    // Calculate data size (excluding 4-byte header)
    size_t data_size = 0;
    if (need_uncompressed) data_size += 8;
    if (need_compressed) data_size += 8;
    if (need_offset) data_size += 8;

    size_t total_size = 4 + data_size;  // Header ID (2) + Data Size (2) + data

    if (buffer_size < total_size) {
        return 0;  // Buffer too small
    }

    size_t offset = 0;

    // Header ID (0x0001) - little-endian
    buffer[offset++] = ZIP_EXTRA_ZIP64_ID & 0xFF;
    buffer[offset++] = (ZIP_EXTRA_ZIP64_ID >> 8) & 0xFF;

    // Data size - little-endian
    buffer[offset++] = data_size & 0xFF;
    buffer[offset++] = (data_size >> 8) & 0xFF;

    // Add fields in required order: uncompressed, compressed, offset
    if (need_uncompressed) {
        memcpy(buffer + offset, &uncompressed_size, 8);
        offset += 8;
    }
    if (need_compressed) {
        memcpy(buffer + offset, &compressed_size, 8);
        offset += 8;
    }
    if (need_offset) {
        memcpy(buffer + offset, &local_header_offset, 8);
        offset += 8;
    }

    return total_size;
}
