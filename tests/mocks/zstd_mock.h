// Subset of zstd.h with only functions used by burst_writer.c
// This avoids mocking the entire massive zstd.h header
#ifndef ZSTD_MOCK_H
#define ZSTD_MOCK_H

#include <stddef.h>

// Opaque context type
typedef struct ZSTD_CCtx_s ZSTD_CCtx;

// Functions used by burst_writer.c (from grep results)
ZSTD_CCtx* ZSTD_createCCtx(void);
size_t ZSTD_freeCCtx(ZSTD_CCtx* cctx);
size_t ZSTD_compressBound(size_t srcSize);
size_t ZSTD_compress(void* dst, size_t dstCapacity,
                     const void* src, size_t srcSize,
                     int compressionLevel);
unsigned ZSTD_isError(size_t code);
const char* ZSTD_getErrorName(size_t code);
unsigned long long ZSTD_getFrameContentSize(const void* src, size_t srcSize);

// Constants used in burst_writer.c
#define ZSTD_CONTENTSIZE_UNKNOWN   (0ULL - 1)
#define ZSTD_CONTENTSIZE_ERROR     (0ULL - 2)

#endif // ZSTD_MOCK_H
