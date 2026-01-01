#!/bin/bash
# Test Unix file metadata preservation (permissions, symlinks) with BURST writer

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
TEST_TMP="$PROJECT_ROOT/tests/tmp/unix_metadata"

# Use native 7zz with Zstandard support
function run_7z() {
    7zz "$@"
}

# Clean and create test directory
rm -rf "$TEST_TMP"
mkdir -p "$TEST_TMP"
cd "$TEST_TMP"

echo "=== Test: Unix File Metadata Preservation ==="
echo

# Test 1: File permissions preservation
echo "Test 1: File permissions (0755)..."
echo "executable content" > exec_file.txt
chmod 0755 exec_file.txt
"$BUILD_DIR/burst-writer" -l 1 -o perms_755.zip exec_file.txt > /dev/null
run_7z t perms_755.zip 2>&1 | grep -q "Everything is Ok" || { echo "FAIL: Archive invalid"; exit 1; }
# Verify permissions in archive listing
run_7z l -slt perms_755.zip | grep -q "rwxr-xr-x" || { echo "FAIL: 0755 permissions not preserved in archive"; exit 1; }
echo "PASS: 0755 permissions stored correctly"

# Test 2: File permissions (0600)
echo "Test 2: File permissions (0600)..."
echo "secret content" > secret_file.txt
chmod 0600 secret_file.txt
"$BUILD_DIR/burst-writer" -l 1 -o perms_600.zip secret_file.txt > /dev/null
run_7z t perms_600.zip 2>&1 | grep -q "Everything is Ok" || { echo "FAIL: Archive invalid"; exit 1; }
# Verify permissions in archive listing
run_7z l -slt perms_600.zip | grep -q "rw-------" || { echo "FAIL: 0600 permissions not preserved in archive"; exit 1; }
echo "PASS: 0600 permissions stored correctly"

# Test 3: File permissions (0644 default)
echo "Test 3: File permissions (0644)..."
echo "normal content" > normal_file.txt
chmod 0644 normal_file.txt
"$BUILD_DIR/burst-writer" -l 1 -o perms_644.zip normal_file.txt > /dev/null
run_7z t perms_644.zip 2>&1 | grep -q "Everything is Ok" || { echo "FAIL: Archive invalid"; exit 1; }
# Verify permissions in archive listing
run_7z l -slt perms_644.zip | grep -q "rw-r--r--" || { echo "FAIL: 0644 permissions not preserved in archive"; exit 1; }
echo "PASS: 0644 permissions stored correctly"

# Test 4: Relative symlink
echo "Test 4: Relative symlink..."
echo "target content" > target.txt
ln -s target.txt relative_link.txt
"$BUILD_DIR/burst-writer" -l 1 -o symlink_rel.zip target.txt relative_link.txt > /dev/null
run_7z t symlink_rel.zip 2>&1 | grep -q "Everything is Ok" || { echo "FAIL: Archive invalid"; exit 1; }
# Verify symlink is in archive
run_7z l symlink_rel.zip | grep -q "relative_link.txt" || { echo "FAIL: Symlink not in archive"; exit 1; }
echo "PASS: Relative symlink archived correctly"

# Test 5: Symlink with path components
echo "Test 5: Symlink with path components..."
mkdir -p subdir
echo "nested target" > subdir/nested.txt
ln -s subdir/nested.txt path_link.txt
"$BUILD_DIR/burst-writer" -l 1 -o symlink_path.zip subdir/nested.txt path_link.txt > /dev/null
run_7z t symlink_path.zip 2>&1 | grep -q "Everything is Ok" || { echo "FAIL: Archive invalid"; exit 1; }
echo "PASS: Symlink with path archived correctly"

# Test 6: Extract with 7zz and verify permissions
echo "Test 6: Extract and verify permissions..."
mkdir -p extract_perms
run_7z x -y -o"extract_perms" perms_755.zip > /dev/null 2>&1
actual_perms=$(stat -c %a extract_perms/exec_file.txt)
if [ "$actual_perms" != "755" ]; then
    echo "FAIL: Expected 755, got $actual_perms"
    exit 1
fi
echo "PASS: Extracted file has correct 0755 permissions"

# Test 7: Extract with 7zz and verify symlink
echo "Test 7: Extract and verify symlink..."
mkdir -p extract_symlink
run_7z x -y -o"extract_symlink" symlink_rel.zip > /dev/null 2>&1
if [ ! -L "extract_symlink/relative_link.txt" ]; then
    echo "FAIL: Extracted file is not a symlink"
    exit 1
fi
actual_target=$(readlink extract_symlink/relative_link.txt)
if [ "$actual_target" != "target.txt" ]; then
    echo "FAIL: Expected target.txt, got $actual_target"
    exit 1
fi
echo "PASS: Extracted symlink has correct target"

# Test 8: Multiple files with different permissions
echo "Test 8: Multiple files with mixed metadata..."
echo "file A" > file_a.txt
echo "file B" > file_b.txt
echo "file C" > file_c.txt
chmod 0755 file_a.txt
chmod 0600 file_b.txt
chmod 0644 file_c.txt
ln -s file_a.txt link_to_a.txt
"$BUILD_DIR/burst-writer" -l 1 -o mixed.zip file_a.txt file_b.txt file_c.txt link_to_a.txt > /dev/null
run_7z t mixed.zip 2>&1 | grep -q "Everything is Ok" || { echo "FAIL: Archive invalid"; exit 1; }

# Extract and verify all
mkdir -p extract_mixed
run_7z x -y -o"extract_mixed" mixed.zip > /dev/null 2>&1
[ "$(stat -c %a extract_mixed/file_a.txt)" = "755" ] || { echo "FAIL: file_a.txt permissions wrong"; exit 1; }
[ "$(stat -c %a extract_mixed/file_b.txt)" = "600" ] || { echo "FAIL: file_b.txt permissions wrong"; exit 1; }
[ "$(stat -c %a extract_mixed/file_c.txt)" = "644" ] || { echo "FAIL: file_c.txt permissions wrong"; exit 1; }
[ -L "extract_mixed/link_to_a.txt" ] || { echo "FAIL: link_to_a.txt is not a symlink"; exit 1; }
[ "$(readlink extract_mixed/link_to_a.txt)" = "file_a.txt" ] || { echo "FAIL: link_to_a.txt target wrong"; exit 1; }
echo "PASS: Mixed metadata preserved correctly"

# Clean up
cd "$PROJECT_ROOT"
rm -rf "$TEST_TMP"

echo
echo "=== All Unix metadata writer tests passed ==="
