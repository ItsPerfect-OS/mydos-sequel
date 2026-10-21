#ifndef RPFS_H
#define RPFS_H
#include "types.h"
#include "blockdev.h"

/* ---------------------------------------------------------------------------
 * RPFS Sequel on-disk format (all integers little-endian, block = 1024 bytes)
 *
 *   block 0                    superblock
 *   bitmap_start ..            block bitmap: 1 bit per block, 1 = in use (8192 blocks per bitmap block)
 *   itable_start ..            inode table: 64-byte inodes, 16 per block, inode numbers start at 1
 *   data_start ..              data blocks (file contents, directory contents, indirect blocks)
 *
 *   inode:  mode (0 free, 1 file, 2 directory), size, mtime (unix seconds),
 *           12 direct block pointers + 1 single-indirect block (256 more pointers)
 *           => files up to (12 + 256) KB = 268 KB
 *   directory: array of 32-byte entries { u32 inode (0 = unused); char name[28] }, always containing
 *              "." and ".."; names are 1..27 bytes, no '/'.
 * ------------------------------------------------------------------------- */
#define RPFS_MAGIC      0x53465052u          /* "RPFS" read as little-endian bytes */
#define RPFS_VERSION    1u
#define RPFS_BLOCK      1024u
#define RPFS_SECTORS    2u                   /* sectors per block */
#define RPFS_ROOT       1u
#define RPFS_NDIRECT    12u
#define RPFS_PTRS       (RPFS_BLOCK / 4)     /* pointers per indirect block */
#define RPFS_NAME_MAX   27u
#define RPFS_MAX_FILE   ((RPFS_NDIRECT + RPFS_PTRS) * RPFS_BLOCK)
#define RPFS_PATH_MAX   128

#define RPFS_FILE 1u
#define RPFS_DIR  2u

struct rpfs_sb {
    u32 magic, version, block_size, total_blocks, inode_count;
    u32 bitmap_start, bitmap_blocks, itable_start, itable_blocks, data_start;
    u32 free_blocks, free_inodes, root_ino;
    char label[16];
} __attribute__((packed));

struct rpfs_inode {
    u16 mode, flags;
    u32 size, mtime;
    u32 blocks[RPFS_NDIRECT];
    u32 indirect;
} __attribute__((packed));

struct rpfs_dirent {
    u32 ino;
    char name[28];
} __attribute__((packed));

typedef char rpfs_sb_size_check[(sizeof(struct rpfs_sb) == 68) ? 1 : -1];
typedef char rpfs_inode_size_check[(sizeof(struct rpfs_inode) == 64) ? 1 : -1];
typedef char rpfs_dirent_size_check[(sizeof(struct rpfs_dirent) == 32) ? 1 : -1];

/* A mounted filesystem. */
struct rpfs {
    struct blockdev *dev;
    struct rpfs_sb sb;
    u32 hint;                                /* block-bitmap block to start allocation searches at */
    u32 cwd;                                 /* inode of the current directory */
    char cwd_path[RPFS_PATH_MAX];
    int mounted;
};

struct rpfs_stat { u32 ino, mode, size, mtime, blocks; };
typedef int (*rpfs_list_cb)(const char *name, const struct rpfs_stat *st, void *ctx);   /* return nonzero to stop */

enum {
    RPFS_OK = 0,
    RPFS_ENOENT = -1, RPFS_EEXIST = -2, RPFS_ENOSPC = -3, RPFS_ENOTDIR = -4, RPFS_EISDIR = -5,
    RPFS_ENOTEMPTY = -6, RPFS_EINVAL = -7, RPFS_EIO = -8, RPFS_EFBIG = -9, RPFS_ENOFS = -10, RPFS_ECORRUPT = -11
};
const char *rpfs_strerror(int err);

int  rpfs_format(struct blockdev *dev, const char *label);
int  rpfs_probe(struct blockdev *dev, char label[17]);       /* 1 if sector 0 holds an RPFS superblock */
int  rpfs_mount(struct rpfs *fs, struct blockdev *dev);
void rpfs_unmount(struct rpfs *fs);

int  rpfs_stat(struct rpfs *fs, const char *path, struct rpfs_stat *st);
int  rpfs_list(struct rpfs *fs, const char *path, rpfs_list_cb cb, void *ctx);
int  rpfs_read(struct rpfs *fs, const char *path, u32 offset, void *buf, u32 len, u32 *got);
int  rpfs_write(struct rpfs *fs, const char *path, const void *data, u32 len, int append);
int  rpfs_mkdir(struct rpfs *fs, const char *path);
int  rpfs_unlink(struct rpfs *fs, const char *path);          /* files only */
int  rpfs_rmdir(struct rpfs *fs, const char *path);
int  rpfs_rename(struct rpfs *fs, const char *from, const char *to);   /* files only */
int  rpfs_chdir(struct rpfs *fs, const char *path);
void rpfs_df(struct rpfs *fs, u32 *total, u32 *free_blocks, u32 *inodes, u32 *free_inodes);
int  rpfs_fsck(struct rpfs *fs, int verbose);                 /* number of problems found; 0 = clean */
int  rpfs_selftest(void);                                     /* `fstest`; runs on a private RAM disk */
#endif
