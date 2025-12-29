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
)
target_include_directories(burst_downloader_lib PUBLIC
    ../include
)
# Enable debug assertions for tests
target_compile_definitions(burst_downloader_lib PUBLIC DEBUG)

# Unit tests
add_unit_test(test_zip_structures)
add_unit_test(test_writer_core)
add_unit_test(test_crc32)
add_unit_test(test_zstd_frames)
add_unit_test(test_alignment)

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

# Integration tests
add_test(NAME test_writer_basic
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_writer_basic.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME test_zip_compatibility
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_zip_compatibility.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME test_zstd_compression
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_zstd_compression.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
