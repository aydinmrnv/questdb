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
    closedir(d);
    if (out->n == 0) { free(out->v); out->v = NULL; return -1; }
    return 0;
}
void corpus_free(corpus_t *c){ for(int i=0;i<c->n;i++) free(c->v[i].buf); free(c->v); }
