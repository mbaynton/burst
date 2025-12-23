# BURST Archive Format Specification Plan

## Overview

Design and document a ZIP-based archive format for BURST (BTRFS Ultrafast Restore From S3 Transfers) that enables concurrent 8 MiB part downloads from S3 with immediate BTRFS_IOC_ENCODED_WRITE operations.

## Key Design Decisions

### 1. Aligned Format Approach ✓
- **Decision**: 8 MiB boundaries in archive MUST align to frame or file boundaries
- **Rationale**: Eliminates buffering during download, simplifies downloader, enables immediate writes
- **Cost**: ~0.8% archive size increase (padding), acceptable for performance gains

### 2. Minimal Metadata Storage ✓
- **Decision**: Store only starting points for each 8 MiB part, not per-frame metadata
- **Rationale**: Based on research findings:
  - `ZSTD_getFrameContentSize()` is O(1) - reads frame header only
  - `ZSTD_findFrameCompressedSize()` is O(num_blocks) but very fast
  - Sequential frame traversal adds negligible overhead (microseconds per 8 MiB part)
  - Example implementation at [loader.c](/home/mbaynton/projects/awslabs/btrfs_zstd/loader.c:59-110) demonstrates this pattern
- **Benefit**: Reduces metadata from ~32 bytes per frame to ~24 bytes per 8 MiB part (99% reduction)

### 3. Frame Size: 128 KiB ✓
- **Decision**: Target 128 KiB uncompressed per frame (BTRFS maximum)
- **Rationale**: Minimizes frame count, reduces metadata, maximizes compression efficiency

### 4. Critical Requirement: Content Size in Frame Headers
- **Requirement**: ALL Zstandard frames MUST include content size in frame header
- **Implementation**: Use `ZSTD_compress()` or `ZSTD_compressCCtx()` which include this by default
- **Verification**: Check `ZSTD_getFrameContentSize() != ZSTD_CONTENTSIZE_UNKNOWN` during creation

## Archive Format Specification

### Structure Overview

```
[ZIP Local Header 1]                    ← Offset 0
[File 1 Zstd Frame 1]
[File 1 Zstd Frame 2]
...
[Zstd Skippable Frame - Padding]       ← Align to 8 MiB boundary
[ZIP Local Header 2]                    ← Offset 8,388,608 (8 MiB)
[File 2 Zstd Frame 1]
...
[Zstd Skippable Frame - Padding]       ← Align to 16 MiB boundary
[Zstd Skippable Frame - Start-of-part metadata ]  ← Offset 16,777,216 (16 MiB)
...
[Central Directory Headers]
[End of Central Directory Record]
```

### Alignment Rule

Each 8 MiB boundary (0, 8,388,608, 16,777,216, ...) MUST begin with:
- A ZIP local file header, OR
- The start of a Zstandard frame (never mid-frame)

### Zstandard Frame Constraints

- **Maximum uncompressed size**: 128 KiB (BTRFS limit)
- **Minimum uncompressed size**: 4 KiB (practical BTRFS limit)
- **Compression level**: -15 to 15 (BTRFS requirement)
- **Frame independence**: Each frame is self-contained (no dictionary dependencies)
- **Content size**: MUST be present in frame header (critical for sequential traversal)

### Padding Strategy

In the loop where we add compressed data to the stream, before writing each frame,
check:
  * Is the reader reading the uncompressed file at EOF? If so, space_required
    to avoid breaking the Alignment Rule = (size of compressed frame) + (size of zip
    trailing data descriptor).
  * Else, space_required to avoid breaking the Alignment Rule = (size of
    compressed frame)
space_until_boundary = next_boundary - current_offset
if (space_until_boundary == space_required) then
  * write frame
  * if we are not at the end of a file, write a zstandard Start-of-Part Metadata Frame at the boundary, then resume normal adding comprssed frames to the stream
  * if we are at end of file, write the local file header for the next file at the boundary.
else,
  space_with_min_pad = space_required + MIN_SKIPPABLE_FRAME_SIZE 
  if (space_until_boundary < space_with_min_pad) then we need to write a zstandard skippable
  padding frame here until the boundary, then
  * if we are not at end of a file, write a zstandard Start-of-Part Metadata Frame
    at the boundary, then resume normal adding compressed frames to the stream, or
  * if we are at end of file, write the local file header for the next file at the boundary.

#### ZStandard skippable padding frame structure:
```
Offset  Size  Field                Value
0       4     Magic number         0x184D2A5B (little-endian, "BURST" marker)
4       4     Frame size           N (padding bytes excluding 8-byte header)
8       N     Padding data         Zeros
```
Thus MIN_SKIPPABLE_FRAME_SIZE = 8 bytes (when N = 0)

#### Start-of-Part Zstandard skippable Metadata Frame structure:
```
Offset  Size  Field                           Value
0       4     Magic number                    0x184D2A5B (little-endian, "BURST" marker)
4       4     Frame size                      16 bytes (fixed)
8       1     BURST info type flag            0x01 (nonzero to clearly differentiate from skippable padding frame; also allows for future extensibility)
9       8     Uncompressed offset             Initial value for uncompressed offset given to BTRFS
```

**Purpose**: This tells the downloader:
- What uncompressed offset to initialize for BTRFS_IOC_ENCODED_WRITE
- Enables immediate processing without needing to traverse from the start of the file

**When to insert**:
1. At the start of any 8 MiB part that continues data from a file in the previous part
2. Not needed if the part starts with a ZIP local file header (new file)

### ZIP Structure

#### Local File Header

Standard ZIP local file header:

```
Offset  Size  Field                        Value
0       4     Signature                    0x04034b50
4       2     Version needed               63 (6.3 = Zstandard support)
6       2     General purpose bit flag     0x0008 (data descriptor present)
8       2     Compression method           93 (Zstandard)
10-25   16    Various standard fields      (times, sizes deferred)
26      2     File name length             n
28      2     Extra field length           m
30      n     File name                    UTF-8 path
30+n    m     Extra field                  Standard fields only
```

**Note**: Use data descriptor (bit flag 0x0008) to defer sizes until after streaming data.

#### Central Directory Metadata

**Analysis**: Standard ZIP central directory already contains sufficient information:

Standard ZIP Central Directory Header fields:
- **Offset 42**: Relative offset of local header (4 bytes) - Archive offset where file starts
- **Offset 20**: Compressed size (4 bytes or 8 bytes with ZIP64)
- **Offset 24**: Uncompressed size (4 bytes or 8 bytes with ZIP64)

**Derived values**:
- **Part index**: `local_header_offset / 8_MiB` (integer division)
- **Compressed start offset**: `local_header_offset + local_header_size` (header + filename + extra)

**Purpose**: This minimal metadata enables the downloader to:
1. Identify which 8 MiB part contains the start of each file
2. Know the archive offset to begin sequential frame traversal
3. Initialize the uncompressed offset counter for BTRFS_IOC_ENCODED_WRITE


## Archive Writer Algorithm

### High-Level Flow

```
1. Initialize: current_offset = 0, current_part = 0

2. For each input file:
   a. Write ZIP local file header

   b. Compress file in 128 KiB chunks:
      For each chunk:
         - Compress with zstd (ensure content size in header)
	 - Decide based on size of compressed frame and the Padding Strategy
           whether to write the compressed frame, or some padding frame(s) first.
         - Advance current_offset

   d. Write data descriptor (CRC, sizes)

   e. Record metadata: (part_index, compressed_start_offset, uncompressed_start_offset)

3. Write central directory

4. Write end of central directory record
```

Important: Use Zip64 to support files larger than 4 GiB.


## Archive Downloader Algorithm

### High-Level Flow

```
1. Download end of S3 object (request last 8 MiB) containing central directory

2. Parse central directory:
   - Extract all file entries
   - Build map: part_index -> list of (file_path, compressed_start_offset, uncompressed_start_offset)

3. For each 8 MiB part (download concurrently):
   a. Issue S3 ranged GET: bytes=[part*8MiB, (part+1)*8MiB)

   b. For each file starting in this part:
      - Locate compressed data at offset_in_part = compressed_start_offset - (part * 8MiB)
      - Open/create target file
      - Traverse frames sequentially (see algorithm below)

4. Close all files, set permissions from ZIP attributes
```

### Sequential Frame Traversal Algorithm

Based on [loader.c](/home/mbaynton/projects/awslabs/btrfs_zstd/loader.c:59-110)

Note that this is conceptual only; actual implementation should use a stream rather than
loading the entire part into memory.

```c
void process_file_data(const uint8_t* part_data, size_t part_size,
                       size_t start_offset_in_part,
                       int target_fd,
                       uint64_t initial_uncompressed_offset) {
    const uint8_t* ptr = part_data + start_offset_in_part;
    size_t remaining = part_size - start_offset_in_part;
    uint64_t current_uncompressed_offset = initial_uncompressed_offset;

    while (remaining > 0) {
        // Check for ZIP local header (new file starting)
        if (remaining >= 4 && *(uint32_t*)ptr == 0x04034b50) {
            break;  // Reached next file
        }

        // Check for skippable frame (padding)
        if (remaining >= 4) {
            uint32_t magic = *(uint32_t*)ptr;
            if (magic >= 0x184D2A50 && magic <= 0x184D2A5F) {
                uint32_t frame_size = *((uint32_t*)(ptr + 4));
                ptr += 8 + frame_size;
                remaining -= 8 + frame_size;
                continue;
            }
        }

        // Get uncompressed size from frame header (O(1))
        unsigned long long uncompressed_size =
            ZSTD_getFrameContentSize(ptr, remaining);

        if (uncompressed_size == ZSTD_CONTENTSIZE_ERROR ||
            uncompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
            fprintf(stderr, "Error: frame missing content size\n");
            break;
        }

        // Get compressed size (O(num_blocks), fast)
        size_t compressed_size =
            ZSTD_findFrameCompressedSize(ptr, remaining);

        if (ZSTD_isError(compressed_size)) {
            fprintf(stderr, "Error finding frame boundary\n");
            break;
        }

        // Handle case where compression made data larger
        if (compressed_size > uncompressed_size) {
            // Decompress and write unencoded
            do_write_unencoded(target_fd, ptr, compressed_size,
                             uncompressed_size, current_uncompressed_offset);
        } else {
            // Write encoded to BTRFS
            do_write_encoded(target_fd, ptr, compressed_size,
                           uncompressed_size, current_uncompressed_offset);
        }

        // Advance to next frame
        ptr += compressed_size;
        remaining -= compressed_size;
        current_uncompressed_offset += uncompressed_size;
    }
}

int do_write_encoded(int fd, const uint8_t* zstd_frame, size_t frame_len,
                     unsigned long long unencoded_len, uint64_t file_offset) {
    struct iovec iov = {
        .iov_base = (void*)zstd_frame,
        .iov_len = frame_len
    };

    struct btrfs_ioctl_encoded_io_args args = {
        .iov = &iov,
        .iovcnt = 1,
        .offset = file_offset,
        .len = unencoded_len,
        .unencoded_len = unencoded_len,
        .unencoded_offset = 0,
        .compression = BTRFS_ENCODED_IO_COMPRESSION_ZSTD,
        .encryption = BTRFS_ENCODED_IO_ENCRYPTION_NONE,
    };

    return ioctl(fd, BTRFS_IOC_ENCODED_WRITE, &args);
}
```

### Concurrency Strategy

- Download 8-16 parts concurrently using aws-c-s3
- Each part processes independently
- Multiple threads can write to same file using different offsets (BTRFS handles locking)
- Track completion per part, retry on failure
- No ordering dependencies between parts

## ZIP Compatibility

### Standard ZIP Extractors (7-Zip-zstd, Info-ZIP)
- ✅ Correctly extracts all files
- ✅ Decompresses Zstandard frames transparently
- ✅ Ignores unknown extra fields (0x8577)
- ✅ Skips padding frames (Zstandard skippable frame property)
- ✅ Maintains full ZIP compatibility

### BURST Downloader Fallback
- If extra fields missing: fall back to sequential extraction
- Parse local headers and decompress normally
- Ensures data integrity over performance optimization


## Success Criteria

1. **Correctness**: Archives extractable by 7-Zip-zstd with identical content
2. **Performance**: 10× faster than sequential extraction on EC2 with S3
3. **Compatibility**: Standard ZIP tools work without modification
4. **Efficiency**: <1% archive size overhead
5. **Memory**: <256 MB memory usage for downloader (16 concurrent parts)
6. **Reliability**: Failed part downloads don't affect other parts

## References

- BTRFS_IOC_ENCODED_WRITE findings: [BTRFS_IOC_ENCODED_WRITE-findings.md](/home/mbaynton/projects/burst/BTRFS_IOC_ENCODED_WRITE-findings.md)
- Example implementation: [loader.c](/home/mbaynton/projects/awslabs/btrfs_zstd/loader.c)
- Zstandard RFC 8878: Frame format specification
- ZIP APPNOTE.TXT: ZIP file format specification
- aws-c-s3: https://github.com/awslabs/aws-c-s3/
