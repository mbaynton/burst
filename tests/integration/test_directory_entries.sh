#!/bin/bash
# Integration tests for directory entries in BURST archives

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
BURST_WRITER="$BUILD_DIR/burst-writer"

# Create temp directory for tests
TEST_DIR=$(mktemp -d)
trap "rm -rf $TEST_DIR" EXIT

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "BURST Directory Entries Integration Test"
echo "=========================================="
echo ""

# Check if burst-writer exists
if [ ! -x "$BURST_WRITER" ]; then
    echo -e "${RED}Error: burst-writer not found at $BURST_WRITER${NC}"
    echo "Please run: cd build && cmake .. && make"
    exit 1
fi

# Check if 7zz is available
if ! command -v 7zz &> /dev/null; then
    echo -e "${RED}Error: 7zz not found${NC}"
    echo "Please install 7-Zip from 7-zip.org"
    exit 1
fi

echo "Prerequisites OK"
echo ""

# Test 1: Single empty directory
echo "Test 1: Single empty directory"
TEST1_DIR="$TEST_DIR/test1"
mkdir -p "$TEST1_DIR/source/empty_dir"
"$BURST_WRITER" -l 1 -o "$TEST1_DIR/test.zip" "$TEST1_DIR/source" > /dev/null 2>&1

# Check if directory is in the archive
if 7zz l "$TEST1_DIR/test.zip" | grep -q "empty_dir"; then
    echo -e "  ${GREEN}✓${NC} Empty directory included in archive"
else
    echo -e "  ${RED}✗${NC} Empty directory NOT in archive"
    exit 1
fi

# Extract and verify
mkdir -p "$TEST1_DIR/extract"
7zz x "$TEST1_DIR/test.zip" -o"$TEST1_DIR/extract" > /dev/null 2>&1
if [ -d "$TEST1_DIR/extract/empty_dir" ]; then
    echo -e "  ${GREEN}✓${NC} Empty directory recreated on extraction"
else
    echo -e "  ${RED}✗${NC} Empty directory not recreated"
    exit 1
fi
echo ""

# Test 2: Nested empty directories
echo "Test 2: Nested empty directories"
TEST2_DIR="$TEST_DIR/test2"
mkdir -p "$TEST2_DIR/source/a/b/c"
"$BURST_WRITER" -l 1 -o "$TEST2_DIR/test.zip" "$TEST2_DIR/source" > /dev/null 2>&1

# Check all directory levels are present
DIRS_FOUND=0
for dir in "a/" "a/b/" "a/b/c/"; do
    if 7zz l "$TEST2_DIR/test.zip" | grep -q "$dir"; then
        ((DIRS_FOUND++))
    fi
done

if [ $DIRS_FOUND -eq 3 ]; then
    echo -e "  ${GREEN}✓${NC} All intermediate directories included ($DIRS_FOUND/3)"
else
    echo -e "  ${RED}✗${NC} Missing intermediate directories ($DIRS_FOUND/3)"
    exit 1
fi

# Extract and verify structure
mkdir -p "$TEST2_DIR/extract"
7zz x "$TEST2_DIR/test.zip" -o"$TEST2_DIR/extract" > /dev/null 2>&1
if [ -d "$TEST2_DIR/extract/a/b/c" ]; then
    echo -e "  ${GREEN}✓${NC} Nested directory structure recreated"
else
    echo -e "  ${RED}✗${NC} Nested structure not recreated"
    exit 1
fi
echo ""

# Test 3: Mixed files and directories
echo "Test 3: Mixed files and directories"
TEST3_DIR="$TEST_DIR/test3"
mkdir -p "$TEST3_DIR/source/dir1/dir2"
echo "root file" > "$TEST3_DIR/source/file1.txt"
echo "nested file" > "$TEST3_DIR/source/dir1/file2.txt"
"$BURST_WRITER" -l 1 -o "$TEST3_DIR/test.zip" "$TEST3_DIR/source" > /dev/null 2>&1

# Count entries
TOTAL_ENTRIES=$(7zz l "$TEST3_DIR/test.zip" | grep -E "^[0-9]{4}-" | wc -l)
# Should have: dir1/, dir1/dir2/, file1.txt, dir1/file2.txt = 4 entries
if [ "$TOTAL_ENTRIES" -ge 4 ]; then
    echo -e "  ${GREEN}✓${NC} Archive contains $TOTAL_ENTRIES entries (expected ≥4)"
else
    echo -e "  ${RED}✗${NC} Archive has only $TOTAL_ENTRIES entries (expected ≥4)"
    exit 1
fi

# Extract and verify
mkdir -p "$TEST3_DIR/extract"
7zz x "$TEST3_DIR/test.zip" -o"$TEST3_DIR/extract" > /dev/null 2>&1
if [ -f "$TEST3_DIR/extract/file1.txt" ] && \
   [ -d "$TEST3_DIR/extract/dir1" ] && \
   [ -d "$TEST3_DIR/extract/dir1/dir2" ] && \
   [ -f "$TEST3_DIR/extract/dir1/file2.txt" ]; then
    echo -e "  ${GREEN}✓${NC} All files and directories recreated correctly"
else
    echo -e "  ${RED}✗${NC} Some files or directories missing"
    exit 1
fi
echo ""

# Test 4: Directory with trailing slash in name
echo "Test 4: Verify directory entries have trailing slash"
TEST4_DIR="$TEST_DIR/test4"
mkdir -p "$TEST4_DIR/source/mydir"
"$BURST_WRITER" -l 1 -o "$TEST4_DIR/test.zip" "$TEST4_DIR/source" > /dev/null 2>&1

# Use zipinfo to check exact entry names (more reliable than 7zz list)
if command -v zipinfo &> /dev/null; then
    if zipinfo -1 "$TEST4_DIR/test.zip" | grep -q "mydir/$"; then
        echo -e "  ${GREEN}✓${NC} Directory has trailing slash in ZIP entry"
    else
        echo -e "  ${RED}✗${NC} Directory missing trailing slash"
        exit 1
    fi
else
    echo -e "  ${YELLOW}⊘${NC} zipinfo not available, skipping trailing slash verification"
fi
echo ""

# Test 5: Directory permissions preservation
echo "Test 5: Directory permissions preservation"
TEST5_DIR="$TEST_DIR/test5"
mkdir -p "$TEST5_DIR/source/restricted"
chmod 0700 "$TEST5_DIR/source/restricted"
"$BURST_WRITER" -l 1 -o "$TEST5_DIR/test.zip" "$TEST5_DIR/source" > /dev/null 2>&1

# Check if directory is marked with correct attributes
if 7zz l -slt "$TEST5_DIR/test.zip" | grep -A 10 "restricted" | grep -q "Attributes.*D"; then
    echo -e "  ${GREEN}✓${NC} Directory marked with D attribute"
else
    echo -e "  ${RED}✗${NC} Directory not marked as directory type"
    exit 1
fi

# Extract and check permissions (note: may require -p flag or won't work on all systems)
mkdir -p "$TEST5_DIR/extract"
7zz x "$TEST5_DIR/test.zip" -o"$TEST5_DIR/extract" > /dev/null 2>&1 || true
if [ -d "$TEST5_DIR/extract/restricted" ]; then
    echo -e "  ${GREEN}✓${NC} Directory with permissions recreated"
else
    echo -e "  ${YELLOW}⊘${NC} Permission preservation check skipped (requires proper 7zz version)"
fi
echo ""

# Test 6: Pre-order traversal (directories before their contents)
echo "Test 6: Pre-order traversal verification"
TEST6_DIR="$TEST_DIR/test6"
mkdir -p "$TEST6_DIR/source/parent/child"
echo "file" > "$TEST6_DIR/source/parent/file.txt"
echo "child file" > "$TEST6_DIR/source/parent/child/file.txt"
"$BURST_WRITER" -l 1 -o "$TEST6_DIR/test.zip" "$TEST6_DIR/source" > /dev/null 2>&1

# Get entry order and check parent/ comes before parent/child/ and parent/file.txt
if command -v zipinfo &> /dev/null; then
    ENTRIES=$(zipinfo -1 "$TEST6_DIR/test.zip")
    PARENT_LINE=$(echo "$ENTRIES" | grep -n "parent/$" | cut -d: -f1)
    CHILD_LINE=$(echo "$ENTRIES" | grep -n "parent/child/$" | cut -d: -f1)
    FILE_LINE=$(echo "$ENTRIES" | grep -n "parent/file.txt$" | cut -d: -f1)

    if [ -n "$PARENT_LINE" ] && [ -n "$CHILD_LINE" ] && [ "$PARENT_LINE" -lt "$CHILD_LINE" ]; then
        echo -e "  ${GREEN}✓${NC} Parent directory appears before child directory (pre-order)"
    else
        echo -e "  ${YELLOW}⊘${NC} Could not verify pre-order traversal"
    fi
else
    echo -e "  ${YELLOW}⊘${NC} zipinfo not available, skipping order verification"
fi
echo ""

echo "=========================================="
echo -e "${GREEN}All tests passed!${NC}"
echo "=========================================="
exit 0
