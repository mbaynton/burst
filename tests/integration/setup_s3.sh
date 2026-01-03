#!/bin/bash
#
# One-time S3 bucket preparation script for BURST integration tests
#
# Creates read-only test fixtures in the configured S3 bucket:
#   - fixtures/small.zip   (< 8 MiB, single part)
#   - fixtures/medium.zip  (8-16 MiB, 2 parts)
#   - fixtures/large.zip   (~20 MiB, 3 parts)
#   - fixtures/compressible-many-files.zip (>= 10 MiB, compressible text data)
#   - fixtures/large-cd.zip (~170k files, central directory > 8 MiB)
#
# Each archive has a corresponding .sha256 file with checksums of all files.
#
# Usage:
#   ./setup_s3.sh
#
# Prerequisites:
#   - burst-writer built in build/
#   - aws CLI configured with appropriate credentials
#   - 7zz installed for archive validation
#   - python3 for alignment verification
#   - .env file with BURST_TEST_BUCKET and AWS_REGION
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
BURST_WRITER="$BUILD_DIR/burst-writer"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Load configuration from .env
if [ -f "$PROJECT_ROOT/.env" ]; then
    echo "Loading configuration from .env..."
    # shellcheck source=/dev/null
    source "$PROJECT_ROOT/.env"
fi

# Set defaults
: "${BURST_TEST_BUCKET:=burst-integration-tests}"
: "${AWS_REGION:=us-east-1}"

S3_PREFIX="fixtures"

echo ""
echo -e "${BLUE}============================================${NC}"
echo -e "${BLUE}BURST S3 Test Fixture Setup${NC}"
echo -e "${BLUE}============================================${NC}"
echo ""
echo "Configuration:"
echo "  S3 Bucket: $BURST_TEST_BUCKET"
echo "  S3 Prefix: $S3_PREFIX"
echo "  AWS Region: $AWS_REGION"
echo ""

# Check prerequisites
check_prerequisites() {
    local missing=0

    if [ ! -f "$BURST_WRITER" ]; then
        echo -e "${RED}✗ Error: burst-writer not found at $BURST_WRITER${NC}"
        echo "  Run: cd $BUILD_DIR && make burst-writer"
        missing=1
    fi

    if ! command -v aws &> /dev/null; then
        echo -e "${RED}✗ Error: aws CLI not found${NC}"
        echo "  Install: pip install awscli"
        missing=1
    fi

    if ! command -v 7zz &> /dev/null; then
        echo -e "${RED}✗ Error: 7zz not found${NC}"
        echo "  Required for archive validation"
        missing=1
    fi

    if ! command -v python3 &> /dev/null; then
        echo -e "${RED}✗ Error: python3 not found${NC}"
        missing=1
    fi

    if [ $missing -eq 1 ]; then
        return 1
    fi

    echo -e "${GREEN}✓${NC} All prerequisites met"
    return 0
}

# Create test file with random data
# Usage: create_test_file <filename> <size_in_bytes>
create_test_file() {
    local filename="$1"
    local size="$2"

    dd if=/dev/urandom of="$filename" bs=1M count=$((size / 1048576)) 2>/dev/null
    local remainder=$((size % 1048576))
    if [ "$remainder" -gt 0 ]; then
        dd if=/dev/urandom of="$filename" bs=1 count="$remainder" oflag=append conv=notrunc 2>/dev/null
    fi
}

# Generate sha256sum manifest for a directory
# Usage: generate_checksums <input_dir> <output_file>
generate_checksums() {
    local input_dir="$1"
    local output_file="$2"

    # Generate checksums with relative paths
    (cd "$input_dir" && find . -type f | sort | while read -r file; do
        # Remove leading ./
        local relpath="${file#./}"
        sha256sum "$relpath"
    done) > "$output_file"
}

# Create test file with compressible data (random words)
# Usage: create_compressible_file <filename> <size_in_bytes>
create_compressible_file() {
    local filename="$1"
    local target_size="$2"
    > "$filename"
    while [ "$(stat -c%s "$filename")" -lt "$target_size" ]; do
        shuf -n 100 /usr/share/dict/words | tr '\n' ' ' >> "$filename"
    done
    truncate -s "$target_size" "$filename"
}

# Generate random lowercase string
# Usage: random_string <length>
random_string() {
    local length="$1"
    tr -dc 'a-z' < /dev/urandom | head -c "$length"
}

# Generate random size between 1 KiB and 1 MiB
random_size() {
    echo $(( (RANDOM * 32768 + RANDOM) % (1048576 - 1024) + 1024 ))
}

# Create and upload a fixture archive
# Usage: create_fixture <name> <num_files> <file_size_bytes>
create_fixture() {
    local name="$1"
    local num_files="$2"
    local file_size="$3"

    echo ""
    echo -e "${BLUE}--- Creating $name fixture ---${NC}"

    local work_dir="$TEMP_DIR/$name"
    local input_dir="$work_dir/input"
    local archive_file="$work_dir/$name.zip"
    local checksum_file="$work_dir/$name.sha256"

    mkdir -p "$input_dir"

    # Create test files
    echo "Creating $num_files files of $((file_size / 1024)) KiB each..."
    for i in $(seq 1 "$num_files"); do
        create_test_file "$input_dir/file_$(printf "%03d" "$i").bin" "$file_size"
    done

    local total_size
    total_size=$(du -sb "$input_dir" | awk '{print $1}')
    echo -e "${GREEN}✓${NC} Created $num_files files (~$((total_size / 1024 / 1024)) MiB total)"

    # Generate checksums (saved OUTSIDE input dir)
    echo "Generating checksums..."
    generate_checksums "$input_dir" "$checksum_file"
    echo -e "${GREEN}✓${NC} Generated checksum manifest"

    # Create BURST archive
    echo "Creating BURST archive..."
    "$BURST_WRITER" -l 3 -o "$archive_file" "$input_dir"

    local archive_size
    archive_size=$(stat -c%s "$archive_file")
    local part_count=$(( (archive_size + 8388607) / 8388608 ))
    echo -e "${GREEN}✓${NC} Created archive: $archive_size bytes ($part_count parts)"

    # Verify archive with 7zz
    echo "Verifying archive with 7zz..."
    if ! 7zz t "$archive_file" 2>&1 | grep -q "Everything is Ok"; then
        echo -e "${RED}✗ Archive validation failed${NC}"
        7zz t "$archive_file"
        return 1
    fi
    echo -e "${GREEN}✓${NC} Archive validation passed (7zz t)"

    # Verify alignment
    echo "Verifying alignment..."
    if ! python3 "$SCRIPT_DIR/verify_alignment.py" "$archive_file" >/dev/null 2>&1; then
        echo -e "${RED}✗ Alignment verification failed${NC}"
        python3 "$SCRIPT_DIR/verify_alignment.py" "$archive_file"
        return 1
    fi
    echo -e "${GREEN}✓${NC} Alignment verification passed"

    # Upload archive to S3
    echo "Uploading archive to S3..."
    if ! aws s3 cp "$archive_file" "s3://$BURST_TEST_BUCKET/$S3_PREFIX/$name.zip" --region "$AWS_REGION" >/dev/null; then
        echo -e "${RED}✗ Failed to upload archive${NC}"
        return 1
    fi
    echo -e "${GREEN}✓${NC} Uploaded s3://$BURST_TEST_BUCKET/$S3_PREFIX/$name.zip"

    # Upload checksums to S3
    echo "Uploading checksums to S3..."
    if ! aws s3 cp "$checksum_file" "s3://$BURST_TEST_BUCKET/$S3_PREFIX/$name.sha256" --region "$AWS_REGION" >/dev/null; then
        echo -e "${RED}✗ Failed to upload checksums${NC}"
        return 1
    fi
    echo -e "${GREEN}✓${NC} Uploaded s3://$BURST_TEST_BUCKET/$S3_PREFIX/$name.sha256"

    echo -e "${GREEN}✓ $name fixture complete${NC}"
}

# Create compressible fixture with variable file sizes and nested directories
# Usage: create_compressible_fixture <name> <min_archive_size_bytes>
create_compressible_fixture() {
    local name="$1"
    local min_archive_size="$2"

    echo ""
    echo -e "${BLUE}--- Creating $name fixture ---${NC}"

    local work_dir="$TEMP_DIR/$name"
    local input_dir="$work_dir/input"
    local archive_file="$work_dir/$name.zip"
    local checksum_file="$work_dir/$name.sha256"

    mkdir -p "$input_dir"

    local target_uncompressed=$((min_archive_size * 3))  # Assume ~3:1 compression
    local uncompressed_total=0
    local file_count=0

    while true; do
        # Add files until target uncompressed size
        echo "Creating compressible files..."
        while [ "$uncompressed_total" -lt "$target_uncompressed" ]; do
            local dir1=$(random_string 3)
            local dir2=$(random_string 3)
            local subdir="$input_dir/$dir1/$dir2"
            mkdir -p "$subdir"

            local file_size=$(random_size)
            file_count=$((file_count + 1))
            local filename=$(printf "file_%04d.txt" "$file_count")
            create_compressible_file "$subdir/$filename" "$file_size"
            uncompressed_total=$((uncompressed_total + file_size))
        done

        echo -e "${GREEN}✓${NC} Created $file_count files (~$((uncompressed_total / 1024 / 1024)) MiB uncompressed)"

        # Generate checksums (saved OUTSIDE input dir)
        echo "Generating checksums..."
        generate_checksums "$input_dir" "$checksum_file"
        echo -e "${GREEN}✓${NC} Generated checksum manifest"

        # Create BURST archive
        echo "Creating BURST archive..."
        "$BURST_WRITER" -l 3 -o "$archive_file" "$input_dir"

        local archive_size=$(stat -c%s "$archive_file")
        if [ "$archive_size" -ge "$min_archive_size" ]; then
            local part_count=$(( (archive_size + 8388607) / 8388608 ))
            echo -e "${GREEN}✓${NC} Created archive: $archive_size bytes ($part_count parts)"
            break
        else
            echo "Archive only $((archive_size / 1024 / 1024)) MiB, adding more files..."
            rm "$archive_file" "$checksum_file"
            target_uncompressed=$((target_uncompressed + min_archive_size / 2))
        fi
    done

    # Verify archive with 7zz
    echo "Verifying archive with 7zz..."
    if ! 7zz t "$archive_file" 2>&1 | grep -q "Everything is Ok"; then
        echo -e "${RED}✗ Archive validation failed${NC}"
        7zz t "$archive_file"
        return 1
    fi
    echo -e "${GREEN}✓${NC} Archive validation passed (7zz t)"

    # Verify alignment
    echo "Verifying alignment..."
    if ! python3 "$SCRIPT_DIR/verify_alignment.py" "$archive_file" >/dev/null 2>&1; then
        echo -e "${RED}✗ Alignment verification failed${NC}"
        python3 "$SCRIPT_DIR/verify_alignment.py" "$archive_file"
        return 1
    fi
    echo -e "${GREEN}✓${NC} Alignment verification passed"

    # Upload archive to S3
    echo "Uploading archive to S3..."
    if ! aws s3 cp "$archive_file" "s3://$BURST_TEST_BUCKET/$S3_PREFIX/$name.zip" --region "$AWS_REGION" >/dev/null; then
        echo -e "${RED}✗ Failed to upload archive${NC}"
        return 1
    fi
    echo -e "${GREEN}✓${NC} Uploaded s3://$BURST_TEST_BUCKET/$S3_PREFIX/$name.zip"

    # Upload checksums to S3
    echo "Uploading checksums to S3..."
    if ! aws s3 cp "$checksum_file" "s3://$BURST_TEST_BUCKET/$S3_PREFIX/$name.sha256" --region "$AWS_REGION" >/dev/null; then
        echo -e "${RED}✗ Failed to upload checksums${NC}"
        return 1
    fi
    echo -e "${GREEN}✓${NC} Uploaded s3://$BURST_TEST_BUCKET/$S3_PREFIX/$name.sha256"

    echo -e "${GREEN}✓ $name fixture complete${NC}"
}

# Create fixture with many tiny files to generate a large central directory
# The goal is to create a CD > 8 MiB to test two-phase CD fetching.
# Each CD entry is ~46 bytes + filename length. With short filenames (~15 chars),
# each entry is ~61 bytes. For a 10 MiB CD, we need ~170k files.
# Usage: create_large_cd_fixture <name> <target_cd_size_bytes>
create_large_cd_fixture() {
    local name="$1"
    local target_cd_size="$2"

    echo ""
    echo -e "${BLUE}--- Creating $name fixture (large central directory) ---${NC}"

    local work_dir="$TEMP_DIR/$name"
    local input_dir="$work_dir/input"
    local archive_file="$work_dir/$name.zip"
    local checksum_file="$work_dir/$name.sha256"

    mkdir -p "$input_dir"

    # Calculate number of files needed
    # CD entry overhead: 46 bytes fixed + filename length
    # Average filename: "dXXX/fXXXXXX.txt" = 17 chars
    # Total per entry: ~63 bytes
    local bytes_per_entry=63
    local num_files=$((target_cd_size / bytes_per_entry))

    echo "Creating $num_files tiny files to generate ~$((target_cd_size / 1024 / 1024)) MiB central directory..."
    echo "This may take a few minutes..."

    # Create files in subdirectories to avoid filesystem limits on directory entries
    # Use 1000 subdirs with files distributed among them
    local num_dirs=1000
    local files_per_dir=$((num_files / num_dirs + 1))

    for dir_idx in $(seq 0 $((num_dirs - 1))); do
        local subdir="$input_dir/d$(printf "%03d" "$dir_idx")"
        mkdir -p "$subdir"

        for file_idx in $(seq 0 $((files_per_dir - 1))); do
            local global_idx=$((dir_idx * files_per_dir + file_idx))
            if [ "$global_idx" -ge "$num_files" ]; then
                break 2
            fi

            # Create tiny file with just a few bytes (file number as content)
            local filename="$subdir/f$(printf "%06d" "$file_idx").txt"
            echo "$global_idx" > "$filename"
        done

        # Progress indicator every 100 directories
        if [ $((dir_idx % 100)) -eq 0 ]; then
            echo "  Created files in $((dir_idx + 1))/$num_dirs directories..."
        fi
    done

    local actual_file_count
    actual_file_count=$(find "$input_dir" -type f | wc -l)
    local total_size
    total_size=$(du -sb "$input_dir" | awk '{print $1}')
    echo -e "${GREEN}✓${NC} Created $actual_file_count files (~$((total_size / 1024 / 1024)) MiB total data)"

    # Generate checksums (saved OUTSIDE input dir)
    echo "Generating checksums (this may take a while for many files)..."
    generate_checksums "$input_dir" "$checksum_file"
    echo -e "${GREEN}✓${NC} Generated checksum manifest"

    # Create BURST archive
    echo "Creating BURST archive..."
    "$BURST_WRITER" -l 3 -o "$archive_file" "$input_dir"

    local archive_size
    archive_size=$(stat -c%s "$archive_file")
    local part_count=$(( (archive_size + 8388607) / 8388608 ))
    echo -e "${GREEN}✓${NC} Created archive: $archive_size bytes ($part_count parts)"

    # Check actual CD size using zipinfo
    echo "Checking central directory size..."
    local cd_info
    cd_info=$(zipinfo -v "$archive_file" 2>/dev/null | grep -E "(central directory|End-of-central)" | head -5 || true)
    echo "$cd_info"

    # Verify archive with 7zz
    echo "Verifying archive with 7zz..."
    if ! 7zz t "$archive_file" 2>&1 | grep -q "Everything is Ok"; then
        echo -e "${RED}✗ Archive validation failed${NC}"
        7zz t "$archive_file"
        return 1
    fi
    echo -e "${GREEN}✓${NC} Archive validation passed (7zz t)"

    # Verify alignment
    echo "Verifying alignment..."
    if ! python3 "$SCRIPT_DIR/verify_alignment.py" "$archive_file" >/dev/null 2>&1; then
        echo -e "${RED}✗ Alignment verification failed${NC}"
        python3 "$SCRIPT_DIR/verify_alignment.py" "$archive_file"
        return 1
    fi
    echo -e "${GREEN}✓${NC} Alignment verification passed"

    # Upload archive to S3
    echo "Uploading archive to S3..."
    if ! aws s3 cp "$archive_file" "s3://$BURST_TEST_BUCKET/$S3_PREFIX/$name.zip" --region "$AWS_REGION" >/dev/null; then
        echo -e "${RED}✗ Failed to upload archive${NC}"
        return 1
    fi
    echo -e "${GREEN}✓${NC} Uploaded s3://$BURST_TEST_BUCKET/$S3_PREFIX/$name.zip"

    # Upload checksums to S3
    echo "Uploading checksums to S3..."
    if ! aws s3 cp "$checksum_file" "s3://$BURST_TEST_BUCKET/$S3_PREFIX/$name.sha256" --region "$AWS_REGION" >/dev/null; then
        echo -e "${RED}✗ Failed to upload checksums${NC}"
        return 1
    fi
    echo -e "${GREEN}✓${NC} Uploaded s3://$BURST_TEST_BUCKET/$S3_PREFIX/$name.sha256"

    echo -e "${GREEN}✓ $name fixture complete${NC}"
}

# Main execution
if ! check_prerequisites; then
    exit 1
fi

# Create temporary working directory
TEMP_DIR=$(mktemp -d)
echo ""
echo "Working directory: $TEMP_DIR"

# Cleanup on exit
cleanup() {
    echo ""
    echo "Cleaning up temporary files..."
    rm -rf "$TEMP_DIR"
    echo -e "${GREEN}✓${NC} Cleanup complete"
}
trap cleanup EXIT

# Create fixtures
# small: < 8 MiB (single part) - 15 files x 500 KiB = 7.5 MiB
create_fixture "small" 15 $((500 * 1024))

# medium: 8-16 MiB (2 parts) - 25 files x 500 KiB = 12.5 MiB
create_fixture "medium" 25 $((500 * 1024))

# large: ~20 MiB (3 parts) - 40 files x 500 KiB = 20 MiB
create_fixture "large" 40 $((500 * 1024))

# compressible-many-files: many small files with compressible text data, min 40 MiB archive
# (large enough to test 16 MiB part size with multiple concurrent parts)
create_compressible_fixture "compressible-many-files" $((40 * 1024 * 1024))

# large-cd: archive with many files to create a central directory > 8 MiB
# This tests the two-phase CD fetch for archives where the CD doesn't fit in
# the initial 8 MiB tail buffer. Target: ~10 MiB CD (requires ~100k files)
create_large_cd_fixture "large-cd" $((10 * 1024 * 1024))

# Print summary
echo ""
echo -e "${BLUE}============================================${NC}"
echo -e "${GREEN}S3 Test Fixtures Created Successfully${NC}"
echo -e "${BLUE}============================================${NC}"
echo ""
echo "Fixtures available at:"
echo "  s3://$BURST_TEST_BUCKET/$S3_PREFIX/small.zip"
echo "  s3://$BURST_TEST_BUCKET/$S3_PREFIX/small.sha256"
echo "  s3://$BURST_TEST_BUCKET/$S3_PREFIX/medium.zip"
echo "  s3://$BURST_TEST_BUCKET/$S3_PREFIX/medium.sha256"
echo "  s3://$BURST_TEST_BUCKET/$S3_PREFIX/large.zip"
echo "  s3://$BURST_TEST_BUCKET/$S3_PREFIX/large.sha256"
echo "  s3://$BURST_TEST_BUCKET/$S3_PREFIX/compressible-many-files.zip"
echo "  s3://$BURST_TEST_BUCKET/$S3_PREFIX/compressible-many-files.sha256"
echo "  s3://$BURST_TEST_BUCKET/$S3_PREFIX/large-cd.zip"
echo "  s3://$BURST_TEST_BUCKET/$S3_PREFIX/large-cd.sha256"
echo ""
echo "These fixtures are persistent and shared across test runs."
echo "Re-run this script only if you need to regenerate them."
echo ""
