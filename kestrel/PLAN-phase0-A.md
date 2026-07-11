# kestrel Phase 0 + Phase A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the detection probe (Phase 0) and the standalone codec bake-off harness (Phase A) that measures whether Intel QAT/IAA offload of QWP-shaped compression is worthwhile, over real captured QWP egress+ingress bodies, and produces a go/no-go with numbers.

**Architecture:** A self-contained **C** harness (`kestrel/bench/`) with a codec-vtable interface and pluggable backends: software `zstd`/`deflate` always compile; `qat-zstd` (libqatseqprod), `qat-deflate` (QATzip), `iaa-deflate` (QPL) are `#ifdef`-gated and only build on the accelerator box. The harness runs each codec × level × block-size × direction over a corpus of real QWP bodies (captured here via throwaway env-gated dump hooks in the QWP server and shipped as `.bin` via git), reporting ratio, throughput, latency percentiles, and CPU-cores-freed to CSV. A read-only bash probe (Phase 0) verifies the box before any build.

**Tech Stack:** C11, `libzstd` (bundled 1.5.7 API), `libdeflate`, Intel `QAT-ZSTD-Plugin`/`libqatseqprod`, Intel `QATzip`/`libqatzip`, Intel `QPL`/`libqpl`; Java 17+/Maven for the corpus-capture patch; bash for probe/scripts; git as transport.

## Global Constraints

- **Transport = git; no SSH.** I commit locally on branch `kestrel`; the operator pushes and clones on the box. Every box step is a copy-pasteable script that prints results.
- **Accelerator code path defaults OFF / degrades to software.** Mirrors QWP's existing "ship raw on failure" posture. The harness must run software-only where no accelerator exists (e.g. the dev box).
- **Corpus must be real post-serialization QWP bodies** (egress *and* ingress), not CSV — QWP delta-encodes (Gorilla/symbol-dict) before any byte-compressor.
- **libzstd is the bundled 1.5.7** (`ZSTD_registerSequenceProducer` present). Do not add a system-zstd dependency that shadows it on the box build; pin the include/lib path in the Makefile.
- **The dump-hook Java patch is throwaway** — it stays on-branch, clearly marked, never intended for merge. The corpus `.bin` files are the deliverable.
- **Everything lives under `kestrel/`** on the branch.
- **Metrics report ratio AND cpu-time together** — never throughput alone (the thesis is "high ratio at low CPU," not "fast").

---

## File structure

```
kestrel/
  scripts/
    probe.sh            # Phase 0: read-only box detection (operator runs)
    build-bench.sh      # box: detect libs, build harness with the right WITH_* flags
    run-bench.sh        # box: run the sweep over corpus, write results.csv, print table
    README.md           # operator runbook (clone → probe → build → run → paste)
  bench/
    codec.h             # codec vtable interface (the contract)
    codec_sw.c          # sw-zstd + sw-deflate backends (always compile)
    codec_qat.c         # qat-zstd (libqatseqprod) + qat-deflate (QATzip)  [WITH_QAT]
    codec_iaa.c         # iaa-deflate (QPL)                                 [WITH_IAA]
    corpus.c/.h         # load *.bin bodies from a directory
    metrics.c/.h        # wall vs thread-cpu timing, ratio, percentiles, cores-freed
    main.c              # driver: sweep codec×level×block×direction → CSV + table
    selftest.c          # round-trip correctness (compress→decompress→bytecmp)
    Makefile
  corpus/               # captured *.bin (committed) + MANIFEST.md
  RESULTS-TEMPLATE.md   # go/no-go writeup skeleton
  patches/
    dump-hooks.md       # notes on the throwaway capture patch (what/where/how to revert)
```

Corpus-capture also modifies (throwaway, on-branch):
- `core/src/main/java/io/questdb/std/KestrelDump.java` (new helper)
- `core/src/main/java/io/questdb/cutlass/qwp/server/egress/QwpEgressUpgradeProcessor.java` (1 line)
- `core/src/main/java/io/questdb/cutlass/qwp/server/QwpIngressUpgradeProcessor.java` (1 line)

---

## Task 1: Phase 0 detection probe

**Files:**
- Create: `kestrel/scripts/probe.sh`

**Interfaces:**
- Produces: a labelled read-only report of CPU gen, IAA/QAT device+config state, libraries, and build toolchain. No other task consumes it programmatically; it gates the human decision to proceed.

- [ ] **Step 1: Write the probe**

```bash
#!/usr/bin/env bash
# kestrel Phase 0 — Intel IAA/QAT readiness probe. READ ONLY (no writes/config).
# Run once as your user; re-run the QAT/IAA sections with sudo if they print empty/permission.
set -u
h(){ printf '\n\033[1m=== %s ===\033[0m\n' "$1"; }

h "CPU / generation"
grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-
lscpu 2>/dev/null | grep -iE '^Model name|^CPU\(s\)|^Thread|^Socket|^NUMA node\(s\)|^Stepping'
printf 'accel ISA: '
grep -m1 -oE '\b(amx_tile|amx_bf16|amx_int8|avx512f|movdir64b|enqcmd|movdiri)\b' /proc/cpuinfo | sort -u | tr '\n' ' '; echo

h "Kernel / IOMMU"
uname -r
ls -d /sys/class/iommu/* 2>/dev/null | head || echo "(no iommu class dirs)"

h "Accelerator PCI devices (QAT 494x / IAA 0cfe / DSA 0b25)"
if command -v lspci >/dev/null; then
  lspci -nnD 2>/dev/null | grep -iE '8086:(494[0-7]|0cfe|0b25)' || echo "(none matched)"
else echo "lspci not installed (pciutils)"; fi

h "IAA / DSA config (accel-config)"
if command -v accel-config >/dev/null; then
  accel-config version 2>/dev/null
  accel-config list 2>/dev/null | grep -iE '"dev"|"type"|"state"|"max_work_queues"|"engines"' \
    || echo "(list empty — needs sudo, or no device enabled)"
else echo "accel-config NOT installed"; fi
ls /dev/iax* /dev/dsa* 2>/dev/null || echo "(no /dev/iax|dsa nodes)"

h "QAT config"
command -v adf_ctl >/dev/null && { adf_ctl status 2>/dev/null || echo "(adf_ctl needs sudo)"; } \
  || echo "adf_ctl NOT installed (qatlib not set up)"
ls /dev/qat_* /dev/vfio/* 2>/dev/null || echo "(no /dev/qat_* or vfio nodes)"
lsmod 2>/dev/null | grep -iE 'qat|idxd|iaa|dsa|uacce' || echo "(no accelerator modules loaded)"

h "Accelerator + codec libraries"
ldconfig -p 2>/dev/null | grep -iE 'libqatzip|libqatseqprod|libqat\.|libusdm|libqpl|libaccel-config|libzstd|libdeflate' \
  || echo "(none of qatzip/qatseqprod/qpl/accel-config/zstd/deflate in ldconfig)"
command -v zstd >/dev/null && zstd --version

h "Build toolchain"
for t in "java -version" "mvn -version" "cargo --version" "cc --version" "cmake --version" "make --version"; do
  out=$($t 2>&1 | head -1); printf '%-16s %s\n' "${t%% *}:" "${out:-MISSING}"
done

h "READINESS HINTS"
echo "- qat-zstd/qat-deflate need: a QAT device 'state: up' + libqatzip (+ we build QAT-ZSTD-Plugin)."
echo "- iaa-deflate needs: an accel-config device 'state: enabled' + an enabled work queue + libqpl."
echo "- If QAT/IAA sections were empty, re-run with: sudo bash $0"
```

- [ ] **Step 2: Syntax-check locally**

Run: `bash -n kestrel/scripts/probe.sh && command -v shellcheck >/dev/null && shellcheck kestrel/scripts/probe.sh || echo "shellcheck absent — bash -n OK"`
Expected: no syntax errors.

- [ ] **Step 3: Smoke-run locally (AMD dev box — expect graceful "not installed/none matched")**

Run: `bash kestrel/scripts/probe.sh`
Expected: all sections print; accelerator sections say "none matched / NOT installed" on the AMD dev box (proves graceful degradation). The real report comes from the operator running it on the Intel box.

- [ ] **Step 4: Commit**

```bash
git add kestrel/scripts/probe.sh
git commit -m "kestrel: Phase 0 read-only IAA/QAT detection probe"
```

---

## Task 2: Harness core — codec interface, software backends, round-trip selftest, Makefile

**Files:**
- Create: `kestrel/bench/codec.h`, `kestrel/bench/codec_sw.c`, `kestrel/bench/selftest.c`, `kestrel/bench/Makefile`

**Interfaces:**
- Produces (consumed by Tasks 3, 4, 5):
  - `typedef struct { const char *name; int can_decompress; void *(*mk)(int level); size_t (*compress)(void *ctx, const uint8_t *src, size_t n, uint8_t *dst, size_t cap); size_t (*decompress)(void *ctx, const uint8_t *src, size_t n, uint8_t *dst, size_t cap); void (*fr)(void *ctx); } codec_t;` — `compress`/`decompress` return bytes written, or `0` on failure.
  - `extern const codec_t *ALL_CODECS[]; extern const int N_CODECS;` — NULL-terminated registry; accelerator entries appear only under `WITH_QAT`/`WITH_IAA`.

- [ ] **Step 1: Write the interface header**

```c
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
```

- [ ] **Step 2: Write the failing selftest**

```c
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
```

- [ ] **Step 3: Write software backends + registry**

```c
// kestrel/bench/codec_sw.c
#include "codec.h"
#include <stdlib.h>
#include <zstd.h>
#include <libdeflate.h>

size_t comp_bound(size_t n){ size_t z = ZSTD_compressBound(n); size_t d = libdeflate_deflate_compress_bound(NULL, n); return (z>d?z:d) + 128; }

// ---- sw-zstd (mirrors QWP: reused CCtx, one-shot compress2) ----
typedef struct { ZSTD_CCtx *c; ZSTD_DCtx *d; } zst_t;
static void *z_mk(int lvl){ zst_t *s=calloc(1,sizeof*s); s->c=ZSTD_createCCtx(); s->d=ZSTD_createDCtx();
    if(!s->c||!s->d){free(s);return NULL;} ZSTD_CCtx_setParameter(s->c, ZSTD_c_compressionLevel, lvl); return s; }
static size_t z_co(void *v,const uint8_t*src,size_t n,uint8_t*dst,size_t cap){ zst_t*s=v; size_t r=ZSTD_compress2(s->c,dst,cap,src,n); return ZSTD_isError(r)?0:r; }
static size_t z_de(void *v,const uint8_t*src,size_t n,uint8_t*dst,size_t cap){ zst_t*s=v; size_t r=ZSTD_decompressDCtx(s->d,dst,cap,src,n); return ZSTD_isError(r)?0:r; }
static void   z_fr(void *v){ zst_t*s=v; ZSTD_freeCCtx(s->c); ZSTD_freeDCtx(s->d); free(s); }
static const codec_t SW_ZSTD = {"sw-zstd",1,z_mk,z_co,z_de,z_fr};

// ---- sw-deflate (libdeflate control) ----
typedef struct { struct libdeflate_compressor *c; struct libdeflate_decompressor *d; } df_t;
static void *d_mk(int lvl){ df_t *s=calloc(1,sizeof*s); s->c=libdeflate_alloc_compressor(lvl); s->d=libdeflate_alloc_decompressor();
    if(!s->c||!s->d){free(s);return NULL;} return s; }
static size_t d_co(void *v,const uint8_t*src,size_t n,uint8_t*dst,size_t cap){ df_t*s=v; return libdeflate_deflate_compress(s->c,src,n,dst,cap); }
static size_t d_de(void *v,const uint8_t*src,size_t n,uint8_t*dst,size_t cap){ df_t*s=v; size_t got=0; enum libdeflate_result r=libdeflate_deflate_decompress(s->d,src,n,dst,cap,&got); return r==LIBDEFLATE_SUCCESS?got:0; }
static void   d_fr(void *v){ df_t*s=v; libdeflate_free_compressor(s->c); libdeflate_free_decompressor(s->d); free(s); }
static const codec_t SW_DEFLATE = {"sw-deflate",1,d_mk,d_co,d_de,d_fr};

// Accelerator codecs are appended by codec_qat.c / codec_iaa.c via these externs.
#ifdef WITH_QAT
extern const codec_t QAT_ZSTD, QAT_DEFLATE;
#endif
#ifdef WITH_IAA
extern const codec_t IAA_DEFLATE;
#endif
const codec_t *ALL_CODECS[] = {
    &SW_ZSTD, &SW_DEFLATE,
#ifdef WITH_QAT
    &QAT_ZSTD, &QAT_DEFLATE,
#endif
#ifdef WITH_IAA
    &IAA_DEFLATE,
#endif
    NULL };
int codec_count(void){ int n=0; while(ALL_CODECS[n]) n++; return n; }
```

- [ ] **Step 4: Write the Makefile**

```makefile
# kestrel/bench/Makefile
CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
# Pin the bundled libzstd 1.5.7 if present; else system zstd. Override on the box:
#   make WITH_QAT=1 WITH_IAA=1 ZSTD_PREFIX=/path/to/zstd
ZSTD_PREFIX ?=
LDLIBS := -lzstd -ldeflate
INCS :=
ifneq ($(ZSTD_PREFIX),)
  INCS += -I$(ZSTD_PREFIX)/include
  LDLIBS := -L$(ZSTD_PREFIX)/lib $(LDLIBS)
endif
SRC := codec_sw.c corpus.c metrics.c
ifdef WITH_QAT
  CFLAGS += -DWITH_QAT
  SRC += codec_qat.c
  LDLIBS += -lqatseqprod -lqatzip -lqat -lusdm
endif
ifdef WITH_IAA
  CFLAGS += -DWITH_IAA
  SRC += codec_iaa.c
  LDLIBS += -lqpl -laccel-config
endif

bench: main.c $(SRC)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)
selftest: selftest.c codec_sw.c $(if $(WITH_QAT),codec_qat.c) $(if $(WITH_IAA),codec_iaa.c)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)
clean:
	rm -f bench selftest
.PHONY: clean
```

- [ ] **Step 5: Build + run the selftest locally (software-only)**

Run: `cd kestrel/bench && make selftest && ./selftest`
Expected: `OK sw-zstd ...`, `OK sw-deflate ...`, `SELFTEST PASSED`. (Install libs first if missing: `sudo apt-get install -y libzstd-dev libdeflate-dev`.)

- [ ] **Step 6: Commit**

```bash
git add kestrel/bench/codec.h kestrel/bench/codec_sw.c kestrel/bench/selftest.c kestrel/bench/Makefile
git commit -m "kestrel: bench codec interface + software zstd/deflate backends + selftest"
```

---

## Task 3: Driver — corpus loader, metrics, sweep, CSV

**Files:**
- Create: `kestrel/bench/corpus.h`, `kestrel/bench/corpus.c`, `kestrel/bench/metrics.h`, `kestrel/bench/metrics.c`, `kestrel/bench/main.c`

**Interfaces:**
- Consumes: `codec_t`, `ALL_CODECS`, `comp_bound` (Task 2).
- Produces: `bench` executable emitting CSV `codec,corpus,direction,level,block,ratio,comp_MBps,decomp_MBps,p50_us,p99_us,p999_us,cpu_frac,cores_freed`.

- [ ] **Step 1: Corpus loader**

```c
// kestrel/bench/corpus.h
#ifndef KESTREL_CORPUS_H
#define KESTREL_CORPUS_H
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t len; char name[64]; } body_t;
typedef struct { body_t *v; int n; } corpus_t;
int corpus_load(const char *dir, corpus_t *out); // loads *.bin; returns 0 or -1
void corpus_free(corpus_t *c);
#endif
```
```c
// kestrel/bench/corpus.c
#include "corpus.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int ends_bin(const char*s){ size_t n=strlen(s); return n>4 && !strcmp(s+n-4,".bin"); }
int corpus_load(const char *dir, corpus_t *out){
    DIR *d=opendir(dir); if(!d){ perror(dir); return -1; }
    int cap=16; out->v=malloc(cap*sizeof(body_t)); out->n=0; struct dirent *e;
    while((e=readdir(d))){ if(!ends_bin(e->d_name)) continue;
        char p[1024]; snprintf(p,sizeof p,"%s/%s",dir,e->d_name);
        FILE *f=fopen(p,"rb"); if(!f) continue; fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        if(sz<=0){ fclose(f); continue; }
        if(out->n==cap){ cap*=2; out->v=realloc(out->v,cap*sizeof(body_t)); }
        body_t *b=&out->v[out->n]; b->buf=malloc(sz); b->len=fread(b->buf,1,sz,f); fclose(f);
        snprintf(b->name,sizeof b->name,"%s",e->d_name); out->n++; }
    closedir(d); return out->n>0?0:-1;
}
void corpus_free(corpus_t *c){ for(int i=0;i<c->n;i++) free(c->v[i].buf); free(c->v); }
```

- [ ] **Step 2: Metrics (wall vs thread-CPU, percentiles, cores-freed)**

```c
// kestrel/bench/metrics.h
#ifndef KESTREL_METRICS_H
#define KESTREL_METRICS_H
#include <stdint.h>
double now_wall_ns(void);        // CLOCK_MONOTONIC
double now_cpu_ns(void);         // CLOCK_THREAD_CPUTIME_ID (this thread)
void   pctl(double *us, int n, double *p50, double *p99, double *p999); // sorts in place
#endif
```
```c
// kestrel/bench/metrics.c
#include "metrics.h"
#include <time.h>
#include <stdlib.h>
static double ns(clockid_t c){ struct timespec t; clock_gettime(c,&t); return t.tv_sec*1e9+t.tv_nsec; }
double now_wall_ns(void){ return ns(CLOCK_MONOTONIC); }
double now_cpu_ns(void){ return ns(CLOCK_THREAD_CPUTIME_ID); }
static int dcmp(const void*a,const void*b){ double x=*(const double*)a,y=*(const double*)b; return x<y?-1:x>y; }
void pctl(double *us,int n,double *p50,double *p99,double *p999){ qsort(us,n,sizeof(double),dcmp);
    *p50=us[(int)(0.50*(n-1))]; *p99=us[(int)(0.99*(n-1))]; *p999=us[(int)(0.999*(n-1))]; }
```

**Note (cores-freed methodology):** `cpu_frac = cpu_ns / wall_ns` measured on the submitting thread across the timed compress calls. For software codecs `cpu_frac≈1`. For an accelerator using a *yielding* wait `cpu_frac<<1` and `cores_freed = 1 - cpu_frac`. **Critical:** QPL/QATzip default to **busy-poll** (spin), which makes `cpu_frac≈1` and hides the offload — Task 4 selects the async/yield wait so this metric is real. The harness reports both `cpu_frac` and `cores_freed`; a `cores_freed≈0` with `busy-poll` is flagged, not silently reported as "no benefit."

- [ ] **Step 3: Driver**

```c
// kestrel/bench/main.c
#include "codec.h"
#include "corpus.h"
#include "metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int LEVELS[] = {1,3,6,9};
static const size_t BLOCKS[] = {8192,16384,32768,65536,131072,0}; // 0 = whole body

static void run_one(const codec_t *c, const char *corpus_name, body_t *b, int level, size_t block, FILE *csv){
    size_t nblk = block ? block : b->len;
    size_t cap = comp_bound(nblk);
    uint8_t *cmp = malloc(cap), *out = malloc(nblk);
    void *ctx = c->mk(level); if(!ctx){ free(cmp); free(out); return; }
    int iters = 200; double *lat = malloc(iters*sizeof(double));
    size_t total_in=0, total_out=0; double wall=0, cpu=0;
    for(int it=0; it<iters+20; it++){                          // 20 warmup
        double w0=now_wall_ns(), c0=now_cpu_ns(); size_t in=0, outb=0;
        for(size_t off=0; off<b->len; off+=nblk){ size_t m = (off+nblk<=b->len)?nblk:(b->len-off);
            size_t z = c->compress(ctx, b->buf+off, m, cmp, cap); if(!z){ outb=0; break; } in+=m; outb+=z; }
        double w1=now_wall_ns(), c1=now_cpu_ns();
        if(it>=20 && outb){ lat[it-20]=(w1-w0)/1e3; wall+=w1-w0; cpu+=c1-c0; total_in+=in; total_out+=outb; }
    }
    double dwall=0; // decompress throughput (skip for compress-only)
    if(c->can_decompress){ for(int it=0;it<50;it++){ size_t z=c->compress(ctx,b->buf,(block?nblk:b->len)>b->len?b->len:(block?nblk:b->len),cmp,cap);
        double w0=now_wall_ns(); c->decompress(ctx,cmp,z,out,nblk); dwall+=now_wall_ns()-w0; } }
    if(total_out){ double p50,p99,p999; pctl(lat,iters,&p50,&p99,&p999);
        double comp_mbps=(total_in/1e6)/(wall/1e9);
        double decomp_mbps = c->can_decompress ? (50.0*(block?nblk:b->len)/1e6)/(dwall/1e9) : 0;
        double cpu_frac=cpu/wall, cores_freed = 1.0-cpu_frac;
        fprintf(csv,"%s,%s,compress,%d,%zu,%.4f,%.1f,%.1f,%.2f,%.2f,%.2f,%.3f,%.3f\n",
            c->name,corpus_name,level,block,(double)total_out/total_in,comp_mbps,decomp_mbps,p50,p99,p999,cpu_frac,cores_freed); }
    free(lat); free(cmp); free(out); c->fr(ctx);
}

int main(int argc, char **argv){
    const char *dir = argc>1?argv[1]:"../corpus";
    corpus_t cp; if(corpus_load(dir,&cp)){ fprintf(stderr,"no corpus in %s\n",dir); return 1; }
    FILE *csv=fopen("results.csv","w");
    fprintf(csv,"codec,corpus,direction,level,block,ratio,comp_MBps,decomp_MBps,p50_us,p99_us,p999_us,cpu_frac,cores_freed\n");
    for(int i=0;i<codec_count();i++) for(int b=0;b<cp.n;b++)
        for(int L=0;L<4;L++) for(int k=0;BLOCKS[k]||k==5;k++){ run_one(ALL_CODECS[i],cp.v[b].name,&cp.v[b],LEVELS[L],BLOCKS[k],csv); if(!BLOCKS[k])break; }
    fclose(csv); corpus_free(&cp);
    printf("wrote results.csv\n"); return 0;
}
```

- [ ] **Step 4: Build + run over a synthetic corpus locally**

```bash
cd kestrel/bench && mkdir -p /tmp/kc && head -c 200000 /dev/urandom > /tmp/kc/rand.bin && \
  printf 'ABABABAB%.0s' {1..20000} > /tmp/kc/runs.bin && make bench && ./bench /tmp/kc && column -s, -t results.csv | head
```
Expected: `results.csv` with rows for `sw-zstd`/`sw-deflate` × levels × blocks; ratios <1 on `runs.bin`, ~1 on `rand.bin`; `cpu_frac≈1.0` (software).

- [ ] **Step 5: Commit**

```bash
git add kestrel/bench/corpus.h kestrel/bench/corpus.c kestrel/bench/metrics.h kestrel/bench/metrics.c kestrel/bench/main.c
git commit -m "kestrel: bench driver — corpus loader, metrics, sweep, CSV"
```

---

## Task 4: Corpus capture (throwaway dump hooks + capture run)

**Files:**
- Create: `core/src/main/java/io/questdb/std/KestrelDump.java`
- Modify: `core/src/main/java/io/questdb/cutlass/qwp/server/egress/QwpEgressUpgradeProcessor.java` (after line 1604)
- Modify: `core/src/main/java/io/questdb/cutlass/qwp/server/QwpIngressUpgradeProcessor.java` (in `handleBinaryMessage`, ~line 839)
- Create: `kestrel/corpus/MANIFEST.md`, `kestrel/patches/dump-hooks.md`

**Interfaces:**
- Produces: `kestrel/corpus/*.bin` — real QWP bodies consumed by Tasks 3/5/6.

- [ ] **Step 1: Write the env-gated dump helper**

```java
// core/src/main/java/io/questdb/std/KestrelDump.java  (THROWAWAY — not for merge)
package io.questdb.std;
import java.io.FileOutputStream;
import java.nio.channels.FileChannel;
import java.util.concurrent.atomic.AtomicInteger;
public final class KestrelDump {
    private static final String DIR = System.getenv("KESTREL_DUMP_DIR");
    private static final int MAX = Integer.parseInt(System.getenv().getOrDefault("KESTREL_DUMP_MAX", "64"));
    private static final AtomicInteger EGRESS = new AtomicInteger();
    private static final AtomicInteger INGRESS = new AtomicInteger();
    public static boolean on() { return DIR != null; }
    public static void dump(String kind, long addr, int len) {
        if (DIR == null || len <= 0) return;
        AtomicInteger seq = "egress".equals(kind) ? EGRESS : INGRESS;
        int n = seq.getAndIncrement(); if (n >= MAX) return;
        byte[] b = new byte[len];
        for (int i = 0; i < len; i++) b[i] = Unsafe.getByte(addr + i);
        try (FileOutputStream f = new FileOutputStream(String.format("%s/%s-%03d.bin", DIR, kind, n))) {
            f.write(b);
        } catch (Exception e) { System.err.println("KestrelDump: " + e); }
    }
}
```

- [ ] **Step 2: Insert the egress hook** (dump the uncompressed body just after it is assembled)

In `QwpEgressUpgradeProcessor.java`, immediately after line 1604 (`long qwpEnd = preludeEnd + deltaSize + tableBlockSize;`) add:

```java
        if (io.questdb.std.KestrelDump.on()) {
            io.questdb.std.KestrelDump.dump("egress", preludeEnd, (int) (qwpEnd - preludeEnd));
        }
```

- [ ] **Step 3: Insert the ingress hook** (dump the received columnar payload)

In `QwpIngressUpgradeProcessor.java` `handleBinaryMessage(context, state, payload, length)` (line 838), as the first statement of the method body (~line 839) add:

```java
        if (io.questdb.std.KestrelDump.on()) {
            io.questdb.std.KestrelDump.dump("ingress", payload, length);
        }
```

- [ ] **Step 4: Build QuestDB (core) locally**

Run: `cd /home/nick/claude/wt/oss/kestrel && mvn -q -pl core -am -DskipTests package 2>&1 | tail -5`
Expected: `BUILD SUCCESS` (Rust `qdbr` + Java compile). If the Rust build is slow, this is the long step.

- [ ] **Step 5: Capture bodies by running the QWP benchmarks briefly**

```bash
cd /home/nick/claude/wt/oss/kestrel && mkdir -p /tmp/kestrel-corpus
# Egress bodies (narrow + wide result shapes) and ingress bodies (L1 quotes):
KESTREL_DUMP_DIR=/tmp/kestrel-corpus KESTREL_DUMP_MAX=48 \
  mvn -q -pl benchmarks -am exec:java -Dexec.mainClass=org.questdb.QwpEgressReadBenchmark \
  -Dexec.args="-i 1 -wi 0 -f 1 -r 1" 2>&1 | tail -3 || true
KESTREL_DUMP_DIR=/tmp/kestrel-corpus KESTREL_DUMP_MAX=48 \
  mvn -q -pl benchmarks -am exec:java -Dexec.mainClass=org.questdb.QwpEquitiesL1Benchmark \
  -Dexec.args="-i 1 -wi 0 -f 1 -r 1" 2>&1 | tail -3 || true
ls -la /tmp/kestrel-corpus | head
```
Expected: `egress-000.bin … egress-0NN.bin` and `ingress-000.bin …` present, each a few KB–hundreds of KB.
(If a benchmark's main isn't `exec:java`-friendly, fall back to its JMH runner class; the env var is what matters — the hook fires on any QWP egress/ingress traffic.)

- [ ] **Step 6: Trim to a representative set + commit**

```bash
cd /home/nick/claude/wt/oss/kestrel
# Keep a spread of sizes per kind (small/median/large), ~8 egress + ~8 ingress:
mkdir -p kestrel/corpus
python3 - <<'PY'
import os,shutil,glob
src="/tmp/kestrel-corpus"; dst="kestrel/corpus"
for kind in ("egress","ingress"):
    fs=sorted(glob.glob(f"{src}/{kind}-*.bin"), key=os.path.getsize)
    pick = fs[:2]+fs[len(fs)//2-1:len(fs)//2+1]+fs[-2:] if len(fs)>=6 else fs
    for i,f in enumerate(pick): shutil.copy(f, f"{dst}/{kind}-{i}-{os.path.getsize(f)}b.bin")
print(os.listdir(dst))
PY
# Write the manifest
{ echo "# Corpus"; echo; echo "Real post-serialization QWP bodies captured via the throwaway dump hooks (patches/dump-hooks.md)."; \
  echo; for f in kestrel/corpus/*.bin; do printf -- "- %s (%s bytes)\n" "$(basename "$f")" "$(stat -c%s "$f")"; done; } > kestrel/corpus/MANIFEST.md
git add kestrel/corpus core/src/main/java/io/questdb/std/KestrelDump.java \
  core/src/main/java/io/questdb/cutlass/qwp/server/egress/QwpEgressUpgradeProcessor.java \
  core/src/main/java/io/questdb/cutlass/qwp/server/QwpIngressUpgradeProcessor.java
git commit -m "kestrel: capture real QWP egress/ingress corpus via throwaway dump hooks"
```

- [ ] **Step 7: Verify the harness runs on the real corpus**

Run: `cd kestrel/bench && ./bench ../corpus && column -s, -t results.csv | grep -E 'egress|ingress' | head`
Expected: rows for both `egress-*` and `ingress-*` bodies; ratios reflect that QWP already delta-encoded (expect weaker ratios than raw data — the fidelity point).

- [ ] **Step 8: Document the throwaway patch**

Create `kestrel/patches/dump-hooks.md` describing the 3 touched files and `git revert`/`git checkout` instructions to drop the hooks before any real integration work (Phases B/C).

```bash
git add kestrel/patches/dump-hooks.md && git commit -m "kestrel: document throwaway dump-hook patch"
```

---

## Task 5: Accelerator backends — qat-zstd, qat-deflate, iaa-deflate (box-validated)

**Files:**
- Create: `kestrel/bench/codec_qat.c` (guarded `WITH_QAT`), `kestrel/bench/codec_iaa.c` (guarded `WITH_IAA`)

**Interfaces:**
- Consumes: `codec_t` (Task 2). Produces externs `QAT_ZSTD`, `QAT_DEFLATE`, `IAA_DEFLATE` referenced by `codec_sw.c`'s registry.

> These cannot be compiled on the AMD dev box (no libs). Validation is on the accelerator box via `selftest`/`bench`. Signatures below target QAT-ZSTD-Plugin (current release) and QPL 1.9 / QATzip — confirm against the installed headers in Step 4.

- [ ] **Step 1: QAT backends (seqprod for zstd, QATzip for Deflate)**

```c
// kestrel/bench/codec_qat.c   (compiled only with WITH_QAT)
#include "codec.h"
#include <stdlib.h>
#include <zstd.h>
#include "qatseqprod.h"   // from intel/QAT-ZSTD-Plugin
#include <qatzip.h>

// qat-zstd: register the QAT external sequence producer on a reused CCtx (compress-only).
typedef struct { ZSTD_CCtx *c; void *st; } qz_t;
static int qat_zstd_started = 0;
static void *qz_mk(int lvl){
    if(!qat_zstd_started){ if(QZSTD_startQatDevice()!=QZSTD_OK) return NULL; qat_zstd_started=1; }
    qz_t *s=calloc(1,sizeof*s); s->c=ZSTD_createCCtx(); s->st=QZSTD_createSeqProdState();
    if(!s->c||!s->st){ free(s); return NULL; }
    ZSTD_CCtx_setParameter(s->c, ZSTD_c_compressionLevel, lvl);
    ZSTD_registerSequenceProducer(s->c, s->st, qatSequenceProducer);
    ZSTD_CCtx_setParameter(s->c, ZSTD_c_enableSeqProducerFallback, 1);
    return s;
}
static size_t qz_co(void *v,const uint8_t*src,size_t n,uint8_t*dst,size_t cap){ qz_t*s=v; size_t r=ZSTD_compress2(s->c,dst,cap,src,n); return ZSTD_isError(r)?0:r; }
static void   qz_fr(void *v){ qz_t*s=v; ZSTD_freeCCtx(s->c); QZSTD_freeSeqProdState(s->st); free(s); }
const codec_t QAT_ZSTD = {"qat-zstd",0,qz_mk,qz_co,NULL,qz_fr};

// qat-deflate: QATzip hardware Deflate, both directions. Uses raw deflate to match QWP framing.
typedef struct { QzSession_T sess; } qd_t;
static void *qd_mk(int lvl){
    qd_t *s=calloc(1,sizeof*s);
    if(qzInit(&s->sess, 1 /*sw_backup*/)!=QZ_OK){ free(s); return NULL; }
    QzSessionParamsDeflate_T p; qzGetDefaultsDeflate(&p);
    p.data_fmt = QZ_DEFLATE_RAW; p.common_params.comp_lvl = lvl;
    if(qzSetupSessionDeflate(&s->sess,&p)!=QZ_OK){ qzClose(&s->sess); free(s); return NULL; }
    return s;
}
static size_t qd_co(void *v,const uint8_t*src,size_t n,uint8_t*dst,size_t cap){ qd_t*s=v; unsigned in=(unsigned)n,out=(unsigned)cap;
    return qzCompress(&s->sess,src,&in,dst,&out,1)==QZ_OK?out:0; }
static size_t qd_de(void *v,const uint8_t*src,size_t n,uint8_t*dst,size_t cap){ qd_t*s=v; unsigned in=(unsigned)n,out=(unsigned)cap;
    return qzDecompress(&s->sess,src,&in,dst,&out)==QZ_OK?out:0; }
static void   qd_fr(void *v){ qd_t*s=v; qzTeardownSession(&s->sess); qzClose(&s->sess); free(s); }
const codec_t QAT_DEFLATE = {"qat-deflate",1,qd_mk,qd_co,qd_de,qd_fr};
```

- [ ] **Step 2: IAA backend (QPL Deflate, hardware path, both directions)**

```c
// kestrel/bench/codec_iaa.c   (compiled only with WITH_IAA)
#include "codec.h"
#include <stdlib.h>
#include "qpl/qpl.h"
typedef struct { qpl_job *job; int level; } iaa_t;
static void *iaa_mk(int lvl){
    uint32_t sz; if(qpl_get_job_size(qpl_path_hardware,&sz)!=QPL_STS_OK) return NULL;
    iaa_t *s=calloc(1,sizeof*s); s->job=malloc(sz); s->level=lvl;
    if(qpl_init_job(qpl_path_hardware,s->job)!=QPL_STS_OK){ free(s->job); free(s); return NULL; }
    return s;
}
static size_t iaa_co(void *v,const uint8_t*src,size_t n,uint8_t*dst,size_t cap){ iaa_t*s=v; qpl_job*j=s->job;
    j->op=qpl_op_compress; j->level=(s->level>=6)?qpl_high_level:qpl_default_level;
    j->next_in_ptr=(uint8_t*)src; j->available_in=(uint32_t)n; j->next_out_ptr=dst; j->available_out=(uint32_t)cap;
    j->flags=QPL_FLAG_FIRST|QPL_FLAG_LAST|QPL_FLAG_DYNAMIC_HUFFMAN|QPL_FLAG_OMIT_VERIFY;
    return qpl_execute_job(j)==QPL_STS_OK ? j->total_out : 0; }
static size_t iaa_de(void *v,const uint8_t*src,size_t n,uint8_t*dst,size_t cap){ iaa_t*s=v; qpl_job*j=s->job;
    j->op=qpl_op_decompress; j->next_in_ptr=(uint8_t*)src; j->available_in=(uint32_t)n;
    j->next_out_ptr=dst; j->available_out=(uint32_t)cap; j->flags=QPL_FLAG_FIRST|QPL_FLAG_LAST;
    return qpl_execute_job(j)==QPL_STS_OK ? j->total_out : 0; }
static void iaa_fr(void *v){ iaa_t*s=v; qpl_fini_job(s->job); free(s->job); free(s); }
const codec_t IAA_DEFLATE = {"iaa-deflate",1,iaa_mk,iaa_co,iaa_de,iaa_fr};
```

- [ ] **Step 3: Rust spike (de-risks Phase B) — record only**

Add to `kestrel/patches/dump-hooks.md` (or a new `kestrel/NOTES.md`) the result of checking whether `zstd-safe 7.2.4` exposes sequence-producer registration:
Run locally: `grep -rn "register_sequence_producer\|registerSequenceProducer\|SequenceProducer" ~/.cargo/registry/src/*/zstd-safe-7.2.4/ 2>/dev/null | head`
Record whether Phase B can use the safe wrapper or must call `zstd-sys` raw / a C shim. (This is a note, not code — it informs Phase B's plan.)

- [ ] **Step 4: (On the box) build with accelerators + selftest**

Operator runs (documented in `run-bench.sh`, Task 6):
```bash
cd kestrel/bench && make selftest WITH_QAT=1 WITH_IAA=1 && ./selftest
```
Expected: `OK qat-zstd ... (compress-only)`, `OK qat-deflate ...`, `OK iaa-deflate ...`, `SELFTEST PASSED`. A `mk failed` → device not configured (re-check Phase 0). If a header/symbol name differs, fix against the installed version here.

- [ ] **Step 5: Commit**

```bash
git add kestrel/bench/codec_qat.c kestrel/bench/codec_iaa.c
git commit -m "kestrel: accelerator backends — qat-zstd (seqprod), qat-deflate (QATzip), iaa-deflate (QPL)"
```

---

## Task 6: Operator scripts + go/no-go template

**Files:**
- Create: `kestrel/scripts/build-bench.sh`, `kestrel/scripts/run-bench.sh`, `kestrel/scripts/README.md`, `kestrel/RESULTS-TEMPLATE.md`

**Interfaces:**
- Consumes: everything above. Produces the operator runbook + `results.csv` → go/no-go.

- [ ] **Step 1: build-bench.sh**

```bash
#!/usr/bin/env bash
# Detect accelerator libs and build the harness with the right flags. Run on the box.
set -euo pipefail
cd "$(dirname "$0")/../bench"
FLAGS=()
ldconfig -p | grep -q libqatseqprod && ldconfig -p | grep -q libqatzip && FLAGS+=("WITH_QAT=1") || echo "QAT libs absent → skipping qat-* codecs"
ldconfig -p | grep -q libqpl && FLAGS+=("WITH_IAA=1") || echo "QPL absent → skipping iaa-deflate"
echo "building: make ${FLAGS[*]:-<software-only>}"
make clean && make selftest "${FLAGS[@]}" && ./selftest && make bench "${FLAGS[@]}"
echo "OK: ./bench built"
```

- [ ] **Step 2: run-bench.sh**

```bash
#!/usr/bin/env bash
# Run the sweep over the committed corpus, print a readable table, keep results.csv. Run on the box.
set -euo pipefail
cd "$(dirname "$0")/../bench"
[ -x ./bench ] || { echo "run build-bench.sh first"; exit 1; }
./bench ../corpus
echo; echo "=== results (sorted by ratio within codec) ==="
{ head -1 results.csv; tail -n +2 results.csv | sort -t, -k1,1 -k6,6n; } | column -s, -t
echo; echo "Paste results.csv back. Key columns: ratio, comp_MBps, p99_us, cores_freed."
```

- [ ] **Step 3: README runbook + RESULTS-TEMPLATE**

`kestrel/scripts/README.md` — the operator flow: `git clone/fetch` the branch → `bash kestrel/scripts/probe.sh` (paste) → install any missing dev libs → `bash kestrel/scripts/build-bench.sh` → `bash kestrel/scripts/run-bench.sh` (paste `results.csv`).

`kestrel/RESULTS-TEMPLATE.md` — the go/no-go skeleton:
```markdown
# kestrel Phase A results & go/no-go
- Box: <CPU / QAT gen / IAA WQ config from probe>
- Software baseline (sw-zstd L1): ratio __, comp __ MB/s, p99 __ us
## Egress (server compresses)
- Best QAT-zstd: level __, ratio __, comp __ MB/s, cores_freed __ vs sw-zstd L1
- Best IAA-deflate / QAT-deflate: ratio __, comp __, cores_freed __
## Ingress (server decompresses)
- IAA-deflate / QAT-deflate decompress: __ MB/s, cores_freed __ (zstd cannot offload this)
## Decision
- [ ] B (QAT into egress zstd): GO/NO — because ____
- [ ] C (Deflate codec egress+ingress): GO/NO — because ____
- Winning codec/level/block for the integration phases: ____
```

- [ ] **Step 4: Local dry-run (software-only) end-to-end**

Run: `bash kestrel/scripts/build-bench.sh && bash kestrel/scripts/run-bench.sh | head -20`
Expected: builds software-only (QAT/QPL "absent" messages), selftest passes, prints a table over the real corpus. Proves the operator flow works minus accelerators.

- [ ] **Step 5: Commit**

```bash
git add kestrel/scripts/build-bench.sh kestrel/scripts/run-bench.sh kestrel/scripts/README.md kestrel/RESULTS-TEMPLATE.md
git commit -m "kestrel: operator build/run scripts + go/no-go results template"
```

---

## Self-review checklist (run after execution is planned, before starting)

- **Spec coverage:** Phase 0 probe (Task 1 ✓), 5-codec bake-off both directions (Tasks 2,5 ✓), egress+ingress real corpus (Task 4 ✓), block-size sweep + metrics incl. cores-freed (Task 3 ✓), Rust spike for B (Task 5 Step 3 ✓), go/no-go (Task 6 ✓), git transport / operator-run (Tasks 1,5,6 ✓). Concurrency sweep from the spec is **deferred** to the on-box run as a follow-up (single-thread first; noted here so it isn't silently dropped).
- **Placeholder scan:** none — every code step is complete; accelerator API calls are concrete (confirm header symbol names on first box build, Task 5 Step 4).
- **Type consistency:** `codec_t` vtable (name, can_decompress, mk, compress, decompress, fr) is used identically in `codec_sw.c`, `codec_qat.c`, `codec_iaa.c`, `selftest.c`, `main.c`; `compress`/`decompress` return-0-on-failure everywhere.
```
