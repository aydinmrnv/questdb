#ifndef KESTREL_METRICS_H
#define KESTREL_METRICS_H
#include <stdint.h>
double now_wall_ns(void);        // CLOCK_MONOTONIC
double now_cpu_ns(void);         // CLOCK_THREAD_CPUTIME_ID (this thread)
void   pctl(double *us, int n, double *p50, double *p99, double *p999); // sorts in place
#endif
