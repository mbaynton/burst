# BURST Implementation Plan (C)

## Overview
Implement both BURST archive writer and downloader in C for maximum performance, using aws-c-s3 for optimal EC2→S3 transfers.

## Language Choice: C
- **Rationale**: Direct aws-c-s3 integration, proven performance, straightforward BTRFS ioctl usage
- **Priority**: Maximum performance over development convenience

---

## Component 1: BURST Archive Writer

### Purpose
Create 8 MiB-aligned ZIP archives with Zstandard compression optimized for concurrent S3 downloads and BTRFS_IOC_ENCODED_WRITE.

### Architecture

#### Core Components
1. **ZIP Structure Writer**
   - Local file headers
   - Central directory generation
   - End of central directory record
   - ZIP64 support for files >4 GiB
   - Data descriptors (flag 0x0008)

2. **Zstandard Frame Controller**
   - Compress in 128 KiB chunks (BTRFS maximum)
   - Use `ZSTD_compress()` to ensure content size in frame headers
   - Verify with `ZSTD_getFrameContentSize() != ZSTD_CONTENTSIZE_UNKNOWN`
   - Maintain frame independence (no dictionary dependencies)

3. **Alignment Engine**
   - Track current write offset
   - Implement padding strategy from format-plan.md:69-86
   - Insert Zstandard skippable frames (magic 0x184D2A5B)
   - Insert Start-of-Part metadata frames at 8 MiB boundaries

4. **Stream Writer**
   - Buffered output for efficiency
   - Position tracking for alignment calculations
   - Support for writing to file or stdout

#### Implementation Strategy

**Phase 1: Basic ZIP Writer** (2-3 days)
- Implement ZIP local header writing
- Implement central directory generation
- Add data descriptor support
- Test with uncompressed files (method 0) first
- Validate with 7-Zip, Info-ZIP

**Phase 2: Zstandard Integration** (2-3 days)
- Link with libzstd
- Implement 128 KiB chunk compression
- Verify content size in frame headers
- Test compression levels (-15 to 15)
- Handle cases where compressed > uncompressed

**Phase 3: Alignment Logic** (2-3 days)
- Implement 8 MiB boundary detection
- Add skippable padding frame generation
- Add Start-of-Part metadata frame generation
- Implement padding strategy decision logic
- Test alignment correctness

**Phase 4: ZIP64 Support** (2 days)
- Add ZIP64 extra field generation
- Support files >4 GiB
- Test with large files

**Phase 5: Integration Testing** (3-4 days)
- Test end-to-end with sample files
- Verify 8 MiB alignment with hex dumps
- Test with 7-Zip-zstd extraction
- Validate metadata correctness
- Test edge cases (tiny files, huge files, exact boundary fits)

#### Key Data Structures

```c
struct burst_writer {
    FILE *output;
    uint64_t current_offset;
    uint32_t current_part;
    ZSTD_CCtx *zstd_ctx;

    // File tracking
    struct file_entry *files;
    size_t num_files;
    size_t files_capacity;

    // Buffering
    uint8_t *write_buffer;
    size_t buffer_size;
    size_t buffer_used;
};

struct file_entry {
    char *filename;
    uint64_t local_header_offset;
    uint64_t compressed_start_offset;
    uint64_t uncompressed_start_offset;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint32_t crc32;
    uint16_t compression_method;
};
```

#### Dependencies
- **libzstd**: Zstandard compression (Debian: libzstd-dev)
- **zlib**: CRC32 calculation (or implement manually)
- **Standard C library**: File I/O, memory management

---

## Component 2: BURST Archive Downloader

### Purpose
Download BURST archives from S3 with 8-16 concurrent part downloads, immediately write Zstandard frames to BTRFS using encoded writes.

### Architecture

#### Core Components
1. **AWS-C-S3 Client**
   - Initialize client with optimal configuration
   - Obtain central directory part (last 8 MiB) and supply to central directory parser
   - Buffer data preceding central directory for later extraction after we download the final part
   - If s3 response headers indicate total obejct size is larger than central directory part, issue concurrent ranged GET requests for each 8 MiB part from start to data already fetched.
   - Handle streaming callbacks
   - Manage retries and failures

2. **Central Directory Parser**
   Given a pointer to a byte buffer ending in the end of a zip file,
   - Parse end of central directory record
   - Parse central directory headers
   - Build map: part_index → files starting in that part

3. **Stream Processor**
   - Sequential frame traversal (based on loader.c pattern)
   - Parse ZIP local headers
   - Detect and skip skippable frames
   - Extract Zstandard frames
   - Handle frames spanning callback boundaries

4. **BTRFS Writer**
   - Open/create target files with correct modes
   - Call BTRFS_IOC_ENCODED_WRITE with proper args
   - Handle cases where compressed > uncompressed (write unencoded)
   - Manage file offsets per file

#### Implementation Strategy

**Phase 1: AWS-C-S3 Integration** (3-4 days)
- Create src/reader, create basic main.c
- Determine how best to build aws-c-s3 functionality relevant to our needs into our project or link to it
- Initialize aws-c-s3 client with optimal config
- Add typical S3 client configuration options to main() (e.g., region, credentials, endpoint)
- Implement basic GET request with Range header
- Test streaming callback with simple S3 object
- Test downloading last 8 MiB of a BURST archive (negative range header), determine total object size from response headers

**Phase 2: Central Directory Parsing** (2-3 days)
- Implement EOCD record parser
- Implement central directory header parser
- Implement unit tests based on known ZIP structures to achieve nearly full code coverage of central directory parsing
- Build part_index → file list mapping
- Test with small (< 8 MiB) zip files created by the `zip` tool
- Test with BURST archives from writer

**Phase 3: Stream Processing** (3-4 days)
- Implement sequential frame traversal algorithm that can start from any 8 MiB boundary and determine required inputs to BTRFS_IOC_ENCODED_WRITE for each non-skippable zstandard frame encountered.
  - Use `ZSTD_getFrameContentSize()` and `ZSTD_findFrameCompressedSize()`
- Parse ZIP local headers
- Detect skippable frames (padding and metadata)
- Define function signature for performing a BTRFS_IOC_ENCODED_WRITE ioctl call, and verify expected calls and parameters
for known inputs (byte streams, parsed central directories) using unit tests and CMock.

**Phase 4: Integrate phases 1 - 3** (2-3 days)
- Update downloader main() to:
  - Download central directory part
  - Parse central directory
  - Issue serial ranged GET requests for each 8 MiB part
  - Invoke stream processor in the callback for each part's data
- Test with actual AWS S3 bucket (infrastructure for actual S3 integration testing already exists)
- Test with actual BTRFS filesystem
- Verify valid extraction by downloading BURST archives with aws CLI and extracting with 7zz to a temporary location for comparison

**Phase 5: Concurrency Management** (2-3 days)
- Implement per-part context tracking
- Handle completion tracking
- Implement retry logic for failed parts
- Add progress reporting
- Test with 8-16 concurrent parts

**Phase 6: Add upload support to writer** (2-3 days)
- Integrate aws-c-s3 into writer component
- Implement S3 PUT with multipart upload
- Update existing e2e tests to use writer's uploader instead of aws CLI for upload

**Phase 7: Integration Testing** (3-5 days)
- End-to-end test: writer → S3 → downloader → BTRFS
- Include compressible files to exercise encoded writes, uncompressible files to exercise unencoded writes
- Verify file integrity (checksums)
- Test error conditions (network failures, disk full, etc.)

**Phase 7: Optimization and Cleanup** (2 days)
- Investigate whether our s3_client_config is always optimal for any ec2 instance type,
  or whether we should support tuning based on the s3-platform_info facility in aws-c-s3.
- Compare central directory offset and crc32 data with the local file header data as an integrity check.
- Add logging and metrics
  * max frame buffer size in stream_processor -- shouldn't need to buffer more than a few frames at once
- Add a detectable signature to the central directory indicating that the archive was generated by BURST writer.
  Then, when the downloader pulls the central directory part, it can fall back to normal zip extraction if the signature is missing.

#### Key Data Structures

```c
struct burst_downloader {
    struct aws_s3_client *s3_client;
    struct aws_allocator *allocator;

    // S3 object info
    char *bucket;
    char *key;
    uint64_t object_size;

    // Central directory metadata
    struct file_metadata *files;
    size_t num_files;

    // Part tracking (map: part_index → part_context)
    struct part_context **parts;
    size_t num_parts;

    // Configuration
    size_t max_concurrent_parts;
    char *output_dir;
};

struct part_context {
    uint32_t part_index;
    uint64_t part_start_offset;
    uint64_t part_end_offset;

    // Files that start in this part
    struct file_context **files;
    size_t num_files;

    // Frame boundary buffering
    uint8_t *frame_buffer;
    size_t frame_buffer_capacity;
    size_t frame_buffer_used;

    // Status
    bool completed;
    int retry_count;
};

struct file_context {
    char *filename;
    int fd;  // BTRFS file descriptor
    uint64_t current_uncompressed_offset;
    uint64_t expected_uncompressed_size;
};

struct file_metadata {
    char *filename;
    uint64_t local_header_offset;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint32_t part_index;  // Derived: local_header_offset / 8MiB
};
```

#### AWS-C-S3 Configuration

```c
struct aws_s3_client_config client_config = {
    .max_active_connections_override = 16,
    .memory_limit_in_bytes = 256 * 1024 * 1024,  // 256 MB
    .part_size = 8 * 1024 * 1024,  // 8 MiB
    .throughput_target_gbps = 10.0,  // Optimize for 10 Gbps (EC2 enhanced networking)
    .enable_read_backpressure = false,  // BTRFS writes are fast enough
};
```

#### Dependencies
- **aws-c-s3**: S3 client (requires full AWS CRT stack)
  - aws-c-common
  - aws-c-io
  - aws-c-http
  - aws-c-auth
  - aws-c-cal
  - aws-c-compression
- **libzstd**: Frame introspection functions
- **Linux kernel headers**: For BTRFS_IOC_ENCODED_WRITE

---

## Build System

### CMakeLists.txt Structure

```cmake
cmake_minimum_required(VERSION 3.15)
project(burst C)

set(CMAKE_C_STANDARD 11)

# Find dependencies
find_package(ZLIB REQUIRED)
find_package(aws-c-s3 REQUIRED)
find_package(zstd REQUIRED)

# Archive writer
add_executable(burst-writer
    src/writer/main.c
    src/writer/zip_writer.c
    src/writer/zstd_frame.c
    src/writer/alignment.c
    src/writer/crc32.c
)
target_link_libraries(burst-writer PRIVATE zstd::zstd ZLIB::ZLIB)

# Archive downloader
add_executable(burst-downloader
    src/downloader/main.c
    src/downloader/s3_client.c
    src/downloader/central_dir_parser.c
    src/downloader/stream_processor.c
    src/downloader/btrfs_writer.c
)
target_link_libraries(burst-downloader PRIVATE
    AWS::aws-c-s3
    zstd::zstd
)
```

---

## Development Timeline

### Archive Writer
- **Phase 1-2**: 4-6 days (Basic ZIP + Zstandard)
- **Phase 3-4**: 4-5 days (Alignment + ZIP64)
- **Phase 5**: 3-4 days (Testing)
- **Total**: ~2 weeks

### Archive Downloader
- **Phase 1-2**: 5-7 days (AWS-C-S3 + Parsing)
- **Phase 3-4**: 5-7 days (Stream processing + BTRFS)
- **Phase 5-6**: 5-8 days (Concurrency + Testing)
- **Total**: ~3 weeks

**Overall Project**: 5-6 weeks for both components

---

## Testing Strategy

### Unit Tests
- ZIP structure generation
- Zstandard frame creation with content size
- Alignment calculations
- Central directory parsing
- Frame traversal logic

### Integration Tests
- Writer: Generate archives, extract with 7-Zip-zstd, verify contents
- Downloader: Upload test archives to S3, download, verify file integrity
- End-to-end: Write → Upload → Download → Compare checksums

### Performance Tests
- Writer: Measure compression throughput (MB/s)
- Downloader: Measure download + write throughput on EC2
- Target: 10× speedup vs sequential extraction

### Edge Cases
- Empty files
- Tiny files (<4 KiB)
- Huge files (>4 GiB, requiring ZIP64)
- Frames that exactly fit 8 MiB boundaries
- Frames spanning multiple callback invocations
- Network failures and retries
- Partial downloads and resume

---

## Success Criteria

1. **Correctness**: Archives extractable by 7-Zip-zstd with identical content
2. **Alignment**: All 8 MiB boundaries align to ZIP headers or Zstandard frame starts
3. **Performance**: 10× faster than sequential extraction on EC2 with S3
4. **Compatibility**: Standard ZIP tools work without modification
5. **Efficiency**: <1% archive size overhead from padding
6. **Memory**: <256 MB memory usage for downloader (16 concurrent parts)
7. **Reliability**: Failed part downloads don't affect other parts

---

## Risk Mitigation

### Writer Risks
- **Complex alignment logic**: Mitigate with extensive unit tests and hex dump validation
- **ZIP format correctness**: Mitigate by testing with multiple ZIP tools
- **Zstandard frame format**: Mitigate by verifying with zstd CLI tool

### Downloader Risks
- **AWS-C-S3 complexity**: Mitigate by starting with simple examples, building incrementally
- **Frame boundary handling**: Mitigate with careful buffer management, unit tests
- **BTRFS ioctl failures**: Mitigate with comprehensive error handling, fallback to unencoded writes
- **Concurrent write races**: Mitigate by testing thoroughly, leveraging BTRFS locking

---

## Next Steps

1. Set up development environment (aws-c-s3, libzstd)
2. Start with writer Phase 1 (basic ZIP)
3. Build incrementally, test at each phase
4. Begin downloader once writer produces valid archives
5. Integrate and performance test on EC2
