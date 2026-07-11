// kestrel/bench/codec.h
#ifndef KESTREL_CODEC_H
#define KESTREL_CODEC_H
#include <stddef.h>
#include <stdint.h>

typedef struct codec_s {
    const char *name;          // e.g. "sw-zstd", "qat-zstd", "iaa-deflate"
    int can_decompress;        // 0 for qat-zstd (seqprod is compress-only)
    void *(*mk)(int level);    // make context at a fixed level; NULL on failure
    size_t (*compress)(void *ctx, const uint8_t *src, size_t n, uint8_t *dst, size_t cap);
    size_t (*decompress)(void *ctx, const uint8_t *src, size_t n, uint8_t *dst, size_t cap);
    void (*fr)(void *ctx);
} codec_t;

extern const codec_t *ALL_CODECS[]; // NULL-terminated
int codec_count(void);              // number of non-NULL entries

size_t comp_bound(size_t n);        // safe compressed-size upper bound
#endif
