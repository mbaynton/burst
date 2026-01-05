#ifndef ZIP_STRUCTURES_H
#define ZIP_STRUCTURES_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <zlib.h>
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

// Padding LFH constants
#define PADDING_LFH_FILENAME ".burst_padding"
#define PADDING_LFH_FILENAME_LEN 14
#define PADDING_LFH_MIN_SIZE 44  // 30 (header) + 14 (filename), no descriptor

// ZIP extra field IDs
#define ZIP_EXTRA_UNIX_7875_ID 0x7875  // Info-ZIP Unix extra field (uid/gid)
#define ZIP_EXTRA_ZIP64_ID 0x0001      // ZIP64 extended information extra field

// BURST EOCD comment format (8 bytes):
// Bytes 0-3: Magic "BRST" (0x54535242 little-endian)
// Byte 4:    Version (uint8_t) - currently 1
// Bytes 5-7: Offset from TAIL START to first complete CDFH (uint24_t, little-endian)
//            The tail is the last 8 MiB of the archive.
//            This offset is relative to (archive_size - 8 MiB), NOT relative to CD start.
//            If entire CD fits in 8 MiB, value is 0
//            If no complete CDFH in last 8 MiB, value is 0xFFFFFF
//            Max representable: ~16 MiB, but always < 8 MiB in practice since offset is within tail
#define BURST_EOCD_COMMENT_MAGIC 0x54535242  // "BRST" in little-endian
#define BURST_EOCD_COMMENT_VERSION 1
#define BURST_EOCD_COMMENT_SIZE 8
#define BURST_EOCD_NO_CDFH_IN_TAIL 0xFFFFFF  // Sentinel: no complete CDFH in tail

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

// ZIP64 data descriptor (follows compressed data when bit 3 is set and sizes > 4GB)
struct zip_data_descriptor_zip64 {
    uint32_t signature;              // 0x08074b50
    uint32_t crc32;                  // CRC-32
    uint64_t compressed_size;        // 64-bit compressed size
    uint64_t uncompressed_size;      // 64-bit uncompressed size
} __attribute__((packed));

// ZIP64 End of Central Directory Record (56 bytes)
struct zip64_end_central_dir {
    uint32_t signature;              // 0x06064b50
    uint64_t eocd64_size;            // Size of EOCD64 minus 12 (44 for version 1)
    uint16_t version_made_by;        // Version made by
    uint16_t version_needed;         // Version needed to extract
    uint32_t disk_number;            // Number of this disk
    uint32_t disk_with_cd;           // Disk where central directory starts
    uint64_t num_entries_this_disk;  // Number of entries on this disk
    uint64_t num_entries_total;      // Total number of entries
    uint64_t central_dir_size;       // Size of central directory
    uint64_t central_dir_offset;     // Offset of central directory
    // No extensible data sector in BURST implementation
} __attribute__((packed));

// ZIP64 End of Central Directory Locator (20 bytes)
struct zip64_end_central_dir_locator {
    uint32_t signature;              // 0x07064b50
    uint32_t disk_with_eocd64;       // Disk where EOCD64 is located
    uint64_t eocd64_offset;          // Offset of EOCD64
    uint32_t total_disks;            // Total number of disks
} __attribute__((packed));

// Function declarations
int write_local_header(struct burst_writer *writer, const char *filename,
                      uint16_t compression_method, uint16_t flags,
                      uint16_t last_mod_time, uint16_t last_mod_date);
int write_data_descriptor(struct burst_writer *writer, uint32_t crc32,
                          uint64_t compressed_size, uint64_t uncompressed_size,
                          bool use_zip64);
int write_central_directory(struct burst_writer *writer);
int write_zip64_end_central_directory(struct burst_writer *writer,
                                      uint64_t central_dir_start,
                                      uint64_t central_dir_size);
int write_zip64_end_central_directory_locator(struct burst_writer *writer,
                                              uint64_t eocd64_offset);
int write_end_central_directory(struct burst_writer *writer,
                                uint64_t central_dir_start,
                                uint64_t central_dir_size,
                                uint32_t first_cdfh_offset_in_tail);

// Build BURST EOCD comment containing offset to first complete CDFH in tail buffer
// Returns: 8-byte comment buffer (caller provides), sets comment to BURST format
void build_burst_eocd_comment(uint8_t *comment, uint32_t first_cdfh_offset_in_tail);

// Utility functions
void dos_datetime_from_time_t(time_t t, uint16_t *time_out, uint16_t *date_out);
size_t get_local_header_size(const char *filename);
size_t get_central_header_size(const char *filename);
size_t get_data_descriptor_size(uint64_t compressed_size, uint64_t uncompressed_size);

// Write an unlisted padding LFH (not added to central directory)
// Used to pad to boundaries for header-only files (empty files, symlinks)
int write_padding_lfh(struct burst_writer *writer, size_t target_size);

// Build Info-ZIP Unix extra field (0x7875) containing uid/gid
// Returns the size of the extra field written, or 0 on error
// The buffer must be at least 15 bytes (2+2+1+1+4+1+4)
// Format: Header ID (2) + TSize (2) + Version (1) + UIDSize (1) + UID (4) + GIDSize (1) + GID (4)
size_t build_unix_extra_field(uint8_t *buffer, size_t buffer_size, uint32_t uid, uint32_t gid);

// Build ZIP64 extended information extra field (0x0001) for central directory
// Returns the size of the extra field written, or 0 if ZIP64 not needed or error
// Fields are included only if they overflow 32-bit, in order: uncompressed, compressed, offset
size_t build_zip64_extra_field(uint8_t *buffer, size_t buffer_size,
                               uint64_t compressed_size,
                               uint64_t uncompressed_size,
                               uint64_t local_header_offset);

#endif // ZIP_STRUCTURES_H
