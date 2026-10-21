/* memmap.c - BIOS E820 physical memory map (collected by stage 2 in real mode) */
#include "memmap.h"
#include "console.h"

#define E820_COUNT_ADDR   0x500
#define E820_ENTRIES_ADDR 0x504
#define E820_MAX          32

static struct e820_entry table[E820_MAX];       /* private copy: page 0 gets unmapped once paging is on */
static const struct e820_entry *entries;
static int  count;
static int  fallback;
static struct e820_entry fallback_entry;

void memmap_init(void) {
    u32 n = *(volatile u32 *)E820_COUNT_ADDR;
    if (n == 0 || n > E820_MAX) {
        /* No E820: assume 15 MB of RAM above 1 MB. Conservative, but lets boot continue. */
        fallback = 1;
        fallback_entry.base = 0x100000;
        fallback_entry.length = 15u * 1024 * 1024;
        fallback_entry.type = E820_USABLE;
        fallback_entry.acpi_attrs = 1;
        entries = &fallback_entry;
        count = 1;
        return;
    }
    const struct e820_entry *src = (const struct e820_entry *)E820_ENTRIES_ADDR;
    for (u32 i = 0; i < n; i++) table[i] = src[i];
    entries = table;
    count = (int)n;
}

int memmap_count(void) { return count; }
int memmap_is_fallback(void) { return fallback; }
const struct e820_entry *memmap_entry(int i) { return &entries[i]; }

u64 memmap_usable_bytes(void) {
    u64 total = 0;
    for (int i = 0; i < count; i++)
        if (entries[i].type == E820_USABLE) total += entries[i].length;
    return total;
}

static const char *type_name(u32 t) {
    switch (t) {
        case E820_USABLE:       return "usable";
        case E820_RESERVED:     return "reserved";
        case E820_ACPI_RECLAIM: return "ACPI reclaimable";
        case E820_ACPI_NVS:     return "ACPI NVS";
        case E820_BAD:          return "bad memory";
        default:                return "unknown";
    }
}

static void put_size(u64 bytes) {
    u32 kb = (u32)(bytes >> 10);
    if (kb >= 1024) { put_dec(kb / 1024); puts(" MB"); }
    else            { put_dec(kb);        puts(" KB"); }
}

void memmap_print(void) {
    console_set_color(COL_CYAN);
    puts(fallback ? "Physical memory (BIOS gave no map - ASSUMED):\n"
                  : "Physical memory map (BIOS E820):\n");
    console_set_color(COL_GREY);
    puts("  start               end                 size       type\n");
    for (int i = 0; i < count; i++) {
        const struct e820_entry *e = &entries[i];
        console_set_color(e->type == E820_USABLE ? COL_GREEN : COL_GREY);
        puts("  "); put_hex64(e->base);
        puts("  "); put_hex64(e->base + e->length - 1);
        puts("  ");
        put_size(e->length);
        {   /* pad the size column to 11 chars: digits + " KB"/" MB" */
            u32 kb = (u32)(e->length >> 10);
            u32 digits = 0, v = (kb >= 1024) ? kb / 1024 : kb;
            do { digits++; v /= 10; } while (v);
            for (u32 pad = digits + 3; pad < 11; pad++) putc(' ');
        }
        puts(type_name(e->type));
        putc('\n');
    }
    console_set_color(COL_YELLOW);
    puts("Usable RAM: "); put_size(memmap_usable_bytes()); putc('\n');
}
