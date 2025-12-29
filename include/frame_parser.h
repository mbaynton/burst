#ifndef FRAME_PARSER_H
#define FRAME_PARSER_H

#include "stream_processor.h"  // For struct frame_info, error codes, magic numbers

/**
 * @file frame_parser.h
 * @brief Frame parser for BURST archive stream processing.
 *
 * This module provides frame detection and parsing for the BURST archive format.
 * It identifies different frame types (ZIP headers, Zstandard frames, BURST
 * skippable frames) and returns their metadata.
 *
 * This is separated from stream_processor.c to allow direct unit testing.
 */

/**
 * Parse the next frame from buffer.
 *
 * Identifies the frame type based on the 4-byte magic number and parses
 * the frame header to determine the total frame size and relevant metadata.
 *
 * @param buffer Input buffer containing frame data
 * @param buffer_len Length of available data in buffer
 * @param info Output frame information (type, size, etc.)
 * @return STREAM_PROC_SUCCESS on success,
 *         STREAM_PROC_NEED_MORE_DATA if more bytes needed,
 *         STREAM_PROC_ERR_INVALID_FRAME if frame is corrupted or unknown
 */
int parse_next_frame(const uint8_t *buffer, size_t buffer_len, struct frame_info *info);

#endif // FRAME_PARSER_H
