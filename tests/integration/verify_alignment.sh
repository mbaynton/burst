#!/bin/bash
# Hex Dump Validation Script for BURST Archive Alignment
# Verifies that all 8 MiB boundaries align to ZIP headers or Zstandard frame starts

set -e

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Check if archive file is provided
if [ $# -lt 1 ]; then
    echo "Usage: $0 <archive.zip>"
    echo "Verifies that all 8 MiB boundaries in a BURST archive are properly aligned."
    exit 1
fi

ARCHIVE="$1"

if [ ! -f "$ARCHIVE" ]; then
    echo "Error: Archive file not found: $ARCHIVE"
    exit 1
fi

echo -e "${BLUE}=== BURST Archive Alignment Verification ===${NC}"
echo "Archive: $ARCHIVE"
echo

# Get archive size
ARCHIVE_SIZE=$(stat -c%s "$ARCHIVE")
echo "Archive size: $(numfmt --to=iec-i --suffix=B $ARCHIVE_SIZE) ($ARCHIVE_SIZE bytes)"
echo

# Calculate number of 8 MiB boundaries in the archive
BOUNDARY_SIZE=$((8 * 1024 * 1024))  # 8 MiB
NUM_BOUNDARIES=$((ARCHIVE_SIZE / BOUNDARY_SIZE))

if [ "$NUM_BOUNDARIES" -eq 0 ]; then
    echo -e "${YELLOW}Warning: Archive is smaller than 8 MiB - no boundaries to check${NC}"
    exit 0
fi

echo "Number of 8 MiB boundaries to check: $NUM_BOUNDARIES"
echo

# Helper function to get magic number at offset
get_magic_hex() {
    local offset=$1
    # Read 4 bytes and convert to hex (little-endian format for display)
    dd if="$ARCHIVE" bs=1 skip="$offset" count=4 2>/dev/null | xxd -p | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/'
}

# Helper function to get skippable frame size (little-endian 4 bytes after magic)
get_skippable_frame_size() {
    local offset=$1
    local size_hex
    # Skip magic (4 bytes), read size field (4 bytes, little-endian)
    size_hex=$(dd if="$ARCHIVE" bs=1 skip=$((offset + 4)) count=4 2>/dev/null | xxd -p)
    # Convert little-endian hex to decimal
    printf "%d" "0x${size_hex:6:2}${size_hex:4:2}${size_hex:2:2}${size_hex:0:2}"
}

# Helper function to identify skippable frame type
identify_skippable_frame() {
    local offset=$1
    local frame_size
    frame_size=$(get_skippable_frame_size "$offset")

    # Start-of-Part metadata frame has exactly 16-byte payload (24 bytes total with header)
    if [ "$frame_size" -eq 16 ]; then
        echo "Start-of-Part metadata frame -- GOOD"
        return 0
    else
        echo "Padding frame (${frame_size} bytes payload)"
        return 1
    fi
}

# Helper function to identify magic number
identify_magic() {
    local magic=$1
    local offset=$2  # Need offset to inspect frame details
    case "$magic" in
        "504b0304")
            echo "ZIP local file header -- GOOD"
            return 0
            ;;
        "504b0102")
            echo "ZIP central directory header"
            return 0
            ;;
        "504b0506")
            echo "ZIP end of central directory"
            return 0
            ;;
        "504b0708")
            echo "ZIP data descriptor"
            return 0
            ;;
        "28b52ffd")
            echo "Zstandard frame"
            return 0
            ;;
        "184d2a5b"|"5b2a4d18")
            # Check if it's Start-of-Part or padding
            identify_skippable_frame "$offset"
            return $?
            ;;
        *)
            echo "UNKNOWN"
            return 1
            ;;
esac
}

# Track results
PASS_COUNT=0
FAIL_COUNT=0
WARN_COUNT=0

# Check each 8 MiB boundary
echo -e "${BLUE}Checking boundaries:${NC}"
echo

for ((i=1; i<=NUM_BOUNDARIES; i++)); do
    OFFSET=$((i * BOUNDARY_SIZE))

    # Format offset for display
    OFFSET_HEX=$(printf "0x%X" "$OFFSET")
    OFFSET_MB=$((OFFSET / 1024 / 1024))

    echo "Boundary $i: offset $OFFSET_HEX (${OFFSET_MB} MiB)"

    # Get magic number at boundary
    MAGIC=$(get_magic_hex "$OFFSET")
    set +e
    IDENTIFIED=$(identify_magic "$MAGIC" "$OFFSET")
    set -e
    RESULT=1
    if [[ "${IDENTIFIED}" =~ "GOOD" ]]; then
        RESULT=0
    fi
    # Display hex dump at boundary (16 bytes)
    echo -n "  Hex dump: "
    dd if="$ARCHIVE" bs=1 skip="$OFFSET" count=16 2>/dev/null | xxd -p -c 16

    # Display interpretation
    if [ $RESULT -eq 0 ]; then
        echo -e "  Magic:    ${GREEN}$MAGIC${NC} ($IDENTIFIED)"
        echo -e "  Status:   ${GREEN}✓ VALID${NC}"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo -e "  Magic:    ${RED}$MAGIC${NC} ($IDENTIFIED)"
        echo -e "  Status:   ${RED}✗ INVALID - Not at frame/header boundary${NC}"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    echo
done

# Summary
echo -e "${BLUE}=== Summary ===${NC}"
echo "Total boundaries checked: $NUM_BOUNDARIES"
echo -e "${GREEN}Passed: $PASS_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failed: $FAIL_COUNT${NC}"
fi

echo

if [ "$FAIL_COUNT" -eq 0 ]; then
    echo -e "${GREEN}✓ All boundaries are properly aligned!${NC}"
    echo "This archive complies with the BURST alignment requirement."
    exit 0
else
    echo -e "${RED}✗ Alignment verification failed!${NC}"
    echo "One or more 8 MiB boundaries do not align to frame/header starts."
    echo "This violates the BURST alignment requirement."
    exit 1
fi
