/**
 * Integration tests for central directory parser.
 *
 * Tests parsing of real ZIP files created by different tools:
 * - burst_writer32.zip: Created by burst-writer (Zstandard compression)
 * - zip_writer32.zip: Created by Info-ZIP (standard deflate)
 *
 * Both archives contain two files: test1.txt and test2.txt
 */

#include "unity.h"
#include "central_dir_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Path to fixtures directory (set by main based on executable location)
static char fixtures_dir[512];

void setUp(void) {
    // Nothing to set up
}

void tearDown(void) {
    // Nothing to tear down
}

/**
 * Read entire file into a buffer.
 * Returns NULL on error, caller must free the buffer.
 */
static uint8_t *read_file(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return NULL;
    }

    uint8_t *buffer = malloc((size_t)size);
    if (!buffer) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buffer, 1, (size_t)size, f);
    fclose(f);

    if (read != (size_t)size) {
        free(buffer);
        return NULL;
    }

    *size_out = (size_t)size;
    return buffer;
}

/**
 * Helper to find a file by name in the parse result.
 * Returns pointer to file_metadata or NULL if not found.
 */
static struct file_metadata *find_file_by_name(
    struct central_dir_parse_result *result,
    const char *filename)
{
    for (size_t i = 0; i < result->num_files; i++) {
        if (strcmp(result->files[i].filename, filename) == 0) {
            return &result->files[i];
        }
    }
    return NULL;
}

/**
 * Common test logic for both ZIP files.
 */
static void test_zip_file(const char *zip_name, const char *description) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", fixtures_dir, zip_name);

    // Read the ZIP file
    size_t file_size;
    uint8_t *buffer = read_file(path, &file_size);
    TEST_ASSERT_NOT_NULL_MESSAGE(buffer, "Failed to read ZIP file");

    // Parse the central directory
    struct central_dir_parse_result result;
    int rc = central_dir_parse(buffer, file_size, file_size, BURST_BASE_PART_SIZE, &result);

    TEST_ASSERT_EQUAL_INT_MESSAGE(CENTRAL_DIR_PARSE_SUCCESS, rc,
        result.error_message[0] ? result.error_message : "Parse failed");

    // Should have exactly 2 files
    TEST_ASSERT_EQUAL_size_t_MESSAGE(2, result.num_files,
        "Expected 2 files in archive");

    // Find test1.txt
    struct file_metadata *test1 = find_file_by_name(&result, "test1.txt");
    TEST_ASSERT_NOT_NULL_MESSAGE(test1, "test1.txt not found in archive");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(14, test1->uncompressed_size,
        "test1.txt should have 14 bytes uncompressed");

    // Find test2.txt
    struct file_metadata *test2 = find_file_by_name(&result, "test2.txt");
    TEST_ASSERT_NOT_NULL_MESSAGE(test2, "test2.txt not found in archive");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(33, test2->uncompressed_size,
        "test2.txt should have 33 bytes uncompressed");

    // Both files should be in part 0 (archive is tiny)
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, test1->part_index,
        "test1.txt should be in part 0");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, test2->part_index,
        "test2.txt should be in part 0");

    // Should have exactly 1 part
    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, result.num_parts,
        "Expected 1 part for small archive");

    // Part 0 should have 2 entries
    TEST_ASSERT_EQUAL_size_t_MESSAGE(2, result.parts[0].num_entries,
        "Part 0 should have 2 file entries");

    // No continuing file in part 0
    TEST_ASSERT_NULL_MESSAGE(result.parts[0].continuing_file,
        "Part 0 should have no continuing file");

    // Entries should be sorted by offset
    TEST_ASSERT_TRUE_MESSAGE(
        result.parts[0].entries[0].offset_in_part <
        result.parts[0].entries[1].offset_in_part,
        "Entries should be sorted by offset_in_part");

    printf("  ✓ %s: Parsed successfully, found test1.txt (%lu bytes) and test2.txt (%lu bytes)\n",
           description,
           (unsigned long)test1->uncompressed_size,
           (unsigned long)test2->uncompressed_size);

    central_dir_parse_result_free(&result);
    free(buffer);
}

void test_parse_burst_writer_zip(void) {
    test_zip_file("burst_writer32.zip", "BURST writer archive (Zstandard)");
}

void test_parse_info_zip_writer_zip(void) {
    test_zip_file("zip_writer32.zip", "Info-ZIP archive (Deflate/Store)");
}

int main(int argc, char **argv) {
    // Determine fixtures directory from executable path
    // Executable is in build/tests/, fixtures are in tests/fixtures/
    if (argc > 0 && argv[0]) {
        // Try to find fixtures relative to executable
        char *last_slash = strrchr(argv[0], '/');
        if (last_slash) {
            size_t dir_len = (size_t)(last_slash - argv[0]);
            snprintf(fixtures_dir, sizeof(fixtures_dir),
                     "%.*s/../../tests/fixtures", (int)dir_len, argv[0]);
        } else {
            // Executable in current directory
            snprintf(fixtures_dir, sizeof(fixtures_dir), "../../tests/fixtures");
        }
    } else {
        // Fallback
        snprintf(fixtures_dir, sizeof(fixtures_dir), "tests/fixtures");
    }

    printf("=== Central Directory Parser Integration Tests ===\n");
    printf("Fixtures directory: %s\n\n", fixtures_dir);

    UNITY_BEGIN();

    RUN_TEST(test_parse_burst_writer_zip);
    RUN_TEST(test_parse_info_zip_writer_zip);

    return UNITY_END();
}
