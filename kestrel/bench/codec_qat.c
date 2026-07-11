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
    qz_t *s=calloc(1,sizeof*s); if(!s) return NULL;
    s->c=ZSTD_createCCtx(); s->st=QZSTD_createSeqProdState();
    if(!s->c||!s->st){ if(s->c)ZSTD_freeCCtx(s->c); if(s->st)QZSTD_freeSeqProdState(s->st); free(s); return NULL; }
    if(ZSTD_isError(ZSTD_CCtx_setParameter(s->c, ZSTD_c_compressionLevel, lvl))){ ZSTD_freeCCtx(s->c); QZSTD_freeSeqProdState(s->st); free(s); return NULL; }
    ZSTD_registerSequenceProducer(s->c, s->st, qatSequenceProducer);
    if(ZSTD_isError(ZSTD_CCtx_setParameter(s->c, ZSTD_c_enableSeqProducerFallback, 1))){ ZSTD_freeCCtx(s->c); QZSTD_freeSeqProdState(s->st); free(s); return NULL; }
    return s;
}
static size_t qz_co(void *v,const uint8_t*src,size_t n,uint8_t*dst,size_t cap){ qz_t*s=v; size_t r=ZSTD_compress2(s->c,dst,cap,src,n); return ZSTD_isError(r)?0:r; }
static void   qz_fr(void *v){ qz_t*s=v; ZSTD_freeCCtx(s->c); QZSTD_freeSeqProdState(s->st); free(s); }
const codec_t QAT_ZSTD = {"qat-zstd",0,qz_mk,qz_co,NULL,qz_fr};

// qat-deflate: QATzip hardware Deflate, both directions. Uses raw deflate to match QWP framing.
typedef struct { QzSession_T sess; } qd_t;
static void *qd_mk(int lvl){
    qd_t *s=calloc(1,sizeof*s); if(!s) return NULL;
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
