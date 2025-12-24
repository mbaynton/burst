#!/bin/bash
# Create synthetic test archives for verify_alignment.py testing

set -e

# Use the script's directory for test files
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PART_SIZE=$((8 * 1024 * 1024))  # 8 MiB

echo "=== Creating Test Archives ==="
echo

# Test 1: File < 8 MiB (should always pass)
echo "Test 1: Creating file < 8 MiB..."
dd if=/dev/zero of=test1_small.zip bs=1M count=5 2>/dev/null
echo "✓ Created test1_small.zip (5 MiB)"
echo

# Test 2: File with central directory starting before 8 MiB boundary
# Uses standard ZIP (not ZIP64) with central directory starting 200 bytes before boundary
echo "Test 2: Creating file with central directory starting before 8 MiB boundary..."
CD_START=$(($PART_SIZE - 200))
{
    # Write random data up to where central directory should start
    dd if=/dev/urandom bs=1 count=$CD_START 2>/dev/null

    # Write a simple central directory header (78 bytes)
    printf '\x50\x4b\x01\x02'  # Central directory signature
    printf '\x14\x00'          # Version made by
    printf '\x14\x00'          # Version needed
    printf '\x00\x00'          # General purpose bit flag
    printf '\x00\x00'          # Compression method (stored)
    printf '\x00\x00\x00\x00'  # Last mod time/date
    printf '\x00\x00\x00\x00'  # CRC-32
    printf '\x0a\x00\x00\x00'  # Compressed size (10 bytes)
    printf '\x0a\x00\x00\x00'  # Uncompressed size (10 bytes)
    printf '\x08\x00'          # File name length (8)
    printf '\x00\x00'          # Extra field length
    printf '\x00\x00'          # File comment length
    printf '\x00\x00'          # Disk number start
    printf '\x00\x00'          # Internal file attributes
    printf '\x00\x00\x00\x00'  # External file attributes
    printf '\x00\x00\x00\x00'  # Relative offset of local header
    printf 'test.bin'          # File name

    # Add bytes to cross the 8 MiB boundary
    # CD header is 78 bytes, need 150 more bytes to cross boundary (200 - 78 = 122, use 150 for margin)
    dd if=/dev/urandom bs=1 count=150 2>/dev/null

    # Now write EOCD after the boundary
    printf '\x50\x4b\x05\x06'  # EOCD signature
    printf '\x00\x00'          # Disk number
    printf '\x00\x00'          # Disk with CD
    printf '\x01\x00'          # Entries on this disk (1)
    printf '\x01\x00'          # Total entries (1)

    # CD size (78 bytes = 0x4e)
    printf '\x4e\x00\x00\x00'

    # CD offset (CD_START in little-endian)
    printf "$(printf '%08x' $CD_START | sed 's/\(..\)\(..\)\(..\)\(..\)/\\x\4\\x\3\\x\2\\x\1/')"

    # Comment length
    printf '\x00\x00'

} > test2_cd_before_boundary.zip
echo "✓ Created test2_cd_before_boundary.zip"
echo "  Central directory starts at offset: $CD_START ($(($CD_START / 1024 / 1024)) MiB + $(($CD_START % (1024 * 1024))) bytes)"
echo "  8 MiB boundary at: $PART_SIZE"
echo "  Boundary falls: $(($PART_SIZE - $CD_START)) bytes into central directory"
echo

# Test 3: File with random data at 8 MiB boundary
echo "Test 3: Creating file with random data at 8 MiB boundary..."
{
    # Write random data for exactly 8 MiB + 1 KiB
    dd if=/dev/urandom bs=1M count=8 2>/dev/null
    dd if=/dev/urandom bs=1K count=1 2>/dev/null
} > test3_random_at_boundary.zip
echo "✓ Created test3_random_at_boundary.zip (random data at boundary)"
echo

# Test 4: File with padding skippable frame (0x00, not 0x01) at 8 MiB
echo "Test 4: Creating file with padding skippable frame at 8 MiB..."
{
    # Write random data up to 8 MiB boundary
    dd if=/dev/urandom bs=1M count=8 2>/dev/null
    
    # Write BURST skippable frame with padding (type byte 0x00)
    # Magic: 5b 2a 4d 18 (little-endian for 0x184D2A5B)
    printf '\x5b\x2a\x4d\x18'
    # Frame size: 16 bytes (little-endian)
    printf '\x10\x00\x00\x00'
    # Type byte: 0x00 (padding, not Start-of-Part)
    printf '\x00'
    # Rest of payload (15 bytes of zeros)
    printf '\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
    
    # Add more data
    dd if=/dev/urandom bs=1K count=100 2>/dev/null
} > test4_padding_frame.zip
echo "✓ Created test4_padding_frame.zip (padding frame at 8 MiB)"
echo

# Test 5: File with Start-of-Part skippable frame at 8 MiB
echo "Test 5: Creating file with Start-of-Part frame at 8 MiB..."
{
    # Write random data up to 8 MiB boundary
    dd if=/dev/urandom bs=1M count=8 2>/dev/null
    
    # Write BURST Start-of-Part skippable frame (type byte 0x01)
    # Magic: 5b 2a 4d 18 (little-endian for 0x184D2A5B)
    printf '\x5b\x2a\x4d\x18'
    # Frame size: 16 bytes (little-endian)
    printf '\x10\x00\x00\x00'
    # Type byte: 0x01 (Start-of-Part)
    printf '\x01'
    # Uncompressed offset: 0x7e0000 (8 bytes, little-endian)
    printf '\x00\x00\x7e\x00\x00\x00\x00\x00'
    # Reserved (7 bytes of zeros)
    printf '\x00\x00\x00\x00\x00\x00\x00'
    
    # Add more data
    dd if=/dev/urandom bs=1K count=100 2>/dev/null
} > test5_start_of_part.zip
echo "✓ Created test5_start_of_part.zip (Start-of-Part frame at 8 MiB)"
echo

# Test 6: File with ZIP local file header at 8 MiB
echo "Test 6: Creating file with ZIP local file header at 8 MiB..."
{
    # Write random data up to 8 MiB boundary
    dd if=/dev/urandom bs=1M count=8 2>/dev/null
    
    # Write ZIP local file header signature
    # Signature: 50 4b 03 04 (little-endian for 0x04034b50)
    printf '\x50\x4b\x03\x04'
    
    # Minimal ZIP local header (rest of header)
    # Version needed (2 bytes)
    printf '\x14\x00'
    # General purpose bit flag (2 bytes)
    printf '\x00\x00'
    # Compression method (2 bytes) - 93 = Zstandard
    printf '\x5d\x00'
    # Last mod time/date (4 bytes)
    printf '\x00\x00\x00\x00'
    # CRC-32 (4 bytes)
    printf '\x00\x00\x00\x00'
    # Compressed size (4 bytes)
    printf '\x00\x00\x00\x00'
    # Uncompressed size (4 bytes)
    printf '\x00\x00\x00\x00'
    # File name length (2 bytes)
    printf '\x08\x00'
    # Extra field length (2 bytes)
    printf '\x00\x00'
    # File name (8 bytes: "test.bin")
    printf 'test.bin'
    
    # Add more data
    dd if=/dev/urandom bs=1K count=100 2>/dev/null
} > test6_zip_header.zip
echo "✓ Created test6_zip_header.zip (ZIP local header at 8 MiB)"
echo

echo "=== All test files created ==="
ls -lh *.zip
