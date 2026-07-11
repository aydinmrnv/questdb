// kestrel/bench/iaa_offload.c — measure IAA core-freeing via ASYNC queue-depth submission.
//
// The sync harness (codec_iaa.c via qpl_execute_job) busy-polls, so cpu_frac≈1.0 and
// cores_freed reads ~0 even though the accelerator did the work. This tool answers the
// real question: from ONE submitting thread, how much aggregate throughput can IAA drive
// at queue depth Q, and how much of that one core does it cost? effective_cores =
// iaa_aggregate / software_single_core is "how many software cores one IAA-driving core replaces".
//
// Build on the box (QPL is a static libqpl.a in /usr/local, needs -lstdc++):
//   cc -O2 -std=c11 -o iaa_offload iaa_offload.c corpus.c metrics.c
//      -ldeflate -L/usr/local/lib -lqpl -laccel-config -lstdc++ -ldl -lpthread
// Run (IAA work queues need access — use sudo):
//   sudo ./iaa_offload ../corpus > offload.csv
#include "corpus.h"
#include "metrics.h"
#include <libdeflate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "qpl/qpl.h"

static const int QDS[] = {1, 2, 4, 8, 16};
static const int NQD = (int)(sizeof(QDS) / sizeof(QDS[0]));
static const double BUDGET_NS = 200000000.0; // 200 ms of steady-state per (body,dir,qd)

static size_t dbound(size_t n) { return n + n / 2 + 1024; } // generous deflate bound

static void setup_c(qpl_job *j, const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap) {
    j->op = qpl_op_compress; j->level = qpl_default_level;
    j->next_in_ptr = (uint8_t *)src; j->available_in = n;
    j->next_out_ptr = dst; j->available_out = cap;
    j->total_in = 0; j->total_out = 0;
    j->flags = QPL_FLAG_FIRST | QPL_FLAG_LAST | QPL_FLAG_DYNAMIC_HUFFMAN | QPL_FLAG_OMIT_VERIFY;
}
static void setup_d(qpl_job *j, const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap) {
    j->op = qpl_op_decompress; j->level = qpl_default_level;
    j->next_in_ptr = (uint8_t *)src; j->available_in = n;
    j->next_out_ptr = dst; j->available_out = cap;
    j->total_in = 0; j->total_out = 0;
    j->flags = QPL_FLAG_FIRST | QPL_FLAG_LAST;
}

// One async queue-depth pass. is_comp: 1=compress the body, 0=decompress `comp`.
// Returns aggregate MB/s (input bytes for compress, original bytes for decompress); sets *cpu_frac.
static double async_pass(int is_comp, int qd, uint32_t job_sz,
                         const uint8_t *in, uint32_t in_len,   // src (body) for comp, comp buf for decomp
                         uint32_t unit_bytes,                  // bytes credited per completed op
                         uint32_t out_cap, double *cpu_frac) {
    qpl_job **jobs = calloc(qd, sizeof *jobs);
    uint8_t **outs = calloc(qd, sizeof *outs);
    int *inflight = calloc(qd, sizeof *inflight);
    if (!jobs || !outs || !inflight) { free(jobs); free(outs); free(inflight); return -1; }
    for (int i = 0; i < qd; i++) {
        jobs[i] = malloc(job_sz);
        outs[i] = malloc(out_cap);
        if (!jobs[i] || !outs[i] || qpl_init_job(qpl_path_hardware, jobs[i]) != QPL_STS_OK) {
            for (int k = 0; k <= i; k++) { if (jobs[k]) { qpl_fini_job(jobs[k]); free(jobs[k]); } free(outs[k]); }
            free(jobs); free(outs); free(inflight); return -1;
        }
        inflight[i] = 0;
    }
    long done = 0;
    double t0w = now_wall_ns(), t0c = now_cpu_ns(), deadline = t0w + BUDGET_NS;
    int err = 0;
    while (now_wall_ns() < deadline && !err) {
        for (int i = 0; i < qd; i++) {
            if (!inflight[i]) {
                if (is_comp) setup_c(jobs[i], in, in_len, outs[i], out_cap);
                else         setup_d(jobs[i], in, in_len, outs[i], out_cap);
                qpl_status s = qpl_submit_job(jobs[i]);
                if (s == QPL_STS_OK) inflight[i] = 1;
                else if (s != QPL_STS_QUEUES_ARE_BUSY_ERR) { fprintf(stderr, "submit err %d\n", (int)s); err = 1; break; }
            } else {
                qpl_status s = qpl_check_job(jobs[i]);
                if (s == QPL_STS_OK) { done++; inflight[i] = 0; }
                else if (s != QPL_STS_BEING_PROCESSED) { fprintf(stderr, "check err %d\n", (int)s); err = 1; break; }
            }
        }
    }
    double t1w = now_wall_ns(), t1c = now_cpu_ns();
    // drain outstanding jobs so the next pass starts clean
    for (int i = 0; i < qd; i++) if (inflight[i]) qpl_wait_job(jobs[i]);
    for (int i = 0; i < qd; i++) { qpl_fini_job(jobs[i]); free(jobs[i]); free(outs[i]); }
    free(jobs); free(outs); free(inflight);
    if (err || done == 0) { *cpu_frac = 1.0; return -1; }
    double wall = (t1w - t0w) / 1e9;
    *cpu_frac = (t1c - t0c) / (t1w - t0w);
    return (double)done * (double)unit_bytes / wall / 1e6;
}

// software single-core reference (libdeflate). is_comp uses level 6; decompress reads `comp`.
static double sw_ref(int is_comp, const uint8_t *body, size_t blen,
                     const uint8_t *comp, size_t clen, uint8_t *scratch, size_t scap) {
    double t0 = now_wall_ns(), deadline = t0 + BUDGET_NS; long reps = 0;
    if (is_comp) {
        struct libdeflate_compressor *c = libdeflate_alloc_compressor(6);
        if (!c) return -1;
        while (now_wall_ns() < deadline) { libdeflate_deflate_compress(c, body, blen, scratch, scap); reps++; }
        libdeflate_free_compressor(c);
        double wall = (now_wall_ns() - t0) / 1e9;
        return (double)reps * blen / wall / 1e6;
    } else {
        struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
        if (!d) return -1;
        while (now_wall_ns() < deadline) { size_t o = 0; libdeflate_deflate_decompress(d, comp, clen, scratch, scap, &o); reps++; }
        libdeflate_free_decompressor(d);
        double wall = (now_wall_ns() - t0) / 1e9;
        return (double)reps * blen / wall / 1e6;
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "../corpus";
    corpus_t cp;
    if (corpus_load(dir, &cp) != 0) { fprintf(stderr, "corpus load failed: %s\n", dir); return 1; }
    uint32_t job_sz;
    if (qpl_get_job_size(qpl_path_hardware, &job_sz) != QPL_STS_OK) { fprintf(stderr, "qpl_get_job_size failed\n"); return 1; }

    printf("body,direction,queue_depth,iaa_agg_MBps,cpu_frac,iaa_ratio,sw_single_MBps,effective_cores\n");
    for (int b = 0; b < cp.n; b++) {
        const uint8_t *body = cp.v[b].buf; size_t blen = cp.v[b].len;
        size_t cbound = dbound(blen);
        uint8_t *comp = malloc(cbound), *scratch = malloc(blen + 64);
        if (!comp || !scratch) { free(comp); free(scratch); continue; }

        // Prep: one IAA compress to get a valid raw-deflate stream + the IAA ratio.
        qpl_job *pj = malloc(job_sz); size_t clen = 0; double iratio = 0;
        if (pj && qpl_init_job(qpl_path_hardware, pj) == QPL_STS_OK) {
            setup_c(pj, body, (uint32_t)blen, comp, (uint32_t)cbound);
            if (qpl_execute_job(pj) == QPL_STS_OK) { clen = pj->total_out; iratio = (double)clen / blen; }
            qpl_fini_job(pj);
        }
        free(pj);
        if (clen == 0) { fprintf(stderr, "prep compress failed for %s\n", cp.v[b].name); free(comp); free(scratch); continue; }

        for (int dirn = 0; dirn < 2; dirn++) { // 0=compress, 1=decompress
            int is_comp = (dirn == 0);
            double sw = sw_ref(is_comp, body, blen, comp, clen, scratch, blen + 64);
            for (int q = 0; q < NQD; q++) {
                double cf = 1.0;
                double agg = is_comp
                    ? async_pass(1, QDS[q], job_sz, body, (uint32_t)blen, (uint32_t)blen, (uint32_t)cbound, &cf)
                    : async_pass(0, QDS[q], job_sz, comp, (uint32_t)clen, (uint32_t)blen, (uint32_t)(blen + 64), &cf);
                double eff = (sw > 0 && agg > 0) ? agg / sw : 0;
                printf("%s,%s,%d,%.1f,%.3f,%.4f,%.1f,%.2f\n",
                       cp.v[b].name, is_comp ? "compress" : "decompress", QDS[q],
                       agg, cf, iratio, sw, eff);
            }
        }
        free(comp); free(scratch);
    }
    corpus_free(&cp);
    return 0;
}
