#ifndef ZIP_STRUCTURES_H
#define ZIP_STRUCTURES_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "burst_writer.h"

// ZIP format signatures
#define ZIP_LOCAL_FILE_HEADER_SIG 0x04034b50
#define ZIP_DATA_DESCRIPTOR_SIG 0x08074b50
#define ZIP_CENTRAL_DIR_HEADER_SIG 0x02014b50
#define ZIP_END_CENTRAL_DIR_SIG 0x06054b50
#define ZIP_ZIP64_END_CENTRAL_DIR_SIG 0x06064b50
#define ZIP_ZIP64_END_CENTRAL_DIR_LOCATOR_SIG 0x07064b50

// ZIP compression methods
#define ZIP_METHOD_STORE 0
#define ZIP_METHOD_DEFLATE 8
#define ZIP_METHOD_ZSTD 93

// ZIP general purpose bit flags
#define ZIP_FLAG_DATA_DESCRIPTOR 0x0008

// ZIP version needed
#define ZIP_VERSION_STORE 10   // 1.0
#define ZIP_VERSION_DEFLATE 20  // 2.0
#define ZIP_VERSION_ZSTD 63     // 6.3

// ZIP local file header (fixed portion)
struct zip_local_header {
    uint32_t signature;           // 0x04034b50
    uint16_t version_needed;      // Version needed to extract
    uint16_t flags;              // General purpose bit flags
    uint16_t compression_method; // Compression method
    uint16_t last_mod_time;      // Last modification time
    uint16_t last_mod_date;      // Last modification date
    uint32_t crc32;              // CRC-32
    uint32_t compressed_size;    // Compressed size (or 0xFFFFFFFF for ZIP64)
    uint32_t uncompressed_size;  // Uncompressed size (or 0xFFFFFFFF for ZIP64)
    uint16_t filename_length;    // Filename length
    uint16_t extra_field_length; // Extra field length
    // Followed by: filename, extra field
} __attribute__((packed));

// ZIP data descriptor (follows compressed data when bit 3 is set)
struct zip_data_descriptor {
    uint32_t signature;          // 0x08074b50 (optional but recommended)
    uint32_t crc32;              // CRC-32
    uint32_t compressed_size;    // Compressed size (or 0xFFFFFFFF for ZIP64)
    uint32_t uncompressed_size;  // Uncompressed size (or 0xFFFFFFFF for ZIP64)
    // Note: For ZIP64, these are 64-bit values instead
} __attribute__((packed));

// ZIP central directory header (fixed portion)
struct zip_central_header {
    uint32_t signature;              // 0x02014b50
    uint16_t version_made_by;        // Version made by
    uint16_t version_needed;         // Version needed to extract
    uint16_t flags;                  // General purpose bit flags
    uint16_t compression_method;     // Compression method
    uint16_t last_mod_time;          // Last modification time
    uint16_t last_mod_date;          // Last modification date
    uint32_t crc32;                  // CRC-32
    uint32_t compressed_size;        // Compressed size
    uint32_t uncompressed_size;      // Uncompressed size
    uint16_t filename_length;        // Filename length
    uint16_t extra_field_length;     // Extra field length
    uint16_t file_comment_length;    // File comment length
    uint16_t disk_number_start;      // Disk number where file starts
    uint16_t internal_file_attributes;
    uint32_t external_file_attributes;
    uint32_t local_header_offset;    // Offset of local header
    // Followed by: filename, extra field, comment
} __attribute__((packed));

// ZIP end of central directory record
struct zip_end_central_dir {
    uint32_t signature;              // 0x06054b50
    uint16_t disk_number;            // Number of this disk
    uint16_t disk_with_cd;           // Disk where central directory starts
    uint16_t num_entries_this_disk;  // Number of entries on this disk
    uint16_t num_entries_total;      // Total number of entries
    uint32_t central_dir_size;       // Size of central directory
    uint32_t central_dir_offset;     // Offset of central directory
    uint16_t comment_length;         // Comment length
    // Followed by: comment
} __attribute__((packed));

// Function declarations
int write_local_header(struct burst_writer *writer, const char *filename,
                      uint16_t compression_method, uint16_t flags);
int write_data_descriptor(struct burst_writer *writer, uint32_t crc32,
                          uint64_t compressed_size, uint64_t uncompressed_size);
int write_central_directory(struct burst_writer *writer);
int write_end_central_directory(struct burst_writer *writer, uint64_t central_dir_start);

// Utility functions
void dos_datetime_from_time_t(time_t t, uint16_t *time_out, uint16_t *date_out);
size_t get_local_header_size(const char *filename);
size_t get_central_header_size(const char *filename);

#endif // ZIP_STRUCTURES_H
