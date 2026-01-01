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
                         uint64_t compressed_size, uint64_t uncompressed_size) {
    struct zip_data_descriptor desc;

    desc.signature = ZIP_DATA_DESCRIPTOR_SIG;
    desc.crc32 = crc32;

    // Phase 1: Use 32-bit sizes (ZIP64 will be added in Phase 4)
    desc.compressed_size = (uint32_t)compressed_size;
    desc.uncompressed_size = (uint32_t)uncompressed_size;

    return burst_writer_write(writer, &desc, sizeof(desc));
}

int write_central_directory(struct burst_writer *writer) {
    for (size_t i = 0; i < writer->num_files; i++) {
        struct file_entry *entry = &writer->files[i];
        struct zip_central_header header;
        memset(&header, 0, sizeof(header));

        // Build Unix extra field for central directory
        uint8_t extra_field[16];
        size_t extra_field_len = build_unix_extra_field(extra_field, sizeof(extra_field),
                                                         entry->uid, entry->gid);

        header.signature = ZIP_CENTRAL_DIR_HEADER_SIG;
        header.version_made_by = (3 << 8) | 63;  // Unix (3) + version 6.3
        header.version_needed = entry->version_needed;
        header.flags = entry->general_purpose_flags;
        header.compression_method = entry->compression_method;
        header.last_mod_time = entry->last_mod_time;
        header.last_mod_date = entry->last_mod_date;
        header.crc32 = entry->crc32;

        // Phase 1: Use 32-bit sizes
        header.compressed_size = (uint32_t)entry->compressed_size;
        header.uncompressed_size = (uint32_t)entry->uncompressed_size;

        header.filename_length = strlen(entry->filename);
        header.extra_field_length = extra_field_len;
        header.file_comment_length = 0;
        header.disk_number_start = 0;
        header.internal_file_attributes = 0;
        header.external_file_attributes = entry->unix_mode << 16;  // Unix mode in upper 16 bits
        header.local_header_offset = (uint32_t)entry->local_header_offset;

        // Write central directory header
        if (burst_writer_write(writer, &header, sizeof(header)) != 0) {
            return -1;
        }

        // Write filename
        if (burst_writer_write(writer, entry->filename, header.filename_length) != 0) {
            return -1;
        }

        // Write extra field
        if (extra_field_len > 0) {
            if (burst_writer_write(writer, extra_field, extra_field_len) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int write_end_central_directory(struct burst_writer *writer, uint64_t central_dir_start) {
    struct zip_end_central_dir eocd;
    memset(&eocd, 0, sizeof(eocd));

    uint64_t central_dir_size = (writer->current_offset + writer->buffer_used) - central_dir_start;

    eocd.signature = ZIP_END_CENTRAL_DIR_SIG;
    eocd.disk_number = 0;
    eocd.disk_with_cd = 0;
    eocd.num_entries_this_disk = (uint16_t)writer->num_files;
    eocd.num_entries_total = (uint16_t)writer->num_files;
    eocd.central_dir_size = (uint32_t)central_dir_size;
    eocd.central_dir_offset = (uint32_t)central_dir_start;
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
