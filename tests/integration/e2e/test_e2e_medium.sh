#!/bin/bash
#
# End-to-End Test 2: Medium Archive (8-16 MiB)
#
# Tests:
#   - Archive spanning 2 parts (8-16 MiB)
#   - Part boundary handling
#   - Multi-part download coordination
#   - 8 MiB alignment verification
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
TEST_NAME="E2E Medium Archive (8-16 MiB, 2 parts)"
TEST_ID="medium-$(date +%s)"
TEST_WORK_DIR="$E2E_TMP_DIR/test_medium_$TEST_ID"
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
echo "  Target size: ~10 MiB (2 parts)"
echo "  S3 Bucket:   $BURST_TEST_BUCKET"
echo "  S3 Key:      $S3_KEY"
echo "  BTRFS Mount: $BTRFS_MOUNT_DIR"
echo ""

mkdir -p "$TEST_WORK_DIR"
cd "$TEST_WORK_DIR"

# Step 1: Create files totaling ~10 MiB of random data
echo "Step 1: Creating test files (10 MiB total random data)..."
mkdir -p input
create_test_file "input/file1.bin" $((4 * 1024 * 1024))  # 4 MiB
create_test_file "input/file2.bin" $((3 * 1024 * 1024))  # 3 MiB
create_test_file "input/file3.bin" $((3 * 1024 * 1024))  # 3 MiB
echo -e "${GREEN}✓${NC} Created 3 files (10 MiB total)"

store_checksums "input" "checksums_original.txt"
echo -e "${GREEN}✓${NC} Stored checksums"

# Step 2: Create archive
echo ""
echo "Step 2: Creating BURST archive..."
"$BURST_WRITER" -l 3 -o test.zip input/*.bin
archive_size=$(stat -c%s "test.zip")
part_count=$(( (archive_size + 8388607) / 8388608 ))
echo -e "${GREEN}✓${NC} Created archive: $archive_size bytes ($part_count parts)"

# Verify it spans 2+ parts
if [ $part_count -lt 2 ]; then
    echo -e "${YELLOW}Warning: Expected 2+ parts, got $part_count${NC}"
fi

# Verify archive
if ! verify_archive "test.zip"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} Archive valid (7zz t)"

# Step 3: Verify alignment (critical for 2-part archive)
echo ""
echo "Step 3: Verifying 8 MiB boundary alignment..."
if ! verify_alignment "test.zip"; then
    echo -e "${RED}✗ Alignment verification failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} Alignment verified"

# Step 4: Upload
echo ""
echo "Step 4: Uploading to S3..."
if ! upload_to_s3 "test.zip" "$S3_KEY"; then
    exit 1
fi
S3_UPLOADED="yes"

# Step 5: Download and extract
echo ""
echo "Step 5: Downloading and extracting ($part_count parts)..."
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

# Step 6: Verify
echo ""
echo "Step 6: Verifying checksums..."
if ! verify_checksums "$EXTRACT_DIR" "checksums_original.txt"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} All checksums match"

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
