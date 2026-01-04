#ifndef PROFILING_H
#define PROFILING_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef BURST_PROFILE
#include <stdatomic.h>
#endif

/**
 * @file profiling.h
 * @brief Profiling and statistics gathering for BURST downloader.
 *
 * When compiled with -DBURST_PROFILE, this module tracks timing for:
 * - Inode management syscalls (open, mkdir, chmod, chown, truncate, symlink, close)
 * - Write operations (BTRFS_IOC_ENCODED_WRITE ioctl and pwrite fallback)
 * - S3 network time (excluding processing time in callbacks)
 *
 * All counters use C11 atomics for thread-safe accumulation from concurrent
 * part downloads.
 */

#ifdef BURST_PROFILE

/**
 * Profiling statistics structure with atomic counters.
 * Thread-safe for concurrent updates from multiple AWS event loop threads.
 */
struct burst_profile_stats {
    // Inode management (open, mkdir, chmod, chown, truncate, symlink, close)
    atomic_uint_fast64_t inode_count;       // Number of inode syscalls
    atomic_uint_fast64_t inode_time_ns;     // Total time in nanoseconds

    // BTRFS encoded writes (BTRFS_IOC_ENCODED_WRITE ioctl)
    atomic_uint_fast64_t write_encoded_count;      // Number of encoded writes
    atomic_uint_fast64_t write_encoded_time_ns;    // Time in encoded writes
    atomic_uint_fast64_t write_encoded_bytes;      // Uncompressed bytes written

    // Unencoded writes (pwrite fallback when encoded write not possible)
    atomic_uint_fast64_t write_unencoded_count;    // Number of pwrite calls
    atomic_uint_fast64_t write_unencoded_time_ns;  // Time in unencoded writes
    atomic_uint_fast64_t write_unencoded_bytes;    // Bytes written via pwrite

    // S3 network time (excludes processing time in callbacks)
    atomic_uint_fast64_t s3_requests;       // Number of S3 requests
    atomic_uint_fast64_t s3_time_ns;        // Network time (total - callback processing)
    atomic_uint_fast64_t s3_bytes;          // Bytes downloaded from S3

    // Overall timing
    struct timespec start_time;             // Profiling start time
    struct timespec end_time;               // Profiling end time
};

// Global stats instance (defined in profiling.c)
extern struct burst_profile_stats g_profile_stats;

/**
 * Initialize profiling. Call at program start.
 * Zeros all counters and records start time.
 */
void burst_profile_init(void);

/**
 * Finalize profiling. Call at program end.
 * Records end time for duration calculation.
 */
void burst_profile_finalize(void);

/**
 * Print statistics summary to stdout.
 * Shows category breakdowns with times and percentages.
 */
void burst_profile_print_stats(void);

/**
 * Write detailed statistics to JSON file.
 * @param output_path Path to write JSON file
 * @return 0 on success, -1 on error
 */
int burst_profile_write_json(const char *output_path);

/**
 * Get current time in nanoseconds using CLOCK_MONOTONIC.
 * @return Current time in nanoseconds
 */
static inline uint64_t burst_profile_get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * Macro for timing a block of code and adding to a time counter.
 * Also preserves errno across the timing operations.
 *
 * Usage:
 *   PROFILE_TIME_BLOCK(g_profile_stats.inode_time_ns, {
 *       result = open(path, O_RDONLY);
 *   });
 */
#define PROFILE_TIME_BLOCK(time_counter, code) \
    do { \
        uint64_t _profile_start = burst_profile_get_time_ns(); \
        code; \
        int _saved_errno = errno; \
        uint64_t _profile_elapsed = burst_profile_get_time_ns() - _profile_start; \
        atomic_fetch_add(&(time_counter), _profile_elapsed); \
        errno = _saved_errno; \
    } while(0)

/**
 * Macro to increment a counter atomically.
 */
#define PROFILE_COUNT(counter) \
    atomic_fetch_add(&(counter), 1)

/**
 * Macro to add a value to a counter atomically.
 */
#define PROFILE_ADD(counter, value) \
    atomic_fetch_add(&(counter), (uint_fast64_t)(value))

#else  // BURST_PROFILE not defined

// No-op versions when profiling is disabled
#define PROFILE_TIME_BLOCK(time_counter, code) do { code; } while(0)
#define PROFILE_COUNT(counter) ((void)0)
#define PROFILE_ADD(counter, value) ((void)0)

// Stub functions (never called, but allows code to compile without #ifdefs everywhere)
static inline void burst_profile_init(void) {}
static inline void burst_profile_finalize(void) {}
static inline void burst_profile_print_stats(void) {}
static inline int burst_profile_write_json(const char *output_path) { (void)output_path; return 0; }
static inline uint64_t burst_profile_get_time_ns(void) { return 0; }

#endif  // BURST_PROFILE

#endif // PROFILING_H
