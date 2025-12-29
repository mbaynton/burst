#ifndef BTRFS_WRITER_H
#define BTRFS_WRITER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @file btrfs_writer.h
 * @brief BTRFS encoded write interface for BURST archive downloader.
 *
 * This module wraps the BTRFS_IOC_ENCODED_WRITE ioctl for writing
 * Zstandard-compressed frames directly to BTRFS without decompression.
 * It also provides a fallback for writing unencoded data when the
 * compressed size exceeds uncompressed size.
 *
 * ## BTRFS Requirements
 *
 * - File must be on a BTRFS filesystem
 * - Compression must be Zstandard with compression level between -15 and 15
 * - Uncompressed size must not exceed 128 KiB
 * - Frame must include content size in header
 *
 * ## Error Handling
 *
 * Functions return 0 on success, negative errno on failure. Common errors:
 * - ENOTTY: File is not on BTRFS
 * - EINVAL: Invalid frame or parameters
 * - ENOSPC: Disk full
 */

// Error codes (in addition to errno values)
#define BTRFS_WRITER_SUCCESS 0
#define BTRFS_WRITER_ERR_INVALID_ARGS -1
#define BTRFS_WRITER_ERR_NOT_BTRFS -2
#define BTRFS_WRITER_ERR_IOCTL_FAILED -3
#define BTRFS_WRITER_ERR_DECOMPRESS_FAILED -4
#define BTRFS_WRITER_ERR_WRITE_FAILED -5

/**
 * Write a Zstandard-compressed frame directly to BTRFS.
 *
 * Uses BTRFS_IOC_ENCODED_WRITE ioctl to write compressed data that will
 * be stored on disk in compressed form and decompressed on read.
 *
 * @param fd File descriptor (must be on BTRFS, opened for writing)
 * @param zstd_frame Pointer to complete Zstandard frame including header
 * @param frame_len Length of compressed frame in bytes
 * @param uncompressed_len Uncompressed size (from ZSTD_getFrameContentSize)
 * @param file_offset Byte offset in file where uncompressed data should appear
 * @return 0 on success, negative error code on failure
 */
int do_write_encoded(
    int fd,
    const uint8_t *zstd_frame,
    size_t frame_len,
    uint64_t uncompressed_len,
    uint64_t file_offset);

/**
 * Fallback: decompress and write normally.
 *
 * Used when compressed size exceeds uncompressed size, or when the
 * filesystem is not BTRFS. Decompresses the frame in memory and writes
 * the uncompressed data using pwrite().
 *
 * @param fd File descriptor (opened for writing)
 * @param zstd_frame Pointer to complete Zstandard frame including header
 * @param frame_len Length of compressed frame in bytes
 * @param uncompressed_len Expected decompressed size
 * @param file_offset Byte offset in file where data should be written
 * @return 0 on success, negative error code on failure
 */
int do_write_unencoded(
    int fd,
    const uint8_t *zstd_frame,
    size_t frame_len,
    uint64_t uncompressed_len,
    uint64_t file_offset);

/**
 * Check if a file descriptor is on a BTRFS filesystem.
 *
 * @param fd File descriptor
 * @return true if on BTRFS, false otherwise
 */
bool is_btrfs_filesystem(int fd);

#endif // BTRFS_WRITER_H
