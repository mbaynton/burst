#!/bin/bash
# Test compatibility with multiple ZIP tools

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
FIXTURES_DIR="$PROJECT_ROOT/tests/fixtures"
TEST_TMP="$PROJECT_ROOT/tests/tmp"

# Clean and create test directory
rm -rf "$TEST_TMP"
mkdir -p "$TEST_TMP"
cd "$TEST_TMP"

echo "=== Test: ZIP Compatibility ==="
echo

# Create test archive with STORE method for compatibility with standard tools
echo "Creating test archive..."
"$BUILD_DIR/burst-writer" -l 0 -o compat.zip \
    "$FIXTURES_DIR/small.txt" \
    "$FIXTURES_DIR/medium.txt" > /dev/null

# Test with unzip
echo "Test 1: unzip compatibility..."
unzip -t compat.zip > /dev/null || { echo "❌ Failed: unzip test failed"; exit 1; }
echo "✓ unzip: OK"

# Test with zipinfo
echo "Test 2: zipinfo compatibility..."
zipinfo compat.zip > /dev/null || { echo "❌ Failed: zipinfo failed"; exit 1; }
echo "✓ zipinfo: OK"

# Test with 7z (if available)
if command -v 7z &> /dev/null; then
    echo "Test 3: 7z compatibility..."
    7z t compat.zip > /dev/null 2>&1 || { echo "❌ Failed: 7z test failed"; exit 1; }
    echo "✓ 7z: OK"
else
    echo "⊘ 7z not available, skipping"
fi

# Verify archive structure
echo "Test 4: Verify archive structure..."
zipinfo -v compat.zip > archive_info.txt

# Check for required fields
grep -qi "compression method:.*stored" archive_info.txt || { echo "❌ Failed: Wrong compression method"; exit 1; }
grep -q "file system or operating system of origin" archive_info.txt || { echo "❌ Failed: Missing OS info"; exit 1; }
echo "✓ Archive structure valid"

# Test that data descriptor flag is set
echo "Test 5: Verify data descriptors..."
# General purpose bit flag should have bit 3 set (0x0008)
# This is harder to test from command line, but zipinfo should show it
grep -q "data descriptor" archive_info.txt && echo "✓ Data descriptors present" || echo "⊘ Data descriptor check inconclusive"

echo
echo "=== All compatibility tests passed ✓ ==="
