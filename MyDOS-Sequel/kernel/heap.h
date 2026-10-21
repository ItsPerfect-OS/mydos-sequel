#ifndef HEAP_H
#define HEAP_H
#include "types.h"

int   heap_init(void);                       /* needs pmm_init(); grabs the first chunk of frames; 1 = ok, 0 = no memory */
void *kmalloc(u32 size);                     /* 16-byte aligned, or NULL (size 0, too big, or out of memory) */
void *kcalloc(u32 count, u32 size);          /* zeroed; NULL on overflow */
void *krealloc(void *p, u32 size);           /* grows by moving; never shrinks in place */
void  kfree(void *p);                        /* NULL is fine; bad and double frees are reported and ignored */
u32   heap_used_bytes(void);                 /* payload bytes currently allocated (for leak checks) */
void  heap_print(void);                      /* statistics, shown by `meminfo` */
int   heap_selftest(void);                   /* `heaptest`; returns 1 on pass */
#endif
