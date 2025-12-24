# BURST project: special notes for CLAUDE

The project is described in these files:

- [README.md](README.md) - overview, includes current progress and status.
- [implementation-plan.md](implementation-plan.md) - More detail of each component and sub-phase of planned development.
- [format-plan.md](format-plan.md) - Details of the BURST archive format and its constraints relative to regular zip files.

A few things to keep in mind when working on the project:
 - `unzip` is not an effective command to run to verify BURST files, as it does not understand Zstandard compression. Use `7zz` or `zipinfo` instead.

## Build System

The project uses **CMake 3.15+** as its build system:

- **Language**: C11 standard with strict compiler warnings (`-Wall -Wextra -Wpedantic`)
- **Build Configuration**:
  - Main executable: `burst-writer` (from [src/writer/](src/writer/) files)
  - Test library: `burst_writer_lib` (static library for unit testing)
  - Debug mode: Enable with `BURST_DEBUG` for extra assertions and validation
- **Dependencies**:
  - ZLIB (required for CRC32 calculations)
  - libzstd (required for Zstandard compression)
- **Testing**: Integrated via `enable_testing()` and CTest

### Building the Project

```bash
mkdir build && cd build
cmake ..
make
ctest  # Run all tests
```

## Unit Testing Framework

The project uses **Unity** (ThrowTheSwitch/Unity) for unit testing:

- Local copy maintained in [tests/unity/](tests/unity/)
- Built as static library (`libunity.a`)
- Standard test structure with `setUp()` and `tearDown()` functions
- Common test macros: `TEST_ASSERT_EQUAL()`, `TEST_ASSERT_NOT_NULL()`, etc.
- Test runner pattern: `UNITY_BEGIN()`, `RUN_TEST()`, `UNITY_END()`

### Example Test Structure

```c
#include "unity.h"

void setUp(void) { /* runs before each test */ }
void tearDown(void) { /* runs after each test */ }

void test_something(void) {
    TEST_ASSERT_EQUAL(expected, actual);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_something);
    return UNITY_END();
}
```

## Mocking Framework

The project uses **CMock v2.5.3** for creating mock objects:

- Fetched via CMake FetchContent from GitHub
- Built as static library (`libcmock.a`)
- **Requires Ruby** for mock generation at build time
- Configuration: [tests/cmock_config.yml](tests/cmock_config.yml)

### CMock Configuration

- Mock prefix: `Mock_`
- Plugins: ignore, callback, return_thru_ptr
- Type formatting: uint8/16/32 as HEX8/16/32
- Strips compiler attributes like `__attribute__`

### Mock Generation Process

1. Mock headers defined in [tests/mocks/](tests/mocks/) (e.g., `compression_mock.h`)
2. CMake/Ruby generates `Mock_*.c` and `Mock_*.h` at build time
3. Generated files placed in `build/tests/mocks/`

### Mock Usage Pattern

```c
#include "unity.h"
#include "Mock_compression_mock.h"

void setUp(void) {
    Mock_compression_mock_Init();
    compress_chunk_Stub(my_callback_function);  // Register stub callback
}

void tearDown(void) {
    Mock_compression_mock_Verify();
    Mock_compression_mock_Destroy();
}
```

## Test Organization

### Directory Structure

```
tests/
├── CMakeLists.txt           # Test build configuration
├── cmock_config.yml         # CMock configuration
├── unity/                   # Unity framework source
├── mocks/                   # Mock header definitions
│   ├── compression_mock.h
│   └── zstd_mock.h
├── unit/                    # Unit test files
│   ├── test_zip_structures.c
│   ├── test_writer_core.c
│   ├── test_crc32.c
│   ├── test_zstd_frames.c
│   ├── test_alignment.c
│   └── test_writer_chunking.c  (uses mocks)
├── integration/             # Integration test scripts
│   ├── test_writer_basic.sh
│   ├── test_zip_compatibility.sh
│   └── test_zstd_compression.sh
└── fixtures/                # Test data files
```

### Test Categories

**Unit Tests** (6 tests in [tests/unit/](tests/unit/)):
- `test_zip_structures` - ZIP header structures and DOS datetime conversion
- `test_writer_core` - Core writer functionality
- `test_crc32` - CRC32 calculation
- `test_zstd_frames` - Zstandard frame handling
- `test_alignment` - 8 MiB boundary alignment logic
- `test_writer_chunking` - 128 KiB chunking with mocked compression

**Integration Tests** (Any .sh script in [tests/integration/](tests/integration/)):
- `test_writer_basic.sh` - Basic ZIP creation and extraction validation
- `test_zip_compatibility.sh` - Compatibility with standard ZIP tools
- `test_zstd_compression.sh` - Zstandard compression validation with 7-Zip
- Others not detailed here

### Helper CMake Functions

The [tests/CMakeLists.txt](tests/CMakeLists.txt) provides two helper functions:

1. **`add_unit_test(test_name)`** - For simple unit tests without mocks
   - Compiles `unit/${test_name}.c`
   - Links with `burst_writer_lib` and `unity`
   - Registers with CTest

2. **`add_mocked_test(test_name, mock_headers)`** - For tests requiring mocks
   - Generates mocks from header files using CMock
   - Links with `unity`, `cmock`, and `burst_writer_lib`
   - Registers with CTest

### Running Tests

```bash
# Via CTest (from build directory)
ctest                    # Run all tests
ctest -V                 # Verbose output
ctest -R test_alignment  # Run specific test

# Direct execution
cd build/tests
./test_alignment         # Run unit test directly
bash ../../tests/integration/test_writer_basic.sh  # Run integration test
```
