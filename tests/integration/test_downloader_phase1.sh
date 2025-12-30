#!/bin/bash
#
# Integration test for burst-downloader
#
# Tests full archive download and extraction against real AWS S3 using
# pre-created fixtures. Run setup_s3.sh first to create the fixtures.
#
# Tests:
#   - S3 client initialization
#   - Archive download from S3
#   - Full extraction of BURST archive
#   - Checksum verification of all extracted files
#

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
DOWNLOADER="$BUILD_DIR/burst-downloader"
TEST_TMP="$PROJECT_ROOT/tests/tmp/downloader_test"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "================================================"
echo "BURST Downloader Integration Test"
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

# Use the small fixture for this test (single part, fastest)
FIXTURE_NAME="small"
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

echo ""

# Setup cleanup trap
cleanup() {
    local exit_code=$?
    echo ""
    echo "Cleaning up..."
    if [ -d "$TEST_TMP" ]; then
        rm -rf "$TEST_TMP"
        echo -e "${GREEN}✓${NC} Removed test directory"
    fi
    if [ $exit_code -ne 0 ]; then
        echo -e "${RED}✗ Test failed with exit code $exit_code${NC}"
    fi
}
trap cleanup EXIT

# Create test directory
rm -rf "$TEST_TMP"
mkdir -p "$TEST_TMP"
cd "$TEST_TMP"

# Verify fixtures exist in S3
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
echo "Running burst-downloader"
echo "================================================"
echo ""

mkdir -p "extracted"

# Build command with optional profile
DOWNLOADER_CMD="$DOWNLOADER --bucket $TEST_BUCKET --key $FIXTURE_KEY --region $AWS_REGION --output-dir extracted"
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
        --output-dir "extracted" \
        --profile "$AWS_PROFILE"
else
    $DOWNLOADER \
        --bucket "$TEST_BUCKET" \
        --key "$FIXTURE_KEY" \
        --region "$AWS_REGION" \
        --output-dir "extracted"
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
ACTUAL_FILE_COUNT=$(find "extracted" -type f | wc -l)
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
    filepath="extracted/$relpath"

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
echo "================================================"
echo -e "${GREEN}✓ ALL TESTS PASSED${NC}"
echo "================================================"
echo ""
echo "Summary:"
echo "  ✓ S3 client initialization"
echo "  ✓ Archive download ($ARCHIVE_SIZE bytes)"
echo "  ✓ Full extraction ($ACTUAL_FILE_COUNT files)"
echo "  ✓ Checksum verification"
echo ""

exit 0
