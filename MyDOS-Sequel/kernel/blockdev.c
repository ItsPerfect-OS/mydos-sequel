/* blockdev.c - block device registry and RAM disks */
#include "blockdev.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"

#define MAX_DEVS 8
static struct blockdev *devs[MAX_DEVS];
static int ndevs;

int blockdev_register(struct blockdev *d) {
    if (ndevs == MAX_DEVS) return -1;
    devs[ndevs++] = d;
    return 0;
}
int blockdev_count(void) { return ndevs; }
struct blockdev *blockdev_get(int i) { return (i >= 0 && i < ndevs) ? devs[i] : 0; }
struct blockdev *blockdev_find(const char *name) {
    for (int i = 0; i < ndevs; i++) if (!strcmp(devs[i]->name, name)) return devs[i];
    return 0;
}

static int ram_read(struct blockdev *d, u32 lba, u32 count, void *buf) {
    if (lba > d->sectors || count > d->sectors - lba) return -1;
    memcpy(buf, (const void *)(d->priv + lba * SECTOR_SIZE), count * SECTOR_SIZE);
    return 0;
}
static int ram_write(struct blockdev *d, u32 lba, u32 count, const void *buf) {
    if (lba > d->sectors || count > d->sectors - lba) return -1;
    memcpy((void *)(d->priv + lba * SECTOR_SIZE), buf, count * SECTOR_SIZE);
    return 0;
}

struct blockdev *ramdisk_create(const char *name, u32 sectors, int do_register) {
    u32 frames = (sectors * SECTOR_SIZE + FRAME_SIZE - 1) / FRAME_SIZE;
    if (sectors == 0 || sectors > 0x100000) return 0;                 /* up to 512 MB */
    struct blockdev *d = kmalloc(sizeof(*d));
    if (!d) return 0;
    u64 base = pmm_alloc_contiguous(frames);
    if (!base) { kfree(d); return 0; }
    memset((void *)base, 0, frames * FRAME_SIZE);
    memset(d, 0, sizeof(*d));
    u32 i = 0;
    for (; name[i] && i < sizeof(d->name) - 1; i++) d->name[i] = name[i];
    const char *m = "RAM disk";
    for (i = 0; m[i]; i++) d->model[i] = m[i];
    d->kind = BD_RAM;
    d->sectors = sectors;
    d->priv = base;
    d->read = ram_read;
    d->write = ram_write;
    if (do_register && blockdev_register(d) != 0) {
        for (u32 f = 0; f < frames; f++) pmm_free_frame(base + (u64)f * FRAME_SIZE);
        kfree(d);
        return 0;
    }
    return d;
}

void ramdisk_destroy(struct blockdev *d) {
    if (!d || d->kind != BD_RAM) return;
    u32 frames = (d->sectors * SECTOR_SIZE + FRAME_SIZE - 1) / FRAME_SIZE;
    for (u32 f = 0; f < frames; f++) pmm_free_frame(d->priv + (u64)f * FRAME_SIZE);
    kfree(d);
}
