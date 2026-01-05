# BURST Downloader Guide

This document describes how a downloader can efficiently process BURST archives by leveraging the format's alignment guarantees for concurrent part processing.

## Overview

BURST archives are designed for concurrent extraction. The 8 MiB alignment rule ensures that:

1. Each part can be downloaded and processed independently
2. No buffering is required between parts
3. Parts can be processed in any order

Additionally, it is worth noting that BTRFS supports multiple threads writing to the same file
at different offsets, even for encoded writes via `BTRFS_IOC_ENCODED_WRITE`.

### Processing Phases

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           BURST Download Flow                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   Phase 1: Initialization                                                   │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │ 1. Request last 8 MiB of archive (HTTP Range or S3 ranged GET)      │   │
│   │ 2. Parse End of Central Directory + BRST comment                    │   │
│   │ 3. Parse Central Directory (fetch more if needed)                   │   │
│   │ 4. Build part → files mapping                                       │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                              ↓                                              │
│   Phase 2: Concurrent Download                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │              ┌──────────┐ ┌──────────┐ ┌──────────┐                 │   │
│   │              │  Part 0  │ │  Part 1  │ │  Part 2  │  ...            │   │
│   │              │ Download │ │ Download │ │ Download │                 │   │
│   │              │ Process  │ │ Process  │ │ Process  │                 │   │
│   │              │  Write   │ │  Write   │ │  Write   │                 │   │
│   │              └──────────┘ └──────────┘ └──────────┘                 │   │
│   │                    ↓           ↓           ↓                        │   │
│   │              (All parts processed concurrently)                     │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                              ↓                                              │
│   Phase 3: Finalization                                                     │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │ Set file permissions/ownership from Central Directory metadata      │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Phase 1: Initialization

### Step 1: Fetch the Tail

Download the last 8 MiB of the archive. This "tail" contains:
- The End of Central Directory (EOCD) record
- The BRST comment with optimization hint
- Usually the entire Central Directory

```
Request: GET archive.burst
         Range: bytes=-8388608   (last 8 MiB)
```

The size of the initial fetch must be >= 8MiB if the implementation wishes to utilize the BRST comment optimization
for partial central directory processing, since the comment contains an offset of a valid CDFH relative to the last
8MiB of the object.

The 8 MiB size is chosen because it provides a good balance between initiation of the bulk, concurrent phase of large
archives and avoidance of multiple round-trips for small archives.

### Step 2: Locate and Parse EOCD

Scan backward from the end of the tail to find the EOCD signature `0x06054b50`.

```c
// Pseudocode: Find EOCD
for (offset = tail_size - 22; offset >= 0; offset--) {
    if (read_u32_le(tail + offset) == 0x06054b50) {
        eocd_offset = offset;
        break;
    }
}
```

Extract the BRST comment from the EOCD:

```c
struct eocd {
    // ... standard fields ...
    uint16_t comment_length;  // Should be 8 for current v1 comment
};

// BRST comment immediately follows EOCD
struct brst_comment {
    uint32_t magic;           // 0x54535242 ("BRST")
    uint8_t  version;         // 1
    uint8_t  first_cdfh_offset[3];  // uint24, little-endian
};
```

### Step 3: Parse the Central Directory

The BRST comment's `first_cdfh_offset` tells you where the first complete Central Directory File Header (CDFH) begins within the tail buffer.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Tail Buffer (8 MiB)                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   If first_cdfh_offset = 0:                                                 │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │ CDFH 1 │ CDFH 2 │ ... │ CDFH N │ [ZIP64] │ EOCD + BRST              │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│   ↑                                                                         │
│   Start parsing here                                                        │
│                                                                             │
│   If first_cdfh_offset = 4096:                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │ (partial CDFH) │ CDFH 1 │ CDFH 2 │ ... │ EOCD + BRST                │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                    ↑                                                        │
│                    Start parsing here (offset 4096)                         │
│                                                                             │
│   If first_cdfh_offset = 0xFFFFFF:                                          │
│   Central Directory is larger than 8 MiB; fetch additional data             │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

Parse each CDFH to build file metadata:

```c
struct file_info {
    char    *path;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint64_t local_header_offset;  // Archive offset of Local File Header
    uint32_t crc32;
    uint32_t unix_mode;
    uint32_t uid, gid;
};
```

### Step 4: Build Part Mapping

For each file, calculate which part it starts in:

```c
uint32_t part_index = file->local_header_offset / 8388608;
```

Build a reverse mapping from part index to the files that start in that part:

```c
struct part_info {
    uint32_t part_index;
    struct file_info **files;      // Files starting in this part
    size_t num_files;
};
```

---

## Phase 2: Concurrent Part Processing

Each 8 MiB part can be processed independently. Launch concurrent workers (usually >=16 recommended on respectable ec2 instances) to download and process parts.

### Part Processing State Machine

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Part Processing State Machine                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌──────────────┐                                                          │
│   │ PART_START   │───────────────────────────────────────────────┐          │
│   └──────┬───────┘                                               │          │
│          │                                                       │          │
│          ▼                                                       │          │
│   ┌──────────────────────────────────────────────────────────┐   │          │
│   │ Read 4 bytes (magic number)                              │   │          │
│   └──────────────────────────────────────────────────────────┘   │          │
│          │                                                       │          │
│          ├──── 0x184D2A5B ────► BURST skippable frame            │          │
│          │                      │                                │          │
│          │                      ├─ type=0x01 → Start-of-Part     │          │
│          │                      │               Extract offset   │          │
│          │                      │               Continue file    │          │
│          │                      │                                │          │
│          │                      └─ type=0x00 → Padding frame     │          │
│          │                                      Skip payload     │          │
│          │                                      Loop back        │          │
│          │                                                       │          │
│          ├──── 0x04034b50 ────► Local File Header                │          │
│          │                      Open new file                    │          │
│          │                      Parse header                     │          │
│          │                      Begin processing frames          │          │
│          │                                                       │          │
│          ├──── 0xFD2FB528 ────► Zstandard frame                  │          │
│          │                      Extract & write                  │          │
│          │                      Loop back                        │          │
│          │                                                       │          │
│          ├──── 0x08074b50 ────► Data Descriptor                  │          │
│          │                      File complete                    │          │
│          │                      Loop back for next file          │          │
│          │                                                       │          │
│          └──── 0x02014b50 ────► Central Directory                │          │
│                                 STOP processing this part ───────┘          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Frame Detection

At any position in the part, read 4 bytes to identify the frame type:

| Magic (LE) | Frame Type | Action |
|------------|------------|--------|
| `0x04034b50` | Local File Header | New file begins |
| `0xFD2FB528` | Zstandard frame | Compressed data to extract |
| `0x184D2A5B` | BURST skippable | Check type byte at offset +8 |
| `0x08074b50` | Data Descriptor | Current file ends |
| `0x02014b50` | Central Directory | Stop processing part |

### Processing Zstandard Frames

For each Zstandard frame:

```c
// Get uncompressed size from frame header (O(1) operation)
uint64_t uncompressed_size = ZSTD_getFrameContentSize(frame_ptr, remaining);

// Get compressed size (fast, O(num_blocks))
size_t compressed_size = ZSTD_findFrameCompressedSize(frame_ptr, remaining);

// Decide: encoded write or decompress?
if (compressed_size <= uncompressed_size) {
    // Write compressed frame directly to BTRFS
    write_encoded(fd, frame_ptr, compressed_size,
                  uncompressed_size, current_uncompressed_offset);
} else {
    // Compression expanded; decompress and write normally
    decompress_and_write(fd, frame_ptr, compressed_size,
                         uncompressed_size, current_uncompressed_offset);
}

// Advance position
frame_ptr += compressed_size;
current_uncompressed_offset += uncompressed_size;
```

### Handling Start-of-Part Frames

When a file continues from a previous part, the part begins with a Start-of-Part frame containing the uncompressed offset:

```c
if (magic == 0x184D2A5B) {
    uint32_t frame_size = read_u32_le(ptr + 4);
    uint8_t type = ptr[8];

    if (type == 0x01) {  // Start-of-Part
        // Extract uncompressed offset (bytes 9-16)
        uint64_t uncompressed_offset = read_u64_le(ptr + 9);

        // Initialize file context for continuing file
        current_file->uncompressed_offset = uncompressed_offset;
    }

    // Skip the frame
    ptr += 8 + frame_size;
}
```

This offset is critical for BTRFS_IOC_ENCODED_WRITE - it tells BTRFS where in the file to write the decompressed data.

### Handling Files Spanning Parts

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                       File Spanning Multiple Parts                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   Part 0                          Part 1                                    │
│   ┌────────────────────────┐     ┌────────────────────────┐                 │
│   │ LFH (large_file.dat)   │     │ SOP Frame              │                 │
│   │ Frame 0: offset=0      │     │   offset = 8,126,464   │                 │
│   │ Frame 1: offset=131072 │     │ Frame 63: offset=...   │                 │
│   │ ...                    │     │ Frame 64               │                 │
│   │ Frame 62               │     │ ...                    │                 │
│   │ Padding Frame          │     │ Data Descriptor        │                 │
│   └────────────────────────┘     └────────────────────────┘                 │
│                                                                             │
│   Part 0 knows:                  Part 1 knows:                              │
│   - File starts here             - SOP says offset = 8,126,464              │
│   - Initial offset = 0           - Continue writing at that offset          │
│   - Write until boundary         - Write until Data Descriptor              │
│                                                                             │
│   Both parts can process concurrently without coordination!                 │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Phase 3: Finalization

After all parts are processed:

1. **Verify file integrity** - Compare CRC-32 from Data Descriptors with computed values
2. **Set permissions** - Unix mode from Central Directory's `external_file_attributes`
3. **Set ownership** - uid/gid from Info-ZIP Unix extra field (0x7875)
4. **Set timestamps** - Convert DOS datetime from Central Directory

---

## Implementation Considerations

### Memory Management

- Stream parts directly; don't buffer entire parts in memory
- Use memory-mapped I/O for output files when possible
- Frame buffer only needs to hold one Zstandard frame (max ~128 KiB compressed)

### Concurrency

- 8-16 concurrent part downloads recommended for S3
- Multiple threads can write to the same file at different offsets
- Use file descriptor per thread, or pwrite() for position-independent writes

### Error Handling

- Failed part downloads can be retried independently
- Validate CRC-32 after all parts are complete
- Handle incomplete files (delete or mark as failed)

### Optimization Tips

1. **Prefetch parts** - Start downloading part N+1 while processing part N
2. **Use encoded writes** - When possible, let BTRFS store compressed data directly

---

## Example: Processing a Single Part

```c
void process_part(uint8_t *data, size_t size, struct part_info *part,
                  struct file_context *continuing_file) {
    uint8_t *ptr = data;
    size_t remaining = size;
    struct file_context *current = continuing_file;

    while (remaining >= 4) {
        uint32_t magic = read_u32_le(ptr);

        switch (magic) {
        case 0x184D2A5B: {  // BURST skippable frame
            uint32_t frame_size = read_u32_le(ptr + 4);
            uint8_t type = ptr[8];

            if (type == 0x01) {  // Start-of-Part
                uint64_t offset = read_u64_le(ptr + 9);
                current->uncompressed_offset = offset;
            }
            // Skip frame (padding or SOP)
            ptr += 8 + frame_size;
            remaining -= 8 + frame_size;
            break;
        }

        case 0x04034b50: {  // Local File Header
            current = open_file_from_header(ptr, remaining);
            size_t header_size = get_lfh_size(ptr);
            ptr += header_size;
            remaining -= header_size;
            break;
        }

        case 0xFD2FB528: {  // Zstandard frame
            uint64_t uncomp_size = ZSTD_getFrameContentSize(ptr, remaining);
            size_t comp_size = ZSTD_findFrameCompressedSize(ptr, remaining);

            write_frame(current, ptr, comp_size, uncomp_size);
            current->uncompressed_offset += uncomp_size;

            ptr += comp_size;
            remaining -= comp_size;
            break;
        }

        case 0x08074b50: {  // Data Descriptor
            finalize_file(current, ptr);
            current = NULL;
            size_t desc_size = get_descriptor_size(ptr);
            ptr += desc_size;
            remaining -= desc_size;
            break;
        }

        case 0x02014b50:  // Central Directory
            return;  // Done with this part

        default:
            // Unknown magic - error
            return;
        }
    }
}
```

---

## References

- [BURST Format Specification](burst-format.md) - Binary format details
- [Zstandard RFC 8878](https://datatracker.ietf.org/doc/html/rfc8878) - Frame format
- [ZIP APPNOTE.TXT](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT) - ZIP specification
- [BTRFS_IOC_ENCODED_WRITE](https://btrfs.readthedocs.io/en/latest/ioctl/encoded-io.html) - BTRFS encoded I/O
