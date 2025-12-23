#!/bin/bash
# Test compatibility with ZIP tools that support Zstandard

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

echo "=== Test: ZIP Compatibility ==="
echo

# Create test archive with Zstandard compression
echo "Creating test archive..."
"$BUILD_DIR/burst-writer" -l 1 -o compat.zip \
    "$FIXTURES_DIR/small.txt" \
    "$FIXTURES_DIR/medium.txt" > /dev/null

# Test with 7zz (has Zstandard support)
echo "Test 1: 7zz compatibility..."
run_7z t compat.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: 7zz test failed"; exit 1; }
echo "✓ 7zz: OK"

# Test with zipinfo (should work for structure even if it can't decompress)
echo "Test 2: zipinfo compatibility..."
zipinfo compat.zip > /dev/null || { echo "❌ Failed: zipinfo failed"; exit 1; }
echo "✓ zipinfo: OK"

# Verify archive structure
echo "Test 3: Verify archive structure..."
zipinfo -v compat.zip > archive_info.txt

# Check for Zstandard compression method (93)
grep -qi "unknown (93)" archive_info.txt || { echo "❌ Failed: Wrong compression method (expected Zstandard/93)"; exit 1; }
grep -q "file system or operating system of origin" archive_info.txt || { echo "❌ Failed: Missing OS info"; exit 1; }
echo "✓ Archive structure valid (Zstandard compression)"

# Test extraction
echo "Test 4: Extract and verify..."
mkdir -p extract
cd extract
run_7z x -y ../compat.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Extraction failed"; exit 1; }
cmp small.txt "$FIXTURES_DIR/small.txt" || { echo "❌ Failed: small.txt differs"; exit 1; }
cmp medium.txt "$FIXTURES_DIR/medium.txt" || { echo "❌ Failed: medium.txt differs"; exit 1; }
echo "✓ Extraction and verification successful"

echo
echo "=== All compatibility tests passed ✓ ==="
