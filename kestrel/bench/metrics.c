#define _POSIX_C_SOURCE 199309L
#include "metrics.h"
#include <time.h>
#include <stdlib.h>
static double ns(clockid_t c){ struct timespec t; clock_gettime(c,&t); return t.tv_sec*1e9+t.tv_nsec; }
double now_wall_ns(void){ return ns(CLOCK_MONOTONIC); }
double now_cpu_ns(void){ return ns(CLOCK_THREAD_CPUTIME_ID); }
static int dcmp(const void*a,const void*b){ double x=*(const double*)a,y=*(const double*)b; return x<y?-1:x>y; }
void pctl(double *us,int n,double *p50,double *p99,double *p999){ qsort(us,n,sizeof(double),dcmp);
    *p50=us[(int)(0.50*(n-1))]; *p99=us[(int)(0.99*(n-1))]; *p999=us[(int)(0.999*(n-1))]; }
