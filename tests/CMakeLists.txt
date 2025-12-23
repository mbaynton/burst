# Unity test framework
add_library(unity STATIC
    unity/unity.c
)
target_include_directories(unity PUBLIC unity)

# Helper library with writer components for testing
add_library(burst_writer_lib STATIC
    ../src/writer/burst_writer.c
    ../src/writer/zip_structures.c
)
target_include_directories(burst_writer_lib PUBLIC
    ../include
    ${ZSTD_INCLUDE_DIR}
)
target_link_libraries(burst_writer_lib PUBLIC
    ZLIB::ZLIB
    ${ZSTD_LIBRARY}
)

# Helper function to add unit tests
function(add_unit_test test_name)
    add_executable(${test_name} unit/${test_name}.c)
    target_link_libraries(${test_name}
        burst_writer_lib
        unity
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

# Unit tests
add_unit_test(test_zip_structures)
add_unit_test(test_writer_core)
add_unit_test(test_crc32)

# Integration tests
add_test(NAME test_writer_basic
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_writer_basic.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME test_zip_compatibility
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_zip_compatibility.sh
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
