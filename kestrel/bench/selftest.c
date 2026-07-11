// kestrel/bench/selftest.c
#include "codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Semi-realistic buffer: runs + noise (compressible but not trivial).
static size_t fill(uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)((i * 1103515245u + 12345u) >> 16) ^ (i / 97);
    return n;
}
int main(void) {
    size_t n = 64 * 1024; uint8_t *in = malloc(n), *cmp = malloc(comp_bound(n)), *out = malloc(n);
    if(!in||!cmp||!out){ fprintf(stderr,"OOM\n"); return 2; }
    fill(in, n);
    int fails = 0;
    for (int i = 0; i < codec_count(); i++) {
        const codec_t *c = ALL_CODECS[i];
        void *ctx = c->mk(6); if (!ctx) { printf("SKIP %-12s (mk failed)\n", c->name); continue; }
        size_t z = c->compress(ctx, in, n, cmp, comp_bound(n));
        if (!z) { printf("FAIL %-12s compress=0\n", c->name); fails++; c->fr(ctx); continue; }
        if (c->can_decompress) {
            size_t d = c->decompress(ctx, cmp, z, out, n);
            if (d != n || memcmp(in, out, n)) { printf("FAIL %-12s roundtrip\n", c->name); fails++; }
            else printf("OK   %-12s ratio=%.3f\n", c->name, (double)z / n);
        } else printf("OK   %-12s ratio=%.3f (compress-only)\n", c->name, (double)z / n);
        c->fr(ctx);
    }
    printf("%s\n", fails ? "SELFTEST FAILED" : "SELFTEST PASSED");
    return fails ? 1 : 0;
}
