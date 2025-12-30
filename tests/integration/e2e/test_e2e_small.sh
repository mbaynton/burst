#!/bin/bash
#
# End-to-End Test 1: Small Archive (< 8 MiB)
#
# Tests:
#   - Archive creation < 8 MiB
#   - Single-part upload to S3
#   - Single-part download with burst-downloader
#   - BTRFS encoded write extraction
#   - Checksum verification
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
TEST_NAME="E2E Small Archive (<8 MiB)"
TEST_ID="small-$(date +%s)"
TEST_WORK_DIR="$E2E_TMP_DIR/test_small_$TEST_ID"
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
echo ""

# Create test workspace
mkdir -p "$TEST_WORK_DIR"
cd "$TEST_WORK_DIR"

# Step 1: Create test files (total ~5 MiB of random data)
echo "Step 1: Creating test files..."
mkdir -p input
create_test_file "input/file1.bin" $((2 * 1024 * 1024))  # 2 MiB
create_test_file "input/file2.bin" $((1 * 1024 * 1024))  # 1 MiB
create_test_file "input/file3.bin" $((2 * 1024 * 1024))  # 2 MiB
echo -e "${GREEN}✓${NC} Created 3 files (5 MiB total random data)"

# Store original checksums
store_checksums "input" "checksums_original.txt"
echo -e "${GREEN}✓${NC} Stored original checksums"

# Step 2: Create BURST archive
echo ""
echo "Step 2: Creating BURST archive..."
"$BURST_WRITER" -l 3 -o test.zip input/*.bin
archive_size=$(stat -c%s "test.zip")
echo -e "${GREEN}✓${NC} Created archive: test.zip ($archive_size bytes)"

# Verify archive is valid
if ! verify_archive "test.zip"; then
    echo -e "${RED}✗ Archive validation failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} Archive validation passed (7zz t)"

# Archive should be < 8 MiB (single part)
if [ "$archive_size" -ge $((8 * 1024 * 1024)) ]; then
    echo -e "${YELLOW}Warning: Archive is >= 8 MiB, expected single part${NC}"
fi

# Step 3: Upload to S3
echo ""
echo "Step 3: Uploading to S3..."
if ! upload_to_s3 "test.zip" "$S3_KEY"; then
    exit 1
fi
S3_UPLOADED="yes"

# Step 4: Download and extract with burst-downloader
echo ""
echo "Step 4: Downloading and extracting with burst-downloader..."
EXTRACT_DIR="$BTRFS_MOUNT_DIR/extracted_$TEST_ID"
mkdir -p "$EXTRACT_DIR"

"$BURST_DOWNLOADER" \
    --bucket "$BURST_TEST_BUCKET" \
    --key "$S3_KEY" \
    --region "$AWS_REGION" \
    --output-dir "$EXTRACT_DIR"

if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Download/extraction failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} Download and extraction completed"

# Step 5: Verify checksums
echo ""
echo "Step 5: Verifying extracted files..."
if ! verify_checksums "$EXTRACT_DIR" "checksums_original.txt"; then
    echo -e "${RED}✗ Checksum verification failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} All checksums match"

# Step 6: Optional - Compare with 7zz extraction
echo ""
echo "Step 6: Cross-validation with 7zz..."
mkdir -p "7zz_extract"
extract_with_7zz "test.zip" "7zz_extract"
if ! verify_checksums "7zz_extract" "checksums_original.txt"; then
    echo -e "${RED}✗ 7zz extraction verification failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} 7zz extraction matches original"

# Success
print_test_result "$TEST_NAME" 0
exit 0
