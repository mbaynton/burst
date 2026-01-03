# Unity test framework
add_library(unity STATIC
    unity/unity.c
)
target_include_directories(unity PUBLIC unity)

# CMock test framework
include(FetchContent)

FetchContent_Declare(
    cmock
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/CMock.git
    GIT_TAG v2.5.3
)
FetchContent_MakeAvailable(cmock)

# Build CMock library
add_library(cmock STATIC
    ${cmock_SOURCE_DIR}/src/cmock.c
)
target_include_directories(cmock PUBLIC
    ${cmock_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/unity
)
target_link_libraries(cmock PUBLIC unity)

# Helper library with writer components for testing
add_library(burst_writer_lib STATIC
    ../src/writer/burst_writer.c
    ../src/writer/zip_structures.c
    ../src/writer/compression.c
    ../src/writer/alignment.c
)
target_include_directories(burst_writer_lib PUBLIC
    ../include
    ${ZSTD_INCLUDE_DIR}
)
target_link_libraries(burst_writer_lib PUBLIC
    ZLIB::ZLIB
    ${ZSTD_LIBRARY}
)
# Enable debug assertions for tests
target_compile_definitions(burst_writer_lib PUBLIC DEBUG)

# Find Ruby for CMock generation
find_program(RUBY_EXECUTABLE ruby)
if(NOT RUBY_EXECUTABLE)
    message(FATAL_ERROR "Ruby required for CMock mock generation. Install with: apt install ruby")
endif()

# Helper function to add unit tests
function(add_unit_test test_name)
    add_executable(${test_name} unit/${test_name}.c)
    target_link_libraries(${test_name}
        burst_writer_lib
        unity
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

# Helper function to add mocked unit tests
function(add_mocked_test test_name mock_headers)
    set(mock_sources "")
    set(mock_includes "")

    foreach(header ${mock_headers})
        get_filename_component(mock_name ${header} NAME_WE)
        set(mock_c "${CMAKE_CURRENT_BINARY_DIR}/mocks/Mock_${mock_name}.c")
        set(mock_h "${CMAKE_CURRENT_BINARY_DIR}/mocks/Mock_${mock_name}.h")
        list(APPEND mock_sources ${mock_c})

        add_custom_command(
            OUTPUT ${mock_c} ${mock_h}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/mocks
            COMMAND ${RUBY_EXECUTABLE} ${cmock_SOURCE_DIR}/lib/cmock.rb
                    -o${CMAKE_CURRENT_SOURCE_DIR}/cmock_config.yml
                    --mock_path=${CMAKE_CURRENT_BINARY_DIR}/mocks
                    ${header}
            DEPENDS ${header} ${CMAKE_CURRENT_SOURCE_DIR}/cmock_config.yml
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating mock for ${header}"
        )
    endforeach()

    add_executable(${test_name}
        unit/${test_name}.c
        ${mock_sources}
    )

    target_include_directories(${test_name} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/mocks  # For Mock_*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks  # For mock headers
        ../include
    )

    target_link_libraries(${test_name}
        unity
        cmock
        burst_writer_lib
        ZLIB::ZLIB  # For CRC32 if needed
        ${ZSTD_LIBRARY}
    )

    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

# Helper library with downloader components for testing (no AWS dependencies)
add_library(burst_downloader_lib STATIC
    ../src/downloader/central_dir_parser.c
    ../src/downloader/stream_processor.c
    ../src/downloader/frame_parser.c
    ../src/downloader/btrfs_writer.c
    ../src/downloader/cd_fetch.c
)
target_include_directories(burst_downloader_lib PUBLIC
    ../include
)
target_link_libraries(burst_downloader_lib PUBLIC
    ${ZSTD_LIBRARY}
)
# Enable debug assertions for tests
target_compile_definitions(burst_downloader_lib PUBLIC DEBUG)

# Unit tests
add_unit_test(test_zip_structures)
add_unit_test(test_writer_core)
add_unit_test(test_crc32)
add_unit_test(test_zstd_frames)
add_unit_test(test_alignment)

# Test for writer helper functions (includes burst_writer.c directly for static function access)
# We include the source file directly but still need the other writer components
add_executable(test_writer_helpers
    unit/test_writer_helpers.c
    ../src/writer/zip_structures.c
    ../src/writer/compression.c
    ../src/writer/alignment.c
)
target_include_directories(test_writer_helpers PRIVATE
    ../include
    ${ZSTD_INCLUDE_DIR}
)
target_link_libraries(test_writer_helpers
    unity
    ZLIB::ZLIB
    ${ZSTD_LIBRARY}
)
add_test(NAME test_writer_helpers COMMAND test_writer_helpers)

# Downloader unit tests (use burst_downloader_lib instead of burst_writer_lib)
add_executable(test_central_dir_parser unit/test_central_dir_parser.c)
target_link_libraries(test_central_dir_parser
    burst_downloader_lib
    unity
)
add_test(NAME test_central_dir_parser COMMAND test_central_dir_parser)

# Mocked unit tests
add_mocked_test(test_writer_chunking
    "${CMAKE_CURRENT_SOURCE_DIR}/mocks/compression_mock.h")

# Entry processor test with mocked burst_writer functions
# This tests error handling paths to prevent double-free bugs
# We need a separate library without burst_writer.c since we mock those functions
add_library(entry_processor_lib STATIC
    ../src/writer/entry_processor.c
    ../src/writer/zip_structures.c
)
target_include_directories(entry_processor_lib PUBLIC
    ../include
    ../src/writer
)
target_link_libraries(entry_processor_lib PUBLIC
    ZLIB::ZLIB
)

# Generate mock for burst_writer functions
set(burst_writer_mock_header "${CMAKE_CURRENT_SOURCE_DIR}/mocks/burst_writer_mock.h")
set(burst_writer_mock_c "${CMAKE_CURRENT_BINARY_DIR}/mocks/Mock_burst_writer_mock.c")
set(burst_writer_mock_h "${CMAKE_CURRENT_BINARY_DIR}/mocks/Mock_burst_writer_mock.h")

add_custom_command(
    OUTPUT ${burst_writer_mock_c} ${burst_writer_mock_h}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/mocks
    COMMAND ${RUBY_EXECUTABLE} ${cmock_SOURCE_DIR}/lib/cmock.rb
            -o${CMAKE_CURRENT_SOURCE_DIR}/cmock_config.yml
            --mock_path=${CMAKE_CURRENT_BINARY_DIR}/mocks
            ${burst_writer_mock_header}
    DEPENDS ${burst_writer_mock_header} ${CMAKE_CURRENT_SOURCE_DIR}/cmock_config.yml
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating mock for burst_writer_mock.h"
)

add_executable(test_entry_processor
    unit/test_entry_processor.c
    ${burst_writer_mock_c}
)

target_include_directories(test_entry_processor PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/mocks
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ../include
    ../src/writer
)

target_link_libraries(test_entry_processor
    entry_processor_lib
    unity
    cmock
    ZLIB::ZLIB
)

add_test(NAME test_entry_processor COMMAND test_entry_processor)

# Stream processor test with mocked BTRFS writer
# Note: This test links against a modified downloader lib that excludes btrfs_writer.c
# because we want to use the mocked version instead
add_library(burst_downloader_lib_no_btrfs STATIC
    ../src/downloader/central_dir_parser.c
    ../src/downloader/stream_processor.c
    ../src/downloader/frame_parser.c
    ../src/downloader/cd_fetch.c
)
target_include_directories(burst_downloader_lib_no_btrfs PUBLIC
    ../include
)
target_link_libraries(burst_downloader_lib_no_btrfs PUBLIC
    ${ZSTD_LIBRARY}
)

# Generate mock for btrfs_writer
set(btrfs_mock_header "${CMAKE_CURRENT_SOURCE_DIR}/mocks/btrfs_writer_mock.h")
set(btrfs_mock_c "${CMAKE_CURRENT_BINARY_DIR}/mocks/Mock_btrfs_writer_mock.c")
set(btrfs_mock_h "${CMAKE_CURRENT_BINARY_DIR}/mocks/Mock_btrfs_writer_mock.h")

add_custom_command(
    OUTPUT ${btrfs_mock_c} ${btrfs_mock_h}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/mocks
    COMMAND ${RUBY_EXECUTABLE} ${cmock_SOURCE_DIR}/lib/cmock.rb
            -o${CMAKE_CURRENT_SOURCE_DIR}/cmock_config.yml
            --mock_path=${CMAKE_CURRENT_BINARY_DIR}/mocks
            ${btrfs_mock_header}
    DEPENDS ${btrfs_mock_header} ${CMAKE_CURRENT_SOURCE_DIR}/cmock_config.yml
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating mock for btrfs_writer_mock.h"
)

add_executable(test_stream_processor
    unit/test_stream_processor.c
    ${btrfs_mock_c}
)

target_include_directories(test_stream_processor PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/mocks
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ../include
)

target_link_libraries(test_stream_processor
    burst_downloader_lib_no_btrfs
    unity
    cmock
    ${ZSTD_LIBRARY}
)

add_test(NAME test_stream_processor COMMAND test_stream_processor)

# Frame parser unit test (tests parse_next_frame directly)
add_executable(test_frame_parser
    unit/test_frame_parser.c
    ../src/downloader/frame_parser.c
)
target_include_directories(test_frame_parser PRIVATE
    ../include
)
target_link_libraries(test_frame_parser
    unity
    ${ZSTD_LIBRARY}
)
add_test(NAME test_frame_parser COMMAND test_frame_parser)

# Parts calculation unit test (tests calculate_parts_to_download)
add_executable(test_parts_calculation
    unit/test_parts_calculation.c
    ../src/downloader/parts_calculation.c
)
target_include_directories(test_parts_calculation PRIVATE
    ../include
)
target_link_libraries(test_parts_calculation
    unity
)
add_test(NAME test_parts_calculation COMMAND test_parts_calculation)

# CD fetch unit test (tests calculate_cd_fetch_ranges, assemble_cd_buffer, etc.)
add_executable(test_cd_fetch
    unit/test_cd_fetch.c
    ../src/downloader/cd_fetch.c
)
target_include_directories(test_cd_fetch PRIVATE
    ../include
)
target_link_libraries(test_cd_fetch
    unity
)
add_test(NAME test_cd_fetch COMMAND test_cd_fetch)

# Downloader integration tests (C-based)
add_executable(test_central_dir_parser_integration integration/test_central_dir_parser.c)
target_link_libraries(test_central_dir_parser_integration
    burst_downloader_lib
    unity
)
add_test(NAME test_central_dir_parser_integration COMMAND test_central_dir_parser_integration)

# Padding LFH integration test (verifies 7zz extraction)
add_executable(test_padding_lfh_extraction integration/test_padding_lfh_extraction.c)
target_link_libraries(test_padding_lfh_extraction
    burst_writer_lib
    unity
)
add_test(NAME test_padding_lfh_extraction COMMAND test_padding_lfh_extraction)
set_tests_properties(test_padding_lfh_extraction PROPERTIES
    LABELS "integration"
    TIMEOUT 60)

# Integration tests (shell-based)

add_test(NAME test_writer_basic
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_writer_basic.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_writer_basic PROPERTIES
    LABELS "integration"
    TIMEOUT 120)

add_test(NAME test_zip_compatibility
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_zip_compatibility.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_zip_compatibility PROPERTIES
    LABELS "integration"
    TIMEOUT 120)

add_test(NAME test_zstd_compression
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_zstd_compression.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_zstd_compression PROPERTIES
    LABELS "integration"
    TIMEOUT 120)

add_test(NAME test_alignment_integration
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_alignment_integration.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_alignment_integration PROPERTIES
    LABELS "integration"
    TIMEOUT 180)

add_test(NAME test_empty_file_alignment
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_empty_file_alignment.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_empty_file_alignment PROPERTIES
    LABELS "integration"
    TIMEOUT 120)

add_test(NAME test_downloader_phase1
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_downloader_phase1.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_downloader_phase1 PROPERTIES
    LABELS "integration;s3"
    TIMEOUT 180)

add_test(NAME test_downloader_encoded_write
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_downloader_encoded_write.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_downloader_encoded_write PROPERTIES
    LABELS "integration;s3"
    TIMEOUT 180)

add_test(NAME test_large_cd
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_large_cd.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_large_cd PROPERTIES
    LABELS "integration;s3;slow"
    TIMEOUT 600)

add_test(NAME test_writer_unix_metadata
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_writer_unix_metadata.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_writer_unix_metadata PROPERTIES
    LABELS "integration"
    TIMEOUT 120)

# End-to-end integration tests (require S3 and BTRFS)
# These tests require:
#   - AWS credentials configured
#   - S3 bucket specified in .env file
#   - BTRFS filesystem (created via loop device if needed)
#   - sudo access (for BTRFS setup)
#
# To run: ctest -L e2e -V
# To skip: ctest -E e2e

add_test(NAME test_e2e_small
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/e2e/test_e2e_small.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_e2e_small PROPERTIES
    LABELS "e2e;integration;s3;btrfs"
    TIMEOUT 300)

add_test(NAME test_e2e_medium
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/e2e/test_e2e_medium.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_e2e_medium PROPERTIES
    LABELS "e2e;integration;s3;btrfs;slow"
    TIMEOUT 300)

add_test(NAME test_e2e_continuing
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/e2e/test_e2e_continuing.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_e2e_continuing PROPERTIES
    LABELS "e2e;integration;s3;btrfs;slow"
    TIMEOUT 300)

add_test(NAME test_e2e_many_files
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/e2e/test_e2e_many_files.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_e2e_many_files PROPERTIES
    LABELS "e2e;integration;s3;btrfs;slow"
    TIMEOUT 600)

add_test(NAME test_e2e_zip64
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/e2e/test_e2e_zip64.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_e2e_zip64 PROPERTIES
    LABELS "e2e;integration;s3;btrfs;slow"
    DISABLED TRUE  # Disable by default, currently uses more disk than available in GHA
    TIMEOUT 1200)

add_test(NAME test_e2e_unix_metadata
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/e2e/test_e2e_unix_metadata.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_e2e_unix_metadata PROPERTIES
    LABELS "e2e;integration;s3;btrfs;sudo"
    TIMEOUT 300)

add_test(NAME test_e2e_padding_lfh
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/e2e/test_e2e_padding_lfh.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_e2e_padding_lfh PROPERTIES
    LABELS "e2e;integration;s3;btrfs"
    TIMEOUT 300
    ENVIRONMENT "BURST_WRITER_TEST_MODE=${CMAKE_BINARY_DIR}/burst-writer-test-mode;BURST_DOWNLOADER=${CMAKE_BINARY_DIR}/burst-downloader")
