#!/bin/bash
# Test Zstandard compression functionality

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
FIXTURES_DIR="$PROJECT_ROOT/tests/fixtures"
TEST_TMP="$PROJECT_ROOT/tests/tmp"

# Wine command for 7z-zstd - use function to handle spaces
function run_7z() {
    wine "/home/$USER/.wine/dosdevices/c:/Program Files/7-Zip-Zstandard/7z.exe" "$@"
}

# Clean and create test directory
rm -rf "$TEST_TMP"
mkdir -p "$TEST_TMP"
cd "$TEST_TMP"

echo "=== Test: Zstandard Compression ==="
echo

# Test 1: Single file with Zstandard compression
echo "Test 1: Single file with Zstandard (default level)..."
"$BUILD_DIR/burst-writer" -o zstd_single.zip "$FIXTURES_DIR/medium.txt" > /dev/null
run_7z t zstd_single.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Zstandard archive invalid"; exit 1; }
echo "✓ Zstandard single file archive valid"

# Test 2: Multiple files
echo "Test 2: Multiple files with Zstandard..."
"$BUILD_DIR/burst-writer" -o zstd_multi.zip \
    "$FIXTURES_DIR/small.txt" \
    "$FIXTURES_DIR/medium.txt" \
    "$FIXTURES_DIR/large.bin" > /dev/null
run_7z t zstd_multi.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Multi-file Zstandard archive invalid"; exit 1; }
echo "✓ Multi-file Zstandard archive valid"

# Test 3: Extract and verify contents
echo "Test 3: Extract and verify Zstandard contents..."
mkdir -p extract_zstd
cd extract_zstd
run_7z x -y ../zstd_multi.zip 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Extraction failed"; exit 1; }

# Verify each file
cmp small.txt "$FIXTURES_DIR/small.txt" || { echo "❌ Failed: small.txt differs"; exit 1; }
cmp medium.txt "$FIXTURES_DIR/medium.txt" || { echo "❌ Failed: medium.txt differs"; exit 1; }
cmp large.bin "$FIXTURES_DIR/large.bin" || { echo "❌ Failed: large.bin differs"; exit 1; }
echo "✓ All extracted Zstandard files match originals"

cd "$TEST_TMP"

# Test 4: Different compression levels
echo "Test 4: Different compression levels..."
for level in 1 3 9; do
    "$BUILD_DIR/burst-writer" -l "$level" -o "zstd_level_${level}.zip" "$FIXTURES_DIR/medium.txt" > /dev/null
    run_7z t "zstd_level_${level}.zip" 2>&1 | grep -q "Everything is Ok" || { echo "❌ Failed: Level $level invalid"; exit 1; }
done
echo "✓ Multiple compression levels work correctly"

# Test 5: Verify compression method is Zstandard (93)
echo "Test 5: Verify compression method..."
zipinfo zstd_single.zip | grep -q "u093" || { echo "❌ Failed: Not using Zstandard method"; exit 1; }
echo "✓ Compression method is Zstandard (93)"

# Test 6: Verify compression ratio
echo "Test 6: Verify compression is working..."
"$BUILD_DIR/burst-writer" -l 3 -o zstd_compress_test.zip "$FIXTURES_DIR/medium.txt" > compress_out.txt 2>&1
# Medium.txt is 35 bytes, compressed should be less (around 30-32 bytes)
compressed_size=$(grep "Compression ratio:" compress_out.txt | awk '{print $3}' | tr -d '%')
if [ "$(echo "$compressed_size < 100" | bc)" -eq 1 ]; then
    echo "✓ Compression working (ratio: ${compressed_size}%)"
else
    echo "❌ Failed: No compression detected (ratio: ${compressed_size}%)"
    exit 1
fi

echo
echo "=== All Zstandard compression tests passed ✓ ==="
