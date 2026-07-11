// kestrel/bench/codec_sw.c
#include "codec.h"
#include <stdlib.h>
#include <zstd.h>
#include <libdeflate.h>

size_t comp_bound(size_t n){ size_t z = ZSTD_compressBound(n); size_t d = libdeflate_deflate_compress_bound(NULL, n); return (z>d?z:d) + 128; }

// ---- sw-zstd (mirrors QWP: reused CCtx, one-shot compress2) ----
typedef struct { ZSTD_CCtx *c; ZSTD_DCtx *d; } zst_t;
static void *z_mk(int lvl){ zst_t *s=calloc(1,sizeof*s); if(!s) return NULL;
    s->c=ZSTD_createCCtx(); s->d=ZSTD_createDCtx();
    if(!s->c||!s->d){ if(s->c)ZSTD_freeCCtx(s->c); if(s->d)ZSTD_freeDCtx(s->d); free(s); return NULL; }
    if(ZSTD_isError(ZSTD_CCtx_setParameter(s->c, ZSTD_c_compressionLevel, lvl))){ ZSTD_freeCCtx(s->c); ZSTD_freeDCtx(s->d); free(s); return NULL; }
    return s; }
static size_t z_co(void *v,const uint8_t*src,size_t n,uint8_t*dst,size_t cap){ zst_t*s=v; size_t r=ZSTD_compress2(s->c,dst,cap,src,n); return ZSTD_isError(r)?0:r; }
static size_t z_de(void *v,const uint8_t*src,size_t n,uint8_t*dst,size_t cap){ zst_t*s=v; size_t r=ZSTD_decompressDCtx(s->d,dst,cap,src,n); return ZSTD_isError(r)?0:r; }
static void   z_fr(void *v){ zst_t*s=v; ZSTD_freeCCtx(s->c); ZSTD_freeDCtx(s->d); free(s); }
static const codec_t SW_ZSTD = {"sw-zstd",1,z_mk,z_co,z_de,z_fr};

// ---- sw-deflate (libdeflate control) ----
typedef struct { struct libdeflate_compressor *c; struct libdeflate_decompressor *d; } df_t;
static void *d_mk(int lvl){ df_t *s=calloc(1,sizeof*s); if(!s) return NULL;
    s->c=libdeflate_alloc_compressor(lvl); s->d=libdeflate_alloc_decompressor();
    if(!s->c||!s->d){ if(s->c)libdeflate_free_compressor(s->c); if(s->d)libdeflate_free_decompressor(s->d); free(s); return NULL; }
    return s; }
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
