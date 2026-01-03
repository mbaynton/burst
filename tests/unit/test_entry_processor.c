/*
 * Unit tests for entry_processor.c
 *
 * Tests error handling paths to ensure no double-free or resource leaks
 * when burst_writer_add_* functions fail.
 */
#include "unity.h"
#include "Mock_burst_writer_mock.h"
#include "entry_processor.h"
#include <sys/stat.h>
#include <string.h>

void setUp(void) {
    Mock_burst_writer_mock_Init();
}

void tearDown(void) {
    Mock_burst_writer_mock_Verify();
    Mock_burst_writer_mock_Destroy();
}

/*
 * Test that directory add failure doesn't cause double-free.
 * Previously, the error path freed lfh and then the unconditional
 * cleanup also freed it, causing a double-free crash.
 */
void test_directory_add_failure_no_double_free(void) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFDIR | 0755;
    st.st_uid = 1000;
    st.st_gid = 1000;
    st.st_mtime = 1700000000;

    // Mock burst_writer_add_directory to return error
    // Use IgnoreAndReturn since we don't care about the exact arguments
    burst_writer_add_directory_IgnoreAndReturn(-1);

    // Should return 0 (failure) but NOT crash from double-free
    // The test passing without SIGABRT proves no double-free occurred
    int result = process_entry(NULL, "/test/dir/", "dir/", NULL, &st, true);
    TEST_ASSERT_EQUAL(0, result);
}

/*
 * Test that symlink add failure doesn't cause double-free.
 */
void test_symlink_add_failure_no_double_free(void) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFLNK | 0777;
    st.st_uid = 1000;
    st.st_gid = 1000;

    // Mock burst_writer_add_symlink to return error
    burst_writer_add_symlink_IgnoreAndReturn(-1);

    int result = process_entry(NULL, "/test/link", "link", "target", &st, false);
    TEST_ASSERT_EQUAL(0, result);
}

/*
 * Test that directory add success returns 1.
 */
void test_directory_add_success(void) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFDIR | 0755;
    st.st_uid = 1000;
    st.st_gid = 1000;
    st.st_mtime = 1700000000;

    burst_writer_add_directory_IgnoreAndReturn(0);

    int result = process_entry(NULL, "/test/dir/", "dir/", NULL, &st, true);
    TEST_ASSERT_EQUAL(1, result);
}

/*
 * Test that symlink add success returns 1.
 */
void test_symlink_add_success(void) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFLNK | 0777;
    st.st_uid = 1000;
    st.st_gid = 1000;

    burst_writer_add_symlink_IgnoreAndReturn(0);

    int result = process_entry(NULL, "/test/link", "link", "target", &st, false);
    TEST_ASSERT_EQUAL(1, result);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_directory_add_failure_no_double_free);
    RUN_TEST(test_symlink_add_failure_no_double_free);
    RUN_TEST(test_directory_add_success);
    RUN_TEST(test_symlink_add_success);
    return UNITY_END();
}
