// kestrel/bench/main.c — sweep codec × level × block × direction over the corpus → CSV
#include "codec.h"
#include "corpus.h"
#include "metrics.h"
#include <stdio.h>
#include <stdlib.h>

static const int LEVELS[] = {1, 3, 6, 9};
static const size_t BLOCKS[] = {8192, 16384, 32768, 65536, 131072, 0}; // 0 = whole body

// Run `op` WARM+ITERS times over one buffer; record per-iteration wall latency
// (us) into lat[] and accumulate wall/cpu ns. Returns the count of successful
// (non-zero-returning) timed iterations — callers pass THAT to pctl(), never a
// fixed count, so an uninitialised lat[] slot is never read.
static int timed_pass(size_t (*op)(void *, const uint8_t *, size_t, uint8_t *, size_t),
                      void *ctx, const uint8_t *src, size_t slen, uint8_t *dst, size_t dcap,
                      double *lat, int iters, double *wall_ns, double *cpu_ns) {
    const int WARM = 20;
    int got = 0; *wall_ns = 0; *cpu_ns = 0;
    for (int it = 0; it < WARM + iters; it++) {
        double w0 = now_wall_ns(), p0 = now_cpu_ns();
        size_t r = op(ctx, src, slen, dst, dcap);
        double w1 = now_wall_ns(), p1 = now_cpu_ns();
        if (it >= WARM && r) { lat[got++] = (w1 - w0) / 1e3; *wall_ns += w1 - w0; *cpu_ns += p1 - p0; }
    }
    return got;
}

static void emit(FILE *csv, const char *codec, const char *corpus, const char *dir,
                 int level, size_t block, double ratio, size_t bytes,
                 double *lat, int got, double wall_ns, double cpu_ns) {
    if (got <= 0 || wall_ns <= 0) return;
    double p50, p99, p999; pctl(lat, got, &p50, &p99, &p999);
    double mbps = ((double)got * bytes / 1e6) / (wall_ns / 1e9);
    double cpu_frac = cpu_ns / wall_ns;
    double cores_freed = 1.0 - cpu_frac;
    if (cores_freed < 0) cores_freed = 0;
    if (cores_freed > 1) cores_freed = 1;
    fprintf(csv, "%s,%s,%s,%d,%zu,%.4f,%.1f,%.2f,%.2f,%.2f,%.3f,%.3f\n",
            codec, corpus, dir, level, block, ratio, mbps, p50, p99, p999, cpu_frac, cores_freed);
}

// One representative block of size m = min(nblk, body), clamped so no op reads
// past the body. block==0 means the whole body.
static void run_one(const codec_t *c, const char *corpus, body_t *b, int level, size_t block, FILE *csv) {
    size_t nblk = block ? block : b->len;
    size_t m = nblk < b->len ? nblk : b->len;
    if (m == 0) return;
    size_t cap = comp_bound(m);
    uint8_t *cmp = malloc(cap), *out = malloc(m);
    if (!cmp || !out) { free(cmp); free(out); return; }
    void *ctx = c->mk(level);
    if (!ctx) { free(cmp); free(out); return; }
    const int ITERS = 200;
    double *lat = malloc(ITERS * sizeof(double));
    if (!lat) { c->fr(ctx); free(cmp); free(out); return; }

    // reference compression → ratio
    size_t zref = c->compress(ctx, b->buf, m, cmp, cap);
    double ratio = zref ? (double)zref / m : 0;

    // compress row
    double cw, cc;
    int cg = timed_pass(c->compress, ctx, b->buf, m, cmp, cap, lat, ITERS, &cw, &cc);
    emit(csv, c->name, corpus, "compress", level, block, ratio, m, lat, cg, cw, cc);

    // decompress row (recompress once for a consistent cmp/z, then time decompress)
    if (c->can_decompress) {
        size_t z = c->compress(ctx, b->buf, m, cmp, cap);
        if (z) {
            double dw, dc;
            int dg = timed_pass(c->decompress, ctx, cmp, z, out, m, lat, ITERS, &dw, &dc);
            emit(csv, c->name, corpus, "decompress", level, block, ratio, m, lat, dg, dw, dc);
        }
    }
    free(lat); c->fr(ctx); free(cmp); free(out);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "../corpus";
    corpus_t cp;
    if (corpus_load(dir, &cp)) { fprintf(stderr, "no corpus in %s\n", dir); return 1; }
    FILE *csv = fopen("results.csv", "w");
    if (!csv) { perror("results.csv"); corpus_free(&cp); return 1; }
    fprintf(csv, "codec,corpus,direction,level,block,ratio,throughput_MBps,p50_us,p99_us,p999_us,cpu_frac,cores_freed\n");
    int nblocks = (int)(sizeof(BLOCKS) / sizeof(*BLOCKS));
    for (int i = 0; i < codec_count(); i++)
        for (int b = 0; b < cp.n; b++)
            for (int L = 0; L < 4; L++)
                for (int k = 0; k < nblocks; k++)
                    run_one(ALL_CODECS[i], cp.v[b].name, &cp.v[b], LEVELS[L], BLOCKS[k], csv);
    fclose(csv); corpus_free(&cp);
    printf("wrote results.csv\n");
    return 0;
}
