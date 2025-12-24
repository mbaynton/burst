#!/bin/bash
# Test empty file alignment edge case
# Verifies that empty files don't violate 8 MiB alignment rule

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
TEST_TMP="$PROJECT_ROOT/tests/tmp/empty_align"

# Use native 7zz with Zstandard support
function run_7z() {
    7zz "$@"
}

# Clean and create test directory
rm -rf "$TEST_TMP"
mkdir -p "$TEST_TMP"
cd "$TEST_TMP"

echo "=== Test: Empty File Alignment Bug Fix ==="
echo

# Test 1: Single empty file (basic case)
echo "Test 1: Single empty file..."
touch empty.txt
"$BUILD_DIR/burst-writer" -o test1.zip empty.txt > test1_out.txt 2>&1

# Verify 7-Zip can extract
run_7z t test1.zip 2>&1 | grep -q "Everything is Ok" || {
    echo "❌ Failed: Single empty file archive invalid"
    exit 1
}
echo "✓ Single empty file works"
echo

# Test 2: Many empty files crossing 8 MiB boundary (stress test)
echo "Test 2: 300 empty files (stress test)..."
for i in $(seq 1 300); do
    touch "empty_$i.txt"
done

"$BUILD_DIR/burst-writer" -o test2.zip empty_*.txt > test2_out.txt 2>&1

# Verify 7-Zip can extract
run_7z t test2.zip 2>&1 | grep -q "Everything is Ok" || {
    echo "❌ Failed: Multi-empty file archive invalid"
    exit 1
}
echo "✓ 300 empty files archive valid"

# Verify alignment using verification script
echo "Test 3: Verify 8 MiB boundary alignment..."
python3 "$PROJECT_ROOT/tests/integration/verify_alignment.py" test2.zip || {
    echo "❌ Failed: Alignment violation detected"
    exit 1
}
echo "✓ All boundaries aligned correctly"
echo

# Test 3: Mixed empty and non-empty files
echo "Test 4: Mixed empty and non-empty files..."
echo "content" > file1.txt
touch empty1.txt
echo "more content" > file2.txt
touch empty2.txt
touch empty3.txt
echo "final content" > file3.txt

"$BUILD_DIR/burst-writer" -o test3.zip file1.txt empty1.txt file2.txt empty2.txt empty3.txt file3.txt > test3_out.txt 2>&1

# Verify archive validity
run_7z t test3.zip 2>&1 | grep -q "Everything is Ok" || {
    echo "❌ Failed: Mixed file archive invalid"
    exit 1
}

# Extract and verify non-empty files
mkdir -p extract
cd extract
run_7z x -y ../test3.zip 2>&1 | grep -q "Everything is Ok" || {
    echo "❌ Failed: Extraction failed"
    exit 1
}

cmp file1.txt ../file1.txt || { echo "❌ Failed: file1.txt differs"; exit 1; }
cmp file2.txt ../file2.txt || { echo "❌ Failed: file2.txt differs"; exit 1; }
cmp file3.txt ../file3.txt || { echo "❌ Failed: file3.txt differs"; exit 1; }

# Verify empty files are empty
[ ! -s empty1.txt ] || { echo "❌ Failed: empty1.txt not empty"; exit 1; }
[ ! -s empty2.txt ] || { echo "❌ Failed: empty2.txt not empty"; exit 1; }
[ ! -s empty3.txt ] || { echo "❌ Failed: empty3.txt not empty"; exit 1; }

cd ..
echo "✓ Mixed files test passed"
echo

echo "=== All empty file alignment tests passed ✓ ==="
echo
echo "Summary:"
echo "  - Single empty file works correctly"
echo "  - 300 empty files maintain alignment"
echo "  - All 8 MiB boundaries aligned to valid markers"
echo "  - Mixed empty and non-empty files work correctly"
echo "  - 7-Zip successfully extracts all files"
