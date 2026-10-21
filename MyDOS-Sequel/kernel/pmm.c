/* pmm.c - physical memory manager: bitmap frame allocator (1 bit per 4 KB frame)
 *
 * Policy:
 *   - Only RAM the BIOS reports as usable (E820 type 1) is ever handed out.
 *   - Everything below the end of the bitmap stays reserved: BIOS data, VGA, our boot structures,
 *     the kernel image and the bitmap itself (the kernel runs at 1 MB, the bitmap follows it).
 *     Frame 0 is therefore never returned, so a return value of 0 can mean "out of memory".
 *   - Up to 64 GB of RAM is managed; the bitmap is sized from the E820 map at boot.
 *
 * Bitmap convention: bit = 1 -> used / not available, bit = 0 -> free.
 */
#include "pmm.h"
#include "memmap.h"
#include "console.h"

#define MAX_RAM       (64ULL << 30)             /* manage at most 64 GB (the bitmap would be 2 MB) */
#define LOW_RESERVED  0x100000ULL               /* the first 1 MB is never allocated */

extern char __kernel_end[];                     /* from linker.ld: end of kernel image incl. .bss */

static u64 *bitmap;                             /* one bit per frame, placed right after the kernel image */
static u64 bitmap_words, bitmap_end, max_frames;
static u64 total_frames, free_frames, search_hint;

static inline int  frame_used(u64 f) { return (bitmap[f >> 6] >> (f & 63)) & 1; }
static inline void mark_used(u64 f)  { bitmap[f >> 6] |=  (1ULL << (f & 63)); }
static inline void mark_free(u64 f)  { bitmap[f >> 6] &= ~(1ULL << (f & 63)); }

int pmm_init(void) {
    /* top of usable RAM */
    u64 top = 0;
    for (int i = 0; i < memmap_count(); i++) {
        const struct e820_entry *e = memmap_entry(i);
        if (e->type != E820_USABLE) continue;
        u64 end = e->base + e->length;
        if (end > top) top = end;
    }
    if (top > MAX_RAM) top = MAX_RAM;
    max_frames = top >> 12;
    bitmap_words = (max_frames + 63) / 64;

    /* the bitmap lives in RAM right after the kernel; it must sit inside a usable E820 region */
    u64 start = ((u64)__kernel_end + FRAME_SIZE - 1) & ~(u64)(FRAME_SIZE - 1);
    u64 bytes = (bitmap_words * 8 + FRAME_SIZE - 1) & ~(u64)(FRAME_SIZE - 1);
    bitmap_end = start + bytes;
    int fits = 0;
    for (int i = 0; i < memmap_count(); i++) {
        const struct e820_entry *e = memmap_entry(i);
        if (e->type == E820_USABLE && start >= e->base && bitmap_end <= e->base + e->length) fits = 1;
    }
    if (!fits || bitmap_end > (1ULL << 30)) return 0;       /* must also be inside the 1 GB boot identity map */
    bitmap = (u64 *)start;

    for (u64 i = 0; i < bitmap_words; i++) bitmap[i] = ~0ULL;   /* everything used until proven usable */
    total_frames = free_frames = search_hint = 0;

    for (int i = 0; i < memmap_count(); i++) {
        const struct e820_entry *e = memmap_entry(i);
        if (e->type != E820_USABLE) continue;
        u64 s = e->base, en = e->base + e->length;                 /* en exclusive */
        if (en > top) en = top;
        if (s < bitmap_end) s = bitmap_end;                        /* never touch the kernel, the bitmap or low memory */
        if (s < LOW_RESERVED) s = LOW_RESERVED;
        if (en <= s) continue;
        u64 first = (s + FRAME_SIZE - 1) >> 12;                    /* round start up */
        u64 last  = en >> 12;                                      /* round end down, exclusive */
        for (u64 f = first; f < last; f++) {
            if (frame_used(f)) { mark_free(f); total_frames++; free_frames++; }   /* skip overlaps */
        }
    }
    return 1;
}

u64 pmm_alloc_frame(void) {
    for (u64 n = 0; n < bitmap_words; n++) {
        u64 w = (search_hint + n) % bitmap_words;
        u64 v = bitmap[w];
        if (v != ~0ULL) {
            u64 f = w * 64 + (u64)__builtin_ctzll(~v);             /* lowest clear bit */
            mark_used(f);
            free_frames--;
            search_hint = w;
            return f << 12;
        }
    }
    return 0;
}

u64 pmm_alloc_contiguous(u64 count) {
    if (count == 0 || count > free_frames) return 0;
    u64 run = 0, start = 0;
    for (u64 w = 0; w < bitmap_words; w++) {
        u64 v = bitmap[w];
        if (v == ~0ULL) { run = 0; continue; }                     /* whole word used */
        for (u64 bit = 0; bit < 64; bit++) {
            if ((v >> bit) & 1) { run = 0; continue; }
            if (run == 0) start = w * 64 + bit;
            if (++run == count) {
                for (u64 i = 0; i < count; i++) mark_used(start + i);
                free_frames -= count;
                return start << 12;
            }
        }
    }
    return 0;
}

/* Is this frame address inside RAM that the PMM manages? */
static int is_managed(u64 addr) {
    if (addr < LOW_RESERVED || addr < bitmap_end) return 0;      /* kernel, bitmap and low memory are never managed */
    for (int i = 0; i < memmap_count(); i++) {
        const struct e820_entry *e = memmap_entry(i);
        if (e->type != E820_USABLE) continue;
        if (addr >= e->base && addr + FRAME_SIZE <= e->base + e->length && (addr >> 12) < max_frames) return 1;
    }
    return 0;
}

/* 0 = freed, 1 = bad address, 2 = double free */
static int try_free(u64 addr) {
    if ((addr & (FRAME_SIZE - 1)) || !is_managed(addr)) return 1;
    u64 f = addr >> 12;
    if (!frame_used(f)) return 2;
    mark_free(f);
    free_frames++;
    if ((f >> 6) < search_hint) search_hint = f >> 6;
    return 0;
}

void pmm_free_frame(u64 addr) {
    int r = try_free(addr);
    if (r == 0) return;
    console_set_color(COL_RED);
    puts(r == 1 ? "PMM: refusing to free invalid frame " : "PMM: double free of frame ");
    put_hex64(addr); putc('\n');
}

u64 pmm_total_frames(void) { return total_frames; }
u64 pmm_free_frames(void)  { return free_frames; }

static void put_frames(u64 frames) {                       /* "N frames (X MB)" */
    u64 kb = frames * 4;
    put_dec((u32)frames); puts(" frames (");
    if (kb >= 16ULL * 1024 * 1024) { put_dec((u32)(kb / (1024 * 1024))); puts(" GB)"); }       /* MB until it gets big */
    else if (kb >= 1024) { put_dec((u32)(kb / 1024)); puts(" MB)"); }
    else { put_dec((u32)kb); puts(" KB)"); }
}

void pmm_print(void) {
    console_set_color(COL_CYAN);
    puts("Physical memory manager (4 KB frames):\n");
    console_set_color(COL_GREY);
    puts("  managed: "); put_frames(total_frames); putc('\n');
    console_set_color(COL_GREEN);
    puts("  free:    "); put_frames(free_frames); putc('\n');
    console_set_color(COL_GREY);
    puts("  used:    "); put_frames(total_frames - free_frames); putc('\n');
    puts("  kernel image (with .bss): 0x100000 - "); put_hex64((u64)__kernel_end); putc('\n');
    puts("  frame bitmap:             "); put_hex64((u64)bitmap); puts(" - "); put_hex64(bitmap_end); putc('\n');
    puts("  everything below the end of the bitmap is reserved and never allocated\n");
}

/* ---------------- self-test (`pmmtest`) ---------------- */

#define TEST_N 256

static void fill(u64 addr, u32 pattern) { volatile u32 *p = (volatile u32 *)addr; for (u32 i = 0; i < FRAME_SIZE / 4; i++) p[i] = pattern; }
static int  check(u64 addr, u32 pattern) { volatile u32 *p = (volatile u32 *)addr; for (u32 i = 0; i < FRAME_SIZE / 4; i++) if (p[i] != pattern) return 0; return 1; }

static int step(const char *name, int ok) {
    console_set_color(COL_GREY); puts("  "); puts(name); puts(": ");
    console_set_color(ok ? COL_GREEN : COL_RED); puts(ok ? "ok\n" : "FAILED\n");
    return ok;
}

int pmm_selftest(void) {
    static u64 fr[TEST_N];
    u64 free0 = free_frames;
    int pass = 1;

    if (free0 < TEST_N + 8) { puts("  not enough free frames to test\n"); return 0; }

    int ok = 1;
    for (int i = 0; i < TEST_N; i++) {
        fr[i] = pmm_alloc_frame();
        if (fr[i] == 0 || (fr[i] & 0xFFF) || fr[i] < LOW_RESERVED || !is_managed(fr[i])) ok = 0;
    }
    pass &= step("allocate 256 frames (aligned, >= 1 MB, in usable RAM)", ok);
    pass &= step("free count dropped by 256", free_frames == free0 - TEST_N);

    ok = 1;
    for (int i = 0; i < TEST_N; i++) for (int j = i + 1; j < TEST_N; j++) if (fr[i] == fr[j]) ok = 0;
    pass &= step("all 256 frames are distinct", ok);

    for (int i = 0; i < TEST_N; i++) fill(fr[i], 0xA5A50000u + (u32)i);
    ok = 1;
    for (int i = 0; i < TEST_N; i++) if (!check(fr[i], 0xA5A50000u + (u32)i)) ok = 0;
    pass &= step("write/read every byte of every frame (no overlaps)", ok);

    ok = 1;
    for (int i = 1; i < TEST_N; i += 2) if (try_free(fr[i]) != 0) ok = 0;
    pass &= step("free every other frame", ok && free_frames == free0 - TEST_N / 2);

    pass &= step("double free is detected", try_free(fr[1]) == 2 && free_frames == free0 - TEST_N / 2);
    pass &= step("unaligned / low-memory frees are rejected",
                 try_free(fr[0] + 1) == 1 && try_free(0x9F000) == 1 && try_free(0x100000) == 1 && try_free(0) == 1);

    ok = 1;
    for (int i = 1; i < TEST_N; i += 2) {                          /* reallocate: must not touch live frames */
        u64 f = pmm_alloc_frame();
        if (f == 0) { ok = 0; continue; }
        for (int j = 0; j < TEST_N; j += 2) if (f == fr[j]) ok = 0;
        fr[i] = f;
    }
    ok &= check(fr[0], 0xA5A50000u);                                /* a live frame is still intact */
    pass &= step("reallocation never returns a live frame", ok);

    ok = 1;
    for (int i = 0; i < TEST_N; i++) if (try_free(fr[i]) != 0) ok = 0;
    pass &= step("free everything back", ok && free_frames == free0);

    {   /* contiguous allocation */
        u64 c = pmm_alloc_contiguous(16);
        ok = (c != 0) && !(c & 0xFFF) && is_managed(c) && free_frames == free0 - 16;
        for (u64 i = 0; c && i < 16; i++) if (try_free(c + i * FRAME_SIZE) != 0) ok = 0;
        pass &= step("16 contiguous frames allocate and free", ok && free_frames == free0);
    }

    console_set_color(pass ? COL_GREEN : COL_RED);
    puts(pass ? "PMM self-test PASSED\n" : "PMM self-test FAILED\n");
    return pass;
}
