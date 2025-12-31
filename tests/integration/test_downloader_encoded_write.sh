#!/bin/bash
#
# Integration test for burst-downloader encoded write functionality
#
# Tests that burst-downloader correctly uses BTRFS_IOC_ENCODED_WRITE
# to write compressed data directly to disk without decompressing.
#
# Prerequisites:
#   - burst-downloader built
#   - compsize utility installed
#   - sudo access for setcap and BTRFS setup
#   - compressible-many-files fixture in S3 (run setup_s3.sh first)
#
# Tests:
#   - S3 archive download
#   - Extraction to BTRFS filesystem with encoded writes
#   - Checksum verification of all extracted files
#   - Verification that files are stored with zstd-compressed extents
#

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
DOWNLOADER="$BUILD_DIR/burst-downloader"
TEST_TMP="$PROJECT_ROOT/tests/tmp/encoded_write_test"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "================================================"
echo "BURST Downloader Encoded Write Integration Test"
echo "================================================"
echo ""

# Load configuration from .env if it exists
if [ -f "$PROJECT_ROOT/.env" ]; then
    echo "Loading configuration from .env..."
    # shellcheck source=/dev/null
    source "$PROJECT_ROOT/.env"
fi

# Use configuration with sensible defaults
TEST_BUCKET="${BURST_TEST_BUCKET:-burst-integration-tests}"
AWS_REGION="${AWS_REGION:-us-east-1}"

# Use the compressible fixture for this test
FIXTURE_NAME="compressible-many-files"
FIXTURE_KEY="fixtures/$FIXTURE_NAME.zip"
CHECKSUM_KEY="fixtures/$FIXTURE_NAME.sha256"

echo "Configuration:"
echo "  Test Bucket:  $TEST_BUCKET"
echo "  Fixture:      $FIXTURE_KEY"
echo "  AWS Region:   $AWS_REGION"
if [ -n "$AWS_PROFILE" ]; then
    echo "  AWS Profile:  $AWS_PROFILE"
fi
echo ""

# Check prerequisites
echo "Checking prerequisites..."

if [ ! -f "$DOWNLOADER" ]; then
    echo -e "${RED}ERROR: burst-downloader not found at $DOWNLOADER${NC}"
    echo "Please run: cd $BUILD_DIR && make burst-downloader"
    exit 1
fi
echo -e "${GREEN}✓${NC} Found burst-downloader"

if ! command -v aws &> /dev/null; then
    echo -e "${RED}ERROR: aws CLI not found${NC}"
    echo "Please install awscli: pip install awscli"
    exit 1
fi
echo -e "${GREEN}✓${NC} Found aws CLI"

if ! command -v compsize &> /dev/null; then
    echo -e "${RED}ERROR: compsize utility not found${NC}"
    echo "Please install compsize (for BTRFS compression analysis)"
    echo "  Ubuntu/Debian: sudo apt install btrfs-compsize"
    echo "  Fedora: sudo dnf install compsize"
    exit 1
fi
echo -e "${GREEN}✓${NC} Found compsize utility"

echo ""

# Set CAP_SYS_ADMIN capability on burst-downloader
echo "Checking capabilities on burst-downloader..."
current_caps=$(getcap "$DOWNLOADER" 2>/dev/null || echo "")

if [[ "$current_caps" != *"cap_sys_admin"* ]]; then
    echo "Setting CAP_SYS_ADMIN on burst-downloader..."
    if ! sudo setcap 'cap_sys_admin=ep' "$DOWNLOADER"; then
        echo -e "${RED}ERROR: Failed to set CAP_SYS_ADMIN capability${NC}"
        echo "sudo access is required to set capabilities"
        exit 1
    fi
fi

# Verify capability was set
if ! getcap "$DOWNLOADER" | grep -q "cap_sys_admin"; then
    echo -e "${RED}ERROR: CAP_SYS_ADMIN capability not set on burst-downloader${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} CAP_SYS_ADMIN capability is set"

echo ""

# Setup BTRFS filesystem
echo -e "${BLUE}Setting up BTRFS filesystem...${NC}"
# shellcheck source=e2e/setup_btrfs.sh
source "$SCRIPT_DIR/e2e/setup_btrfs.sh"

EXTRACT_DIR="$BTRFS_MOUNT_DIR/encoded_write_test"

# Setup cleanup trap
cleanup() {
    local exit_code=$?
    echo ""
    echo "Cleaning up..."
    if [ -d "$TEST_TMP" ]; then
        rm -rf "$TEST_TMP"
        echo -e "${GREEN}✓${NC} Removed test temp directory"
    fi
    if [ -d "$EXTRACT_DIR" ]; then
        rm -rf "$EXTRACT_DIR"
        echo -e "${GREEN}✓${NC} Removed extracted files from BTRFS"
    fi
    if [ $exit_code -ne 0 ]; then
        echo -e "${RED}✗ Test failed with exit code $exit_code${NC}"
    fi
}
trap cleanup EXIT

# Create test directories
rm -rf "$TEST_TMP"
mkdir -p "$TEST_TMP"
rm -rf "$EXTRACT_DIR"
mkdir -p "$EXTRACT_DIR"
cd "$TEST_TMP"

# Verify fixtures exist in S3
echo ""
echo "Verifying S3 fixtures exist..."

if ! aws s3api head-object --bucket "$TEST_BUCKET" --key "$FIXTURE_KEY" --region "$AWS_REGION" >/dev/null 2>&1; then
    echo -e "${RED}ERROR: Fixture not found: s3://$TEST_BUCKET/$FIXTURE_KEY${NC}"
    echo ""
    echo "Please run setup_s3.sh first to create test fixtures:"
    echo "  bash $SCRIPT_DIR/setup_s3.sh"
    exit 1
fi
echo -e "${GREEN}✓${NC} Found archive fixture"

if ! aws s3api head-object --bucket "$TEST_BUCKET" --key "$CHECKSUM_KEY" --region "$AWS_REGION" >/dev/null 2>&1; then
    echo -e "${RED}ERROR: Checksum file not found: s3://$TEST_BUCKET/$CHECKSUM_KEY${NC}"
    echo ""
    echo "Please run setup_s3.sh first to create test fixtures:"
    echo "  bash $SCRIPT_DIR/setup_s3.sh"
    exit 1
fi
echo -e "${GREEN}✓${NC} Found checksum fixture"

# Get archive size for reporting
ARCHIVE_SIZE=$(aws s3api head-object \
    --bucket "$TEST_BUCKET" \
    --key "$FIXTURE_KEY" \
    --query ContentLength \
    --output text \
    --region "$AWS_REGION")
echo -e "${GREEN}✓${NC} Archive size: $ARCHIVE_SIZE bytes"
echo ""

# Download checksum file
echo "Downloading checksum manifest..."
aws s3 cp "s3://$TEST_BUCKET/$CHECKSUM_KEY" "checksums.sha256" --region "$AWS_REGION" >/dev/null
EXPECTED_FILE_COUNT=$(wc -l < "checksums.sha256")
echo -e "${GREEN}✓${NC} Downloaded checksums ($EXPECTED_FILE_COUNT files expected)"
echo ""

# Run burst-downloader
echo "================================================"
echo "Running burst-downloader (extracting to BTRFS)"
echo "================================================"
echo ""
echo "Extract directory: $EXTRACT_DIR"

# Build command with optional profile
DOWNLOADER_CMD="$DOWNLOADER --bucket $TEST_BUCKET --key $FIXTURE_KEY --region $AWS_REGION --output-dir $EXTRACT_DIR"
if [ -n "$AWS_PROFILE" ]; then
    DOWNLOADER_CMD="$DOWNLOADER_CMD --profile $AWS_PROFILE"
fi

echo "Command: $DOWNLOADER_CMD"
echo ""

set +e # Disable exit on error to capture exit code
if [ -n "$AWS_PROFILE" ]; then
    $DOWNLOADER \
        --bucket "$TEST_BUCKET" \
        --key "$FIXTURE_KEY" \
        --region "$AWS_REGION" \
        --output-dir "$EXTRACT_DIR" \
        --profile "$AWS_PROFILE"
else
    $DOWNLOADER \
        --bucket "$TEST_BUCKET" \
        --key "$FIXTURE_KEY" \
        --region "$AWS_REGION" \
        --output-dir "$EXTRACT_DIR"
fi

DOWNLOADER_EXIT_CODE=$?
set -e

if [ $DOWNLOADER_EXIT_CODE -ne 0 ]; then
    echo -e "${RED}✗ FAILED: burst-downloader exited with code $DOWNLOADER_EXIT_CODE${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} Extraction completed successfully"
echo ""

# Verify file count
echo "Verifying extracted files..."
ACTUAL_FILE_COUNT=$(find "$EXTRACT_DIR" -type f | wc -l)
if [ "$ACTUAL_FILE_COUNT" -ne "$EXPECTED_FILE_COUNT" ]; then
    echo -e "${RED}✗ FAILED: File count mismatch${NC}"
    echo "  Expected: $EXPECTED_FILE_COUNT files"
    echo "  Got:      $ACTUAL_FILE_COUNT files"
    exit 1
fi
echo -e "${GREEN}✓${NC} File count matches: $ACTUAL_FILE_COUNT files"

# Verify checksums
echo "Verifying checksums..."
CHECKSUM_FAILED=0

while IFS= read -r line; do
    expected_sum=$(echo "$line" | awk '{print $1}')
    relpath=$(echo "$line" | awk '{print $2}')
    filepath="$EXTRACT_DIR/$relpath"

    if [ ! -f "$filepath" ]; then
        echo -e "${RED}✗ Missing file: $relpath${NC}"
        CHECKSUM_FAILED=1
        continue
    fi

    actual_sum=$(sha256sum "$filepath" | awk '{print $1}')
    if [ "$expected_sum" != "$actual_sum" ]; then
        echo -e "${RED}✗ Checksum mismatch: $relpath${NC}"
        echo "  Expected: $expected_sum"
        echo "  Got:      $actual_sum"
        CHECKSUM_FAILED=1
    fi
done < "checksums.sha256"

if [ $CHECKSUM_FAILED -ne 0 ]; then
    echo -e "${RED}✗ FAILED: Checksum verification failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓${NC} All checksums verified"
echo ""

# Verify compressed storage with compsize
echo "================================================"
echo "Verifying BTRFS Compressed Storage (compsize)"
echo "================================================"
echo ""

# Run compsize and capture output
set +e # Disable exit on error to capture compsize exit code
COMPSIZE_OUTPUT=$(sudo compsize "$EXTRACT_DIR" 2>&1)
COMPSIZE_EXIT_CODE=$?

if [ $COMPSIZE_EXIT_CODE -ne 0 ]; then
    echo -e "${RED}✗ FAILED: compsize failed with exit code $COMPSIZE_EXIT_CODE${NC}"
    echo "$COMPSIZE_OUTPUT"
    exit 1
fi

echo "$COMPSIZE_OUTPUT"
echo ""

# Parse compsize output to verify zstd compression
# Expected format:
# Type       Perc     Disk Usage   Uncompressed Referenced
# TOTAL       33%       10M          30M          30M
# zstd        33%       10M          30M          30M

if ! echo "$COMPSIZE_OUTPUT" | grep -q "zstd"; then
    echo -e "${RED}✗ FAILED: No zstd-compressed extents found${NC}"
    echo "Expected files to be stored with zstd compression via BTRFS_IOC_ENCODED_WRITE"
    exit 1
fi
echo -e "${GREEN}✓${NC} Found zstd-compressed extents"

# Extract compression percentage from TOTAL line
COMPRESSION_PERC=$(echo "$COMPSIZE_OUTPUT" | grep "^TOTAL" | awk '{print $2}' | tr -d '%')
if [ -n "$COMPRESSION_PERC" ] && [ "$COMPRESSION_PERC" -lt 100 ]; then
    echo -e "${GREEN}✓${NC} Compression ratio verified: ${COMPRESSION_PERC}% of original size"
else
    echo -e "${YELLOW}Warning:${NC} Could not verify compression ratio (got: $COMPRESSION_PERC%)"
fi
set -e

echo ""
echo "================================================"
echo -e "${GREEN}✓ ALL TESTS PASSED${NC}"
echo "================================================"
echo ""
echo "Summary:"
echo "  ✓ CAP_SYS_ADMIN capability set"
echo "  ✓ BTRFS filesystem ready"
echo "  ✓ S3 archive download ($ARCHIVE_SIZE bytes)"
echo "  ✓ Extraction to BTRFS ($ACTUAL_FILE_COUNT files)"
echo "  ✓ Checksum verification"
echo "  ✓ BTRFS encoded write verified (zstd compression at ${COMPRESSION_PERC:-??}%)"
echo ""

exit 0
