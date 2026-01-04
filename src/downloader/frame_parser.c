#include "frame_parser.h"
#include "zip_structures.h"

#include <string.h>
#include <zstd.h>
#include <zstd_errors.h>

// Minimum bytes needed to identify frame type
#define MIN_FRAME_HEADER_SIZE 4

// Parse frame at current position and identify its type and size
int parse_next_frame(const uint8_t *buffer, size_t buffer_len, struct frame_info *info)
{
    if (buffer_len < MIN_FRAME_HEADER_SIZE) {
        return STREAM_PROC_NEED_MORE_DATA;
    }

    memset(info, 0, sizeof(*info));

    // Read 4-byte magic number (little-endian)
    uint32_t magic;
    memcpy(&magic, buffer, sizeof(magic));

    if (magic == ZIP_LOCAL_FILE_HEADER_SIG) {
        info->type = FRAME_ZIP_LOCAL_HEADER;
        // Need at least the fixed header to determine total size
        if (buffer_len < sizeof(struct zip_local_header)) {
            return STREAM_PROC_NEED_MORE_DATA;
        }
        const struct zip_local_header *lfh = (const struct zip_local_header *)buffer;
        info->frame_size = sizeof(struct zip_local_header) +
                          lfh->filename_length + lfh->extra_field_length;
        return STREAM_PROC_SUCCESS;
    }

    if (magic == ZIP_DATA_DESCRIPTOR_SIG) {
        info->type = FRAME_ZIP_DATA_DESCRIPTOR;
        info->frame_size = sizeof(struct zip_data_descriptor);
        return STREAM_PROC_SUCCESS;
    }

    if (magic == ZIP_CENTRAL_DIR_HEADER_SIG) {
        // Central Directory reached - signals end of file data in this part
        info->type = FRAME_ZIP_CENTRAL_DIRECTORY;
        info->frame_size = 0;  // Not consumed - just signals end
        return STREAM_PROC_SUCCESS;
    }

    if (magic == ZSTD_MAGIC_NUMBER) {
        info->type = FRAME_ZSTD_COMPRESSED;

        // Use ZSTD functions to get frame info
        size_t frame_size = ZSTD_findFrameCompressedSize(buffer, buffer_len);
        if (ZSTD_isError(frame_size)) {
            if (frame_size == (size_t)-ZSTD_error_srcSize_wrong) {
                return STREAM_PROC_NEED_MORE_DATA;
            }
            return STREAM_PROC_ERR_INVALID_FRAME;
        }

        unsigned long long content_size = ZSTD_getFrameContentSize(buffer, buffer_len);
        if (content_size == ZSTD_CONTENTSIZE_ERROR) {
            return STREAM_PROC_ERR_INVALID_FRAME;
        }
        if (content_size == ZSTD_CONTENTSIZE_UNKNOWN) {
            // BURST archives require content size in frame header
            return STREAM_PROC_ERR_INVALID_FRAME;
        }

        info->frame_size = frame_size;
        info->uncompressed_size = content_size;
        return STREAM_PROC_SUCCESS;
    }

    if (magic == BURST_SKIPPABLE_MAGIC) {
        // Need 8 bytes minimum (magic + size)
        if (buffer_len < 8) {
            return STREAM_PROC_NEED_MORE_DATA;
        }

        uint32_t payload_size;
        memcpy(&payload_size, buffer + 4, sizeof(payload_size));
        info->frame_size = 8 + payload_size;  // Header + payload

        // Need full frame to check type byte
        if (buffer_len < info->frame_size) {
            return STREAM_PROC_NEED_MORE_DATA;
        }

        // Check type byte at offset 8 (first byte of payload)
        if (payload_size > 0) {
            uint8_t type_byte = buffer[8];
            if (type_byte == BURST_TYPE_START_OF_PART && payload_size == 16) {
                info->type = FRAME_BURST_START_OF_PART;
                // Extract uncompressed offset (bytes 9-16)
                memcpy(&info->start_of_part_offset, buffer + 9, sizeof(uint64_t));
            } else {
                info->type = FRAME_BURST_PADDING;
            }
        } else {
            info->type = FRAME_BURST_PADDING;
        }
        return STREAM_PROC_SUCCESS;
    }

    // Unknown magic
    info->type = FRAME_UNKNOWN;
    return STREAM_PROC_ERR_INVALID_FRAME;
}
