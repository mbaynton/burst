#include "zip_structures.h"
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
                      uint16_t compression_method, uint16_t flags) {
    struct zip_local_header header;
    memset(&header, 0, sizeof(header));

    header.signature = ZIP_LOCAL_FILE_HEADER_SIG;
    header.version_needed = (compression_method == ZIP_METHOD_ZSTD) ? ZIP_VERSION_ZSTD :
                           (compression_method == ZIP_METHOD_DEFLATE) ? ZIP_VERSION_DEFLATE :
                           ZIP_VERSION_STORE;
    header.flags = flags;
    header.compression_method = compression_method;

    // Get current time
    time_t now = time(NULL);
    dos_datetime_from_time_t(now, &header.last_mod_time, &header.last_mod_date);

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
        header.extra_field_length = 0;
        header.file_comment_length = 0;
        header.disk_number_start = 0;
        header.internal_file_attributes = 0;
        header.external_file_attributes = 0100644 << 16;  // Unix permissions: rw-r--r--
        header.local_header_offset = (uint32_t)entry->local_header_offset;

        // Write central directory header
        if (burst_writer_write(writer, &header, sizeof(header)) != 0) {
            return -1;
        }

        // Write filename
        if (burst_writer_write(writer, entry->filename, header.filename_length) != 0) {
            return -1;
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
