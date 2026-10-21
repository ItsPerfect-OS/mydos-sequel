/* heap.c - kernel heap: kmalloc / kfree on top of the physical memory manager
 *
 * Design (simple and checkable, not fast):
 *   - The heap is made of "chunks": runs of contiguous physical frames from
 *     pmm_alloc_contiguous(). There is no paging yet, so physical == virtual.
 *   - Every chunk is cut into blocks, each with a 16-byte header, kept in one
 *     doubly linked list. Allocation is first-fit with splitting; kfree merges
 *     neighbours, but only inside the same chunk.
 *   - When a free leaves a non-initial chunk as one single free block, the chunk's
 *     frames go back to the PMM. The heap grows on demand (at least 64 KB at a time).
 *   - Headers carry magic values, so double frees and wild pointers are caught.
 */
#include "heap.h"
#include "pmm.h"
#include "console.h"
#include "string.h"

#define MAGIC_USED    0xA110C8EDu
#define MAGIC_FREE    0xF4EEF4EEu
#define ALIGN         16u
#define INITIAL_CHUNK (256u * 1024)
#define MIN_CHUNK     (64u * 1024)
#define MAX_CHUNKS    32
#define MAX_ALLOC     0x40000000u          /* 1 GB: anything larger is a bug */

struct block {
    u32 magic;
    u32 size;                              /* payload bytes, multiple of 16 */
    struct block *prev, *next;             /* list order = creation order of chunks, address order inside a chunk */
    u64 pad;                               /* keeps the header at 32 bytes so payloads stay 16-byte aligned */
};
#define HDR ((u32)sizeof(struct block))
typedef char hdr_is_32_bytes[(sizeof(struct block) == 32) ? 1 : -1];

static struct block *head, *tail;
static struct { u64 base; u32 size; } chunks[MAX_CHUNKS];
static int nchunks;

static int chunk_of(u64 addr) {
    for (int i = 0; i < nchunks; i++)
        if (addr >= chunks[i].base && addr < chunks[i].base + chunks[i].size) return i;
    return -1;
}

/* Append a fresh chunk of `bytes` (multiple of 4096) as one big free block. */
static struct block *add_chunk(u32 bytes) {
    if (nchunks == MAX_CHUNKS) return 0;
    u64 base = pmm_alloc_contiguous(bytes / FRAME_SIZE);
    if (!base) return 0;
    chunks[nchunks].base = base;
    chunks[nchunks].size = bytes;
    nchunks++;

    struct block *b = (struct block *)base;
    b->magic = MAGIC_FREE;
    b->size  = bytes - HDR;
    b->prev  = tail;
    b->next  = 0;
    if (tail) tail->next = b; else head = b;
    tail = b;
    return b;
}

int heap_init(void) {
    head = tail = 0;
    nchunks = 0;
    return add_chunk(INITIAL_CHUNK) != 0;
}

void *kmalloc(u32 n) {
    if (n == 0 || n > MAX_ALLOC) return 0;
    n = (n + ALIGN - 1) & ~(ALIGN - 1);

    for (;;) {
        for (struct block *b = head; b; b = b->next) {
            if (b->magic != MAGIC_FREE || b->size < n) continue;
            if (b->size >= n + HDR + ALIGN) {                 /* split off the remainder */
                struct block *nb = (struct block *)((u8 *)b + HDR + n);
                nb->magic = MAGIC_FREE;
                nb->size  = b->size - n - HDR;
                nb->prev  = b;
                nb->next  = b->next;
                if (b->next) b->next->prev = nb; else tail = nb;
                b->next = nb;
                b->size = n;
            }
            b->magic = MAGIC_USED;
            return (void *)(b + 1);
        }
        u32 need = (n + HDR + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
        if (need < MIN_CHUNK) need = MIN_CHUNK;
        if (!add_chunk(need)) return 0;                       /* out of memory */
    }
}

void *kcalloc(u32 count, u32 size) {
    u32 total;
    if (__builtin_mul_overflow(count, size, &total)) return 0;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static int same_chunk(const void *a, const void *b) {
    int i = chunk_of((u64)a);
    return i >= 0 && i == chunk_of((u64)b);
}

/* Give a chunk back to the PMM when it has collapsed into one free block (never the first chunk). */
static void maybe_release(struct block *b) {
    for (int i = 1; i < nchunks; i++) {
        if (chunks[i].base != (u64)b || chunks[i].size != b->size + HDR) continue;
        if (b->prev) b->prev->next = b->next; else head = b->next;
        if (b->next) b->next->prev = b->prev; else tail = b->prev;
        u64 base = chunks[i].base; u32 size = chunks[i].size;
        for (int j = i; j < nchunks - 1; j++) chunks[j] = chunks[j + 1];
        nchunks--;
        for (u32 off = 0; off < size; off += FRAME_SIZE) pmm_free_frame(base + off);
        return;
    }
}

/* 0 = freed, 1 = invalid pointer, 2 = double free */
static int try_kfree(void *p) {
    u64 a = (u64)p;
    if (!p || (a & (ALIGN - 1)) || chunk_of(a) < 0 || a < HDR) return 1;
    struct block *b = (struct block *)(a - HDR);
    if (chunk_of((u64)b) != chunk_of(a)) return 1;
    if (b->magic == MAGIC_FREE) return 2;
    if (b->magic != MAGIC_USED) return 1;
    b->magic = MAGIC_FREE;

    struct block *nx = b->next;
    if (nx && nx->magic == MAGIC_FREE && same_chunk(b, nx)) {
        b->size += HDR + nx->size;
        b->next = nx->next;
        if (nx->next) nx->next->prev = b; else tail = b;
        nx->magic = 0;
    }
    struct block *pv = b->prev;
    if (pv && pv->magic == MAGIC_FREE && same_chunk(pv, b)) {
        pv->size += HDR + b->size;
        pv->next = b->next;
        if (b->next) b->next->prev = pv; else tail = pv;
        b->magic = 0;
        b = pv;
    }
    maybe_release(b);
    return 0;
}

void kfree(void *p) {
    if (!p) return;
    int r = try_kfree(p);
    if (r == 0) return;
    console_set_color(COL_RED);
    puts(r == 2 ? "heap: double free of " : "heap: invalid pointer passed to kfree: ");
    put_hex64((u64)p); putc('\n');
}

void *krealloc(void *p, u32 n) {
    if (!p) return kmalloc(n);
    if (n == 0) { kfree(p); return 0; }
    struct block *b = (struct block *)((u8 *)p - HDR);
    if (chunk_of((u64)p) < 0 || ((u64)p & (ALIGN - 1)) || b->magic != MAGIC_USED) {
        console_set_color(COL_RED);
        puts("heap: invalid pointer passed to krealloc: "); put_hex64((u64)p); putc('\n');
        return 0;
    }
    if (b->size >= n) return p;
    void *q = kmalloc(n);
    if (!q) return 0;                                         /* old block stays valid */
    memcpy(q, p, b->size);
    kfree(p);
    return q;
}

/* ---------------- statistics ---------------- */

struct heap_stats { u32 chunks, total, used_bytes, used_blocks, free_bytes, free_blocks, largest_free; };

static void collect(struct heap_stats *s) {
    memset(s, 0, sizeof(*s));
    s->chunks = (u32)nchunks;
    for (int i = 0; i < nchunks; i++) s->total += chunks[i].size;
    for (struct block *b = head; b; b = b->next) {
        if (b->magic == MAGIC_USED) { s->used_bytes += b->size; s->used_blocks++; }
        else { s->free_bytes += b->size; s->free_blocks++; if (b->size > s->largest_free) s->largest_free = b->size; }
    }
}

static void put_bytes(u32 b) {
    if (b >= 1024 * 1024) { put_dec(b / (1024 * 1024)); puts(" MB"); }
    else if (b >= 1024)   { put_dec(b / 1024); puts(" KB"); }
    else                  { put_dec(b); puts(" B"); }
}

u32 heap_used_bytes(void) { struct heap_stats s; collect(&s); return s.used_bytes; }

void heap_print(void) {
    struct heap_stats s;
    collect(&s);
    console_set_color(COL_CYAN);  puts("Kernel heap:\n");
    console_set_color(COL_GREY);
    puts("  chunks: "); put_dec(s.chunks); puts("  ("); put_bytes(s.total); puts(" from the PMM)\n");
    puts("  used:   "); put_bytes(s.used_bytes); puts(" in "); put_dec(s.used_blocks); puts(" blocks\n");
    console_set_color(COL_GREEN);
    puts("  free:   "); put_bytes(s.free_bytes); puts(" in "); put_dec(s.free_blocks);
    puts(" blocks (largest "); put_bytes(s.largest_free); puts(")\n");
}

/* ---------------- self-test (`heaptest`) ---------------- */

#define TEST_N 100

static int step(const char *name, int ok) {
    console_set_color(COL_GREY); puts("  "); puts(name); puts(": ");
    console_set_color(ok ? COL_GREEN : COL_RED); puts(ok ? "ok\n" : "FAILED\n");
    return ok;
}
static void fill_bytes(void *p, u32 n, u8 v) { u8 *q = p; for (u32 i = 0; i < n; i++) q[i] = v; }
static int  check_bytes(const void *p, u32 n, u8 v) { const u8 *q = p; for (u32 i = 0; i < n; i++) if (q[i] != v) return 0; return 1; }

int heap_selftest(void) {
    static void *ptr[TEST_N];
    static u32 size[TEST_N];
    struct heap_stats s0, s;
    int pass = 1, ok;

    collect(&s0);
    u32 seed = 12345;

    ok = 1;
    for (int i = 0; i < TEST_N; i++) {
        seed = seed * 1103515245u + 12345u;
        size[i] = 1 + ((seed >> 16) % 2000);
        ptr[i] = kmalloc(size[i]);
        if (!ptr[i] || ((u64)ptr[i] & 15)) ok = 0;
    }
    pass &= step("100 allocations of 1-2000 bytes (non-NULL, 16-byte aligned)", ok);

    for (int i = 0; i < TEST_N; i++) if (ptr[i]) fill_bytes(ptr[i], size[i], (u8)(i + 1));
    ok = 1;
    for (int i = 0; i < TEST_N; i++) if (!ptr[i] || !check_bytes(ptr[i], size[i], (u8)(i + 1))) ok = 0;
    pass &= step("blocks do not overlap (every byte keeps its pattern)", ok);

    ok = 1;
    for (int i = 1; i < TEST_N; i += 2) if (try_kfree(ptr[i]) != 0) ok = 0;
    for (int i = 0; i < TEST_N; i += 2) if (!check_bytes(ptr[i], size[i], (u8)(i + 1))) ok = 0;
    pass &= step("free every other block; the rest stay intact", ok);

    pass &= step("double free is detected", try_kfree(ptr[1]) == 2);
    pass &= step("wild and misaligned pointers are rejected",
                 try_kfree((void *)0x1234) == 1 && try_kfree((u8 *)ptr[0] + 1) == 1 && try_kfree(0) == 1);

    ok = 1;
    static void *again[TEST_N / 2];
    for (int i = 0; i < TEST_N / 2; i++) {
        again[i] = kmalloc(60);
        if (!again[i]) { ok = 0; continue; }
        fill_bytes(again[i], 60, 0xEE);
    }
    for (int i = 0; i < TEST_N; i += 2) if (!check_bytes(ptr[i], size[i], (u8)(i + 1))) ok = 0;
    pass &= step("freed space is reused without touching live blocks", ok);

    for (int i = 0; i < TEST_N / 2; i++) if (again[i]) try_kfree(again[i]);
    for (int i = 0; i < TEST_N; i += 2) try_kfree(ptr[i]);
    collect(&s);
    pass &= step("everything freed: heap merged back to its starting shape",
                 s.used_blocks == s0.used_blocks && s.used_bytes == s0.used_bytes && s.free_blocks == s0.free_blocks);

    {   u8 *z = kcalloc(100, 10);
        ok = z && check_bytes(z, 1000, 0);
        pass &= step("kcalloc returns zeroed memory", ok);
        pass &= step("kcalloc detects multiplication overflow", kcalloc(0x10000000u, 0x20u) == 0);
        kfree(z); }

    {   u8 *p = kmalloc(32);
        fill_bytes(p, 32, 0x5A);
        u8 *q = krealloc(p, 5000);
        ok = q && check_bytes(q, 32, 0x5A);
        pass &= step("krealloc grows and keeps the data", ok);
        kfree(q); }

    {   u32 frames0 = pmm_free_frames();
        collect(&s0);
        u8 *big = kmalloc(300 * 1024);
        collect(&s);
        ok = big && s.chunks == s0.chunks + 1;
        if (big) { fill_bytes(big, 300 * 1024, 0x77); ok &= check_bytes(big, 300 * 1024, 0x77); }
        pass &= step("300 KB request grows the heap by one chunk", ok);
        kfree(big);
        collect(&s);
        pass &= step("the grown chunk goes back to the PMM on free",
                     s.chunks == s0.chunks && pmm_free_frames() == frames0); }

    pass &= step("absurd and zero-size requests return NULL", kmalloc(0xFFFFFFF0u) == 0 && kmalloc(0) == 0);

    console_set_color(pass ? COL_GREEN : COL_RED);
    puts(pass ? "Heap self-test PASSED\n" : "Heap self-test FAILED\n");
    return pass;
}
