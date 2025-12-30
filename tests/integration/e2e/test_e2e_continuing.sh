#!/bin/bash
#
# End-to-End Test 3: Archive with Continuing File
#
# Tests:
#   - Single large file (>8 MiB) spanning parts
#   - Start-of-Part metadata frame handling
#   - File continuation logic in stream processor
#   - Verifies Start-of-Part frame appears in alignment output
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
TEST_NAME="E2E Continuing File (12 MiB single file)"
TEST_ID="continuing-$(date +%s)"
TEST_WORK_DIR="$E2E_TMP_DIR/test_continuing_$TEST_ID"
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

    if [ -d "$TEST_WORK_DIR" ]; then
        rm -rf "$TEST_WORK_DIR"
        echo -e "${GREEN}✓${NC} Removed test directory"
    fi

    if [ -d "$BTRFS_MOUNT_DIR/extracted_$TEST_ID" ]; then
        rm -rf "$BTRFS_MOUNT_DIR/extracted_$TEST_ID"
        echo -e "${GREEN}✓${NC} Removed extracted files"
    fi

    if [ $TEST_EXIT_CODE -eq 0 ] && [ -n "$S3_UPLOADED" ]; then
        delete_from_s3 "$S3_KEY"
        echo -e "${GREEN}✓${NC} Removed S3 object"
    elif [ -n "$S3_UPLOADED" ]; then
        echo -e "${YELLOW}Note:${NC} Leaving S3 object for debugging: s3://$BURST_TEST_BUCKET/$S3_KEY"
    fi
}

trap cleanup EXIT

# Main test
print_test_header "$TEST_NAME"

if ! check_prerequisites; then
    exit 1
fi

echo "Test configuration:"
echo "  Single file: 12 MiB (spans 2 parts)"
echo "  Tests: Start-of-Part metadata frame handling"
echo "  S3 Bucket:   $BURST_TEST_BUCKET"
echo "  S3 Key:      $S3_KEY"
echo "  BTRFS Mount: $BTRFS_MOUNT_DIR"
echo ""

mkdir -p "$TEST_WORK_DIR"
cd "$TEST_WORK_DIR"

# Step 1: Create single large file (12 MiB of random data)
echo "Step 1: Creating large test file (12 MiB random data)..."
mkdir -p input
create_test_file "input/largefile.bin" $((12 * 1024 * 1024))  # 12 MiB
echo -e "${GREEN}✓${NC} Created 12 MiB file"

store_checksums "input" "checksums_original.txt"
echo -e "${GREEN}✓${NC} Stored checksum"

# Step 2: Create archive
echo ""
echo "Step 2: Creating BURST archive..."
"$BURST_WRITER" -l 3 -o test.zip input/largefile.bin
archive_size=$(stat -c%s "test.zip")
echo -e "${GREEN}✓${NC} Created archive: $archive_size bytes"

# Verify
if ! verify_archive "test.zip"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} Archive valid (7zz t)"

# Step 3: Verify alignment - CRITICAL for this test
# The alignment check should show a Start-of-Part metadata frame at the 8 MiB boundary
echo ""
echo "Step 3: Verifying alignment (expecting Start-of-Part frame)..."
python3 "$E2E_PROJECT_ROOT/tests/integration/verify_alignment.py" -v "test.zip" 2>&1 | tee alignment_output.txt

# Check that Start-of-Part frame was detected
if grep -q "Start-of-Part" alignment_output.txt; then
    echo -e "${GREEN}✓${NC} Start-of-Part metadata frame detected"
else
    echo -e "${RED}✗ No Start-of-Part frame detected${NC}"
    echo "This test specifically requires a continuing file scenario."
    echo "A 12 MiB file should produce a Start-of-Part frame at the 8 MiB boundary."
    exit 1
fi

if ! verify_alignment "test.zip"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} Alignment verified with Start-of-Part frame"

# Step 4: Upload
echo ""
echo "Step 4: Uploading to S3..."
if ! upload_to_s3 "test.zip" "$S3_KEY"; then
    exit 1
fi
S3_UPLOADED="yes"

# Step 5: Download and extract
echo ""
echo "Step 5: Downloading and extracting..."
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
echo -e "${GREEN}✓${NC} Download completed"

# Step 6: Verify file integrity
echo ""
echo "Step 6: Verifying file integrity..."
if ! verify_checksums "$EXTRACT_DIR" "checksums_original.txt"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} Large file extracted correctly across part boundary"

# Verify file size
extracted_size=$(stat -c%s "$EXTRACT_DIR/largefile.bin")
expected_size=$((12 * 1024 * 1024))
if [ "$extracted_size" -ne "$expected_size" ]; then
    echo -e "${RED}✗ File size mismatch: expected $expected_size, got $extracted_size${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} File size verified: $extracted_size bytes"

# Step 7: Cross-validate with 7zz
echo ""
echo "Step 7: Cross-validation with 7zz..."
mkdir -p "7zz_extract"
extract_with_7zz "test.zip" "7zz_extract"
if ! verify_checksums "7zz_extract" "checksums_original.txt"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} 7zz extraction matches"

print_test_result "$TEST_NAME" 0
exit 0
