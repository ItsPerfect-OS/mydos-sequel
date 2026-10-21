#ifndef PAGING_H
#define PAGING_H
#include "types.h"

#define PAGE_SIZE    4096ULL
#define PAGE_PRESENT 0x001ULL
#define PAGE_WRITE   0x002ULL
#define PAGE_USER    0x004ULL          /* ring 3 may access this page */
#define PAGE_LARGE   0x080ULL          /* 2 MB page (kernel identity map only) */

/* Virtual layout (4-level paging, lower canonical half):
 *   [0, RAM top)                kernel: all RAM identity-mapped, supervisor-only (shared by every address space)
 *   [USER_BASE, ...)            one program's image, heap and stack (each program has its own page tables)
 */
#define USER_BASE      0x0000008000000000ULL        /* 512 GB: PML4 slot 1 */
#define USER_STACK_TOP (USER_BASE + 0x40000000ULL)  /* 1 GB above the image */

int  paging_init(void);                /* build the kernel's page tables (identity map, NULL guard, read-only code), turn them on; 1 = ok */
int  paging_map(u64 virt, u64 phys, u64 flags);       /* map one 4 KB page; allocates page-table pages if needed; 1 = ok */
u64  paging_unmap(u64 virt);                          /* remove a mapping; returns the old physical address or 0 */
int  paging_translate(u64 virt, u64 *phys, u64 *flags);   /* 1 if mapped */

/* Per-program address spaces: a PML4 that shares the kernel's identity map and owns everything from USER_BASE up. */
u64  paging_kernel_pd(void);
u64  paging_create_space(void);                       /* new PML4 (physical address), or 0 */
void paging_destroy_space(u64 pml4);                  /* frees the user pages, their page tables and the PML4 */
int  paging_map_in(u64 pml4, u64 virt, u64 phys, u64 flags);
int  paging_translate_in(u64 pml4, u64 virt, u64 *phys, u64 *flags);
void paging_switch(u64 pml4);                         /* load CR3 */

/* Demand paging: pages of [start, start + pages*4096) get a zeroed frame on first touch. */
int  paging_demand_add(u64 start, u64 pages, u64 flags);
void paging_demand_remove(u64 start);                 /* unmaps the region and returns its frames to the PMM */

int  paging_handle_fault(u64 fault_addr, u64 error_code);   /* called by the #PF handler; 1 = fixed, retry */
u32  paging_fault_count(void);         /* demand faults served so far */
u32  paging_table_count(void);         /* page-table pages allocated (4 KB each, not counting the PML4) */
void paging_print(void);               /* shown by `meminfo` */
int  paging_selftest(void);            /* `pagetest`; returns 1 on pass */
#endif
