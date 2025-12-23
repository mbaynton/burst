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

### 🚧 In Development
- Phase 2: Zstandard compression with 128 KiB frame control
- Phase 3: 8 MiB alignment with padding frames
- Phase 4: ZIP64 support for files >4 GiB
- Phase 5: Archive downloader with aws-c-s3 integration

## Building

### Prerequisites
```bash
sudo apt-get install -y cmake libzstd-dev zlib1g-dev
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
```bash
# Basic writer functionality
bash tests/integration/test_writer_basic.sh

# ZIP compatibility
bash tests/integration/test_zip_compatibility.sh
```

### Test Coverage
- ✅ Single and multiple file archives
- ✅ Empty file handling
- ✅ Special characters in filenames
- ✅ Archive integrity validation
- ✅ Content verification (byte-for-byte comparison)
- ✅ Compatibility with unzip, 7z, zipinfo

### Test Results
```
100% tests passed, 0 tests failed out of 2
Total Test time (real) = 0.10 sec
```

## Project Structure

```
burst/
├── CMakeLists.txt          # Build configuration
├── README.md               # This file
├── format-plan.md          # BURST format specification
├── testing-strategy.md     # Comprehensive testing plan
├── include/
│   ├── burst_writer.h      # Writer API and data structures
│   └── zip_structures.h    # ZIP format definitions
├── src/writer/
│   ├── main.c              # CLI interface
│   ├── burst_writer.c      # Core writer implementation
│   └── zip_structures.c    # ZIP format writing
├── tests/
│   ├── fixtures/           # Test data files
│   └── integration/        # Integration test scripts
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
- 7z (p7zip-full) - optional
- zipinfo

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
