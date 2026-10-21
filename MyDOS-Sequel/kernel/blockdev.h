#ifndef BLOCKDEV_H
#define BLOCKDEV_H
#include "types.h"

#define SECTOR_SIZE 512

enum { BD_ATA = 1, BD_RAM = 2 };

/* A disk made of 512-byte sectors. read/write return 0 on success, -1 on error. */
struct blockdev {
    char name[8];
    char model[41];
    int  kind;
    u32  sectors;
    uptr priv;                                   /* ATA: (channel << 1) | slave; RAM: base address */
    int (*read)(struct blockdev *d, u32 lba, u32 count, void *buf);
    int (*write)(struct blockdev *d, u32 lba, u32 count, const void *buf);
};

int  blockdev_register(struct blockdev *d);
int  blockdev_count(void);
struct blockdev *blockdev_get(int i);
struct blockdev *blockdev_find(const char *name);

/* RAM disks live in contiguous PMM frames (identity-mapped, so plain memcpy works). */
struct blockdev *ramdisk_create(const char *name, u32 sectors, int do_register);
void ramdisk_destroy(struct blockdev *d);        /* unregistered disks only */

int  ata_init(void);                             /* probe both IDE channels; registers hda..hdd; returns count */
#endif
