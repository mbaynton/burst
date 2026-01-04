#include "btrfs_writer.h"
#include "profiling.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/statfs.h>
#include <zstd.h>

// BTRFS magic number for statfs
#define BTRFS_SUPER_MAGIC 0x9123683E

// Include BTRFS ioctl definitions
// On Ubuntu/Debian, install libbtrfs-dev for <btrfs/ioctl.h>
// Alternatively, define the structures inline for portability
#ifdef __has_include
#if __has_include(<btrfs/ioctl.h>)
#include <btrfs/ioctl.h>
#define HAVE_BTRFS_IOCTL_H 1
#endif
#endif

#ifndef HAVE_BTRFS_IOCTL_H
// Define BTRFS encoded write structures inline if header not available
// These match the kernel definitions

#define BTRFS_IOC_ENCODED_WRITE _IOW(0x94, 64, struct btrfs_ioctl_encoded_io_args)

#define BTRFS_ENCODED_IO_COMPRESSION_ZSTD 2

struct btrfs_ioctl_encoded_io_args {
    const struct iovec *iov;
    unsigned long iovcnt;
    int64_t offset;
    uint64_t flags;
    uint64_t len;
    uint64_t unencoded_len;
    uint64_t unencoded_offset;
    uint32_t compression;
    uint32_t encryption;
    uint8_t reserved[64];
};
#endif

// Maximum extent size for BTRFS encoded writes (128 KiB)
#define MAX_EXTENT_SIZE (128 * 1024)

// Thread-local decompression context for unencoded writes
static __thread ZSTD_DCtx *decompress_ctx = NULL;
static __thread uint8_t *decompress_buffer = NULL;

int do_write_encoded(
    int fd,
    const uint8_t *zstd_frame,
    size_t frame_len,
    uint64_t uncompressed_len,
    uint64_t file_offset)
{
    if (fd < 0 || zstd_frame == NULL || frame_len == 0) {
        return BTRFS_WRITER_ERR_INVALID_ARGS;
    }

    // BTRFS requires compressed size < uncompressed size for encoded writes
    if (frame_len >= uncompressed_len) {
        // Fallback to unencoded write
        return do_write_unencoded(fd, zstd_frame, frame_len, uncompressed_len, file_offset);
    }

    // BTRFS requires uncompressed size <= 128 KiB
    if (uncompressed_len > MAX_EXTENT_SIZE) {
        fprintf(stderr, "btrfs_writer: uncompressed size %lu exceeds max %d\n",
                (unsigned long)uncompressed_len, MAX_EXTENT_SIZE);
        return BTRFS_WRITER_ERR_INVALID_ARGS;
    }

    struct iovec iov;
    iov.iov_base = (void *)zstd_frame;
    iov.iov_len = frame_len;

    struct btrfs_ioctl_encoded_io_args enc;
    memset(&enc, 0, sizeof(enc));
    enc.iov = &iov;
    enc.iovcnt = 1;
    enc.offset = (int64_t)file_offset;
    enc.flags = 0;
    enc.len = uncompressed_len;
    enc.unencoded_len = uncompressed_len;
    enc.unencoded_offset = 0;
    enc.compression = BTRFS_ENCODED_IO_COMPRESSION_ZSTD;
    enc.encryption = 0;

#ifdef BURST_PROFILE
    uint64_t start_time = burst_profile_get_time_ns();
#endif

    int ret = ioctl(fd, BTRFS_IOC_ENCODED_WRITE, &enc);

#ifdef BURST_PROFILE
    int saved_errno_profile = errno;
    uint64_t elapsed = burst_profile_get_time_ns() - start_time;
    errno = saved_errno_profile;
#endif

    if (ret < 0) {
        int saved_errno = errno;
        if (saved_errno == ENOTTY || saved_errno == EOPNOTSUPP) {
            // Not a BTRFS filesystem or encoded writes not supported
            // Fall back to unencoded write
            return do_write_unencoded(fd, zstd_frame, frame_len, uncompressed_len, file_offset);
        }
        fprintf(stderr, "btrfs_writer: BTRFS_IOC_ENCODED_WRITE failed: %s\n"
                "  fd=%d, file_offset=%lu, frame_len=%zu, uncompressed_len=%lu\n"
                "  enc.offset=%ld, enc.len=%lu, enc.unencoded_len=%lu, enc.unencoded_offset=%lu\n"
                "  enc.compression=%u, enc.encryption=%u, enc.flags=%lu\n",
                strerror(saved_errno),
                fd, (unsigned long)file_offset, frame_len, (unsigned long)uncompressed_len,
                (long)enc.offset, (unsigned long)enc.len, (unsigned long)enc.unencoded_len,
                (unsigned long)enc.unencoded_offset,
                enc.compression, enc.encryption, (unsigned long)enc.flags);
        return BTRFS_WRITER_ERR_IOCTL_FAILED;
    }

#ifdef BURST_PROFILE
    PROFILE_ADD(g_profile_stats.write_encoded_time_ns, elapsed);
    PROFILE_COUNT(g_profile_stats.write_encoded_count);
    PROFILE_ADD(g_profile_stats.write_encoded_bytes, uncompressed_len);
#endif

    return BTRFS_WRITER_SUCCESS;
}

int do_write_unencoded(
    int fd,
    const uint8_t *zstd_frame,
    size_t frame_len,
    uint64_t uncompressed_len,
    uint64_t file_offset)
{
    if (fd < 0 || zstd_frame == NULL || frame_len == 0) {
        return BTRFS_WRITER_ERR_INVALID_ARGS;
    }

    if (uncompressed_len > MAX_EXTENT_SIZE) {
        fprintf(stderr, "btrfs_writer: uncompressed size %lu exceeds max %d\n",
                (unsigned long)uncompressed_len, MAX_EXTENT_SIZE);
        return BTRFS_WRITER_ERR_INVALID_ARGS;
    }

    // Lazy allocation of thread-local decompression resources
    if (decompress_ctx == NULL) {
        decompress_ctx = ZSTD_createDCtx();
        if (decompress_ctx == NULL) {
            fprintf(stderr, "btrfs_writer: failed to create ZSTD decompression context\n");
            return BTRFS_WRITER_ERR_DECOMPRESS_FAILED;
        }
    }

    if (decompress_buffer == NULL) {
        decompress_buffer = malloc(MAX_EXTENT_SIZE);
        if (decompress_buffer == NULL) {
            fprintf(stderr, "btrfs_writer: failed to allocate decompression buffer\n");
            return BTRFS_WRITER_ERR_DECOMPRESS_FAILED;
        }
    }

    // Decompress the frame
    size_t actual_size = ZSTD_decompressDCtx(
        decompress_ctx,
        decompress_buffer,
        MAX_EXTENT_SIZE,
        zstd_frame,
        frame_len);

    if (ZSTD_isError(actual_size)) {
        fprintf(stderr, "btrfs_writer: decompression failed: %s\n",
                ZSTD_getErrorName(actual_size));
        return BTRFS_WRITER_ERR_DECOMPRESS_FAILED;
    }

    if (actual_size != uncompressed_len) {
        fprintf(stderr, "btrfs_writer: decompressed size %zu != expected %lu\n",
                actual_size, (unsigned long)uncompressed_len);
        // Continue anyway, write what we got
    }

    // Write uncompressed data using pwrite for atomic positioning
#ifdef BURST_PROFILE
    uint64_t start_time = burst_profile_get_time_ns();
#endif

    ssize_t written = pwrite(fd, decompress_buffer, actual_size, (off_t)file_offset);

#ifdef BURST_PROFILE
    int saved_errno_profile = errno;
    uint64_t elapsed = burst_profile_get_time_ns() - start_time;
    errno = saved_errno_profile;
#endif

    if (written < 0) {
        fprintf(stderr, "btrfs_writer: pwrite failed: %s\n", strerror(errno));
        return BTRFS_WRITER_ERR_WRITE_FAILED;
    }

    if ((size_t)written != actual_size) {
        fprintf(stderr, "btrfs_writer: short write: %zd of %zu bytes\n",
                written, actual_size);
        return BTRFS_WRITER_ERR_WRITE_FAILED;
    }

#ifdef BURST_PROFILE
    PROFILE_ADD(g_profile_stats.write_unencoded_time_ns, elapsed);
    PROFILE_COUNT(g_profile_stats.write_unencoded_count);
    PROFILE_ADD(g_profile_stats.write_unencoded_bytes, actual_size);
#endif

    return BTRFS_WRITER_SUCCESS;
}

bool is_btrfs_filesystem(int fd)
{
    struct statfs sfs;
    if (fstatfs(fd, &sfs) != 0) {
        return false;
    }
    return sfs.f_type == BTRFS_SUPER_MAGIC;
}
