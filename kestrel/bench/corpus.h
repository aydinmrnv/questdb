#ifndef KESTREL_CORPUS_H
#define KESTREL_CORPUS_H
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *buf; size_t len; char name[64]; } body_t;
typedef struct { body_t *v; int n; } corpus_t;
int corpus_load(const char *dir, corpus_t *out); // loads *.bin; returns 0 or -1
void corpus_free(corpus_t *c);
#endif
