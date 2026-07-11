// kestrel/bench/iaa_scale.c — SUSTAINED, COLD-CACHE offload at scale.
//
// iaa_offload.c hammered one 1.3MB body (cache-hot). This tool builds a multi-GB pool of
// the real bodies tiled at distinct addresses, then streams the accelerator over DISTINCT
// cold tiles for a sustained duration — so throughput reflects memory-bound reality at
// "hundreds of millions of rows" scale, not an L2-resident buffer. Same async queue-depth
// model as iaa_offload.c (proven), just over a cold working set for N seconds.
//
// Build on the box:
//   cc -O2 -std=c11 -o iaa_scale iaa_scale.c corpus.c metrics.c -ldeflate
//      -L/usr/local/lib -lqpl -laccel-config -lstdc++ -ldl -lpthread
// Run (needs ~pool_gb + up to 8 GB of RAM; IAA work queues need access → sudo):
//   sudo ./iaa_scale ../corpus [pool_gb=8] [dur_s=3] | tee scale.csv
// pool_gb 8-16 is already far past cache (representative sustained-cold); much larger just
// adds TLB/NUMA page-walk overhead that real MB-sized bodies from warm buffers don't see.
// The decompress (compressed) pool is capped at 8 GB internally — cold but RAM-bounded.
#include "corpus.h"
#include "metrics.h"
#include <libdeflate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "qpl/qpl.h"

static const int QDS[] = {1, 8, 16, 32};
static const int NQD = (int)(sizeof(QDS) / sizeof(QDS[0]));

typedef struct { uint64_t off; uint32_t len; } tile_t; // slice into a pool

static size_t dbound(size_t n) { return n + n / 2 + 1024; }

static void setup_c(qpl_job *j, const uint8_t *s, uint32_t n, uint8_t *d, uint32_t cap) {
    j->op = qpl_op_compress; j->level = qpl_default_level;
    j->next_in_ptr = (uint8_t *)s; j->available_in = n; j->next_out_ptr = d; j->available_out = cap;
    j->total_in = 0; j->total_out = 0;
    j->flags = QPL_FLAG_FIRST | QPL_FLAG_LAST | QPL_FLAG_DYNAMIC_HUFFMAN | QPL_FLAG_OMIT_VERIFY;
}
static void setup_d(qpl_job *j, const uint8_t *s, uint32_t n, uint8_t *d, uint32_t cap) {
    j->op = qpl_op_decompress; j->level = qpl_default_level;
    j->next_in_ptr = (uint8_t *)s; j->available_in = n; j->next_out_ptr = d; j->available_out = cap;
    j->total_in = 0; j->total_out = 0;
    j->flags = QPL_FLAG_FIRST | QPL_FLAG_LAST;
}

// Sustained async pass over cold tiles. Returns GB/s of ORIGINAL bytes; sets *cpu_frac.
static double async_scale(int is_comp, int qd, uint32_t job_sz, const uint8_t *pool,
                          const tile_t *tiles, const uint32_t *origlen, int T,
                          uint32_t out_cap, double dur_s, double *cpu_frac) {
    qpl_job **jobs = calloc(qd, sizeof *jobs);
    uint8_t **outs = calloc(qd, sizeof *outs);
    int *busy = calloc(qd, sizeof *busy), *stile = calloc(qd, sizeof *stile);
    if (!jobs || !outs || !busy || !stile) { free(jobs); free(outs); free(busy); free(stile); return -1; }
    for (int i = 0; i < qd; i++) {
        jobs[i] = malloc(job_sz); outs[i] = malloc(out_cap);
        if (!jobs[i] || !outs[i] || qpl_init_job(qpl_path_hardware, jobs[i]) != QPL_STS_OK) {
            for (int k = 0; k <= i; k++) { if (jobs[k]) { qpl_fini_job(jobs[k]); free(jobs[k]); } free(outs[k]); }
            free(jobs); free(outs); free(busy); free(stile); return -1;
        }
        busy[i] = 0;
    }
    long cursor = 0; unsigned long long bytes = 0;
    double t0w = now_wall_ns(), t0c = now_cpu_ns(), deadline = t0w + dur_s * 1e9;
    int err = 0;
    while (now_wall_ns() < deadline && !err) {
        for (int i = 0; i < qd; i++) {
            if (!busy[i]) {
                int t = (int)(cursor++ % T); stile[i] = t;
                const uint8_t *in = pool + tiles[t].off;
                if (is_comp) setup_c(jobs[i], in, tiles[t].len, outs[i], out_cap);
                else         setup_d(jobs[i], in, tiles[t].len, outs[i], out_cap);
                qpl_status s = qpl_submit_job(jobs[i]);
                if (s == QPL_STS_OK) busy[i] = 1;
                else if (s != QPL_STS_QUEUES_ARE_BUSY_ERR) { fprintf(stderr, "submit err %d\n", (int)s); err = 1; break; }
            } else {
                qpl_status s = qpl_check_job(jobs[i]);
                if (s == QPL_STS_OK) { bytes += origlen[stile[i]]; busy[i] = 0; }
                else if (s != QPL_STS_BEING_PROCESSED) { fprintf(stderr, "check err %d\n", (int)s); err = 1; break; }
            }
        }
    }
    double t1w = now_wall_ns(), t1c = now_cpu_ns();
    for (int i = 0; i < qd; i++) if (busy[i]) qpl_wait_job(jobs[i]);
    for (int i = 0; i < qd; i++) { qpl_fini_job(jobs[i]); free(jobs[i]); free(outs[i]); }
    free(jobs); free(outs); free(busy); free(stile);
    if (err || bytes == 0) { *cpu_frac = 1.0; return -1; }
    double wall = (t1w - t0w) / 1e9;
    *cpu_frac = (t1c - t0c) / (t1w - t0w);
    return (double)bytes / wall / 1e9;
}

// software single-core sustained reference over the same cold tiles. Returns GB/s original.
static double sw_scale(int is_comp, const uint8_t *pool, const tile_t *tiles,
                       const uint32_t *origlen, int T, uint8_t *scratch, size_t scap, double dur_s) {
    double t0 = now_wall_ns(), deadline = t0 + dur_s * 1e9; unsigned long long bytes = 0; long cursor = 0;
    if (is_comp) {
        struct libdeflate_compressor *c = libdeflate_alloc_compressor(6);
        if (!c) return -1;
        while (now_wall_ns() < deadline) { int t = (int)(cursor++ % T); libdeflate_deflate_compress(c, pool + tiles[t].off, tiles[t].len, scratch, scap); bytes += tiles[t].len; }
        libdeflate_free_compressor(c);
    } else {
        struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
        if (!d) return -1;
        while (now_wall_ns() < deadline) { int t = (int)(cursor++ % T); size_t o = 0; libdeflate_deflate_decompress(d, pool + tiles[t].off, tiles[t].len, scratch, scap, &o); bytes += origlen[t]; }
        libdeflate_free_decompressor(d);
    }
    double wall = (now_wall_ns() - t0) / 1e9;
    return (double)bytes / wall / 1e9;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "../corpus";
    double pool_gb = argc > 2 ? atof(argv[2]) : 8.0;
    double dur_s = argc > 3 ? atof(argv[3]) : 3.0;
    corpus_t cp;
    if (corpus_load(dir, &cp) != 0) { fprintf(stderr, "corpus load failed: %s\n", dir); return 1; }
    uint32_t job_sz;
    if (qpl_get_job_size(qpl_path_hardware, &job_sz) != QPL_STS_OK) { fprintf(stderr, "qpl_get_job_size failed\n"); return 1; }

    size_t POOL = (size_t)(pool_gb * (double)(1ull << 30));
    size_t maxlen = 0;
    for (int i = 0; i < cp.n; i++) if (cp.v[i].len > maxlen) maxlen = cp.v[i].len;

    // 1) uncompressed pool: real bodies tiled round-robin until POOL is full.
    uint8_t *upool = malloc(POOL);
    if (!upool) { fprintf(stderr, "cannot alloc %.1f GB uncompressed pool\n", pool_gb); return 1; }
    int tcap = 1024, T = 0; tile_t *ut = malloc(tcap * sizeof *ut);
    size_t off = 0;
    for (int bi = 0; ; bi++) {
        body_t *B = &cp.v[bi % cp.n];
        if (off + B->len > POOL) break;
        if (T == tcap) { tcap *= 2; ut = realloc(ut, tcap * sizeof *ut); }
        memcpy(upool + off, B->buf, B->len);
        ut[T].off = off; ut[T].len = (uint32_t)B->len; off += B->len; T++;
    }

    // 2) compressed pool: IAA-compress each tile once (cold-decompress source).
    size_t CCAP = POOL / 2 + (1u << 26);          // compressed <= ~0.42x, so half the pool holds it,
    if (CCAP > (8ull << 30)) CCAP = 8ull << 30;   // but cap the decompress pool at 8 GB (>> cache = cold, RAM-bounded)
    uint8_t *cpool = malloc(CCAP);
    uint32_t *origlen = malloc((size_t)T * sizeof *origlen);
    tile_t *ct = malloc((size_t)T * sizeof *ct);
    if (!cpool || !origlen || !ct) { fprintf(stderr, "alloc failed\n"); return 1; }
    qpl_job *pj = malloc(job_sz); size_t coff = 0; int Tc = 0;
    if (!pj || qpl_init_job(qpl_path_hardware, pj) != QPL_STS_OK) { fprintf(stderr, "qpl_init_job failed\n"); return 1; }
    for (int t = 0; t < T; t++) {
        if (coff + dbound(ut[t].len) > CCAP) break;
        // available_out is a uint32_t — pass the per-tile bound, NOT (CCAP-coff) which overflows 32 bits.
        setup_c(pj, upool + ut[t].off, ut[t].len, cpool + coff, (uint32_t)dbound(ut[t].len));
        if (qpl_execute_job(pj) != QPL_STS_OK) { fprintf(stderr, "prep compress failed at tile %d\n", t); break; }
        ct[Tc].off = coff; ct[Tc].len = (uint32_t)pj->total_out; origlen[Tc] = ut[t].len;
        coff += pj->total_out; Tc++;
    }
    qpl_fini_job(pj); free(pj);
    if (Tc == 0) { fprintf(stderr, "no compressed tiles built\n"); return 1; }

    double eff_gb = (double)off / (double)(1ull << 30);
    fprintf(stderr, "pool=%.1f GB (%d tiles), compressed pool=%.2f GB (%d tiles), dur=%.1fs/pass\n",
            eff_gb, T, (double)coff / (double)(1ull << 30), Tc, dur_s);

    uint8_t *scratch = malloc(dbound(maxlen));
    if (!scratch) { fprintf(stderr, "scratch alloc failed\n"); return 1; }
    printf("direction,pool_gb,dur_s,queue_depth,iaa_GBps,cpu_frac,sw_GBps,effective_cores\n");
    for (int dirn = 0; dirn < 2; dirn++) {
        int is_comp = (dirn == 0);
        const uint8_t *pool = is_comp ? upool : cpool;
        const tile_t *tiles = is_comp ? ut : ct;
        int nT = is_comp ? T : Tc;
        uint32_t out_cap = is_comp ? (uint32_t)dbound(maxlen) : (uint32_t)(maxlen + 64);
        double sw = sw_scale(is_comp, pool, tiles, origlen, nT, scratch, dbound(maxlen), dur_s);
        for (int q = 0; q < NQD; q++) {
            double cf = 1.0;
            double agg = async_scale(is_comp, QDS[q], job_sz, pool, tiles, origlen, nT, out_cap, dur_s, &cf);
            double e = (sw > 0 && agg > 0) ? agg / sw : 0;
            printf("%s,%.1f,%.1f,%d,%.2f,%.3f,%.3f,%.1f\n",
                   is_comp ? "compress" : "decompress", eff_gb, dur_s, QDS[q], agg, cf, sw, e);
            fflush(stdout);
        }
    }
    free(scratch); free(cpool); free(upool); free(ut); free(ct); free(origlen);
    corpus_free(&cp);
    return 0;
}
