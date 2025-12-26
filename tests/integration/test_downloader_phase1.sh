#!/bin/bash
#
# Integration test for Phase 1 downloader functionality
# Tests S3 client initialization, object size detection, and range GET against real AWS S3
#

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
DOWNLOADER="$BUILD_DIR/burst-downloader"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "================================================"
echo "BURST Downloader Phase 1 Integration Test"
echo "================================================"
echo ""

# Load configuration from .env if it exists
if [ -f "$PROJECT_ROOT/.env" ]; then
    echo "Loading configuration from .env..."
    source "$PROJECT_ROOT/.env"
fi

# Use configuration with sensible defaults
TEST_BUCKET="${BURST_TEST_BUCKET:-burst-test}"
TEST_KEY_PREFIX="${BURST_TEST_KEY_PREFIX:-test-artifacts/}"
# Use timestamp to avoid conflicts between parallel test runs
TEST_KEY="${TEST_KEY_PREFIX}test-file-$(date +%s).bin"
AWS_REGION="${AWS_REGION:-us-east-1}"
TEST_OUTPUT_DIR="${BURST_TEST_OUTPUT_DIR:-/tmp/burst-test-output}"

# Test configuration
TEST_FILE="/tmp/burst-test-file-$(date +%s).bin"
TEST_SIZE=10485760  # 10 MiB

echo "Configuration:"
echo "  Test Bucket:  $TEST_BUCKET"
echo "  Test Key:     $TEST_KEY"
echo "  AWS Region:   $AWS_REGION"
echo "  Output Dir:   $TEST_OUTPUT_DIR"
if [ -n "$AWS_PROFILE" ]; then
    echo "  AWS Profile:  $AWS_PROFILE"
fi
echo ""

# Check if downloader exists
if [ ! -f "$DOWNLOADER" ]; then
    echo -e "${RED}ERROR: burst-downloader not found at $DOWNLOADER${NC}"
    echo "Please run: cd $BUILD_DIR && make burst-downloader"
    exit 1
fi

echo -e "${GREEN}✓${NC} Found burst-downloader at $DOWNLOADER"

# Check if aws CLI is available
if ! command -v aws &> /dev/null; then
    echo -e "${RED}ERROR: aws CLI not found${NC}"
    echo "Please install awscli: pip install awscli"
    exit 1
fi

echo -e "${GREEN}✓${NC} Prerequisites met"
echo ""

# Setup cleanup trap to ensure S3 cleanup even on failure
cleanup() {
    local exit_code=$?
    echo ""
    echo "Cleaning up..."
    if [ -f "$TEST_FILE" ]; then
        rm -f "$TEST_FILE"
        echo -e "${GREEN}✓${NC} Removed local test file"
    fi
    if [ -n "$TEST_KEY_UPLOADED" ]; then
        aws s3 rm "s3://$TEST_BUCKET/$TEST_KEY" --region "$AWS_REGION" 2>/dev/null || true
        echo -e "${GREEN}✓${NC} Removed S3 test object"
    fi
    if [ -d "$TEST_OUTPUT_DIR" ]; then
        rm -rf "$TEST_OUTPUT_DIR"
        echo -e "${GREEN}✓${NC} Removed output directory"
    fi
    if [ $exit_code -ne 0 ]; then
        echo -e "${RED}✗ Test failed with exit code $exit_code${NC}"
    fi
}
trap cleanup EXIT

# Create test file (10 MiB of pseudo-random data)
echo "Creating test file ($TEST_SIZE bytes)..."
dd if=/dev/urandom of="$TEST_FILE" bs=1M count=10 2>/dev/null
ACTUAL_SIZE=$(stat -c%s "$TEST_FILE" 2>/dev/null || stat -f%z "$TEST_FILE" 2>/dev/null)
echo -e "${GREEN}✓${NC} Created test file: $TEST_FILE ($ACTUAL_SIZE bytes)"

# Calculate expected first 16 bytes for verification
EXPECTED_HEX=$(xxd -p -l 16 "$TEST_FILE" | tr -d '\n')
echo "  First 16 bytes (hex): $EXPECTED_HEX"
echo ""

# Upload test file to S3
echo "Testing against real AWS S3"
echo "Uploading test file to s3://$TEST_BUCKET/$TEST_KEY..."
if aws s3 cp "$TEST_FILE" "s3://$TEST_BUCKET/$TEST_KEY" --region "$AWS_REGION"; then
    TEST_KEY_UPLOADED=1
    echo -e "${GREEN}✓${NC} File uploaded"
else
    echo -e "${RED}✗ FAILED: Could not upload file to S3${NC}"
    echo ""
    echo "Possible issues:"
    echo "  - Bucket '$TEST_BUCKET' does not exist"
    echo "  - AWS credentials not configured"
    echo "  - Insufficient IAM permissions"
    echo ""
    echo "Setup instructions:"
    echo "  1. Create S3 bucket: aws s3 mb s3://$TEST_BUCKET"
    echo "  2. Configure credentials: aws sso login --profile <profile-name>"
    echo "  3. Copy .env.example to .env and customize"
    exit 1
fi
echo ""

# Verify upload
UPLOADED_SIZE=$(aws s3api head-object \
    --bucket "$TEST_BUCKET" \
    --key "$TEST_KEY" \
    --query ContentLength \
    --output text \
    --region "$AWS_REGION")
echo "Uploaded file size: $UPLOADED_SIZE bytes"
echo ""

# Run Phase 1 tests with downloader
echo "================================================"
echo "Running burst-downloader Phase 1 tests"
echo "================================================"
echo ""

mkdir -p "$TEST_OUTPUT_DIR"

echo "Command: $DOWNLOADER -b $TEST_BUCKET -k $TEST_KEY -r $AWS_REGION -o $TEST_OUTPUT_DIR"
echo ""

# Run downloader and capture output
DOWNLOADER_OUTPUT=$("$DOWNLOADER" \
    --bucket "$TEST_BUCKET" \
    --key "$TEST_KEY" \
    --region "$AWS_REGION" \
    --output-dir "$TEST_OUTPUT_DIR" \
    2>&1)

DOWNLOADER_EXIT_CODE=$?

echo "$DOWNLOADER_OUTPUT"
echo ""

# Check exit code
if [ $DOWNLOADER_EXIT_CODE -ne 0 ]; then
    echo -e "${RED}✗ FAILED: burst-downloader exited with code $DOWNLOADER_EXIT_CODE${NC}"
    exit 1
fi

# Verify output contains expected success indicators
if ! echo "$DOWNLOADER_OUTPUT" | grep -q "✓ Object size:"; then
    echo -e "${RED}✗ FAILED: Object size test did not pass${NC}"
    exit 1
fi

if ! echo "$DOWNLOADER_OUTPUT" | grep -q "✓ Downloaded.*bytes"; then
    echo -e "${RED}✗ FAILED: Range GET test did not pass${NC}"
    exit 1
fi

if ! echo "$DOWNLOADER_OUTPUT" | grep -q "✓ Phase 1 tests completed successfully"; then
    echo -e "${RED}✗ FAILED: Phase 1 tests did not complete successfully${NC}"
    exit 1
fi

# Extract object size from output
DETECTED_SIZE=$(echo "$DOWNLOADER_OUTPUT" | grep "✓ Object size:" | sed -E 's/.*: ([0-9]+) bytes.*/\1/')
if [ "$DETECTED_SIZE" != "$ACTUAL_SIZE" ]; then
    echo -e "${RED}✗ FAILED: Object size mismatch${NC}"
    echo "  Expected: $ACTUAL_SIZE"
    echo "  Got: $DETECTED_SIZE"
    exit 1
fi

echo -e "${GREEN}✓${NC} Object size detection: PASS ($DETECTED_SIZE bytes)"

# Extract downloaded hex from output
DOWNLOADED_HEX=$(echo "$DOWNLOADER_OUTPUT" | grep "First 16 bytes" | sed -E 's/.*: (.*)/\1/' | tr -d ' ')
if [ "$DOWNLOADED_HEX" != "$EXPECTED_HEX" ]; then
    echo -e "${RED}✗ FAILED: Downloaded data mismatch${NC}"
    echo "  Expected: $EXPECTED_HEX"
    echo "  Got: $DOWNLOADED_HEX"
    exit 1
fi

echo -e "${GREEN}✓${NC} Range GET data verification: PASS"

echo ""
echo "================================================"
echo -e "${GREEN}✓ ALL PHASE 1 TESTS PASSED${NC}"
echo "================================================"
echo ""
echo "Summary:"
echo "  ✓ S3 client initialization"
echo "  ✓ Object size detection ($DETECTED_SIZE bytes)"
echo "  ✓ Range GET (1024 bytes)"
echo "  ✓ Real S3 integration"
echo ""

exit 0
