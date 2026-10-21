/* ata.c - legacy IDE (ATA) driver: PIO mode, LBA28, polling, no interrupts.
 *
 * Works with the classic primary (0x1F0) and secondary (0x170) channels, which is what
 * QEMU's "IDE" interface and UTM's i440FX/PIIX machine provide. (A Q35 machine exposes
 * SATA/AHCI instead, which this driver can't see.) Every wait loop is bounded: a missing
 * or dead drive must never hang the kernel.
 */
#include "blockdev.h"
#include "io.h"
#include "string.h"

#define ATA_TIMEOUT 400000u

#define R_DATA   0
#define R_COUNT  2
#define R_LBA0   3
#define R_LBA1   4
#define R_LBA2   5
#define R_DRIVE  6
#define R_STATUS 7
#define R_CMD    7

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_BSY 0x80

static const struct { u16 base, ctrl; } chan[2] = { { 0x1F0, 0x3F6 }, { 0x170, 0x376 } };
static struct blockdev ata_devs[4];
static int ata_count;

static void delay400ns(int c) { for (int i = 0; i < 4; i++) (void)inb(chan[c].ctrl); }

/* Wait until BSY clears. Returns 0 on success, -1 on timeout or floating bus. */
static int wait_ready(int c) {
    for (u32 i = 0; i < ATA_TIMEOUT; i++) {
        u8 s = inb(chan[c].base + R_STATUS);
        if (s == 0xFF) return -1;                     /* nothing is driving the bus */
        if (!(s & ST_BSY)) return 0;
    }
    return -1;
}

/* After a command: wait for DRQ. 0 = data ready, -1 = error/timeout. */
static int wait_drq(int c) {
    delay400ns(c);
    for (u32 i = 0; i < ATA_TIMEOUT; i++) {
        u8 s = inb(chan[c].base + R_STATUS);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) return -1;
        if (s & ST_DRQ) return 0;
    }
    return -1;
}

static int ata_rw(struct blockdev *d, u32 lba, u32 count, void *buf, int write) {
    int c = (int)((d->priv >> 1) & 1), slave = (int)(d->priv & 1);
    u16 base = chan[c].base;
    if (lba > d->sectors || count > d->sectors - lba) return -1;

    for (u32 n = 0; n < count; n++, lba++) {
        if (wait_ready(c) != 0) return -1;
        outb(base + R_DRIVE, 0xE0 | (slave << 4) | ((lba >> 24) & 0x0F));
        outb(base + R_COUNT, 1);
        outb(base + R_LBA0, lba & 0xFF);
        outb(base + R_LBA1, (lba >> 8) & 0xFF);
        outb(base + R_LBA2, (lba >> 16) & 0xFF);
        outb(base + R_CMD, write ? 0x30 : 0x20);
        if (wait_drq(c) != 0) return -1;
        if (write) {
            outsw(base + R_DATA, (const u8 *)buf + n * SECTOR_SIZE, 256);
            if (wait_ready(c) != 0) return -1;
        } else {
            insw(base + R_DATA, (u8 *)buf + n * SECTOR_SIZE, 256);
        }
    }
    if (write) {                                      /* flush the drive's write cache */
        outb(base + R_CMD, 0xE7);
        if (wait_ready(c) != 0) return -1;
    }
    return 0;
}

static int ata_read(struct blockdev *d, u32 lba, u32 count, void *buf)        { return ata_rw(d, lba, count, buf, 0); }
static int ata_write(struct blockdev *d, u32 lba, u32 count, const void *buf) { return ata_rw(d, lba, count, (void *)buf, 1); }

/* IDENTIFY one drive; fills `d` and returns 1 if a usable ATA disk is there. */
static int identify(int c, int slave, struct blockdev *d) {
    u16 base = chan[c].base;
    outb(base + R_DRIVE, 0xA0 | (slave << 4));
    delay400ns(c);
    outb(base + R_COUNT, 0); outb(base + R_LBA0, 0); outb(base + R_LBA1, 0); outb(base + R_LBA2, 0);
    outb(base + R_CMD, 0xEC);
    u8 s = inb(base + R_STATUS);
    if (s == 0 || s == 0xFF) return 0;                /* no drive */
    if (wait_ready(c) != 0) return 0;
    if (inb(base + R_LBA1) || inb(base + R_LBA2)) return 0;   /* ATAPI (CD-ROM) or SATA signature: not ours */
    if (wait_drq(c) != 0) return 0;

    u16 id[256];
    insw(base + R_DATA, id, 256);
    u32 sectors = (u32)id[60] | ((u32)id[61] << 16);  /* LBA28 capacity */
    if (sectors == 0) return 0;
    if (sectors > 0x0FFFFFFF) sectors = 0x0FFFFFFF;

    memset(d, 0, sizeof(*d));
    d->kind = BD_ATA;
    d->sectors = sectors;
    d->priv = ((u32)c << 1) | (u32)slave;
    d->read = ata_read;
    d->write = ata_write;
    int n = 0;
    for (int w = 27; w <= 46; w++) {                  /* model string: big-endian byte pairs */
        d->model[n++] = (char)(id[w] >> 8);
        d->model[n++] = (char)(id[w] & 0xFF);
    }
    while (n > 0 && (d->model[n - 1] == ' ' || d->model[n - 1] == 0)) n--;
    d->model[n] = 0;
    return 1;
}

int ata_init(void) {
    static const char names[4][4] = { "hda", "hdb", "hdc", "hdd" };
    ata_count = 0;
    for (int c = 0; c < 2; c++) {
        if (inb(chan[c].base + R_STATUS) == 0xFF) continue;     /* channel not present */
        outb(chan[c].ctrl, 0x02);                                /* nIEN: we poll, no IRQs */
        for (int slave = 0; slave < 2; slave++) {
            int idx = c * 2 + slave;
            if (!identify(c, slave, &ata_devs[idx])) continue;
            for (int i = 0; i < 4; i++) ata_devs[idx].name[i] = names[idx][i];
            if (blockdev_register(&ata_devs[idx]) == 0) ata_count++;
        }
    }
    return ata_count;
}
