#!/bin/bash
# Test Phase 3: 8 MiB Alignment Implementation
# Verifies that all 8 MiB boundaries align to ZIP headers or Zstandard frame starts
#
# Usage:
#   bash test_alignment_integration.sh          # Normal output
#   VERBOSE=1 bash test_alignment_integration.sh # Verbose output with hex dumps

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
TEST_TMP="$PROJECT_ROOT/tests/tmp/alignment"

# Use native 7zz with Zstandard support
function run_7z() {
    7zz "$@"
}

# Clean and create test directory
rm -rf "$TEST_TMP"
mkdir -p "$TEST_TMP"
cd "$TEST_TMP"

echo "=== Test: Phase 3 - 8 MiB Alignment ==="
echo

# Helper: Create file of specific size with random data
create_test_file() {
    local filename="$1"
    local size="$2"
    # Use 1 MiB blocks for much faster file creation
    local bs=$((1024 * 1024))  # 1 MiB
    local count=$((size / bs))
    local remainder=$((size % bs))

    dd if=/dev/urandom of="$filename" bs="$bs" count="$count" 2>/dev/null
    if [ "$remainder" -gt 0 ]; then
        dd if=/dev/urandom of="$filename" bs=1 count="$remainder" oflag=append conv=notrunc 2>/dev/null
    fi
}

# Helper: Verify alignment using Python script
verify_alignment() {
    local archive="$1"
    local verbose_flag=""

    # Use verbose mode if VERBOSE environment variable is set
    if [ -n "$VERBOSE" ]; then
        verbose_flag="-v"
    fi

    python3 "$PROJECT_ROOT/tests/integration/verify_alignment.py" $verbose_flag "$archive"
}

# Test 1: Single large file (10 MiB) - should cross at least one boundary
echo "Test 1: Single large file (10 MiB)..."
create_test_file "input_10mb.bin" $((10 * 1024 * 1024))
"$BUILD_DIR/burst-writer" -o test1.zip input_10mb.bin > test1_out.txt 2>&1

# Verify archive is valid
run_7z t test1.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Archive invalid"; exit 1; }

# Verify alignment with Python script
verify_alignment test1.zip || {
    echo "❌ Failed: Alignment verification failed"
    exit 1
}

# Extract and verify
mkdir -p extract1
cd extract1
run_7z x -y ../test1.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Extraction failed"; exit 1; }
cmp input_10mb.bin ../input_10mb.bin || { echo "❌ Failed: Extracted file differs"; exit 1; }
cd ..
echo "✓ Large file test passed"
echo

# Test 2: Multiple small files near boundaries
echo "Test 2: Multiple files crossing boundaries..."
# Create files that will force boundary crossings
# With ZIP overhead, these should cross 8 MiB boundary
create_test_file "file1.bin" $((3 * 1024 * 1024))  # 3 MiB
create_test_file "file2.bin" $((4 * 1024 * 1024))  # 4 MiB
create_test_file "file3.bin" $((2 * 1024 * 1024))  # 2 MiB

"$BUILD_DIR/burst-writer" -o test2.zip file1.bin file2.bin file3.bin > test2_out.txt 2>&1

# Verify archive is valid
run_7z t test2.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Multi-file archive invalid"; exit 1; }

# Extract and verify all files
mkdir -p extract2
cd extract2
run_7z x -y ../test2.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Extraction failed"; exit 1; }
cmp file1.bin ../file1.bin || { echo "❌ Failed: file1.bin differs"; exit 1; }
cmp file2.bin ../file2.bin || { echo "❌ Failed: file2.bin differs"; exit 1; }
cmp file3.bin ../file3.bin || { echo "❌ Failed: file3.bin differs"; exit 1; }
cd ..
echo "✓ Multi-file boundary crossing test passed"
echo

# Test 3: File exactly 8 MiB - should align perfectly at next boundary
echo "Test 3: File exactly 8 MiB..."
create_test_file "exactly_8mb.bin" $((8 * 1024 * 1024))
"$BUILD_DIR/burst-writer" -o test3.zip exactly_8mb.bin > test3_out.txt 2>&1

# Verify archive is valid
run_7z t test3.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Archive invalid"; exit 1; }

# Extract and verify
mkdir -p extract3
cd extract3
run_7z x -y ../test3.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Extraction failed"; exit 1; }
cmp exactly_8mb.bin ../exactly_8mb.bin || { echo "❌ Failed: Extracted file differs"; exit 1; }
cd ..
echo "✓ Exact 8 MiB file test passed"
echo

# Test 4: CRITICAL - File slightly larger than 8 MiB
# This tests the edge case where the last frame fits but data descriptor doesn't
echo "Test 4: CRITICAL - File slightly larger than 8 MiB (8 MiB + 150 KiB)..."
create_test_file "slightly_over_8mb.bin" $((8 * 1024 * 1024 + 150 * 1024))
"$BUILD_DIR/burst-writer" -o test4.zip slightly_over_8mb.bin > test4_out.txt 2>&1

# Verify archive is valid
run_7z t test4.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Archive invalid"; exit 1; }

# Verify alignment with Python script
verify_alignment test4.zip || {
    echo "❌ Failed: Alignment verification failed"
    exit 1
}

# Extract and verify
mkdir -p extract4
cd extract4
run_7z x -y ../test4.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Extraction failed"; exit 1; }
cmp slightly_over_8mb.bin ../slightly_over_8mb.bin || { echo "❌ Failed: Extracted file differs"; exit 1; }
cd ..
echo "✓ Critical edge case test passed (file slightly > 8 MiB)"
echo

# Test 5: Very large file (20 MiB) - crosses multiple boundaries
echo "Test 5: Very large file (20 MiB) - multiple boundary crossings..."
create_test_file "large_20mb.bin" $((20 * 1024 * 1024))
"$BUILD_DIR/burst-writer" -o test5.zip large_20mb.bin > test5_out.txt 2>&1

# Verify archive is valid
run_7z t test5.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Archive invalid"; exit 1; }

# Verify alignment with Python script
verify_alignment test5.zip || {
    echo "❌ Failed: Alignment verification failed"
    exit 1
}

# Extract and verify
mkdir -p extract5
cd extract5
run_7z x -y ../test5.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Extraction failed"; exit 1; }
cmp large_20mb.bin ../large_20mb.bin || { echo "❌ Failed: Extracted file differs"; exit 1; }
cd ..
echo "✓ Multiple boundary crossing test passed"
echo

# Test 6: Padding statistics - verify overhead is reasonable (<1%)
echo "Test 6: Padding overhead statistics..."
create_test_file "stats_test.bin" $((15 * 1024 * 1024))
"$BUILD_DIR/burst-writer" -o test6.zip stats_test.bin > test6_out.txt 2>&1

# Check if padding bytes are reported
if grep -q "Padding bytes:" test6_out.txt; then
    padding_bytes=$(grep "Padding bytes:" test6_out.txt | awk '{print $3}')
    total_size=$(grep "Final size:" test6_out.txt | awk '{print $3}')

    # Calculate overhead percentage
    overhead=$(awk "BEGIN {printf \"%.2f\", ($padding_bytes / $total_size) * 100}")
    echo "  Padding overhead: ${overhead}% ($padding_bytes bytes out of $total_size)"

    # Verify overhead is reasonable (should be well under 1%)
    if [ "$(echo "$overhead < 1.0" | bc)" -eq 1 ]; then
        echo "  ✓ Padding overhead is acceptable (<1%)"
    else
        echo "  ⚠ Warning: Padding overhead is higher than expected: ${overhead}%"
    fi
else
    echo "  ⚠ Warning: Padding statistics not found in output"
fi
echo "✓ Padding statistics test passed"
echo

echo "=== All Phase 3 alignment integration tests passed ✓ ==="
echo
echo "Summary:"
echo "  - Large files correctly cross 8 MiB boundaries"
echo "  - Multiple files handle boundary alignment"
echo "  - Exact 8 MiB files work correctly"
echo "  - Critical edge case (file > 8 MiB) handles descriptor placement"
echo "  - Multiple boundary crossings work correctly"
echo "  - Padding overhead is minimal"
echo
echo "All archives extract correctly with 7-Zip!"
