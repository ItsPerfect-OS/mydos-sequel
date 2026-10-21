#ifndef MEMMAP_H
#define MEMMAP_H
#include "types.h"

#define E820_USABLE   1
#define E820_RESERVED 2
#define E820_ACPI_RECLAIM 3
#define E820_ACPI_NVS 4
#define E820_BAD      5

/* One BIOS E820 entry, exactly as stage 2 stored it. */
struct e820_entry {
    u64 base;
    u64 length;
    u32 type;
    u32 acpi_attrs;
} __attribute__((packed));

void  memmap_init(void);                       /* read the table stage 2 left at 0x500 */
int   memmap_count(void);
const struct e820_entry *memmap_entry(int i);
int   memmap_is_fallback(void);                /* 1 if the BIOS gave us no map and we guessed */
u64   memmap_usable_bytes(void);               /* sum of all E820_USABLE entries */
void  memmap_print(void);                      /* the `meminfo` output */
#endif
