#include "profiling.h"

#ifdef BURST_PROFILE

#include <stdio.h>
#include <string.h>
#include <errno.h>

// Global stats instance
struct burst_profile_stats g_profile_stats;

void burst_profile_init(void) {
    // Zero all counters
    memset(&g_profile_stats, 0, sizeof(g_profile_stats));

    // Record start time
    clock_gettime(CLOCK_MONOTONIC, &g_profile_stats.start_time);
}

void burst_profile_finalize(void) {
    // Record end time
    clock_gettime(CLOCK_MONOTONIC, &g_profile_stats.end_time);
}

// Helper to convert nanoseconds to seconds
static double ns_to_seconds(uint64_t ns) {
    return (double)ns / 1000000000.0;
}

// Helper to format bytes as human-readable
static void format_bytes(uint64_t bytes, char *buf, size_t buf_size) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(buf, buf_size, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024) {
        snprintf(buf, buf_size, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(buf, buf_size, "%.2f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, buf_size, "%lu bytes", (unsigned long)bytes);
    }
}

// Helper to calculate throughput in MB/s
static double calc_throughput_mbps(uint64_t bytes, uint64_t time_ns) {
    if (time_ns == 0) return 0.0;
    double seconds = ns_to_seconds(time_ns);
    double megabytes = (double)bytes / (1024.0 * 1024.0);
    return megabytes / seconds;
}

void burst_profile_print_stats(void) {
    // Calculate total duration
    uint64_t start_ns = (uint64_t)g_profile_stats.start_time.tv_sec * 1000000000ULL +
                        (uint64_t)g_profile_stats.start_time.tv_nsec;
    uint64_t end_ns = (uint64_t)g_profile_stats.end_time.tv_sec * 1000000000ULL +
                      (uint64_t)g_profile_stats.end_time.tv_nsec;
    uint64_t total_duration_ns = end_ns - start_ns;
    double total_duration_s = ns_to_seconds(total_duration_ns);

    // Load atomic values
    uint64_t inode_count = atomic_load(&g_profile_stats.inode_count);
    uint64_t inode_time = atomic_load(&g_profile_stats.inode_time_ns);

    uint64_t encoded_count = atomic_load(&g_profile_stats.write_encoded_count);
    uint64_t encoded_time = atomic_load(&g_profile_stats.write_encoded_time_ns);
    uint64_t encoded_bytes = atomic_load(&g_profile_stats.write_encoded_bytes);

    uint64_t unencoded_count = atomic_load(&g_profile_stats.write_unencoded_count);
    uint64_t unencoded_time = atomic_load(&g_profile_stats.write_unencoded_time_ns);
    uint64_t unencoded_bytes = atomic_load(&g_profile_stats.write_unencoded_bytes);

    uint64_t s3_requests = atomic_load(&g_profile_stats.s3_requests);
    uint64_t s3_time = atomic_load(&g_profile_stats.s3_time_ns);
    uint64_t s3_bytes = atomic_load(&g_profile_stats.s3_bytes);

    // Calculate percentages
    double inode_pct = total_duration_ns > 0 ? 100.0 * inode_time / total_duration_ns : 0.0;
    double encoded_pct = total_duration_ns > 0 ? 100.0 * encoded_time / total_duration_ns : 0.0;
    double unencoded_pct = total_duration_ns > 0 ? 100.0 * unencoded_time / total_duration_ns : 0.0;
    double s3_pct = total_duration_ns > 0 ? 100.0 * s3_time / total_duration_ns : 0.0;

    // Format byte counts
    char encoded_bytes_str[32], unencoded_bytes_str[32], s3_bytes_str[32];
    format_bytes(encoded_bytes, encoded_bytes_str, sizeof(encoded_bytes_str));
    format_bytes(unencoded_bytes, unencoded_bytes_str, sizeof(unencoded_bytes_str));
    format_bytes(s3_bytes, s3_bytes_str, sizeof(s3_bytes_str));

    printf("BURST Downloader Profile:\n");
    printf("========================\n");
    printf("Total Duration: %.3f seconds\n\n", total_duration_s);

    printf("Inode Management:\n");
    printf("  Operations: %lu\n", (unsigned long)inode_count);
    printf("  Total time: %.3fs (%.1f%% of total)\n", ns_to_seconds(inode_time), inode_pct);
    if (inode_count > 0) {
        printf("  Avg time: %.3fms per operation\n",
               ns_to_seconds(inode_time) * 1000.0 / inode_count);
    }

    printf("\nWrite Operations:\n");
    printf("  BTRFS Encoded: %lu ops, %s, %.3fs (%.1f%%)\n",
           (unsigned long)encoded_count, encoded_bytes_str,
           ns_to_seconds(encoded_time), encoded_pct);
    if (encoded_time > 0) {
        printf("    Throughput: %.1f MB/s\n", calc_throughput_mbps(encoded_bytes, encoded_time));
    }

    printf("  Unencoded Fallback: %lu ops, %s, %.3fs (%.1f%%)\n",
           (unsigned long)unencoded_count, unencoded_bytes_str,
           ns_to_seconds(unencoded_time), unencoded_pct);
    if (unencoded_time > 0) {
        printf("    Throughput: %.1f MB/s\n", calc_throughput_mbps(unencoded_bytes, unencoded_time));
    }

    printf("\nS3 Network:\n");
    printf("  Requests: %lu, %s, %.3fs (%.1f%%)\n",
           (unsigned long)s3_requests, s3_bytes_str,
           ns_to_seconds(s3_time), s3_pct);
    if (s3_time > 0) {
        printf("  Throughput: %.1f MB/s\n", calc_throughput_mbps(s3_bytes, s3_time));
    }

    // Show accounted vs unaccounted time
    uint64_t accounted_time = inode_time + encoded_time + unencoded_time + s3_time;
    double accounted_pct = total_duration_ns > 0 ? 100.0 * accounted_time / total_duration_ns : 0.0;
    printf("\nTime Accounting:\n");
    printf("  Accounted: %.3fs (%.1f%%)\n", ns_to_seconds(accounted_time), accounted_pct);
    if (total_duration_ns > accounted_time) {
        uint64_t unaccounted = total_duration_ns - accounted_time;
        printf("  Unaccounted: %.3fs (%.1f%%) - overhead, CD parsing, etc.\n",
               ns_to_seconds(unaccounted), 100.0 - accounted_pct);
    }
}

int burst_profile_write_json(const char *output_path) {
    FILE *f = fopen(output_path, "w");
    if (!f) {
        fprintf(stderr, "Warning: failed to create profile JSON at %s: %s\n",
                output_path, strerror(errno));
        return -1;
    }

    // Calculate total duration
    uint64_t start_ns = (uint64_t)g_profile_stats.start_time.tv_sec * 1000000000ULL +
                        (uint64_t)g_profile_stats.start_time.tv_nsec;
    uint64_t end_ns = (uint64_t)g_profile_stats.end_time.tv_sec * 1000000000ULL +
                      (uint64_t)g_profile_stats.end_time.tv_nsec;
    uint64_t total_duration_ns = end_ns - start_ns;

    // Load atomic values
    uint64_t inode_count = atomic_load(&g_profile_stats.inode_count);
    uint64_t inode_time = atomic_load(&g_profile_stats.inode_time_ns);

    uint64_t encoded_count = atomic_load(&g_profile_stats.write_encoded_count);
    uint64_t encoded_time = atomic_load(&g_profile_stats.write_encoded_time_ns);
    uint64_t encoded_bytes = atomic_load(&g_profile_stats.write_encoded_bytes);

    uint64_t unencoded_count = atomic_load(&g_profile_stats.write_unencoded_count);
    uint64_t unencoded_time = atomic_load(&g_profile_stats.write_unencoded_time_ns);
    uint64_t unencoded_bytes = atomic_load(&g_profile_stats.write_unencoded_bytes);

    uint64_t s3_requests = atomic_load(&g_profile_stats.s3_requests);
    uint64_t s3_time = atomic_load(&g_profile_stats.s3_time_ns);
    uint64_t s3_bytes = atomic_load(&g_profile_stats.s3_bytes);

    fprintf(f, "{\n");
    fprintf(f, "  \"version\": \"1.0\",\n");
    fprintf(f, "  \"duration_seconds\": %.6f,\n", ns_to_seconds(total_duration_ns));

    fprintf(f, "  \"inode_management\": {\n");
    fprintf(f, "    \"count\": %lu,\n", (unsigned long)inode_count);
    fprintf(f, "    \"time_seconds\": %.6f\n", ns_to_seconds(inode_time));
    fprintf(f, "  },\n");

    fprintf(f, "  \"write_operations\": {\n");
    fprintf(f, "    \"encoded\": {\n");
    fprintf(f, "      \"count\": %lu,\n", (unsigned long)encoded_count);
    fprintf(f, "      \"bytes\": %lu,\n", (unsigned long)encoded_bytes);
    fprintf(f, "      \"time_seconds\": %.6f\n", ns_to_seconds(encoded_time));
    fprintf(f, "    },\n");
    fprintf(f, "    \"unencoded\": {\n");
    fprintf(f, "      \"count\": %lu,\n", (unsigned long)unencoded_count);
    fprintf(f, "      \"bytes\": %lu,\n", (unsigned long)unencoded_bytes);
    fprintf(f, "      \"time_seconds\": %.6f\n", ns_to_seconds(unencoded_time));
    fprintf(f, "    }\n");
    fprintf(f, "  },\n");

    fprintf(f, "  \"s3_network\": {\n");
    fprintf(f, "    \"requests\": %lu,\n", (unsigned long)s3_requests);
    fprintf(f, "    \"bytes\": %lu,\n", (unsigned long)s3_bytes);
    fprintf(f, "    \"time_seconds\": %.6f\n", ns_to_seconds(s3_time));
    fprintf(f, "  }\n");

    fprintf(f, "}\n");

    fclose(f);
    return 0;
}

#endif  // BURST_PROFILE
