# BURST - BTRFS Ultrafast Restore From S3 Transfers

High-performance archive format optimized for concurrent S3 downloads and direct BTRFS_IOC_ENCODED_WRITE operations.

## Overview

BURST creates ZIP archives with 8 MiB alignment boundaries, enabling:
- Concurrent part downloads from S3 (8-16 parts simultaneously)
- Direct writing of compressed Zstandard frames to BTRFS
- 10× faster extraction compared to sequential methods

## Current Status

### ✅ Phase 1 Complete: Basic ZIP Writer
- Creates valid ZIP archives with STORE (uncompressed) method
- Proper ZIP structure (local headers, data descriptors, central directory)
- Compatible with standard ZIP tools (unzip, 7z, zipinfo)
- Comprehensive test suite with 100% pass rate

### ✅ Phase 2 Complete: Zstandard Compression
- Zstandard compression (method 93) with configurable levels (-15 to 22)
- 128 KiB chunk boundaries for BTRFS_IOC_ENCODED_WRITE compatibility
- Frame headers with content size for encoded write operations
- Unit tests with CMock framework for compression logic
- Integration tests validating Zstandard archive compatibility

### 🚧 In Development
- Phase 3: 8 MiB alignment with padding frames
- Phase 4: ZIP64 support for files >4 GiB
- Phase 5: Archive downloader with aws-c-s3 integration

## Building

### Prerequisites
```bash
sudo apt-get install -y cmake libzstd-dev zlib1g-dev
```

### Testing Prerequisites

**Important**: To run the Zstandard compression tests, you need 7-Zip with Zstandard codec support.

⚠️ **Ubuntu/Debian Package Issue**: The distribution-packaged `p7zip-full` (7-Zip 23.01+dfsg) **strips Zstandard codec support** for DFSG (Debian Free Software Guidelines) compliance. You must install the official 7-Zip from 7-zip.org instead.

#### Install Official 7-Zip with Zstandard Support

```bash
# Download official 7-Zip (includes Zstandard codec)
wget https://www.7-zip.org/a/7z2408-linux-x64.tar.xz
tar -xf 7z2408-linux-x64.tar.xz
sudo cp 7zz /usr/local/bin/
sudo chmod +x /usr/local/bin/7zz

# Verify installation
7zz --help | head -2
```

#### Additional Testing Dependencies
```bash
# Install Ruby (required for CMock unit test framework)
sudo apt-get install -y ruby unzip
```

### Build Steps
```bash
mkdir build
cd build
cmake ..
make
```

### Build Artifacts
- `build/burst-writer` - Archive creation tool

## Usage

### Create Archive
```bash
# Basic usage
./build/burst-writer -o archive.zip file1.txt file2.bin

# With compression level (Phase 2+)
./build/burst-writer -l 3 -o archive.zip files/*
```

### Options
- `-o, --output FILE` - Output archive path (required)
- `-l, --level LEVEL` - Compression level -15 to 22 (default: 3)
- `-h, --help` - Show help message

## Testing

### Run All Tests
```bash
cd build
ctest
```

### Run Individual Tests

#### Unit Tests
```bash
# ZIP structure tests (DOS datetime, header sizes)
./build/tests/test_zip_structures

# Writer core tests (buffering, flushing)
./build/tests/test_writer_core

# CRC32 calculation tests
./build/tests/test_crc32

# Zstandard frame tests (content size verification)
./build/tests/test_zstd_frames

# Writer chunking tests (128 KiB boundaries with mocks)
./build/tests/test_writer_chunking
```

#### Integration Tests
```bash
# Basic writer functionality
bash tests/integration/test_writer_basic.sh

# ZIP compatibility
bash tests/integration/test_zip_compatibility.sh

# Zstandard compression (requires 7zz from 7-zip.org)
bash tests/integration/test_zstd_compression.sh
```

### Test Coverage

#### Unit Tests (26 tests)
- ✅ DOS datetime conversion (epoch and normal dates)
- ✅ Header size calculations (local and central directory)
- ✅ Writer creation and destruction
- ✅ Buffered writing and flushing
- ✅ CRC32 calculation with known values
- ✅ CRC32 incremental calculation
- ✅ Zstandard frame content size headers
- ✅ 128 KiB chunk boundary behavior (mocked)
- ✅ Compression method selection
- ✅ Multi-chunk file handling

#### Integration Tests
- ✅ Single and multiple file archives
- ✅ Empty file handling
- ✅ Special characters in filenames
- ✅ Archive integrity validation
- ✅ Content verification (byte-for-byte comparison)
- ✅ Compatibility with unzip, 7z, zipinfo
- ✅ Zstandard compression levels (1, 3, 9)
- ✅ Zstandard archive extraction and verification
- ✅ Compression method detection (method 93)
- ✅ Compression ratio validation

### Test Results
```
100% tests passed, 0 tests failed out of 8
Total Test time (real) = 0.15 sec
```

### Test Framework
- **Unity**: Lightweight C unit testing framework
- **CMock**: Mock generation for unit tests (requires Ruby)
- Located in `tests/unity/` and fetched via CMake
- Unit tests in `tests/unit/`
- Integration tests in `tests/integration/`
- Mock definitions in `tests/mocks/`

## Project Structure

```
burst/
├── CMakeLists.txt          # Build configuration
├── README.md               # This file
├── format-plan.md          # BURST format specification
├── testing-strategy.md     # Comprehensive testing plan
├── include/
│   ├── burst_writer.h      # Writer API and data structures
│   ├── zip_structures.h    # ZIP format definitions
│   └── compression.h       # Compression abstraction layer
├── src/writer/
│   ├── main.c              # CLI interface
│   ├── burst_writer.c      # Core writer implementation
│   ├── zip_structures.c    # ZIP format writing
│   └── compression.c       # Zstandard compression wrapper
├── tests/
│   ├── CMakeLists.txt      # Test build configuration
│   ├── cmock_config.yml    # CMock configuration
│   ├── unity/              # Unity test framework
│   │   ├── unity.c
│   │   ├── unity.h
│   │   └── unity_internals.h
│   ├── unit/               # Unit tests
│   │   ├── test_zip_structures.c
│   │   ├── test_writer_core.c
│   │   ├── test_crc32.c
│   │   ├── test_zstd_frames.c
│   │   └── test_writer_chunking.c
│   ├── mocks/              # Mock headers for CMock
│   │   ├── compression_mock.h
│   │   └── zstd_mock.h
│   ├── integration/        # Integration test scripts
│   │   ├── test_writer_basic.sh
│   │   ├── test_zip_compatibility.sh
│   │   └── test_zstd_compression.sh
│   └── fixtures/           # Test data files
└── tmp/                    # Local testing (gitignored)
```

## Documentation

- [format-plan.md](format-plan.md) - Detailed BURST format specification
- [testing-strategy.md](testing-strategy.md) - Testing approach for all phases
- [BTRFS_IOC_ENCODED_WRITE-findings.md](BTRFS_IOC_ENCODED_WRITE-findings.md) - BTRFS research

## Development Timeline

- **Week 1-2**: ✅ Phase 1 complete - Basic ZIP writer with tests
- **Week 3**: Phase 2 - Zstandard compression
- **Week 4**: Phase 3 - 8 MiB alignment logic
- **Week 5**: Phase 4 - ZIP64 support
- **Weeks 6-8**: Phase 5 - Downloader with aws-c-s3

## Dependencies

### Runtime
- libzstd (>= 1.4.0) - Zstandard compression
- zlib - CRC32 calculation

### Development
- CMake (>= 3.15)
- GCC or Clang with C11 support
- Git

### Testing
- bash
- unzip (Info-ZIP)
- zipinfo
- **7zz from 7-zip.org (>= 21.01)** - Required for Zstandard test validation
  - ⚠️ Do NOT use Ubuntu/Debian's `p7zip-full` - it strips Zstandard codec
  - See [Testing Prerequisites](#testing-prerequisites) for installation
- Ruby (>= 2.0) - Required for CMock mock generation

## Contributing

1. Follow existing code style (K&R with 4-space indents)
2. Add tests for new features
3. Ensure all tests pass: `ctest`
4. Update documentation as needed

## License

(License to be determined)

## References

- [ZIP File Format Specification (APPNOTE.TXT)](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT)
- [Zstandard RFC 8878](https://datatracker.ietf.org/doc/html/rfc8878)
- [BTRFS Documentation](https://btrfs.readthedocs.io/)
- [aws-c-s3](https://github.com/awslabs/aws-c-s3/)
