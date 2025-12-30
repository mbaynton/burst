#!/bin/bash
#
# Common functions for BURST end-to-end integration tests
#
# This file should be sourced by test scripts:
#   source "$SCRIPT_DIR/e2e_common.sh"
#

# Ensure this file is only sourced once
if [ -n "$E2E_COMMON_LOADED" ]; then
    return 0
fi
E2E_COMMON_LOADED=1

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Determine project directories
E2E_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
E2E_PROJECT_ROOT="$(cd "$E2E_SCRIPT_DIR/../.." && pwd)"
E2E_BUILD_DIR="$E2E_PROJECT_ROOT/build"
E2E_TMP_DIR="$E2E_PROJECT_ROOT/tests/tmp/e2e_test_files"

# Binary paths
BURST_WRITER="$E2E_BUILD_DIR/burst-writer"
BURST_DOWNLOADER="$E2E_BUILD_DIR/burst-downloader"

# S3 Configuration (loaded from .env if available)
load_s3_config() {
    if [ -f "$E2E_PROJECT_ROOT/.env" ]; then
        # shellcheck source=/dev/null
        source "$E2E_PROJECT_ROOT/.env"
    fi

    # Set defaults
    : "${BURST_TEST_BUCKET:=burst-integration-tests}"
    : "${AWS_REGION:=us-east-1}"
    : "${BURST_TEST_KEY_PREFIX:=e2e-tests/}"

    export BURST_TEST_BUCKET
    export AWS_REGION
    export BURST_TEST_KEY_PREFIX
}

# Check prerequisites
check_prerequisites() {
    local missing=0

    # Check burst-writer
    if [ ! -f "$BURST_WRITER" ]; then
        echo -e "${RED}✗ Error: burst-writer not found at $BURST_WRITER${NC}"
        echo "  Run: cd $E2E_BUILD_DIR && make burst-writer"
        missing=1
    fi

    # Check burst-downloader
    if [ ! -f "$BURST_DOWNLOADER" ]; then
        echo -e "${RED}✗ Error: burst-downloader not found at $BURST_DOWNLOADER${NC}"
        echo "  Run: cd $E2E_BUILD_DIR && make burst-downloader"
        missing=1
    fi

    # Check aws CLI
    if ! command -v aws &> /dev/null; then
        echo -e "${RED}✗ Error: aws CLI not found${NC}"
        echo "  Install: pip install awscli"
        missing=1
    fi

    # Check 7zz
    if ! command -v 7zz &> /dev/null; then
        echo -e "${RED}✗ Error: 7zz not found${NC}"
        echo "  Required for archive validation"
        missing=1
    fi

    # Check Python 3
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

# Create test file of specific size with random data
# Usage: create_test_file <filename> <size_in_bytes>
create_test_file() {
    local filename="$1"
    local size="$2"

    # Use dd with /dev/urandom for random data (ensures low compressibility)
    dd if=/dev/urandom of="$filename" bs=1M count=$((size / 1048576)) 2>/dev/null

    # Handle remainder if size is not a multiple of 1 MiB
    local remainder=$((size % 1048576))
    if [ "$remainder" -gt 0 ]; then
        dd if=/dev/urandom of="$filename" bs=1 count="$remainder" oflag=append conv=notrunc 2>/dev/null
    fi
}

# Calculate SHA256 checksum
# Usage: checksum=$(get_checksum <file>)
get_checksum() {
    sha256sum "$1" | awk '{print $1}'
}

# Store checksums for files in directory
# Usage: store_checksums <dir> <output_file>
store_checksums() {
    local dir="$1"
    local output="$2"

    find "$dir" -type f | sort | while read -r file; do
        local relpath="${file#$dir/}"
        local checksum
        checksum=$(get_checksum "$file")
        echo "$checksum  $relpath"
    done > "$output"
}

# Verify checksums match
# Usage: verify_checksums <dir> <checksum_file>
verify_checksums() {
    local dir="$1"
    local checksum_file="$2"

    local failed=0
    while IFS= read -r line; do
        local expected_sum
        expected_sum=$(echo "$line" | awk '{print $1}')
        local relpath
        relpath=$(echo "$line" | awk '{print $2}')
        local filepath="$dir/$relpath"

        if [ ! -f "$filepath" ]; then
            echo -e "${RED}✗ Missing file: $relpath${NC}"
            failed=1
            continue
        fi

        local actual_sum
        actual_sum=$(get_checksum "$filepath")
        if [ "$expected_sum" != "$actual_sum" ]; then
            echo -e "${RED}✗ Checksum mismatch: $relpath${NC}"
            echo "  Expected: $expected_sum"
            echo "  Got:      $actual_sum"
            failed=1
        fi
    done < "$checksum_file"

    return $failed
}

# Upload file to S3
# Usage: upload_to_s3 <local_file> <s3_key>
upload_to_s3() {
    local local_file="$1"
    local s3_key="$2"

    if aws s3 cp "$local_file" "s3://$BURST_TEST_BUCKET/$s3_key" --region "$AWS_REGION" >/dev/null 2>&1; then
        echo -e "${GREEN}✓${NC} Uploaded to s3://$BURST_TEST_BUCKET/$s3_key"
        return 0
    else
        echo -e "${RED}✗ Failed to upload to S3${NC}"
        echo ""
        echo "Possible issues:"
        echo "  - Bucket '$BURST_TEST_BUCKET' does not exist"
        echo "  - AWS credentials not configured"
        echo "  - Insufficient IAM permissions"
        return 1
    fi
}

# Delete S3 object
# Usage: delete_from_s3 <s3_key>
delete_from_s3() {
    local s3_key="$1"
    aws s3 rm "s3://$BURST_TEST_BUCKET/$s3_key" --region "$AWS_REGION" 2>/dev/null || true
}

# Verify archive with 7zz
# Usage: verify_archive <archive_file>
verify_archive() {
    local archive="$1"

    if 7zz t "$archive" 2>&1 | grep -q "Everything is Ok"; then
        return 0
    else
        echo -e "${RED}✗ Archive validation failed${NC}"
        7zz t "$archive"
        return 1
    fi
}

# Verify alignment with Python script
# Usage: verify_alignment <archive_file>
verify_alignment() {
    local archive="$1"

    if python3 "$E2E_PROJECT_ROOT/tests/integration/verify_alignment.py" "$archive" >/dev/null 2>&1; then
        return 0
    else
        echo -e "${RED}✗ Alignment verification failed${NC}"
        python3 "$E2E_PROJECT_ROOT/tests/integration/verify_alignment.py" "$archive"
        return 1
    fi
}

# Extract archive with 7zz (for reference comparison)
# Usage: extract_with_7zz <archive> <output_dir>
extract_with_7zz() {
    local archive="$1"
    local output_dir="$2"

    mkdir -p "$output_dir"
    7zz x -y -o"$output_dir" "$archive" >/dev/null 2>&1
}

# Print test header
# Usage: print_test_header <test_name>
print_test_header() {
    echo ""
    echo -e "${BLUE}============================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}============================================${NC}"
    echo ""
}

# Print test result
# Usage: print_test_result <test_name> <result_code>
print_test_result() {
    local test_name="$1"
    local result="$2"

    echo ""
    if [ "$result" -eq 0 ]; then
        echo -e "${GREEN}============================================${NC}"
        echo -e "${GREEN}$test_name: PASSED${NC}"
        echo -e "${GREEN}============================================${NC}"
    else
        echo -e "${RED}============================================${NC}"
        echo -e "${RED}$test_name: FAILED${NC}"
        echo -e "${RED}============================================${NC}"
    fi
    echo ""
}

# Initialize S3 config on source
load_s3_config
