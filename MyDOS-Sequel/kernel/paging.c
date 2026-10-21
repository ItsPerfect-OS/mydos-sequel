/* paging.c - x86-64 four-level paging (PML4 -> PDPT -> PD -> PT), 4 KB pages
 *
 * Layout after paging_init():
 *   - All RAM from 0x1000 to its top is identity-mapped (virtual == physical), so every pointer, the heap,
 *     and the frames the PMM hands out keep working. Above 4 MB this uses 2 MB pages (a few table pages
 *     even for tens of GB); the first 4 MB uses 4 KB pages so the guards below are possible.
 *   - Page 0 is NOT mapped: a NULL dereference faults instead of reading BIOS data.
 *   - Kernel code and read-only data (0x100000 .. __ro_end) are read-only, and CR0.WP makes the CPU
 *     enforce that even for kernel-mode writes.
 *   - Page tables are ordinary frames from the PMM; because RAM is identity-mapped they can be edited
 *     through their physical addresses at any time.
 */
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "memmap.h"
#include "console.h"
#include "string.h"

extern char __ro_end[];                       /* from linker.ld: end of kernel code + rodata (page aligned) */

#define ADDR_MASK    0x000FFFFFFFFFF000ULL
#define KERNEL_BASE  0x0000000000100000ULL    /* where the kernel is linked and runs */
#define MAX_RAM_MAP  (512ULL << 30)           /* the identity map is one PML4 slot: 512 GB */
#define L4(v) (((v) >> 39) & 0x1FF)
#define L3(v) (((v) >> 30) & 0x1FF)
#define L2(v) (((v) >> 21) & 0x1FF)
#define L1(v) (((v) >> 12) & 0x1FF)

static u64 *kernel_pml4;
static u64 mapped_top;                         /* identity map covers [PAGE_SIZE, mapped_top) */
static u64 ro_end;
static u32 tables, demand_faults;
static int enabled;

static inline void invlpg(u64 v) { __asm__ volatile("invlpg (%0)" :: "r"(v) : "memory"); }
static u64 current_cr3(void) { u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }

static u64 *alloc_table(void) {                /* zeroed frame, usable as a page table */
    u64 f = pmm_alloc_frame();
    if (!f) return 0;
    memset((void *)f, 0, PAGE_SIZE);
    return (u64 *)f;
}

/* The table that entry `idx` of `table` points to; optionally create it. 0 if absent (or a large page). */
static u64 *next_table(u64 *table, u64 idx, int create, u64 flags, int count) {
    u64 e = table[idx];
    if (e & PAGE_PRESENT) {
        if (e & PAGE_LARGE) return 0;
        if ((flags & PAGE_USER) && !(e & PAGE_USER)) table[idx] |= PAGE_USER;      /* every level must allow ring 3 */
        return (u64 *)(e & ADDR_MASK);
    }
    if (!create) return 0;
    u64 *t = alloc_table();
    if (!t) return 0;
    if (count) tables++;
    table[idx] = (u64)t | PAGE_PRESENT | PAGE_WRITE | (flags & PAGE_USER);          /* the page flags decide the real access */
    return t;
}

static int canonical_lower(u64 v) { return v < 0x0000800000000000ULL; }

int paging_map_in(u64 pml4_addr, u64 virt, u64 phys, u64 flags) {
    u64 *pml4 = (u64 *)pml4_addr;
    if (((virt | phys) & (PAGE_SIZE - 1)) || !canonical_lower(virt)) return 0;
    if ((flags & PAGE_USER) && virt < USER_BASE) return 0;                           /* ring 3 never gets kernel addresses */
    int count = pml4 == kernel_pml4;
    u64 *pdpt = next_table(pml4, L4(virt), 1, flags, count);
    u64 *pd   = pdpt ? next_table(pdpt, L3(virt), 1, flags, count) : 0;
    u64 *pt   = pd ? next_table(pd, L2(virt), 1, flags, count) : 0;
    if (!pt) return 0;
    pt[L1(virt)] = phys | (flags & 0xFFFULL) | PAGE_PRESENT;
    if (enabled && current_cr3() == pml4_addr) invlpg(virt);
    return 1;
}

int paging_translate_in(u64 pml4_addr, u64 virt, u64 *phys, u64 *flags) {
    u64 *pml4 = (u64 *)pml4_addr;
    if (!canonical_lower(virt)) return 0;
    u64 *pdpt = next_table(pml4, L4(virt), 0, 0, 0);
    u64 *pd   = pdpt ? next_table(pdpt, L3(virt), 0, 0, 0) : 0;
    if (!pd) return 0;
    u64 pde = pd[L2(virt)];
    if (!(pde & PAGE_PRESENT)) return 0;
    if (pde & PAGE_LARGE) {                                                           /* 2 MB identity-map page */
        if (phys)  *phys  = (pde & 0x000FFFFFFFE00000ULL) | (virt & 0x1FFFFFULL);
        if (flags) *flags = pde & 0xFFFULL & ~PAGE_LARGE;
        return 1;
    }
    u64 *pt = (u64 *)(pde & ADDR_MASK);
    u64 pte = pt[L1(virt)];
    if (!(pte & PAGE_PRESENT)) return 0;
    if (phys)  *phys  = (pte & ADDR_MASK) | (virt & 0xFFFULL);
    if (flags) *flags = pte & 0xFFFULL;
    return 1;
}

int paging_map(u64 virt, u64 phys, u64 flags) { return paging_map_in((u64)kernel_pml4, virt, phys, flags); }

u64 paging_unmap(u64 virt) {
    if (!canonical_lower(virt)) return 0;
    u64 *pdpt = next_table(kernel_pml4, L4(virt), 0, 0, 0);
    u64 *pd   = pdpt ? next_table(pdpt, L3(virt), 0, 0, 0) : 0;
    u64 *pt   = pd ? next_table(pd, L2(virt), 0, 0, 0) : 0;
    if (!pt) return 0;
    u64 pte = pt[L1(virt)];
    if (!(pte & PAGE_PRESENT)) return 0;
    pt[L1(virt)] = 0;
    if (enabled) invlpg(virt);
    return pte & ADDR_MASK;
}

int paging_translate(u64 virt, u64 *phys, u64 *flags) { return paging_translate_in((u64)kernel_pml4, virt, phys, flags); }

u64 paging_kernel_pd(void) { return (u64)kernel_pml4; }
void paging_switch(u64 pml4) { __asm__ volatile("mov %0, %%cr3" :: "r"(pml4) : "memory"); }

u64 paging_create_space(void) {
    u64 *pml4 = alloc_table();
    if (!pml4) return 0;
    pml4[0] = kernel_pml4[0];                                                         /* share the kernel's identity map */
    return (u64)pml4;
}

static void free_tree(u64 *table, int level) {                                        /* level 3 = PDPT, 2 = PD, 1 = PT */
    for (int i = 0; i < 512; i++) {
        u64 e = table[i];
        if (!(e & PAGE_PRESENT)) continue;
        if (level == 1) pmm_free_frame(e & ADDR_MASK);                                /* leaf: a program's page */
        else if (!(e & PAGE_LARGE)) free_tree((u64 *)(e & ADDR_MASK), level - 1);
    }
    pmm_free_frame((u64)table);
}

void paging_destroy_space(u64 pml4_addr) {
    u64 *pml4 = (u64 *)pml4_addr;
    if (!pml4 || pml4 == kernel_pml4) return;
    for (int i = 1; i < 512; i++)                                                     /* slot 0 is the shared kernel map */
        if (pml4[i] & PAGE_PRESENT) free_tree((u64 *)(pml4[i] & ADDR_MASK), 3);
    pmm_free_frame(pml4_addr);
}

/* ---------------- boot-time setup ---------------- */

static u64 ram_top(void) {                     /* end of the highest usable RAM, page aligned */
    u64 top = 0;
    for (int i = 0; i < memmap_count(); i++) {
        const struct e820_entry *e = memmap_entry(i);
        if (e->type != E820_USABLE) continue;
        u64 end = e->base + e->length;
        if (end > top) top = end;
    }
    if (top > MAX_RAM_MAP) top = MAX_RAM_MAP;
    return top & ~(PAGE_SIZE - 1);
}

int paging_init(void) {
    kernel_pml4 = alloc_table();
    u64 *pdpt = alloc_table();
    if (!kernel_pml4 || !pdpt) return 0;
    kernel_pml4[0] = (u64)pdpt | PAGE_PRESENT | PAGE_WRITE;
    mapped_top = ram_top();
    ro_end = (u64)__ro_end;

    for (u64 g = 0; g * (1ULL << 30) < mapped_top; g++) {                             /* one PD per GB */
        u64 *pd = alloc_table();
        if (!pd) return 0;
        tables++;
        pdpt[g] = (u64)pd | PAGE_PRESENT | PAGE_WRITE;
        for (u64 j = 0; j < 512; j++) {
            u64 base = g * (1ULL << 30) + j * (2ULL << 20);
            if (base >= mapped_top) break;
            if (base >= (4ULL << 20)) { pd[j] = base | PAGE_PRESENT | PAGE_WRITE | PAGE_LARGE; continue; }
            u64 *pt = alloc_table();                                                  /* first 4 MB: 4 KB pages */
            if (!pt) return 0;
            tables++;
            for (u64 k = 0; k < 512; k++) {
                u64 a = base + k * PAGE_SIZE;
                if (a == 0 || a >= mapped_top) continue;                              /* page 0 stays unmapped */
                pt[k] = a | PAGE_PRESENT | ((a >= KERNEL_BASE && a < ro_end) ? 0 : PAGE_WRITE);
            }
            pd[j] = (u64)pt | PAGE_PRESENT | PAGE_WRITE;
        }
    }
    tables++;                                                                          /* the PDPT */

    u64 cr0;
    __asm__ volatile("mov %0, %%cr3" :: "r"((u64)kernel_pml4));
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x10000;                                                                    /* WP: honour read-only pages in ring 0 */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
    enabled = 1;
    return 1;
}

/* ---------------- demand paging ---------------- */

#define MAX_REGIONS 8
static struct { u64 start, end, flags; int used; } regions[MAX_REGIONS];

int paging_demand_add(u64 start, u64 pages, u64 flags) {
    if ((start & (PAGE_SIZE - 1)) || pages == 0 || start + pages * PAGE_SIZE < start) return 0;
    for (int i = 0; i < MAX_REGIONS; i++) {
        if (regions[i].used) continue;
        regions[i].start = start;
        regions[i].end   = start + pages * PAGE_SIZE;
        regions[i].flags = flags;
        regions[i].used  = 1;
        return 1;
    }
    return 0;
}

void paging_demand_remove(u64 start) {
    for (int i = 0; i < MAX_REGIONS; i++) {
        if (!regions[i].used || regions[i].start != start) continue;
        for (u64 a = regions[i].start; a < regions[i].end; a += PAGE_SIZE) {
            u64 phys = paging_unmap(a);
            if (phys) pmm_free_frame(phys);
        }
        regions[i].used = 0;
        return;
    }
}

int paging_handle_fault(u64 addr, u64 err) {
    if (err & 1) return 0;                                    /* protection violation: a real bug */
    for (int i = 0; i < MAX_REGIONS; i++) {
        if (!regions[i].used || addr < regions[i].start || addr >= regions[i].end) continue;
        u64 frame = pmm_alloc_frame();
        if (!frame) return 0;
        memset((void *)frame, 0, PAGE_SIZE);
        if (!paging_map(addr & ~(PAGE_SIZE - 1), frame, regions[i].flags)) { pmm_free_frame(frame); return 0; }
        demand_faults++;
        return 1;                                             /* CPU retries the faulting instruction */
    }
    return 0;
}

u32 paging_fault_count(void) { return demand_faults; }
u32 paging_table_count(void) { return tables; }

void paging_print(void) {
    console_set_color(COL_CYAN);  puts("Paging (x86-64, 4 levels, 4 KB pages):\n");
    if (!enabled) { console_set_color(COL_YELLOW); puts("  not enabled\n"); return; }
    console_set_color(COL_GREY);
    puts("  identity map: "); put_hex64(PAGE_SIZE); puts(" - "); put_hex64(mapped_top); putc('\n');
    puts("  page tables:  "); put_dec(tables); puts(" ("); put_dec(tables * 4); puts(" KB) + 1 PML4\n");
    puts("  page 0 is unmapped (NULL pointer guard)\n");
    puts("  read-only:    "); put_hex64(KERNEL_BASE); puts(" - "); put_hex64(ro_end); puts(" (kernel code + rodata)\n");
    puts("  user space:   from "); put_hex64(USER_BASE); putc('\n');
    puts("  demand-paging faults served: "); put_dec(demand_faults); putc('\n');
}

/* ---------------- self-test (`pagetest`) ---------------- */

#define TEST_ALIAS_A 0x0000010000000000ULL     /* 1 TB: far above any RAM the identity map covers */
#define TEST_ALIAS_B (TEST_ALIAS_A + 0x1000)
#define TEST_DEMAND  0x0000010001000000ULL

static int step(const char *name, int ok) {
    console_set_color(COL_GREY); puts("  "); puts(name); puts(": ");
    console_set_color(ok ? COL_GREEN : COL_RED); puts(ok ? "ok\n" : "FAILED\n");
    return ok;
}

int paging_selftest(void) {
    static volatile u32 some_data;             /* lives in .bss: must be writable */
    int pass = 1, ok;
    u64 phys, flags;

    if (!enabled) { puts("  paging is not enabled\n"); return 0; }
    if (paging_translate(TEST_ALIAS_A, 0, 0) || paging_translate(TEST_DEMAND, 0, 0)) {
        puts("  test addresses are inside the identity map - skipping\n");
        return 0;
    }

    void *p = kmalloc(64);
    ok = paging_translate(KERNEL_BASE, &phys, 0) && phys == KERNEL_BASE
      && paging_translate(0xB8000, &phys, 0) && phys == 0xB8000
      && p && paging_translate((u64)p, &phys, 0) && phys == (u64)p;
    pass &= step("identity map: kernel, VGA memory and heap translate to themselves", ok);
    kfree(p);

    pass &= step("page 0 is unmapped (NULL guard)", !paging_translate(0, 0, 0) && !paging_translate(0xFFF, 0, 0));

    ok = paging_translate(KERNEL_BASE, 0, &flags) && !(flags & PAGE_WRITE);
    ok &= paging_translate((u64)&some_data, 0, &flags) && (flags & PAGE_WRITE);
    pass &= step("kernel code is read-only, data is writable", ok);

    u32 tables0 = tables;
    u64 free0 = pmm_free_frames();
    u64 f = pmm_alloc_frame();
    ok = f && paging_map(TEST_ALIAS_A, f, PAGE_WRITE);
    if (ok) { *(volatile u32 *)TEST_ALIAS_A = 0xCAFEBABEu; ok = *(volatile u32 *)f == 0xCAFEBABEu; }
    ok &= paging_translate(TEST_ALIAS_A + 0x123, &phys, 0) && phys == f + 0x123;
    pass &= step("map a frame at a non-identity address; reads and writes reach it", ok);

    ok = paging_map(TEST_ALIAS_B, f, PAGE_WRITE);
    if (ok) { *(volatile u32 *)(TEST_ALIAS_B + 4) = 0x12345678u; ok = *(volatile u32 *)(TEST_ALIAS_A + 4) == 0x12345678u; }
    pass &= step("two virtual pages alias the same frame", ok);

    ok = paging_unmap(TEST_ALIAS_A) == f && paging_unmap(TEST_ALIAS_B) == f
      && !paging_translate(TEST_ALIAS_A, 0, 0) && !paging_translate(TEST_ALIAS_B, 0, 0)
      && paging_unmap(TEST_ALIAS_A) == 0;
    pass &= step("unmap removes the mappings (second unmap is a no-op)", ok);
    pmm_free_frame(f);

    u32 faults0 = demand_faults, tables1 = tables;
    u64 free1 = pmm_free_frames();
    ok = paging_demand_add(TEST_DEMAND, 16, PAGE_WRITE);
    for (u32 i = 0; i < 15; i++) *(volatile u32 *)(TEST_DEMAND + i * PAGE_SIZE + 8) = 0xD000u + i;
    u32 untouched = *(volatile u32 *)(TEST_DEMAND + 15 * PAGE_SIZE + 8);          /* a read also faults the page in */
    ok &= (demand_faults - faults0 == 16) && untouched == 0;
    pass &= step("demand paging: 16 pages appear on first touch (reads of fresh pages are zero)", ok);

    ok = 1;
    for (u32 i = 0; i < 15; i++) if (*(volatile u32 *)(TEST_DEMAND + i * PAGE_SIZE + 8) != 0xD000u + i) ok = 0;
    ok &= (demand_faults - faults0 == 16);                                         /* no new faults: pages stay mapped */
    ok &= (free1 - pmm_free_frames()) == 16 + (tables - tables1);
    pass &= step("the data survives, and exactly 16 frames (+ page tables) were used", ok);

    paging_demand_remove(TEST_DEMAND);
    ok = !paging_translate(TEST_DEMAND, 0, 0) && pmm_free_frames() == free0 - (tables - tables0);
    pass &= step("removing the region returns every frame (page tables stay allocated)", ok);

    console_set_color(pass ? COL_GREEN : COL_RED);
    puts(pass ? "Paging self-test PASSED\n" : "Paging self-test FAILED\n");
    return pass;
}
