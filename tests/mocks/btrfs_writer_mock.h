// Mockable version of btrfs_writer.h for testing
#ifndef BTRFS_WRITER_MOCK_H
#define BTRFS_WRITER_MOCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Error codes (must match btrfs_writer.h)
#define BTRFS_WRITER_SUCCESS 0
#define BTRFS_WRITER_ERR_INVALID_ARGS -1
#define BTRFS_WRITER_ERR_NOT_BTRFS -2
#define BTRFS_WRITER_ERR_IOCTL_FAILED -3
#define BTRFS_WRITER_ERR_DECOMPRESS_FAILED -4
#define BTRFS_WRITER_ERR_WRITE_FAILED -5

/**
 * Write a Zstandard-compressed frame directly to BTRFS.
 * Mockable version for unit testing.
 */
int do_write_encoded(
    int fd,
    const uint8_t *zstd_frame,
    size_t frame_len,
    uint64_t uncompressed_len,
    uint64_t file_offset);

/**
 * Fallback: decompress and write normally.
 * Mockable version for unit testing.
 */
int do_write_unencoded(
    int fd,
    const uint8_t *zstd_frame,
    size_t frame_len,
    uint64_t uncompressed_len,
    uint64_t file_offset);

/**
 * Check if a file descriptor is on a BTRFS filesystem.
 * Mockable version for unit testing.
 */
bool is_btrfs_filesystem(int fd);

#endif // BTRFS_WRITER_MOCK_H
