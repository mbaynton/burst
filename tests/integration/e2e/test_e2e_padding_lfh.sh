#!/bin/bash
#
# End-to-End Test: Padding LFH Handling at Boundaries
#
# Tests:
#   - Archive with padding LFH near 8 MiB boundary
#   - Padding LFH NOT in central directory
#   - .burst_padding file NOT extracted
#   - Real files extracted correctly with checksums
#   - Cross-validation with 7zz
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source common functions
# shellcheck source=e2e_common.sh
source "$SCRIPT_DIR/e2e_common.sh"

# Source BTRFS setup
# shellcheck source=setup_btrfs.sh
source "$SCRIPT_DIR/setup_btrfs.sh"

# Test configuration
TEST_NAME="E2E Padding LFH at Boundaries"
TEST_ID="padding-lfh-$(date +%s)"
TEST_WORK_DIR="$E2E_TMP_DIR/test_padding_lfh_$TEST_ID"
S3_KEY="${BURST_TEST_KEY_PREFIX}${TEST_ID}.zip"

# Tracking variables
S3_UPLOADED=""
TEST_EXIT_CODE=0

# Cleanup function
cleanup() {
    local exit_code=$?
    if [ $exit_code -ne 0 ]; then
        TEST_EXIT_CODE=$exit_code
    fi

    echo ""
    echo "Cleaning up..."

    # Remove test files
    if [ -d "$TEST_WORK_DIR" ]; then
        rm -rf "$TEST_WORK_DIR"
        echo -e "${GREEN}✓${NC} Removed test directory"
    fi

    # Remove extracted files from BTRFS
    if [ -d "$BTRFS_MOUNT_DIR/extracted_$TEST_ID" ]; then
        rm -rf "$BTRFS_MOUNT_DIR/extracted_$TEST_ID"
        echo -e "${GREEN}✓${NC} Removed extracted files"
    fi

    # Only delete S3 object on success (leave for debugging on failure)
    if [ $TEST_EXIT_CODE -eq 0 ] && [ -n "$S3_UPLOADED" ]; then
        delete_from_s3 "$S3_KEY"
        echo -e "${GREEN}✓${NC} Removed S3 object"
    elif [ -n "$S3_UPLOADED" ]; then
        echo -e "${YELLOW}Note:${NC} Leaving S3 object for debugging: s3://$BURST_TEST_BUCKET/$S3_KEY"
    fi

    # Note: BTRFS mount is intentionally kept for subsequent test runs
}

trap cleanup EXIT

# Main test
print_test_header "$TEST_NAME"

# Check prerequisites
if ! check_prerequisites; then
    exit 1
fi

echo "Test configuration:"
echo "  S3 Bucket:   $BURST_TEST_BUCKET"
echo "  S3 Key:      $S3_KEY"
echo "  AWS Region:  $AWS_REGION"
echo "  BTRFS Mount: $BTRFS_MOUNT_DIR"
echo "  Test Writer: ${BURST_WRITER_TEST_MODE:-NOT SET}"
echo ""

# Verify test-mode writer binary exists
if [ -z "$BURST_WRITER_TEST_MODE" ] || [ ! -x "$BURST_WRITER_TEST_MODE" ]; then
    echo -e "${RED}✗${NC} BURST_WRITER_TEST_MODE not set or not executable"
    echo "Expected: burst-writer-test-mode binary path"
    exit 1
fi

# Create test workspace
mkdir -p "$TEST_WORK_DIR"
cd "$TEST_WORK_DIR"

# Step 1: Create test files to trigger padding near 8 MiB boundary
echo "Step 1: Creating test files..."
mkdir -p input

# Create files that will result in padding LFH near 8 MiB boundary
# The test mode writer forces padding at 8388608 - 1024 = 8387584
# Use 9 MiB of random data to definitely cross the boundary (random data doesn't compress much)
create_test_file "input/file1.bin" $((9 * 1024 * 1024))     # 9 MiB
create_test_file "input/file2.bin" $((512 * 1024))          # 512 KiB
create_test_file "input/file3.bin" $((1024 * 1024))         # 1 MiB
echo -e "${GREEN}✓${NC} Created 3 files (~10.5 MiB total)"

# Store original checksums
store_checksums "input" "checksums_original.txt"
echo -e "${GREEN}✓${NC} Stored original checksums"

# Step 2: Create archive with test-mode writer (forces padding LFH)
echo "Step 2: Creating BURST archive with test mode writer..."
"$BURST_WRITER_TEST_MODE" -l 3 -o test.zip input/*.bin
if [ ! -f test.zip ]; then
    echo -e "${RED}✗${NC} Failed to create test.zip"
    exit 1
fi
ARCHIVE_SIZE=$(stat -f%z test.zip 2>/dev/null || stat -c%s test.zip)
echo -e "${GREEN}✓${NC} Created test.zip ($ARCHIVE_SIZE bytes)"

# Step 3: Verify archive structure with zipinfo
echo "Step 3: Verifying archive structure..."
zipinfo -v test.zip > zipinfo_output.txt

# Count entries in central directory (should be 3, not 4)
ENTRY_COUNT=$(zipinfo -h test.zip | grep "number of entries" | sed 's/.*number of entries: \([0-9]*\).*/\1/')
if [ "$ENTRY_COUNT" != "3" ]; then
    echo -e "${RED}✗${NC} Expected 3 entries in central directory, found $ENTRY_COUNT"
    cat zipinfo_output.txt
    exit 1
fi
echo -e "${GREEN}✓${NC} Central directory has 3 entries (padding LFH not listed)"

# Verify .burst_padding is NOT in central directory
if grep -q "\.burst_padding" zipinfo_output.txt; then
    echo -e "${RED}✗${NC} .burst_padding should NOT be in central directory"
    cat zipinfo_output.txt
    exit 1
fi
echo -e "${GREEN}✓${NC} .burst_padding not in central directory"

# Step 4: Upload to S3
echo "Step 4: Uploading to S3..."
upload_to_s3 "test.zip" "$S3_KEY"
S3_UPLOADED="true"
echo -e "${GREEN}✓${NC} Uploaded to s3://$BURST_TEST_BUCKET/$S3_KEY"

# Step 5: Download and extract with burst-downloader
echo "Step 5: Downloading and extracting..."
EXTRACT_DIR="$BTRFS_MOUNT_DIR/extracted_$TEST_ID"
mkdir -p "$EXTRACT_DIR"
"$BURST_DOWNLOADER" \
    --bucket "$BURST_TEST_BUCKET" \
    --key "$S3_KEY" \
    --region "$AWS_REGION" \
    --output-dir "$EXTRACT_DIR"
echo -e "${GREEN}✓${NC} Downloaded and extracted"

# Step 6: Verify .burst_padding file NOT created
echo "Step 6: Verifying .burst_padding file not extracted..."
if [ -f "$EXTRACT_DIR/.burst_padding" ]; then
    echo -e "${RED}✗${NC} .burst_padding file should NOT be extracted"
    ls -la "$EXTRACT_DIR/"
    exit 1
fi
echo -e "${GREEN}✓${NC} .burst_padding file not extracted"

# Step 7: Verify extracted file count
EXTRACTED_COUNT=$(find "$EXTRACT_DIR" -type f | wc -l)
if [ "$EXTRACTED_COUNT" != "3" ]; then
    echo -e "${RED}✗${NC} Expected 3 extracted files, found $EXTRACTED_COUNT"
    find "$EXTRACT_DIR" -type f
    exit 1
fi
echo -e "${GREEN}✓${NC} Extracted 3 files (correct count)"

# Step 8: Verify checksums
echo "Step 8: Verifying checksums..."
verify_checksums "$EXTRACT_DIR" "checksums_original.txt"
echo -e "${GREEN}✓${NC} All checksums match"

# Step 9: Cross-validate with 7zz
echo "Step 9: Cross-validating with 7zz..."
mkdir -p "7zz_extract"
extract_with_7zz "test.zip" "7zz_extract"
verify_checksums "7zz_extract" "checksums_original.txt"
echo -e "${GREEN}✓${NC} 7zz extraction verified"

print_test_result "$TEST_NAME" 0
