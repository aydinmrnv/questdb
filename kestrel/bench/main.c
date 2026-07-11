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
