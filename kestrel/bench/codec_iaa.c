// kestrel/bench/codec_iaa.c   (compiled only with WITH_IAA)
#include "codec.h"
#include <stdlib.h>
#include "qpl/qpl.h"
typedef struct { qpl_job *job; int level; } iaa_t;
static void *iaa_mk(int lvl){
    uint32_t sz; if(qpl_get_job_size(qpl_path_hardware,&sz)!=QPL_STS_OK) return NULL;
    iaa_t *s=calloc(1,sizeof*s); if(!s) return NULL;
    s->job=malloc(sz); if(!s->job){ free(s); return NULL; }
    s->level=lvl;
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
