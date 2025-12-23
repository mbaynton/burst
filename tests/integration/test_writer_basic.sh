#!/bin/bash
# Test basic ZIP creation with Zstandard compression

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
FIXTURES_DIR="$PROJECT_ROOT/tests/fixtures"
TEST_TMP="$PROJECT_ROOT/tests/tmp"

# Use native 7zz with Zstandard support
function run_7z() {
    7zz "$@"
}

# Clean and create test directory
rm -rf "$TEST_TMP"
mkdir -p "$TEST_TMP"
cd "$TEST_TMP"

echo "=== Test: Basic ZIP Writer ==="
echo

# Test 1: Single file
echo "Test 1: Single small file..."
"$BUILD_DIR/burst-writer" -l 1 -o single.zip "$FIXTURES_DIR/small.txt" > /dev/null
run_7z t single.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Single file archive invalid"; exit 1; }
echo "✓ Single file archive valid"

# Test 2: Multiple files
echo "Test 2: Multiple files..."
"$BUILD_DIR/burst-writer" -l 1 -o multi.zip \
    "$FIXTURES_DIR/small.txt" \
    "$FIXTURES_DIR/medium.txt" \
    "$FIXTURES_DIR/large.bin" > /dev/null
run_7z t multi.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Multi-file archive invalid"; exit 1; }
echo "✓ Multi-file archive valid"

# Test 3: Extract and verify contents
echo "Test 3: Extract and verify contents..."
mkdir -p extract
cd extract
run_7z x -y ../multi.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Extraction failed"; exit 1; }

# Verify each file
cmp small.txt "$FIXTURES_DIR/small.txt" || { echo "❌ Failed: small.txt differs"; exit 1; }
cmp medium.txt "$FIXTURES_DIR/medium.txt" || { echo "❌ Failed: medium.txt differs"; exit 1; }
cmp large.bin "$FIXTURES_DIR/large.bin" || { echo "❌ Failed: large.bin differs"; exit 1; }
echo "✓ All extracted files match originals"

cd "$TEST_TMP"

# Test 4: Empty file handling
echo "Test 4: Empty file..."
touch empty.txt
"$BUILD_DIR/burst-writer" -l 1 -o empty.zip empty.txt > /dev/null
run_7z t empty.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Empty file archive invalid"; exit 1; }
echo "✓ Empty file handled correctly"

# Test 5: Special characters in filenames
echo "Test 5: Special characters in filename..."
echo "content" > "file with spaces.txt"
"$BUILD_DIR/burst-writer" -l 1 -o special.zip "file with spaces.txt" > /dev/null
run_7z t special.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Special chars archive invalid"; exit 1; }
echo "✓ Special characters handled correctly"

echo
echo "=== All basic writer tests passed ✓ ==="
