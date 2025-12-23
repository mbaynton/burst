# BURST Testing Strategy

## Overview
Comprehensive testing strategy for BURST archive writer and downloader, covering unit tests, integration tests, and validation.

---

## Test Organization

### Directory Structure
```
tests/
├── unit/              # Unit tests for individual components
│   ├── test_zip_structures.c
│   ├── test_alignment.c
│   ├── test_zstd_frames.c
│   └── test_crc32.c
├── integration/       # Integration tests for complete workflows
│   ├── test_writer_basic.sh
│   ├── test_writer_zstd.sh
│   ├── test_writer_alignment.sh
│   └── test_zip_compatibility.sh
├── fixtures/          # Test data and expected outputs
│   ├── small_file.txt (< 4 KiB)
│   ├── medium_file.txt (~128 KiB)
│   ├── large_file.bin (> 4 GiB, for ZIP64)
│   └── expected/
└── tmp/              # Temporary test artifacts (gitignored)
```

---

## Phase 1 Testing (Current - Basic ZIP Writer)

### Unit Tests

**test_zip_structures.c**
- Test DOS date/time conversion
- Test local header writing with various filenames
- Test data descriptor creation
- Test central directory generation
- Test EOCD record writing
- Test filename encoding (UTF-8, special characters)

**test_crc32.c**
- Test CRC32 calculation matches zlib
- Test CRC32 on empty files
- Test CRC32 on large files

### Integration Tests

**test_writer_basic.sh**
```bash
#!/bin/bash
# Test basic ZIP creation with STORE method

# Create test files of various sizes
echo "test" > small.txt
dd if=/dev/urandom of=medium.bin bs=1K count=128
dd if=/dev/zero of=large.bin bs=1M count=10

# Create archive
burst-writer -o test.zip small.txt medium.bin large.bin

# Validate with unzip
unzip -t test.zip || exit 1

# Extract and compare
mkdir extract
cd extract
unzip ../test.zip
cmp ../small.txt small.txt || exit 1
cmp ../medium.bin medium.bin || exit 1
cmp ../large.bin large.bin || exit 1

echo "✓ Basic writer test passed"
```

**test_zip_compatibility.sh**
```bash
#!/bin/bash
# Test compatibility with multiple ZIP tools

burst-writer -o test.zip fixtures/*.txt

# Test with unzip
unzip -t test.zip || exit 1

# Test with 7zip (if available)
if command -v 7z &> /dev/null; then
    7z t test.zip || exit 1
fi

# Test with zipinfo
zipinfo -v test.zip > /dev/null || exit 1

echo "✓ ZIP compatibility test passed"
```

### Validation Checks

**Checklist for Phase 1:**
- [ ] Archives validate with `unzip -t`
- [ ] Archives validate with `7z t`
- [ ] Archives list correctly with `zipinfo`
- [ ] Extracted files match originals (byte-for-byte)
- [ ] CRC32 values are correct
- [ ] Timestamps are preserved
- [ ] File permissions are preserved (Unix)
- [ ] Empty files work correctly
- [ ] Files with special characters in names work
- [ ] Large files (> 4 GiB) fail gracefully (until ZIP64 implemented)

---

## Phase 2 Testing (Zstandard Compression)

### Unit Tests

**test_zstd_frames.c**
- Test 128 KiB frame creation
- Test content size is in frame header
- Test ZSTD_getFrameContentSize() returns correct value
- Test frames are independent (no dictionary)
- Test compression levels (-15 to 22)
- Test cases where compressed > uncompressed

### Integration Tests

**test_writer_zstd.sh**
```bash
#!/bin/bash
# Test Zstandard compression

burst-writer -l 3 -o test.zstd.zip fixtures/*.txt

# Validate archive
unzip -t test.zstd.zip || exit 1

# Verify it's actually using ZSTD (method 93)
zipinfo -v test.zstd.zip | grep "compression method: 93" || exit 1

# Extract with 7-Zip-zstd
7z-zstd x test.zstd.zip -o extract/

# Compare extracted files
diff -r fixtures/ extract/ || exit 1

echo "✓ Zstandard compression test passed"
```

### Validation Checks

**Checklist for Phase 2:**
- [ ] Zstandard compression works
- [ ] Compression method is 93 in ZIP headers
- [ ] Content size is present in all frames
- [ ] Frames are exactly 128 KiB uncompressed (except last)
- [ ] Archives extract correctly with 7-Zip-zstd
- [ ] Compression levels work (-15 to 22)
- [ ] Incompressible data handled correctly
- [ ] Compression ratio is reported correctly

---

## Phase 3 Testing (8 MiB Alignment)

### Unit Tests

**test_alignment.c**
- Test boundary detection
- Test padding frame generation
- Test Start-of-Part metadata frame
- Test alignment calculations
- Test exact boundary fit edge case

### Integration Tests

**test_writer_alignment.sh**
```bash
#!/bin/bash
# Test 8 MiB alignment

# Create files that will trigger alignment padding
dd if=/dev/zero of=file1.bin bs=1M count=7
dd if=/dev/zero of=file2.bin bs=1M count=2
dd if=/dev/zero of=file3.bin bs=1M count=10

burst-writer -l 3 -o aligned.zip file1.bin file2.bin file3.bin

# Verify 8 MiB alignment
python3 << 'EOF'
import sys

with open('aligned.zip', 'rb') as f:
    data = f.read()

# Check all 8 MiB boundaries
PART_SIZE = 8 * 1024 * 1024
ZIP_LOCAL_SIG = b'PK\x03\x04'
ZSTD_FRAME_MAGIC = b'\x28\xb5\x2f\xfd'
BURST_SKIPPABLE_MAGIC = b'\x5b\x2a\x4d\x18'  # 0x184D2A5B little-endian

for part_num in range(1, len(data) // PART_SIZE):
    offset = part_num * PART_SIZE
    boundary_bytes = data[offset:offset+4]

    # Must be ZIP local header, Zstandard frame, or BURST skippable frame
    if boundary_bytes not in [ZIP_LOCAL_SIG, ZSTD_FRAME_MAGIC, BURST_SKIPPABLE_MAGIC]:
        print(f"❌ Invalid bytes at {offset}: {boundary_bytes.hex()}")
        sys.exit(1)

    print(f"✓ Part {part_num} boundary OK: {boundary_bytes.hex()}")

print("✓ All 8 MiB boundaries aligned correctly")
EOF

# Verify extraction still works
unzip -t aligned.zip || exit 1

echo "✓ Alignment test passed"
```

### Validation Checks

**Checklist for Phase 3:**
- [ ] All 8 MiB boundaries align to ZIP headers or Zstandard frames
- [ ] Padding frames use correct magic number (0x184D2A5B)
- [ ] Start-of-Part metadata frames are correct
- [ ] Padding overhead is < 1% of archive size
- [ ] Archives still extract correctly with standard tools
- [ ] Hex dump verification shows correct alignment

---

## Phase 4 Testing (ZIP64)

### Integration Tests

**test_zip64.sh**
```bash
#!/bin/bash
# Test ZIP64 for large files

# Create a 5 GiB file
dd if=/dev/zero of=huge.bin bs=1M count=5120

burst-writer -o huge.zip huge.bin

# Verify ZIP64 extra fields are present
zipinfo -v huge.zip | grep "ZIP64" || exit 1

# Extract and verify
unzip huge.zip
cmp huge.bin huge.bin.extracted || exit 1

echo "✓ ZIP64 test passed"
```

### Validation Checks

**Checklist for Phase 4:**
- [ ] Files > 4 GiB use ZIP64 format
- [ ] ZIP64 extra fields are present
- [ ] Archives > 4 GiB total size use ZIP64
- [ ] Standard tools recognize ZIP64 archives
- [ ] Large files extract correctly

---

## Continuous Integration

### GitHub Actions Workflow
```yaml
name: BURST Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest

    steps:
    - uses: actions/checkout@v2

    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y libzstd-dev zlib1g-dev cmake
        sudo apt-get install -y unzip p7zip-full

    - name: Build
      run: |
        mkdir build
        cd build
        cmake ..
        make

    - name: Run unit tests
      run: |
        cd build
        ctest --output-on-failure

    - name: Run integration tests
      run: |
        cd tests/integration
        bash test_writer_basic.sh
        bash test_zip_compatibility.sh
```

---

## Performance Testing

### Benchmarks

**test_performance.sh**
```bash
#!/bin/bash
# Measure compression throughput

# Create 1 GB test file
dd if=/dev/zero of=1gb.bin bs=1M count=1024

# Test different compression levels
for level in -5 0 3 9 15; do
    echo "Testing level $level..."
    time burst-writer -l $level -o test_$level.zip 1gb.bin
done

# Report sizes and times
ls -lh test_*.zip
```

### Metrics to Track
- Compression throughput (MB/s)
- Archive creation time
- Memory usage (via valgrind/massif)
- Padding overhead percentage

---

## Test Automation

### CMake Integration

**tests/CMakeLists.txt**
```cmake
enable_testing()

# Unit tests
add_executable(test_zip_structures unit/test_zip_structures.c)
target_link_libraries(test_zip_structures burst_writer_lib)
add_test(NAME zip_structures COMMAND test_zip_structures)

# Integration tests
add_test(NAME writer_basic COMMAND bash integration/test_writer_basic.sh)
add_test(NAME zip_compatibility COMMAND bash integration/test_zip_compatibility.sh)
```

### Running Tests
```bash
# Run all tests
cd build
ctest

# Run specific test
ctest -R zip_structures

# Run with verbose output
ctest -V

# Run integration tests only
ctest -R integration
```

---

## Edge Cases to Test

### Input Variations
- Empty files
- Files with no extension
- Files with Unicode names
- Files with spaces in names
- Symbolic links (should warn/skip)
- Directories (should warn/skip)
- Device files (should warn/skip)

### Archive Variations
- Empty archive (no files)
- Single file archive
- Many small files (1000+)
- Mix of file sizes
- Files exactly at 8 MiB boundaries
- Files that compress poorly (random data)
- Files that compress well (zeros, repeated patterns)

### Error Conditions
- Out of disk space
- Permission denied
- File deleted during compression
- Invalid compression level
- Output file already exists

---

## Validation Tools

### Custom Validators

**validate_burst_alignment.py**
```python
#!/usr/bin/env python3
"""Validate BURST archive alignment"""

def validate_alignment(zip_path):
    with open(zip_path, 'rb') as f:
        data = f.read()

    PART_SIZE = 8 * 1024 * 1024
    errors = []

    for part in range(1, len(data) // PART_SIZE):
        offset = part * PART_SIZE
        # Check alignment...

    return errors

if __name__ == '__main__':
    import sys
    errors = validate_alignment(sys.argv[1])
    if errors:
        for e in errors:
            print(f"❌ {e}")
        sys.exit(1)
    print("✓ All alignments valid")
```

### Hex Dump Analysis
```bash
# View 8 MiB boundary
hexdump -C aligned.zip | grep "00800000"  # 8 MiB offset

# View structure around boundary
dd if=aligned.zip bs=1 skip=$((8*1024*1024-16)) count=32 | hexdump -C
```

---

## Current Status

### Implemented
- ✅ Basic integration test setup (tmp/ directory)
- ✅ Manual testing with unzip
- ✅ File comparison validation

### TODO
- [ ] Create tests/ directory structure
- [ ] Implement unit tests framework (using CTest or similar)
- [ ] Write test_zip_structures.c
- [ ] Write integration test scripts
- [ ] Create test fixtures
- [ ] Set up CI/CD pipeline
- [ ] Add performance benchmarks
- [ ] Document test procedures

---

## Next Steps

1. Create basic test infrastructure (Phase 1 tests)
2. Run tests after each phase completion
3. Add regression tests for bugs found
4. Measure and track performance metrics
5. Set up automated testing in CI
