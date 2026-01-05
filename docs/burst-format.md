# BURST Archive Format Specification

## Overview

BURST (**B**TRFS **U**ltrafast **R**estore from **S**3 **T**ransfers) is a ZIP-based archive format optimized for concurrent extraction. It uses Zstandard compression and enforces alignment constraints that enable independent processing of 8 MiB archive segments ("parts").

### Key Characteristics

- **Base format**: Standard ZIP with Zstandard compression (method 93)
- **Compression**: Zstandard frames, max 128 KiB uncompressed each
- **Part size**: 8 MiB (8,388,608 bytes)
- **Alignment rule**: Every 8 MiB boundary must start with a valid entry point
- **ZIP64**: Supported for archives and files larger than 4 GiB

A BURST archive is a valid ZIP file and can be extracted by any ZIP tool that supports Zstandard compression (7-Zip, Info-ZIP with zstd plugin, etc.).

---

## The Alignment Rule

The defining characteristic of BURST archives is the **alignment rule**:

> Every 8 MiB boundary (offsets 0, 8388608, 16777216, ...) MUST begin with either:
> 1. A ZIP Local File Header, OR
> 2. A Start-of-Part metadata frame

**Exception**: Once the Central Directory begins at the end of the archive, alignment rules no longer apply. The Central Directory may cross boundaries freely.

This rule ensures that each 8 MiB part can be processed independently without requiring data from adjacent parts.

---

## Archive Structure

```
                                          Archive
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│   Part 0                                              Offset 0              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ Local File Header 1                                                   │  │
│  │ ┌─────────────────────────────────────────────────────────────────┐   │  │
│  │ │ Zstd Frame 1 (≤128 KiB uncompressed)                            │   │  │
│  │ │ Zstd Frame 2                                                    │   │  │
│  │ │ ...                                                             │   │  │
│  │ │ Zstd Frame N                                                    │   │  │
│  │ └─────────────────────────────────────────────────────────────────┘   │  │
│  │ Skippable Padding Frame (aligns to 8 MiB boundary)                    │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│  ════════════════════════════════════════════════════════════════════════   │
│   Part 1                                              Offset 8,388,608      │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ Start-of-Part Frame (file 1 continues)                                │  │
│  │ ┌─────────────────────────────────────────────────────────────────┐   │  │
│  │ │ Zstd Frame (continuation of file 1)                             │   │  │
│  │ │ ...                                                             │   │  │
│  │ └─────────────────────────────────────────────────────────────────┘   │  │
│  │ Data Descriptor 1                                                     │  │
│  │ Local File Header 2                                                   │  │
│  │ ┌─────────────────────────────────────────────────────────────────┐   │  │
│  │ │ Zstd Frames for file 2...                                       │   │  │
│  │ └─────────────────────────────────────────────────────────────────┘   │  │
│  │ Data Descriptor 2                                                     │  │
│  │ ...                                                                   │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│  ════════════════════════════════════════════════════════════════════════   │
│   Part 2                                              Offset 16,777,216     │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ Local File Header 3 (new file starts exactly at boundary)             │  │
│  │ ...                                                                   │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                    ...                                      │
│  ════════════════════════════════════════════════════════════════════════   │
│   Final Part(s)                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ ... remaining file data ...                                           │  │
│  │ Central Directory File Headers (may cross boundaries)                 │  │
│  │ ZIP64 End of Central Directory Record (if needed)                     │  │
│  │ ZIP64 End of Central Directory Locator (if needed)                    │  │
│  │ End of Central Directory Record + BRST Comment                        │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Zstandard Frame Constraints

All compressed file data in BURST archives consists of Zstandard frames with specific constraints:

| Constraint | Value | Reason |
|------------|-------|--------|
| Maximum uncompressed size | 128 KiB (131,072 bytes) | BTRFS_IOC_ENCODED_WRITE limit |
| Minimum uncompressed size | 4 KiB (4,096 bytes) | Practical BTRFS limit |
| Content size in header | **Required** | Enables O(1) size lookup |
| Compression level | -15 to 15 | BTRFS compatibility |
| Dictionary | None | Frames must be self-contained |

### Frame Header Requirement

Every Zstandard frame **MUST** include the uncompressed content size in its frame header. This enables:
- O(1) lookup via `ZSTD_getFrameContentSize()`
- Sequential frame traversal without decompression
- Immediate BTRFS encoded writes

Standard `ZSTD_compress()` includes content size by default. Verify with:
```c
ZSTD_getFrameContentSize(frame, frame_size) != ZSTD_CONTENTSIZE_UNKNOWN
```

---

## BURST-Specific Structures

BURST archives use Zstandard's skippable frame format for metadata and padding. All BURST skippable frames use the magic number `0x184D2A5B`.

### Skippable Padding Frame

Used to fill space before an 8 MiB boundary when a compressed frame would cross it.

```
┌─────────────────────────────────────────────────────────────────────┐
│ Offset │ Size │ Field         │ Value                               │
├────────┼──────┼───────────────┼─────────────────────────────────────┤
│ 0      │ 4    │ Magic         │ 0x184D2A5B (little-endian)          │
│ 4      │ 4    │ Frame Size    │ N (payload size, excludes header)   │
│ 8      │ N    │ Padding       │ Zero bytes                          │
└─────────────────────────────────────────────────────────────────────┘
Total size: 8 + N bytes
Minimum size: 8 bytes (when N = 0)
```

**Hex example** (minimum padding frame, 8 bytes):
```
5B 2A 4D 18  00 00 00 00
│           │
│           └── Frame size: 0 (no payload)
└── Magic: 0x184D2A5B (little-endian)
```

**Hex example** (padding frame with 100 bytes payload):
```
5B 2A 4D 18  64 00 00 00  00 00 00 00 ... (100 zero bytes)
│           │
│           └── Frame size: 100 (0x64)
└── Magic: 0x184D2A5B (little-endian)
```

### Start-of-Part (SOP) Frame

Placed at 8 MiB boundaries when a file's compressed data continues from the previous part. Contains the uncompressed offset needed for BTRFS_IOC_ENCODED_WRITE.

```
┌─────────────────────────────────────────────────────────────────────┐
│ Offset │ Size │ Field                │ Value                        │
├────────┼──────┼──────────────────────┼──────────────────────────────┤
│ 0      │ 4    │ Magic                │ 0x184D2A5B (little-endian)   │
│ 4      │ 4    │ Frame Size           │ 16 (fixed)                   │
│ 8      │ 1    │ Type Flag            │ 0x01 (Start-of-Part)         │
│ 9      │ 8    │ Uncompressed Offset  │ uint64_t, little-endian      │
│ 17     │ 7    │ Reserved             │ Zero bytes                   │
└─────────────────────────────────────────────────────────────────────┘
Total size: 24 bytes (fixed)
```

**Type flag values**:
- `0x00` - Padding frame (no meaningful payload)
- `0x01` - Start-of-Part frame (contains uncompressed offset)

**Hex example** (SOP frame with uncompressed offset = 8,650,752 = 0x840000):
```
5B 2A 4D 18  10 00 00 00  01  00 00 84 00 00 00 00 00  00 00 00 00 00 00 00
│           │            │   │                        │
│           │            │   │                        └── Reserved (7 bytes)
│           │            │   └── Uncompressed offset: 0x840000 (8,650,752)
│           │            └── Type: 0x01 (Start-of-Part)
│           └── Frame size: 16
└── Magic: 0x184D2A5B (little-endian)
```

### .burst-padding Local File Header

For "header-only" files (empty files, symlinks, directories), padding cannot be inserted into the compressed data stream. Instead, an unlisted Local File Header is used.

```
┌─────────────────────────────────────────────────────────────────────┐
│ Component            │ Size  │ Notes                                │
├──────────────────────┼───────┼──────────────────────────────────────┤
│ Local File Header    │ 30    │ Standard ZIP LFH structure           │
│ Filename             │ 14    │ ".burst_padding"                     │
│ Extra Field          │ var   │ Zero bytes to reach target size      │
└─────────────────────────────────────────────────────────────────────┘
Minimum size: 44 bytes (30 + 14)
```

**Key properties**:
- Filename: `.burst_padding`
- Compression method: STORE (0)
- Compressed/uncompressed size: 0
- **NOT listed in Central Directory**
- No Data Descriptor follows

Standard ZIP tools ignore these entries because they only process files listed in the Central Directory. 7-Zip has been observed to sometimes scan Local File Headers directly
and will extract them as 0-byte files; exclude with `-xr!.burst_padding`.

**When used**: Before header-only entries (empty files, symlinks, directories) when there isn't enough space before the next 8 MiB boundary for both the padding and the entry.

---

## BRST EOCD Comment

The End of Central Directory record includes an 8-byte BRST comment that optimizes Central Directory parsing.

```
┌─────────────────────────────────────────────────────────────────────┐
│ Offset │ Size │ Field              │ Value                          │
├────────┼──────┼────────────────────┼────────────────────────────────┤
│ 0      │ 4    │ Magic              │ 0x54535242 ("BRST" LE)         │
│ 4      │ 1    │ Version            │ 1                              │
│ 5      │ 3    │ First CDFH Offset  │ uint24_t, little-endian        │
└─────────────────────────────────────────────────────────────────────┘
Total size: 8 bytes
```

The **First CDFH Offset** is relative to the start of the "tail" (the last 8 MiB of the archive). This tells a downloader where the first complete Central Directory File Header begins within the tail buffer.

**Special values**:
| Value | Meaning |
|-------|---------|
| `0x000000` | First CDFH is at the start of the tail (entire CD fits in tail) |
| `0xFFFFFF` | No complete CDFH exists in the tail (sentinel) |

**Hex examples**:

CD fits entirely in the last 8 MiB:
```
42 52 53 54  01  00 00 00
│           │   │
│           │   └── Offset: 0 (CD starts at tail beginning)
│           └── Version: 1
└── Magic: "BRST" (0x54535242 little-endian)
```

First complete CDFH at offset 4096 into tail:
```
42 52 53 54  01  00 10 00
│           │   │
│           │   └── Offset: 0x001000 (4096)
│           └── Version: 1
└── Magic: "BRST" (0x54535242 little-endian)
```

---

## Standard ZIP Structures

BURST uses standard ZIP structures with the following specifics:

### Local File Header

```
┌─────────────────────────────────────────────────────────────────────┐
│ Offset │ Size │ Field                │ BURST Value                  │
├────────┼──────┼──────────────────────┼──────────────────────────────┤
│ 0      │ 4    │ Signature            │ 0x04034b50                   │
│ 4      │ 2    │ Version Needed       │ 63 (6.3 for Zstandard)       │
│ 6      │ 2    │ Flags                │ 0x0008 (data descriptor)     │
│ 8      │ 2    │ Compression Method   │ 93 (Zstandard)               │
│ 10     │ 2    │ Last Mod Time        │ DOS time                     │
│ 12     │ 2    │ Last Mod Date        │ DOS date                     │
│ 14     │ 4    │ CRC-32               │ 0 (deferred)                 │
│ 18     │ 4    │ Compressed Size      │ 0 (deferred)                 │
│ 22     │ 4    │ Uncompressed Size    │ 0 (deferred)                 │
│ 26     │ 2    │ Filename Length      │ n                            │
│ 28     │ 2    │ Extra Field Length   │ m                            │
│ 30     │ n    │ Filename             │ UTF-8 path                   │
│ 30+n   │ m    │ Extra Field          │ Optional fields              │
└─────────────────────────────────────────────────────────────────────┘
```

**Notes**:
- Flag bit 3 (0x0008) indicates sizes are in Data Descriptor
- CRC-32 and sizes are set to 0 in the header, computed during streaming

### Data Descriptor

Follows the compressed data for each file:

```
Standard (32-bit sizes):
┌─────────────────────────────────────────────────────────────────────┐
│ Offset │ Size │ Field              │                                │
├────────┼──────┼────────────────────┼────────────────────────────────┤
│ 0      │ 4    │ Signature          │ 0x08074b50                     │
│ 4      │ 4    │ CRC-32             │                                │
│ 8      │ 4    │ Compressed Size    │                                │
│ 12     │ 4    │ Uncompressed Size  │                                │
└─────────────────────────────────────────────────────────────────────┘
Total: 16 bytes

ZIP64 (64-bit sizes):
┌─────────────────────────────────────────────────────────────────────┐
│ Offset │ Size │ Field              │                                │
├────────┼──────┼────────────────────┼────────────────────────────────┤
│ 0      │ 4    │ Signature          │ 0x08074b50                     │
│ 4      │ 4    │ CRC-32             │                                │
│ 8      │ 8    │ Compressed Size    │                                │
│ 16     │ 8    │ Uncompressed Size  │                                │
└─────────────────────────────────────────────────────────────────────┘
Total: 24 bytes
```

### Central Directory File Header

Standard ZIP Central Directory header. Key fields for BURST:
- `local_header_offset` (offset 42): Archive offset where file's Local File Header begins
- `compressed_size` / `uncompressed_size`: Actual sizes (not deferred)
- `external_file_attributes`: Contains Unix mode bits (upper 16 bits)

### End of Central Directory

Standard ZIP EOCD with:
- `comment_length` = 8
- Comment = BRST comment (see above)

---

## Magic Number Reference

| Magic (LE) | Hex Bytes | Structure |
|------------|-----------|-----------|
| 0x04034b50 | `50 4B 03 04` | ZIP Local File Header |
| 0x08074b50 | `50 4B 07 08` | ZIP Data Descriptor |
| 0x02014b50 | `50 4B 01 02` | ZIP Central Directory Header |
| 0x06054b50 | `50 4B 05 06` | ZIP End of Central Directory |
| 0x06064b50 | `50 4B 06 06` | ZIP64 End of Central Directory |
| 0x07064b50 | `50 4B 06 07` | ZIP64 EOCD Locator |
| 0xFD2FB528 | `28 B5 2F FD` | Zstandard frame |
| 0x184D2A5B | `5B 2A 4D 18` | BURST skippable frame |
| 0x54535242 | `42 52 53 54` | BRST EOCD comment magic |

---

## Compatibility

BURST archives should be compatible with standard ZIP extractors:

| Tool | Compatibility | Notes |
|------|---------------|-------|
| 7-Zip (7zz) | Full | Requires Zstd support; may extract `.burst_padding` files |
| Info-ZIP | Full | Requires Zstd plugin |
| Python zipfile | Partial | Requires Zstd decompressor registration |

Currently however, testing only takes place against 7-Zip.
