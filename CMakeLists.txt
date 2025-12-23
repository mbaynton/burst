cmake_minimum_required(VERSION 3.15)
project(burst VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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
)

target_include_directories(burst-writer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${ZSTD_INCLUDE_DIR}
)

target_link_libraries(burst-writer PRIVATE
    ZLIB::ZLIB
    ${ZSTD_LIBRARY}
)

# Installation
install(TARGETS burst-writer DESTINATION bin)
