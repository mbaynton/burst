#!/bin/bash
#
# End-to-End Test 4: Archive with Many Small Files
#
# Tests:
#   - Many small files (60) across multiple parts
#   - File-per-part mapping in central directory
#   - Sequential file extraction within parts
#   - Part coordination across 3+ parts
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
TEST_NAME="E2E Many Small Files (60 files, ~20 MiB)"
TEST_ID="many-$(date +%s)"
TEST_WORK_DIR="$E2E_TMP_DIR/test_many_$TEST_ID"
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
echo "  File count: 60 files"
echo "  Total size: ~20 MiB (3 parts)"
echo "  Tests: Many-file coordination"
echo "  S3 Bucket:   $BURST_TEST_BUCKET"
echo "  S3 Key:      $S3_KEY"
echo "  BTRFS Mount: $BTRFS_MOUNT_DIR"
echo ""

mkdir -p "$TEST_WORK_DIR"
cd "$TEST_WORK_DIR"

# Step 1: Create many small files
echo "Step 1: Creating many small files..."
mkdir -p input

NUM_FILES=60
FILE_SIZE=$((350 * 1024))  # ~350 KiB each = ~21 MiB total

for i in $(seq 1 $NUM_FILES); do
    create_test_file "input/file_$(printf "%03d" "$i").bin" $FILE_SIZE
    # Show progress every 10 files
    if [ $((i % 10)) -eq 0 ]; then
        echo "  Created $i/$NUM_FILES files..."
    fi
done

total_size=$(du -sb input | awk '{print $1}')
echo -e "${GREEN}✓${NC} Created $NUM_FILES files (~$((total_size / 1024 / 1024)) MiB total random data)"

store_checksums "input" "checksums_original.txt"
echo -e "${GREEN}✓${NC} Stored checksums for $NUM_FILES files"

# Step 2: Create archive
echo ""
echo "Step 2: Creating BURST archive..."
"$BURST_WRITER" -l 3 -o test.zip input/*.bin
archive_size=$(stat -c%s "test.zip")
part_count=$(( (archive_size + 8388607) / 8388608 ))
echo -e "${GREEN}✓${NC} Created archive: $archive_size bytes ($part_count parts)"

# Verify part count
if [ $part_count -lt 2 ]; then
    echo -e "${YELLOW}Warning: Expected 2+ parts, got $part_count${NC}"
fi

# Verify archive
if ! verify_archive "test.zip"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} Archive valid (7zz t)"

# Step 3: Verify alignment
echo ""
echo "Step 3: Verifying alignment..."
if ! verify_alignment "test.zip"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} All boundaries aligned"

# Step 4: Upload
echo ""
echo "Step 4: Uploading to S3..."
if ! upload_to_s3 "test.zip" "$S3_KEY"; then
    exit 1
fi
S3_UPLOADED="yes"

# Step 5: Create pre-existing files with larger sizes (tests ftruncate behavior)
echo ""
echo "Step 5: Creating pre-existing files (larger than archive contents)..."
EXTRACT_DIR="$BTRFS_MOUNT_DIR/extracted_$TEST_ID"
mkdir -p "$EXTRACT_DIR"

# Create files 50% larger than originals, filled with 0xFF bytes
# These must be correctly truncated and overwritten during extraction
for i in $(seq 1 $NUM_FILES); do
    filename="$EXTRACT_DIR/file_$(printf "%03d" "$i").bin"
    larger_size=$(( FILE_SIZE + FILE_SIZE / 2 ))
    dd if=/dev/zero bs=1 count="$larger_size" 2>/dev/null | tr '\0' '\377' > "$filename"
done
echo -e "${GREEN}✓${NC} Created $NUM_FILES pre-existing files (each 50% larger, filled with 0xFF)"

# Step 6: Download and extract
echo ""
echo "Step 6: Downloading and extracting ($part_count parts, $NUM_FILES files)..."

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

# Step 7: Verify all files
echo ""
echo "Step 7: Verifying all $NUM_FILES files..."
extracted_count=$(find "$EXTRACT_DIR" -type f | wc -l)
if [ "$extracted_count" -ne "$NUM_FILES" ]; then
    echo -e "${RED}✗ File count mismatch: expected $NUM_FILES, got $extracted_count${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} All $NUM_FILES files extracted"

if ! verify_checksums "$EXTRACT_DIR" "checksums_original.txt"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} All checksums match"

# Step 8: Cross-validate with 7zz
echo ""
echo "Step 8: Cross-validation with 7zz..."
mkdir -p "7zz_extract"
extract_with_7zz "test.zip" "7zz_extract"
if ! verify_checksums "7zz_extract" "checksums_original.txt"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} 7zz extraction matches"

print_test_result "$TEST_NAME" 0
exit 0
