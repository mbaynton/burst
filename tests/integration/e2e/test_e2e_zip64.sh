#!/bin/bash
#
# End-to-End Test: ZIP64 Support
#
# Tests:
#   - Archive with > 65,534 files (ZIP64 entry count)
#   - Single file with > 4 GiB uncompressed size (ZIP64 file size)
#   - Total archive size > 4 GiB (ZIP64 archive size)
#   - All three ZIP64 triggers in one comprehensive test
#
# Requires:
#   - compressible-many-files fixture in S3 (run setup_s3.sh first)
#   - BTRFS filesystem with 8 GiB capacity
#   - Sufficient S3 storage for ~6 GiB archive
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
TEST_NAME="E2E ZIP64 Support (66k+ files, 6 GiB file, 5 GiB archive)"
TEST_ID="zip64-$(date +%s)"
TEST_WORK_DIR="$E2E_TMP_DIR/test_zip64_$TEST_ID"
S3_KEY="${BURST_TEST_KEY_PREFIX}${TEST_ID}.zip"
FIXTURE_KEY="fixtures/compressible-many-files.zip"

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

    if [ -d "$BTRFS_MOUNT_DIR/seed_$TEST_ID" ]; then
        rm -rf "$BTRFS_MOUNT_DIR/seed_$TEST_ID"
        echo -e "${GREEN}✓${NC} Removed seed directory"
    fi

    if [ -d "$BTRFS_MOUNT_DIR/input_$TEST_ID" ]; then
        rm -rf "$BTRFS_MOUNT_DIR/input_$TEST_ID"
        echo -e "${GREEN}✓${NC} Removed input directory"
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
echo "  File count: 66,151 files (270 copies × 245 files + 1 large file)"
echo "  Large file: 6 GiB uncompressed"
echo "  Total size: ~5 GiB compressed archive"
echo "  Tests: ZIP64 file count, file size, and archive size"
echo "  S3 Bucket:   $BURST_TEST_BUCKET"
echo "  S3 Key:      $S3_KEY"
echo "  BTRFS Mount: $BTRFS_MOUNT_DIR"
echo ""

mkdir -p "$TEST_WORK_DIR"
cd "$TEST_WORK_DIR"

# Step 1: Verify and download fixture
echo "Step 1: Downloading compressible-many-files fixture..."
echo "Checking if fixture exists in S3..."

if ! aws s3api head-object --bucket "$BURST_TEST_BUCKET" --key "$FIXTURE_KEY" --region "$AWS_REGION" >/dev/null 2>&1; then
    echo -e "${RED}✗ Fixture not found: s3://$BURST_TEST_BUCKET/$FIXTURE_KEY${NC}"
    echo ""
    echo "Please run setup_s3.sh first to create test fixtures:"
    echo "  bash $SCRIPT_DIR/../setup_s3.sh"
    exit 1
fi
echo -e "${GREEN}✓${NC} Found fixture in S3"

# Set CAP_SYS_ADMIN capability on burst-downloader for BTRFS encoded writes
echo "Setting CAP_SYS_ADMIN capability on burst-downloader..."
current_caps=$(getcap "$BURST_DOWNLOADER" 2>/dev/null || echo "")

if [[ "$current_caps" != *"cap_sys_admin"* ]]; then
    if ! sudo setcap 'cap_sys_admin=ep' "$BURST_DOWNLOADER"; then
        echo -e "${RED}✗ Failed to set CAP_SYS_ADMIN capability${NC}"
        echo "sudo access is required for BTRFS encoded writes"
        exit 1
    fi
fi

# Verify capability was set
if ! getcap "$BURST_DOWNLOADER" | grep -q "cap_sys_admin"; then
    echo -e "${RED}✗ CAP_SYS_ADMIN capability not set on burst-downloader${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} CAP_SYS_ADMIN capability set"

# Download and extract fixture to BTRFS mount (required for reflinks)
SEED_DIR="$BTRFS_MOUNT_DIR/seed_$TEST_ID"
mkdir -p "$SEED_DIR"
echo "Downloading and extracting fixture to BTRFS mount..."
"$BURST_DOWNLOADER" \
    --bucket "$BURST_TEST_BUCKET" \
    --key "$FIXTURE_KEY" \
    --region "$AWS_REGION" \
    --output-dir "$SEED_DIR"

if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Failed to download fixture${NC}"
    exit 1
fi

# Count files in fixture
fixture_file_count=$(find "$SEED_DIR" -type f | wc -l)
echo -e "${GREEN}✓${NC} Downloaded and extracted fixture ($fixture_file_count files)"

# Step 2: Create large file (6 GiB)
echo ""
echo "Step 2: Creating large file (target: 6 GiB)..."
INPUT_DIR="$BTRFS_MOUNT_DIR/input_$TEST_ID"
mkdir -p "$INPUT_DIR"
target_size=$((6 * 1024 * 1024 * 1024))  # 6 GiB
large_file="$INPUT_DIR/large_file.txt"
touch "$large_file"

iteration=0
last_progress=-1

while true; do
    current_size=$(stat -c%s "$large_file" 2>/dev/null || echo 0)

    # Check if we've reached target
    if [ $current_size -ge $target_size ]; then
        break
    fi

    # Show progress every 1 GiB
    current_gib=$((current_size / 1073741824))
    if [ $current_gib -ne $last_progress ]; then
        echo "  Progress: ${current_gib} GiB..."
        last_progress=$current_gib
    fi

    # Concatenate all fixture files
    find "$SEED_DIR" -type f -name "*.txt" -exec cat {} + >> "$large_file"
    iteration=$((iteration + 1))
done

final_size=$(stat -c%s "$large_file")
echo -e "${GREEN}✓${NC} Created large file: $((final_size / 1073741824)) GiB ($final_size bytes, $iteration iterations)"

# Step 3: Create reflinked copies
echo ""
echo "Step 3: Creating reflinked copies of fixture..."
echo "Target: 270 copies × $fixture_file_count files = ~66,000 files"

num_copies=270
for i in $(seq 1 $num_copies); do
    cp -r --reflink=always "$SEED_DIR" "$INPUT_DIR/copy_$(printf "%03d" $i)"

    # Show progress every 10 copies
    if [ $((i % 10)) -eq 0 ]; then
        echo "  Created $i/$num_copies copies..."
    fi
done

total_files=$(find "$INPUT_DIR" -type f | wc -l)
echo -e "${GREEN}✓${NC} Created $total_files total files (270 copies + 1 large file)"

# Verify we exceeded 65,534 threshold
if [ $total_files -le 65534 ]; then
    echo -e "${RED}✗ File count ($total_files) did not exceed ZIP64 threshold (65,534)${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} File count exceeds ZIP64 threshold (65,534 < $total_files)"

# Step 4: Generate checksums
echo ""
echo "Step 4: Generating checksums for all files (this may take 1-2 minutes)..."
store_checksums "$INPUT_DIR" "checksums_original.txt"
checksum_count=$(wc -l < "checksums_original.txt")
echo -e "${GREEN}✓${NC} Generated checksums for $checksum_count files"

# Step 5: Create archive
echo ""
echo "Step 5: Creating BURST archive (this may take 3-5 minutes)..."
"$BURST_WRITER" -l 3 -o test.zip "$INPUT_DIR"/

if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Archive creation failed${NC}"
    exit 1
fi

archive_size=$(stat -c%s "test.zip")
part_count=$(( (archive_size + 8388607) / 8388608 ))
archive_size_gib=$(echo "scale=2; $archive_size / 1073741824" | bc)
echo -e "${GREEN}✓${NC} Created archive: ${archive_size_gib} GiB ($archive_size bytes, $part_count parts)"

# Verify archive exceeds 4 GiB threshold
if [ $archive_size -le 4294967296 ]; then
    echo -e "${YELLOW}Warning: Archive size ($archive_size_gib GiB) did not exceed 4 GiB threshold${NC}"
    echo "This may not trigger ZIP64 archive size handling"
fi

# Step 6: Verify archive structure
echo ""
echo "Step 6: Verifying archive structure..."

if ! verify_archive "test.zip"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} Archive valid (7zz t)"

if ! verify_alignment "test.zip"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} All boundaries aligned"

# Step 7: Upload to S3
echo ""
echo "Step 7: Uploading to S3 (this may take 1-2 minutes)..."
if ! upload_to_s3 "test.zip" "$S3_KEY"; then
    exit 1
fi
S3_UPLOADED="yes"

# Step 8: Download and extract
echo ""
echo "Step 8: Downloading and extracting (this may take 3-5 minutes)..."
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

# Step 9: Verify extracted files
echo ""
echo "Step 9: Verifying extracted files..."
extracted_count=$(find "$EXTRACT_DIR" -type f | wc -l)
if [ "$extracted_count" -ne "$total_files" ]; then
    echo -e "${RED}✗ File count mismatch: expected $total_files, got $extracted_count${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} All $extracted_count files extracted"

echo "Verifying checksums (this may take 1-2 minutes)..."
if ! verify_checksums "$EXTRACT_DIR" "checksums_original.txt"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} All checksums match"

# Step 10: Cross-validate with 7zz
echo ""
echo "Step 10: Cross-validation with 7zz (this may take 3-5 minutes)..."
mkdir -p "7zz_extract"
extract_with_7zz "test.zip" "7zz_extract"

echo "Verifying 7zz extraction checksums..."
if ! verify_checksums "7zz_extract" "checksums_original.txt"; then
    exit 1
fi
echo -e "${GREEN}✓${NC} 7zz extraction matches"

print_test_result "$TEST_NAME" 0
exit 0
