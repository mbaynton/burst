#!/usr/bin/env python3
"""
BURST Archive Alignment Verification Tool

Verifies that all 8 MiB boundaries in a BURST archive align to:
- ZIP local file headers, OR
- Start-of-Part Zstandard skippable frames

Exception: Once the ZIP central directory has begun, alignment is not required.

Supports both standard ZIP and ZIP64 archives.
"""

import sys
import struct
import os
import argparse


# Magic number constants (all little-endian)
ZIP_LOCAL_HEADER_SIG = b'\x50\x4b\x03\x04'  # 0x04034b50
ZIP_CENTRAL_DIR_SIG = b'\x50\x4b\x01\x02'   # 0x02014b50
ZIP_EOCD_SIG = b'\x50\x4b\x05\x06'          # 0x06054b50
ZIP64_EOCD_SIG = b'\x50\x4b\x06\x06'        # 0x06064b50
ZIP64_LOCATOR_SIG = b'\x50\x4b\x06\x07'     # 0x07064b50
ZSTD_FRAME_MAGIC = b'\x28\xb5\x2f\xfd'      # 0x28b52ffd
BURST_SKIPPABLE_MAGIC = b'\x5b\x2a\x4d\x18' # 0x184D2A5B

# Constants
PART_SIZE = 8 * 1024 * 1024  # 8 MiB
START_OF_PART_TYPE_BYTE = 0x01


class Colors:
    """ANSI color codes for terminal output."""
    GREEN = '\033[0;32m'
    RED = '\033[0;31m'
    BLUE = '\033[0;34m'
    YELLOW = '\033[1;33m'
    NC = '\033[0m'  # No Color

    @classmethod
    def disable(cls):
        """Disable all colors."""
        cls.GREEN = ''
        cls.RED = ''
        cls.BLUE = ''
        cls.YELLOW = ''
        cls.NC = ''


def format_size(size_bytes):
    """Format byte size in human-readable format."""
    if size_bytes < 1024:
        return f"{size_bytes} bytes"
    elif size_bytes < 1024 * 1024:
        return f"{size_bytes / 1024:.1f} KiB"
    elif size_bytes < 1024 * 1024 * 1024:
        return f"{size_bytes / (1024 * 1024):.1f} MiB"
    else:
        return f"{size_bytes / (1024 * 1024 * 1024):.1f} GiB"


def hex_dump(data, offset, length=16):
    """Format bytes as hex dump."""
    chunk = data[offset:offset+length]
    hex_str = ' '.join(f'{b:02x}' for b in chunk)
    return hex_str


def find_central_directory_start(data):
    """
    Find offset where central directory begins (ZIP64-aware).

    Returns:
        int or None: Offset of central directory start, or None if not found
    """
    # Search backwards for EOCD signature (0x06054b50)
    # ZIP spec: max comment size = 64 KiB, so search last 64 KiB + EOCD size
    search_start = max(0, len(data) - 65536)
    eocd_pos = data.rfind(ZIP_EOCD_SIG, search_start)

    if eocd_pos == -1:
        return None  # No EOCD found

    # Check if this is a ZIP64 archive
    # ZIP64 EOCD Locator appears 20 bytes before standard EOCD
    if eocd_pos >= 20:
        locator_sig = data[eocd_pos-20:eocd_pos-16]
        if locator_sig == ZIP64_LOCATOR_SIG:
            # Parse locator to get ZIP64 EOCD offset
            # Locator structure: 4-byte sig, 4-byte disk num, 8-byte offset, 4-byte total disks
            # Offset is at bytes 8-15 of locator (bytes -12 to -4 relative to EOCD)
            zip64_eocd_offset = struct.unpack('<Q', data[eocd_pos-12:eocd_pos-4])[0]

            # Parse ZIP64 EOCD for central directory offset
            # ZIP64 EOCD structure has CD offset at byte 48 (8 bytes)
            if zip64_eocd_offset + 56 <= len(data):
                cd_offset = struct.unpack('<Q',
                    data[zip64_eocd_offset+48:zip64_eocd_offset+56])[0]
                return cd_offset

    # Standard ZIP: parse EOCD to get central directory offset
    # EOCD structure has CD offset at bytes 16-19 (4 bytes, little-endian)
    if eocd_pos + 20 <= len(data):
        cd_offset = struct.unpack('<I', data[eocd_pos+16:eocd_pos+20])[0]
        return cd_offset

    return None


def verify_boundary(data, offset, cd_start_offset, verbose=False):
    """
    Verify alignment at given boundary offset.

    Args:
        data: The archive file data
        offset: Boundary offset to check
        cd_start_offset: Offset where central directory begins (or None)
        verbose: Whether to show detailed output

    Returns:
        tuple: (is_valid, message, detail)
    """
    # If we're past the central directory start, alignment rule doesn't apply
    if cd_start_offset is not None and offset >= cd_start_offset:
        return True, "Within central directory - alignment not required", None

    # Ensure we have at least 4 bytes to read
    if offset + 4 > len(data):
        return False, "File too short (boundary beyond file end)", None

    # Read 4-byte magic at boundary
    magic = data[offset:offset+4]

    # Check for valid boundary markers
    if magic == ZIP_LOCAL_HEADER_SIG:
        return True, "ZIP local file header", None

    elif magic == BURST_SKIPPABLE_MAGIC:
        # Verify it's a Start-of-Part frame (not padding)
        if offset + 9 > len(data):
            return False, "BURST skippable frame but file too short to verify type byte", None

        type_byte = data[offset + 8]
        if type_byte == START_OF_PART_TYPE_BYTE:
            # Read uncompressed offset if verbose
            detail = None
            if verbose and offset + 17 <= len(data):
                uncomp_offset = struct.unpack('<Q', data[offset+9:offset+17])[0]
                detail = f"Uncompressed offset: {uncomp_offset:#x}"
            return True, "Start-of-Part metadata frame", detail
        else:
            return False, (f"BURST skippable frame but wrong type byte: "
                          f"0x{type_byte:02x} (expected 0x{START_OF_PART_TYPE_BYTE:02x})"), None

    elif magic == ZSTD_FRAME_MAGIC:
        return False, "Zstandard frame (not at frame boundary)", None

    elif magic[:2] == b'\x50\x4b':  # Some kind of ZIP signature
        magic_hex = magic.hex()
        return False, f"ZIP signature but not local file header: {magic_hex}", None

    else:
        magic_hex = magic.hex()
        return False, f"Invalid magic: {magic_hex}", None


def verify_alignment(filepath, verbose=False, use_color=True):
    """
    Main verification function.

    Args:
        filepath: Path to ZIP archive to verify
        verbose: Whether to show hex dumps
        use_color: Whether to use colored output

    Returns:
        int: Number of errors found
    """
    if not use_color:
        Colors.disable()

    # Read entire file
    try:
        with open(filepath, 'rb') as f:
            data = f.read()
    except IOError as e:
        print(f"{Colors.RED}Error reading file: {e}{Colors.NC}")
        return 1

    file_size = len(data)

    # Print header
    print(f"{Colors.BLUE}=== BURST Archive Alignment Verification ==={Colors.NC}")
    print(f"Archive: {filepath}")
    print(f"File size: {format_size(file_size)} ({file_size:,} bytes)")

    # Find central directory start
    cd_start = find_central_directory_start(data)
    if cd_start is not None:
        print(f"Central directory starts at: {cd_start:#x} ({format_size(cd_start)})")
    else:
        print(f"{Colors.YELLOW}Warning: Could not locate central directory{Colors.NC}")
    print()

    # Calculate number of boundaries
    num_boundaries = file_size // PART_SIZE

    if num_boundaries == 0:
        print(f"{Colors.YELLOW}No 8 MiB boundaries to check (file smaller than 8 MiB){Colors.NC}")
        return 0

    print(f"Checking {num_boundaries} {'boundary' if num_boundaries == 1 else 'boundaries'}:")
    print()

    # Check each boundary
    errors = []
    passed = 0

    for i in range(1, num_boundaries + 1):
        offset = i * PART_SIZE

        if offset >= file_size:
            break

        valid, message, detail = verify_boundary(data, offset, cd_start, verbose)

        if valid:
            passed += 1
            offset_mib = offset // (1024 * 1024)
            print(f"{Colors.GREEN}✓{Colors.NC} Boundary {i} at {offset:#x} "
                  f"({offset_mib} MiB): {message}")

            if detail:
                print(f"  {detail}")

            if verbose:
                print(f"  Hex dump: {hex_dump(data, offset, 16)}")
        else:
            errors.append((i, offset, message))
            offset_mib = offset // (1024 * 1024)
            print(f"{Colors.RED}✗{Colors.NC} Boundary {i} at {offset:#x} "
                  f"({offset_mib} MiB): {message}")

            if verbose:
                print(f"  Hex dump: {hex_dump(data, offset, 16)}")

        if verbose or not valid:
            print()

    # Print summary
    print(f"{Colors.BLUE}=== Summary ==={Colors.NC}")
    print(f"Total boundaries checked: {num_boundaries}")
    print(f"{Colors.GREEN}Passed: {passed}{Colors.NC}")

    if errors:
        print(f"{Colors.RED}Failed: {len(errors)}{Colors.NC}")
        print()
        print(f"{Colors.RED}✗ Alignment verification failed!{Colors.NC}")
        print("One or more 8 MiB boundaries do not align to frame/header starts.")
        print("This violates the BURST alignment requirement.")
        return len(errors)
    else:
        print()
        print(f"{Colors.GREEN}✓ All boundaries properly aligned!{Colors.NC}")
        print("This archive complies with the BURST alignment requirement.")
        return 0


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description='Verify 8 MiB boundary alignment in BURST archives',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Each 8 MiB boundary must align to either:
  - A ZIP local file header, OR
  - A Start-of-Part Zstandard skippable frame

Exception: Boundaries within the central directory are exempt.

Exit codes:
  0 - All boundaries properly aligned
  1 - One or more alignment violations found
"""
    )

    parser.add_argument('archive',
                       help='Path to ZIP archive to verify')
    parser.add_argument('-v', '--verbose',
                       action='store_true',
                       help='Show hex dumps at each boundary')
    parser.add_argument('--no-color',
                       action='store_true',
                       help='Disable colored output')

    args = parser.parse_args()

    # Check if file exists
    if not os.path.exists(args.archive):
        print(f"Error: File not found: {args.archive}", file=sys.stderr)
        return 1

    # Auto-disable color if not a TTY (unless explicitly requested)
    use_color = not args.no_color and sys.stdout.isatty()

    # Run verification
    error_count = verify_alignment(args.archive, args.verbose, use_color)

    # Return exit code
    return 0 if error_count == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
