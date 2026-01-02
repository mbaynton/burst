cmake_minimum_required(VERSION 3.15)
project(burst VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# AWS-C-S3 for downloader (optional build)
option(BUILD_DOWNLOADER "Build BURST downloader with AWS-C-S3 support" ON)

# Debug mode option
option(BURST_DEBUG "Enable debug assertions and validation checks" OFF)
if(BURST_DEBUG)
    add_compile_definitions(DEBUG)
    message(STATUS "Debug mode enabled (frame content size verification active)")
endif()

# Compiler warnings
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find dependencies
find_package(ZLIB REQUIRED)

# Try to find zstd
find_library(ZSTD_LIBRARY NAMES zstd)
find_path(ZSTD_INCLUDE_DIR NAMES zstd.h)

if(NOT ZSTD_LIBRARY OR NOT ZSTD_INCLUDE_DIR)
    message(FATAL_ERROR "libzstd not found. Install with: sudo apt-get install libzstd-dev")
endif()

message(STATUS "Found zstd library: ${ZSTD_LIBRARY}")
message(STATUS "Found zstd headers: ${ZSTD_INCLUDE_DIR}")

# Archive writer executable
add_executable(burst-writer
    src/writer/main.c
    src/writer/burst_writer.c
    src/writer/zip_structures.c
    src/writer/compression.c
    src/writer/alignment.c
)

target_include_directories(burst-writer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${ZSTD_INCLUDE_DIR}
)

target_link_libraries(burst-writer PRIVATE
    ZLIB::ZLIB
    ${ZSTD_LIBRARY}
)

# Test-mode burst-writer with forced padding LFH
add_executable(burst-writer-test-mode
    src/writer/main.c
    src/writer/burst_writer.c
    src/writer/zip_structures.c
    src/writer/compression.c
    src/writer/alignment.c
)

target_include_directories(burst-writer-test-mode PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${ZSTD_INCLUDE_DIR}
)

target_link_libraries(burst-writer-test-mode PRIVATE
    ZLIB::ZLIB
    ${ZSTD_LIBRARY}
)

# Define test mode flag
target_compile_definitions(burst-writer-test-mode PRIVATE
    BURST_TEST_FORCE_PADDING_LFH
)

# Don't install test mode binary (for testing only)

# Installation
install(TARGETS burst-writer DESTINATION bin)

# Testing
enable_testing()
add_subdirectory(tests)

if(BUILD_DOWNLOADER)
    message(STATUS "Building downloader with AWS-C-S3 support")
    message(STATUS "Note: First build will take 10-15 minutes to fetch and compile AWS CRT")

    include(ExternalProject)

    # Set common build configuration
    set(AWS_CRT_BUILD_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/aws-crt-install
        -DBUILD_TESTING=OFF
        -DBUILD_SHARED_LIBS=OFF
    )

    set(AWS_INSTALL_DIR ${CMAKE_BINARY_DIR}/aws-crt-install)

    # Build aws-c-common first (base library)
    ExternalProject_Add(aws-c-common-ext
        GIT_REPOSITORY https://github.com/awslabs/aws-c-common.git
        GIT_TAG main
        CMAKE_ARGS ${AWS_CRT_BUILD_ARGS}
        INSTALL_DIR ${AWS_INSTALL_DIR}
    )

    # Build s2n-tls (depends on OpenSSL)
    ExternalProject_Add(s2n-ext
        DEPENDS aws-c-common-ext
        GIT_REPOSITORY https://github.com/aws/s2n-tls.git
        GIT_TAG main
        CMAKE_ARGS ${AWS_CRT_BUILD_ARGS} -DCMAKE_PREFIX_PATH=${AWS_INSTALL_DIR}
        INSTALL_DIR ${AWS_INSTALL_DIR}
    )

    # Build aws-c-sdkutils
    ExternalProject_Add(aws-c-sdkutils-ext
        DEPENDS aws-c-common-ext
        GIT_REPOSITORY https://github.com/awslabs/aws-c-sdkutils.git
        GIT_TAG main
        CMAKE_ARGS ${AWS_CRT_BUILD_ARGS} -DCMAKE_PREFIX_PATH=${AWS_INSTALL_DIR}
        INSTALL_DIR ${AWS_INSTALL_DIR}
    )

    # Build aws-checksums
    ExternalProject_Add(aws-checksums-ext
        DEPENDS aws-c-common-ext
        GIT_REPOSITORY https://github.com/awslabs/aws-checksums.git
        GIT_TAG main
        CMAKE_ARGS ${AWS_CRT_BUILD_ARGS} -DCMAKE_PREFIX_PATH=${AWS_INSTALL_DIR}
        INSTALL_DIR ${AWS_INSTALL_DIR}
    )

    # Build aws-c-cal (crypto abstraction)
    ExternalProject_Add(aws-c-cal-ext
        DEPENDS aws-c-common-ext
        GIT_REPOSITORY https://github.com/awslabs/aws-c-cal.git
        GIT_TAG main
        CMAKE_ARGS ${AWS_CRT_BUILD_ARGS} -DCMAKE_PREFIX_PATH=${AWS_INSTALL_DIR}
        INSTALL_DIR ${AWS_INSTALL_DIR}
    )

    # Build aws-c-io
    ExternalProject_Add(aws-c-io-ext
        DEPENDS aws-c-common-ext aws-c-cal-ext s2n-ext
        GIT_REPOSITORY https://github.com/awslabs/aws-c-io.git
        GIT_TAG main
        CMAKE_ARGS ${AWS_CRT_BUILD_ARGS} -DCMAKE_PREFIX_PATH=${AWS_INSTALL_DIR}
        INSTALL_DIR ${AWS_INSTALL_DIR}
    )

    # Build aws-c-compression
    ExternalProject_Add(aws-c-compression-ext
        DEPENDS aws-c-common-ext
        GIT_REPOSITORY https://github.com/awslabs/aws-c-compression.git
        GIT_TAG main
        CMAKE_ARGS ${AWS_CRT_BUILD_ARGS} -DCMAKE_PREFIX_PATH=${AWS_INSTALL_DIR}
        INSTALL_DIR ${AWS_INSTALL_DIR}
    )

    # Build aws-c-http
    ExternalProject_Add(aws-c-http-ext
        DEPENDS aws-c-io-ext aws-c-compression-ext
        GIT_REPOSITORY https://github.com/awslabs/aws-c-http.git
        GIT_TAG main
        CMAKE_ARGS ${AWS_CRT_BUILD_ARGS} -DCMAKE_PREFIX_PATH=${AWS_INSTALL_DIR}
        INSTALL_DIR ${AWS_INSTALL_DIR}
    )

    # Build aws-c-auth
    ExternalProject_Add(aws-c-auth-ext
        DEPENDS aws-c-http-ext aws-c-cal-ext aws-c-io-ext aws-c-sdkutils-ext
        GIT_REPOSITORY https://github.com/awslabs/aws-c-auth.git
        GIT_TAG main
        CMAKE_ARGS ${AWS_CRT_BUILD_ARGS} -DCMAKE_PREFIX_PATH=${AWS_INSTALL_DIR}
        INSTALL_DIR ${AWS_INSTALL_DIR}
    )

    # Build aws-c-s3
    ExternalProject_Add(aws-c-s3-ext
        DEPENDS aws-c-auth-ext aws-c-http-ext aws-c-io-ext aws-c-compression-ext aws-c-cal-ext aws-checksums-ext aws-c-sdkutils-ext
        GIT_REPOSITORY https://github.com/awslabs/aws-c-s3.git
        GIT_TAG main
        CMAKE_ARGS ${AWS_CRT_BUILD_ARGS} -DCMAKE_PREFIX_PATH=${AWS_INSTALL_DIR}
        INSTALL_DIR ${AWS_INSTALL_DIR}
    )

    # Set variables so our executable can link against aws-c-s3
    set(AWS_C_S3_LIBRARIES
        ${AWS_INSTALL_DIR}/lib/libaws-c-s3.a
        ${AWS_INSTALL_DIR}/lib/libaws-c-auth.a
        ${AWS_INSTALL_DIR}/lib/libaws-c-http.a
        ${AWS_INSTALL_DIR}/lib/libaws-c-io.a
        ${AWS_INSTALL_DIR}/lib/libaws-c-compression.a
        ${AWS_INSTALL_DIR}/lib/libaws-c-cal.a
        ${AWS_INSTALL_DIR}/lib/libaws-checksums.a
        ${AWS_INSTALL_DIR}/lib/libaws-c-sdkutils.a
        ${AWS_INSTALL_DIR}/lib/libaws-c-common.a
        ${AWS_INSTALL_DIR}/lib/libs2n.a
    )

    set(AWS_C_S3_INCLUDE_DIRS ${AWS_INSTALL_DIR}/include)

    # Archive downloader executable
    add_executable(burst-downloader
        src/downloader/main.c
        src/downloader/s3_client.c
        src/downloader/s3_operations.c
        src/downloader/parts_calculation.c
        src/downloader/central_dir_parser.c
        src/downloader/stream_processor.c
        src/downloader/frame_parser.c
        src/downloader/btrfs_writer.c
    )

    # burst-downloader depends on all AWS CRT external projects completing
    add_dependencies(burst-downloader aws-c-s3-ext)

    target_include_directories(burst-downloader PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${ZSTD_INCLUDE_DIR}
        ${AWS_C_S3_INCLUDE_DIRS}
    )

    target_link_libraries(burst-downloader PRIVATE
        ${AWS_C_S3_LIBRARIES}
        ${ZSTD_LIBRARY}
        ssl
        crypto
        pthread
        dl
        m
    )

    install(TARGETS burst-downloader DESTINATION bin)
endif(BUILD_DOWNLOADER)

