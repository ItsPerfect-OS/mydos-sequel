#ifndef PMM_H
#define PMM_H
#include "types.h"

#define FRAME_SIZE 4096u

int  pmm_init(void);                 /* build the frame bitmap from the E820 map (needs memmap_init first); 1 = ok */
u64  pmm_alloc_frame(void);          /* physical address of a free 4 KB frame, or 0 if out of memory */
u64  pmm_alloc_contiguous(u64 count);/* `count` frames in a row (for the heap, DMA...); 0 if none. Free them one by one. */
void pmm_free_frame(u64 addr);       /* return a frame; bad / double frees are reported and ignored */
u64  pmm_total_frames(void);         /* frames under PMM control */
u64  pmm_free_frames(void);
void pmm_print(void);                /* statistics, shown by `meminfo` */
int  pmm_selftest(void);             /* `pmmtest`; returns 1 on pass */
#endif
