/* rpfs.c - RPFS Sequel: a small Unix-style filesystem (see rpfs.h for the on-disk format)
 *
 * Everything is write-through: each change goes to the device immediately, in an order
 * that leaves at worst leaked blocks (which rpfs_fsck can find) rather than damaged files.
 */
#include "rpfs.h"
#include "rtc.h"
#include "heap.h"
#include "string.h"
#include "console.h"

const char *rpfs_strerror(int e) {
    switch (e) {
        case RPFS_OK:        return "ok";
        case RPFS_ENOENT:    return "no such file or directory";
        case RPFS_EEXIST:    return "already exists";
        case RPFS_ENOSPC:    return "no space left on device";
        case RPFS_ENOTDIR:   return "not a directory";
        case RPFS_EISDIR:    return "is a directory";
        case RPFS_ENOTEMPTY: return "directory not empty";
        case RPFS_EINVAL:    return "invalid argument or name";
        case RPFS_EIO:       return "disk I/O error";
        case RPFS_EFBIG:     return "file too large (max 268 KB)";
        case RPFS_ENOFS:     return "no filesystem mounted";
        case RPFS_ECORRUPT:  return "filesystem is corrupt (run fsck)";
        default:             return "unknown error";
    }
}

/* ======================= block, superblock and inode I/O ======================= */

static int dev_rd(struct blockdev *d, u32 blk, void *buf) {
    return d->read(d, blk * RPFS_SECTORS, RPFS_SECTORS, buf) ? RPFS_EIO : 0;
}
static int dev_wr(struct blockdev *d, u32 blk, const void *buf) {
    return d->write(d, blk * RPFS_SECTORS, RPFS_SECTORS, buf) ? RPFS_EIO : 0;
}
static int rd_block(struct rpfs *fs, u32 blk, void *buf) {
    if (blk >= fs->sb.total_blocks) return RPFS_ECORRUPT;
    return dev_rd(fs->dev, blk, buf);
}
static int wr_block(struct rpfs *fs, u32 blk, const void *buf) {
    if (blk >= fs->sb.total_blocks) return RPFS_ECORRUPT;
    return dev_wr(fs->dev, blk, buf);
}
static int wr_sb(struct rpfs *fs) {
    u8 buf[RPFS_BLOCK];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &fs->sb, sizeof(fs->sb));
    return dev_wr(fs->dev, 0, buf);
}

static int rd_inode(struct rpfs *fs, u32 ino, struct rpfs_inode *in) {
    if (ino == 0 || ino > fs->sb.inode_count) return RPFS_ECORRUPT;
    u8 buf[RPFS_BLOCK];
    u32 idx = ino - 1;
    int r = rd_block(fs, fs->sb.itable_start + idx / 16, buf);
    if (r) return r;
    memcpy(in, buf + (idx % 16) * sizeof(*in), sizeof(*in));
    return 0;
}
static int wr_inode(struct rpfs *fs, u32 ino, const struct rpfs_inode *in) {
    if (ino == 0 || ino > fs->sb.inode_count) return RPFS_ECORRUPT;
    u8 buf[RPFS_BLOCK];
    u32 idx = ino - 1, blk = fs->sb.itable_start + idx / 16;
    int r = rd_block(fs, blk, buf);
    if (r) return r;
    memcpy(buf + (idx % 16) * sizeof(*in), in, sizeof(*in));
    return wr_block(fs, blk, buf);
}

/* ======================= block and inode allocation ======================= */

static int alloc_block(struct rpfs *fs, u32 *out) {
    if (fs->sb.free_blocks == 0) return RPFS_ENOSPC;
    u8 buf[RPFS_BLOCK];
    u32 nb = fs->sb.bitmap_blocks;
    for (u32 n = 0; n < nb; n++) {
        u32 b = (fs->hint + n) % nb;
        int r = rd_block(fs, fs->sb.bitmap_start + b, buf);
        if (r) return r;
        for (u32 i = 0; i < RPFS_BLOCK; i++) {
            if (buf[i] == 0xFF) continue;
            for (u32 bit = 0; bit < 8; bit++) {
                if (buf[i] & (1u << bit)) continue;
                u32 blk = b * RPFS_BLOCK * 8 + i * 8 + bit;
                if (blk < fs->sb.data_start || blk >= fs->sb.total_blocks) continue;
                buf[i] |= (u8)(1u << bit);
                r = wr_block(fs, fs->sb.bitmap_start + b, buf);
                if (r) return r;
                fs->sb.free_blocks--;
                fs->hint = b;
                r = wr_sb(fs);
                if (r) return r;
                *out = blk;
                return 0;
            }
        }
    }
    return RPFS_ENOSPC;
}

static int alloc_block_zeroed(struct rpfs *fs, u32 *out) {
    u8 zero[RPFS_BLOCK];
    memset(zero, 0, sizeof(zero));
    int r = alloc_block(fs, out);
    if (r) return r;
    return wr_block(fs, *out, zero);
}

static int free_block(struct rpfs *fs, u32 blk) {
    if (blk < fs->sb.data_start || blk >= fs->sb.total_blocks) return RPFS_ECORRUPT;
    u8 buf[RPFS_BLOCK];
    u32 b = blk / (RPFS_BLOCK * 8), i = (blk % (RPFS_BLOCK * 8)) / 8, bit = blk % 8;
    int r = rd_block(fs, fs->sb.bitmap_start + b, buf);
    if (r) return r;
    if (!(buf[i] & (1u << bit))) return RPFS_ECORRUPT;          /* double free */
    buf[i] &= (u8)~(1u << bit);
    r = wr_block(fs, fs->sb.bitmap_start + b, buf);
    if (r) return r;
    fs->sb.free_blocks++;
    return wr_sb(fs);
}

static int alloc_inode(struct rpfs *fs, u32 mode, u32 *out) {
    if (fs->sb.free_inodes == 0) return RPFS_ENOSPC;
    u8 buf[RPFS_BLOCK];
    for (u32 b = 0; b < fs->sb.itable_blocks; b++) {
        int r = rd_block(fs, fs->sb.itable_start + b, buf);
        if (r) return r;
        struct rpfs_inode *tbl = (struct rpfs_inode *)buf;
        for (u32 i = 0; i < RPFS_BLOCK / sizeof(struct rpfs_inode); i++) {
            if (tbl[i].mode != 0) continue;
            memset(&tbl[i], 0, sizeof(tbl[i]));
            tbl[i].mode = (u16)mode;
            tbl[i].mtime = rtc_unix_time();
            r = wr_block(fs, fs->sb.itable_start + b, buf);
            if (r) return r;
            fs->sb.free_inodes--;
            r = wr_sb(fs);
            if (r) return r;
            *out = b * 16 + i + 1;
            return 0;
        }
    }
    return RPFS_ENOSPC;
}

static int free_inode(struct rpfs *fs, u32 ino) {
    struct rpfs_inode blank;
    memset(&blank, 0, sizeof(blank));
    int r = wr_inode(fs, ino, &blank);
    if (r) return r;
    fs->sb.free_inodes++;
    return wr_sb(fs);
}

/* ======================= file data: block mapping, read, write, truncate ======================= */

/* Disk block holding file block `idx` (0 = hole). With alloc, missing blocks are created. */
static int bmap(struct rpfs *fs, struct rpfs_inode *in, u32 idx, int alloc, u32 *out, int *dirty) {
    *out = 0;
    if (idx < RPFS_NDIRECT) {
        if (in->blocks[idx] == 0 && alloc) {
            u32 b;
            int r = alloc_block_zeroed(fs, &b);
            if (r) return r;
            in->blocks[idx] = b;
            *dirty = 1;
        }
        *out = in->blocks[idx];
        return 0;
    }
    idx -= RPFS_NDIRECT;
    if (idx >= RPFS_PTRS) return RPFS_EFBIG;
    if (in->indirect == 0) {
        if (!alloc) return 0;
        u32 b;
        int r = alloc_block_zeroed(fs, &b);
        if (r) return r;
        in->indirect = b;
        *dirty = 1;
    }
    u32 ptrs[RPFS_PTRS];
    int r = rd_block(fs, in->indirect, ptrs);
    if (r) return r;
    if (ptrs[idx] == 0 && alloc) {
        u32 b;
        r = alloc_block_zeroed(fs, &b);
        if (r) return r;
        ptrs[idx] = b;
        r = wr_block(fs, in->indirect, ptrs);
        if (r) return r;
    }
    *out = ptrs[idx];
    return 0;
}

/* Free every data block of the inode (caller stores the inode afterwards). */
static int truncate_inode(struct rpfs *fs, struct rpfs_inode *in) {
    int result = 0;
    for (u32 i = 0; i < RPFS_NDIRECT; i++) {
        if (in->blocks[i]) { int r = free_block(fs, in->blocks[i]); if (r && !result) result = r; in->blocks[i] = 0; }
    }
    if (in->indirect) {
        u32 ptrs[RPFS_PTRS];
        if (rd_block(fs, in->indirect, ptrs) == 0) {
            for (u32 i = 0; i < RPFS_PTRS; i++)
                if (ptrs[i]) { int r = free_block(fs, ptrs[i]); if (r && !result) result = r; }
        }
        int r = free_block(fs, in->indirect);
        if (r && !result) result = r;
        in->indirect = 0;
    }
    in->size = 0;
    return result;
}

static int file_read(struct rpfs *fs, struct rpfs_inode *in, u32 off, void *buf, u32 len, u32 *got) {
    *got = 0;
    if (off >= in->size) return 0;
    if (len > in->size - off) len = in->size - off;
    u8 tmp[RPFS_BLOCK];
    u32 done = 0;
    while (done < len) {
        u32 pos = off + done, boff = pos % RPFS_BLOCK, n = RPFS_BLOCK - boff;
        if (n > len - done) n = len - done;
        u32 blk; int dirty = 0;
        int r = bmap(fs, in, pos / RPFS_BLOCK, 0, &blk, &dirty);
        if (r) return r;
        if (blk == 0) memset((u8 *)buf + done, 0, n);           /* hole reads as zeros */
        else {
            r = rd_block(fs, blk, tmp);
            if (r) return r;
            memcpy((u8 *)buf + done, tmp + boff, n);
        }
        done += n;
        *got = done;
    }
    return 0;
}

static int write_at(struct rpfs *fs, u32 ino, struct rpfs_inode *in, u32 off, const u8 *data, u32 len) {
    if (off + len < off || off + len > RPFS_MAX_FILE) return RPFS_EFBIG;
    u8 tmp[RPFS_BLOCK];
    u32 done = 0;
    int dirty = 0, result = 0;
    while (done < len) {
        u32 pos = off + done, boff = pos % RPFS_BLOCK, n = RPFS_BLOCK - boff;
        if (n > len - done) n = len - done;
        u32 blk;
        int r = bmap(fs, in, pos / RPFS_BLOCK, 1, &blk, &dirty);
        if (r) { result = r; break; }
        if (blk == 0) { result = RPFS_ECORRUPT; break; }
        if (n == RPFS_BLOCK) r = wr_block(fs, blk, data + done);
        else {
            r = rd_block(fs, blk, tmp);
            if (!r) { memcpy(tmp + boff, data + done, n); r = wr_block(fs, blk, tmp); }
        }
        if (r) { result = r; break; }
        done += n;
    }
    if (off + done > in->size) { in->size = off + done; dirty = 1; }
    if (done) { in->mtime = rtc_unix_time(); dirty = 1; }
    if (dirty) { int r = wr_inode(fs, ino, in); if (r && !result) result = r; }
    return result;
}

/* ======================= directories ======================= */

static int valid_name(const char *n) {
    u32 len = 0;
    for (; n[len]; len++) if ((u8)n[len] < 32 || n[len] == '/') return 0;
    if (len == 0 || len > RPFS_NAME_MAX) return 0;
    if (!strcmp(n, ".") || !strcmp(n, "..")) return 0;
    return 1;
}

/* Find `name` in a directory. 0 = found; blk/slot/ino describe the entry. */
static int dir_find(struct rpfs *fs, u32 dirino, const char *name, u32 *blk_out, u32 *slot_out, u32 *ino_out) {
    struct rpfs_inode din;
    int r = rd_inode(fs, dirino, &din);
    if (r) return r;
    if (din.mode != RPFS_DIR) return RPFS_ENOTDIR;
    u8 buf[RPFS_BLOCK];
    for (u32 b = 0; b < din.size / RPFS_BLOCK; b++) {
        u32 blk; int dirty = 0;
        r = bmap(fs, &din, b, 0, &blk, &dirty);
        if (r) return r;
        if (!blk) continue;
        r = rd_block(fs, blk, buf);
        if (r) return r;
        struct rpfs_dirent *e = (struct rpfs_dirent *)buf;
        for (u32 i = 0; i < RPFS_BLOCK / sizeof(*e); i++) {
            if (e[i].ino && !strncmp(e[i].name, name, sizeof(e[i].name))) {
                if (blk_out) *blk_out = blk;
                if (slot_out) *slot_out = i;
                if (ino_out) *ino_out = e[i].ino;
                return 0;
            }
        }
    }
    return RPFS_ENOENT;
}

static int dir_lookup(struct rpfs *fs, u32 dirino, const char *name, u32 *ino) {
    return dir_find(fs, dirino, name, 0, 0, ino);
}

static int dir_add(struct rpfs *fs, u32 dirino, const char *name, u32 ino) {
    struct rpfs_inode din;
    int r = rd_inode(fs, dirino, &din);
    if (r) return r;
    if (din.mode != RPFS_DIR) return RPFS_ENOTDIR;
    u8 buf[RPFS_BLOCK];
    u32 nblocks = din.size / RPFS_BLOCK;
    for (u32 b = 0; b < nblocks; b++) {
        u32 blk; int dirty = 0;
        r = bmap(fs, &din, b, 0, &blk, &dirty);
        if (r) return r;
        if (!blk) continue;
        r = rd_block(fs, blk, buf);
        if (r) return r;
        struct rpfs_dirent *e = (struct rpfs_dirent *)buf;
        for (u32 i = 0; i < RPFS_BLOCK / sizeof(*e); i++) {
            if (e[i].ino) continue;
            memset(&e[i], 0, sizeof(e[i]));
            e[i].ino = ino;
            for (u32 k = 0; name[k] && k < RPFS_NAME_MAX; k++) e[i].name[k] = name[k];
            r = wr_block(fs, blk, buf);
            if (r) return r;
            din.mtime = rtc_unix_time();
            return wr_inode(fs, dirino, &din);
        }
    }
    /* every slot is taken: give the directory one more block */
    u32 blk; int dirty = 0;
    r = bmap(fs, &din, nblocks, 1, &blk, &dirty);
    if (r) return r;
    memset(buf, 0, sizeof(buf));
    struct rpfs_dirent *e = (struct rpfs_dirent *)buf;
    e[0].ino = ino;
    for (u32 k = 0; name[k] && k < RPFS_NAME_MAX; k++) e[0].name[k] = name[k];
    r = wr_block(fs, blk, buf);
    if (r) return r;
    din.size += RPFS_BLOCK;
    din.mtime = rtc_unix_time();
    return wr_inode(fs, dirino, &din);
}

static int dir_remove(struct rpfs *fs, u32 dirino, const char *name) {
    u32 blk, slot;
    int r = dir_find(fs, dirino, name, &blk, &slot, 0);
    if (r) return r;
    u8 buf[RPFS_BLOCK];
    r = rd_block(fs, blk, buf);
    if (r) return r;
    memset(buf + slot * sizeof(struct rpfs_dirent), 0, sizeof(struct rpfs_dirent));
    return wr_block(fs, blk, buf);
}

static int dir_is_empty(struct rpfs *fs, u32 dirino, int *empty) {
    struct rpfs_inode din;
    int r = rd_inode(fs, dirino, &din);
    if (r) return r;
    u8 buf[RPFS_BLOCK];
    *empty = 1;
    for (u32 b = 0; b < din.size / RPFS_BLOCK; b++) {
        u32 blk; int dirty = 0;
        r = bmap(fs, &din, b, 0, &blk, &dirty);
        if (r) return r;
        if (!blk) continue;
        r = rd_block(fs, blk, buf);
        if (r) return r;
        struct rpfs_dirent *e = (struct rpfs_dirent *)buf;
        for (u32 i = 0; i < RPFS_BLOCK / sizeof(*e); i++)
            if (e[i].ino && strncmp(e[i].name, ".", 28) && strncmp(e[i].name, "..", 28)) { *empty = 0; return 0; }
    }
    return 0;
}

/* New directory: inode + one block holding "." and "..". parent == 0 means "I am the root". */
static int make_dir(struct rpfs *fs, u32 parent, u32 *out) {
    u32 ino, blk;
    int r = alloc_inode(fs, RPFS_DIR, &ino);
    if (r) return r;
    r = alloc_block(fs, &blk);
    if (r) { free_inode(fs, ino); return r; }
    u8 buf[RPFS_BLOCK];
    memset(buf, 0, sizeof(buf));
    struct rpfs_dirent *e = (struct rpfs_dirent *)buf;
    e[0].ino = ino;                      e[0].name[0] = '.';
    e[1].ino = parent ? parent : ino;    e[1].name[0] = '.'; e[1].name[1] = '.';
    r = wr_block(fs, blk, buf);
    struct rpfs_inode in;
    if (!r) r = rd_inode(fs, ino, &in);
    if (!r) { in.blocks[0] = blk; in.size = RPFS_BLOCK; r = wr_inode(fs, ino, &in); }
    if (r) { free_block(fs, blk); free_inode(fs, ino); return r; }
    *out = ino;
    return 0;
}

/* ======================= paths ======================= */

/* Resolve a path to an inode ("." and ".." work; relative paths start at the cwd). */
static int walk(struct rpfs *fs, const char *path, u32 *out) {
    u32 cur = (path[0] == '/') ? RPFS_ROOT : fs->cwd;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[RPFS_NAME_MAX + 1];
        u32 n = 0;
        while (*p && *p != '/') {
            if (n >= RPFS_NAME_MAX) return RPFS_EINVAL;
            comp[n++] = *p++;
        }
        comp[n] = 0;
        if (!strcmp(comp, ".")) continue;
        u32 next;
        int r = dir_lookup(fs, cur, comp, &next);
        if (r) return r;
        cur = next;
    }
    *out = cur;
    return 0;
}

/* Split "a/b/leaf" into the inode of "a/b" and the name "leaf". */
static int walk_parent(struct rpfs *fs, const char *path, u32 *parent, char *leaf) {
    char dirpart[RPFS_PATH_MAX];
    u32 len = (u32)strlen(path);
    if (len == 0 || len >= RPFS_PATH_MAX) return RPFS_EINVAL;
    while (len > 1 && path[len - 1] == '/') len--;                 /* ignore trailing slashes */
    u32 slash = len;
    while (slash > 0 && path[slash - 1] != '/') slash--;           /* leaf starts at `slash` */
    u32 leaflen = len - slash;
    if (leaflen == 0 || leaflen > RPFS_NAME_MAX) return RPFS_EINVAL;
    for (u32 i = 0; i < leaflen; i++) leaf[i] = path[slash + i];
    leaf[leaflen] = 0;
    if (!valid_name(leaf)) return RPFS_EINVAL;
    if (slash == 0) { *parent = fs->cwd; return 0; }
    u32 dlen = slash;
    while (dlen > 1 && path[dlen - 1] == '/') dlen--;              /* "/file" -> directory "/" */
    for (u32 i = 0; i < dlen; i++) dirpart[i] = path[i];
    dirpart[dlen] = 0;
    return walk(fs, dirpart, parent);
}

/* Textual cwd update: base + path with "." and ".." folded away. */
static int normalize(const char *base, const char *path, char *out) {
    if (path[0] == '/') { out[0] = '/'; out[1] = 0; }
    else strcpy(out, base);
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[RPFS_NAME_MAX + 1];
        u32 n = 0;
        while (*p && *p != '/') { if (n >= RPFS_NAME_MAX) return RPFS_EINVAL; comp[n++] = *p++; }
        comp[n] = 0;
        if (!strcmp(comp, ".")) continue;
        u32 len = (u32)strlen(out);
        if (!strcmp(comp, "..")) {
            while (len > 1 && out[len - 1] != '/') len--;
            if (len > 1) len--;
            out[len] = 0;
            continue;
        }
        if (len + 1 + n + 1 > RPFS_PATH_MAX) return RPFS_EINVAL;
        if (len > 1) out[len++] = '/';
        for (u32 i = 0; i < n; i++) out[len++] = comp[i];
        out[len] = 0;
    }
    return 0;
}

/* ======================= format / mount ======================= */

int rpfs_format(struct blockdev *dev, const char *label) {
    u32 total = dev->sectors / RPFS_SECTORS;
    if (total > (1u << 20)) total = 1u << 20;                      /* up to 1 GB */
    if (total < 64) return RPFS_EINVAL;

    struct rpfs fs;
    memset(&fs, 0, sizeof(fs));
    fs.dev = dev;
    struct rpfs_sb *sb = &fs.sb;
    sb->magic = RPFS_MAGIC;
    sb->version = RPFS_VERSION;
    sb->block_size = RPFS_BLOCK;
    sb->total_blocks = total;
    u32 ic = (total / 4) & ~15u;
    if (ic < 32) ic = 32;
    if (ic > 8192) ic = 8192;
    sb->inode_count = ic;
    sb->bitmap_start = 1;
    sb->bitmap_blocks = (total + RPFS_BLOCK * 8 - 1) / (RPFS_BLOCK * 8);
    sb->itable_start = sb->bitmap_start + sb->bitmap_blocks;
    sb->itable_blocks = ic / 16;
    sb->data_start = sb->itable_start + sb->itable_blocks;
    if (sb->data_start + 8 > total) return RPFS_EINVAL;
    sb->free_blocks = total - sb->data_start;
    sb->free_inodes = ic;
    sb->root_ino = RPFS_ROOT;
    for (u32 i = 0; i < 15 && label && label[i]; i++) sb->label[i] = label[i];

    u8 buf[RPFS_BLOCK];
    memset(buf, 0, sizeof(buf));
    for (u32 b = 0; b < sb->itable_blocks; b++)                     /* empty inode table */
        if (dev_wr(dev, sb->itable_start + b, buf)) return RPFS_EIO;
    for (u32 b = 0; b < sb->bitmap_blocks; b++) {                   /* bitmap: metadata and the tail are "used" */
        memset(buf, 0, sizeof(buf));
        for (u32 bit = 0; bit < RPFS_BLOCK * 8; bit++) {
            u32 blk = b * RPFS_BLOCK * 8 + bit;
            if (blk < sb->data_start || blk >= total) buf[bit / 8] |= (u8)(1u << (bit % 8));
        }
        if (dev_wr(dev, sb->bitmap_start + b, buf)) return RPFS_EIO;
    }
    int r = wr_sb(&fs);
    if (r) return r;

    fs.mounted = 1;                                                 /* create the root directory (inode 1) */
    u32 root;
    r = make_dir(&fs, 0, &root);
    if (r) return r;
    if (root != RPFS_ROOT) return RPFS_ECORRUPT;
    return 0;
}

int rpfs_probe(struct blockdev *dev, char label[17]) {
    u8 buf[RPFS_BLOCK];
    if (dev->sectors < RPFS_SECTORS || dev_rd(dev, 0, buf)) return 0;
    struct rpfs_sb *sb = (struct rpfs_sb *)buf;
    if (sb->magic != RPFS_MAGIC) return 0;
    if (label) { memcpy(label, sb->label, 16); label[16] = 0; }
    return 1;
}

int rpfs_mount(struct rpfs *fs, struct blockdev *dev) {
    memset(fs, 0, sizeof(*fs));
    u8 buf[RPFS_BLOCK];
    if (dev_rd(dev, 0, buf)) return RPFS_EIO;
    struct rpfs_sb *sb = (struct rpfs_sb *)buf;
    u32 total = sb->total_blocks;
    int ok = sb->magic == RPFS_MAGIC && sb->version == RPFS_VERSION && sb->block_size == RPFS_BLOCK
          && total >= 64 && total <= (1u << 20) && total * RPFS_SECTORS <= dev->sectors
          && sb->inode_count >= 32 && sb->inode_count <= 8192 && (sb->inode_count % 16) == 0
          && sb->bitmap_start == 1
          && sb->bitmap_blocks == (total + RPFS_BLOCK * 8 - 1) / (RPFS_BLOCK * 8)
          && sb->itable_start == sb->bitmap_start + sb->bitmap_blocks
          && sb->itable_blocks == sb->inode_count / 16
          && sb->data_start == sb->itable_start + sb->itable_blocks && sb->data_start < total
          && sb->free_blocks <= total - sb->data_start && sb->free_inodes <= sb->inode_count
          && sb->root_ino == RPFS_ROOT;
    if (!ok) return RPFS_ECORRUPT;
    fs->dev = dev;
    memcpy(&fs->sb, sb, sizeof(fs->sb));
    fs->mounted = 1;
    fs->cwd = RPFS_ROOT;
    strcpy(fs->cwd_path, "/");
    struct rpfs_inode root;
    int r = rd_inode(fs, RPFS_ROOT, &root);
    if (r || root.mode != RPFS_DIR) { fs->mounted = 0; return RPFS_ECORRUPT; }
    return 0;
}

void rpfs_unmount(struct rpfs *fs) { fs->mounted = 0; }       /* write-through: nothing left to flush */

/* ======================= public operations ======================= */

#define NEED_MOUNT(fs) do { if (!(fs) || !(fs)->mounted) return RPFS_ENOFS; } while (0)

static void fill_stat(u32 ino, const struct rpfs_inode *in, struct rpfs_stat *st) {
    st->ino = ino; st->mode = in->mode; st->size = in->size; st->mtime = in->mtime;
    st->blocks = (in->size + RPFS_BLOCK - 1) / RPFS_BLOCK;
}

int rpfs_stat(struct rpfs *fs, const char *path, struct rpfs_stat *st) {
    NEED_MOUNT(fs);
    u32 ino;
    int r = walk(fs, path, &ino);
    if (r) return r;
    struct rpfs_inode in;
    r = rd_inode(fs, ino, &in);
    if (r) return r;
    fill_stat(ino, &in, st);
    return 0;
}

int rpfs_list(struct rpfs *fs, const char *path, rpfs_list_cb cb, void *ctx) {
    NEED_MOUNT(fs);
    u32 dino;
    int r = walk(fs, path, &dino);
    if (r) return r;
    struct rpfs_inode din;
    r = rd_inode(fs, dino, &din);
    if (r) return r;
    if (din.mode != RPFS_DIR) return RPFS_ENOTDIR;
    u8 buf[RPFS_BLOCK];
    for (u32 b = 0; b < din.size / RPFS_BLOCK; b++) {
        u32 blk; int dirty = 0;
        r = bmap(fs, &din, b, 0, &blk, &dirty);
        if (r) return r;
        if (!blk) continue;
        r = rd_block(fs, blk, buf);
        if (r) return r;
        struct rpfs_dirent *e = (struct rpfs_dirent *)buf;
        for (u32 i = 0; i < RPFS_BLOCK / sizeof(*e); i++) {
            if (!e[i].ino) continue;
            char name[sizeof(e[i].name) + 1];
            memcpy(name, e[i].name, sizeof(e[i].name));
            name[sizeof(e[i].name)] = 0;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            struct rpfs_inode in;
            r = rd_inode(fs, e[i].ino, &in);
            if (r) return r;
            struct rpfs_stat st;
            fill_stat(e[i].ino, &in, &st);
            if (cb(name, &st, ctx)) return 0;
        }
    }
    return 0;
}

int rpfs_read(struct rpfs *fs, const char *path, u32 offset, void *buf, u32 len, u32 *got) {
    NEED_MOUNT(fs);
    u32 ino;
    int r = walk(fs, path, &ino);
    if (r) return r;
    struct rpfs_inode in;
    r = rd_inode(fs, ino, &in);
    if (r) return r;
    if (in.mode == RPFS_DIR) return RPFS_EISDIR;
    return file_read(fs, &in, offset, buf, len, got);
}

int rpfs_write(struct rpfs *fs, const char *path, const void *data, u32 len, int append) {
    NEED_MOUNT(fs);
    u32 parent, ino;
    char leaf[RPFS_NAME_MAX + 1];
    int r = walk_parent(fs, path, &parent, leaf);
    if (r) return r;
    struct rpfs_inode in;
    r = dir_lookup(fs, parent, leaf, &ino);
    if (r == 0) {
        r = rd_inode(fs, ino, &in);
        if (r) return r;
        if (in.mode == RPFS_DIR) return RPFS_EISDIR;
        if ((append ? in.size : 0) + len > RPFS_MAX_FILE) return RPFS_EFBIG;
        if (!append) {
            r = truncate_inode(fs, &in);
            int w = wr_inode(fs, ino, &in);
            if (r) return r;
            if (w) return w;
        }
    } else if (r == RPFS_ENOENT) {
        if (len > RPFS_MAX_FILE) return RPFS_EFBIG;
        r = alloc_inode(fs, RPFS_FILE, &ino);
        if (r) return r;
        r = dir_add(fs, parent, leaf, ino);
        if (r) { free_inode(fs, ino); return r; }
        r = rd_inode(fs, ino, &in);
        if (r) return r;
    } else {
        return r;
    }
    return write_at(fs, ino, &in, in.size, (const u8 *)data, len);
}

int rpfs_mkdir(struct rpfs *fs, const char *path) {
    NEED_MOUNT(fs);
    u32 parent, ino;
    char leaf[RPFS_NAME_MAX + 1];
    int r = walk_parent(fs, path, &parent, leaf);
    if (r) return r;
    r = dir_lookup(fs, parent, leaf, &ino);
    if (r == 0) return RPFS_EEXIST;
    if (r != RPFS_ENOENT) return r;
    r = make_dir(fs, parent, &ino);
    if (r) return r;
    r = dir_add(fs, parent, leaf, ino);
    if (r) {                                                         /* undo */
        struct rpfs_inode in;
        if (rd_inode(fs, ino, &in) == 0) { truncate_inode(fs, &in); }
        free_inode(fs, ino);
    }
    return r;
}

int rpfs_unlink(struct rpfs *fs, const char *path) {
    NEED_MOUNT(fs);
    u32 parent, ino;
    char leaf[RPFS_NAME_MAX + 1];
    int r = walk_parent(fs, path, &parent, leaf);
    if (r) return r;
    r = dir_lookup(fs, parent, leaf, &ino);
    if (r) return r;
    struct rpfs_inode in;
    r = rd_inode(fs, ino, &in);
    if (r) return r;
    if (in.mode == RPFS_DIR) return RPFS_EISDIR;
    r = dir_remove(fs, parent, leaf);                                /* unlink first, then free */
    if (r) return r;
    r = truncate_inode(fs, &in);
    int f = free_inode(fs, ino);
    return r ? r : f;
}

int rpfs_rmdir(struct rpfs *fs, const char *path) {
    NEED_MOUNT(fs);
    u32 parent, ino;
    char leaf[RPFS_NAME_MAX + 1];
    int r = walk_parent(fs, path, &parent, leaf);
    if (r) return r;
    r = dir_lookup(fs, parent, leaf, &ino);
    if (r) return r;
    struct rpfs_inode in;
    r = rd_inode(fs, ino, &in);
    if (r) return r;
    if (in.mode != RPFS_DIR) return RPFS_ENOTDIR;
    if (ino == RPFS_ROOT || ino == fs->cwd) return RPFS_EINVAL;      /* not the root, not where we stand */
    int empty;
    r = dir_is_empty(fs, ino, &empty);
    if (r) return r;
    if (!empty) return RPFS_ENOTEMPTY;
    r = dir_remove(fs, parent, leaf);
    if (r) return r;
    r = truncate_inode(fs, &in);
    int f = free_inode(fs, ino);
    return r ? r : f;
}

int rpfs_rename(struct rpfs *fs, const char *from, const char *to) {
    NEED_MOUNT(fs);
    u32 op, np, ino, other;
    char ol[RPFS_NAME_MAX + 1], nl[RPFS_NAME_MAX + 1];
    int r = walk_parent(fs, from, &op, ol);
    if (!r) r = walk_parent(fs, to, &np, nl);
    if (r) return r;
    r = dir_lookup(fs, op, ol, &ino);
    if (r) return r;
    struct rpfs_inode in;
    r = rd_inode(fs, ino, &in);
    if (r) return r;
    if (in.mode == RPFS_DIR) return RPFS_EISDIR;                     /* moving directories isn't supported yet */
    r = dir_lookup(fs, np, nl, &other);
    if (r == 0) return RPFS_EEXIST;
    if (r != RPFS_ENOENT) return r;
    r = dir_add(fs, np, nl, ino);
    if (r) return r;
    return dir_remove(fs, op, ol);
}

int rpfs_chdir(struct rpfs *fs, const char *path) {
    NEED_MOUNT(fs);
    u32 ino;
    int r = walk(fs, path, &ino);
    if (r) return r;
    struct rpfs_inode in;
    r = rd_inode(fs, ino, &in);
    if (r) return r;
    if (in.mode != RPFS_DIR) return RPFS_ENOTDIR;
    char np[RPFS_PATH_MAX];
    r = normalize(fs->cwd_path, path, np);
    if (r) return r;
    fs->cwd = ino;
    strcpy(fs->cwd_path, np);
    return 0;
}

void rpfs_df(struct rpfs *fs, u32 *total, u32 *free_blocks, u32 *inodes, u32 *free_inodes) {
    *total = fs->sb.total_blocks - fs->sb.data_start;                 /* data blocks only */
    *free_blocks = fs->sb.free_blocks;
    *inodes = fs->sb.inode_count;
    *free_inodes = fs->sb.free_inodes;
}

/* ======================= fsck ======================= */

static void bad(int verbose, int *count, const char *msg, u32 val) {
    (*count)++;
    if (verbose && *count <= 12) {
        console_set_color(COL_RED);
        puts("  fsck: "); puts(msg); puts(" ("); put_dec(val); puts(")\n");
    }
}

static int bit_get(const u8 *map, u32 i) { return (map[i >> 3] >> (i & 7)) & 1; }
static void bit_set(u8 *map, u32 i)      { map[i >> 3] |= (u8)(1u << (i & 7)); }

/*
 * Cross-checks everything that can be checked without trusting any one structure:
 *   1. every in-use inode: valid mode, every block pointer in range and used exactly once
 *   2. every directory: "." and ".." correct, every entry points at an in-use inode,
 *      every inode referenced exactly once
 *   3. every in-use inode can be reached from the root (no orphans, no cycles)
 *   4. the block bitmap equals the set of blocks actually in use
 *   5. the free-block and free-inode counters in the superblock
 */
int rpfs_fsck(struct rpfs *fs, int verbose) {
    NEED_MOUNT(fs);
    struct rpfs_sb *sb = &fs->sb;
    int problems = 0;
    u32 ni = sb->inode_count, total = sb->total_blocks;

    u8 *used   = kcalloc(1, (total + 7) / 8);
    u8 *inuse  = kcalloc(1, ni + 1);
    u16 *refs  = kcalloc(ni + 1, sizeof(u16));
    u32 *par   = kcalloc(ni + 1, sizeof(u32));       /* directory that lists this inode */
    u32 *dotdot = kcalloc(ni + 1, sizeof(u32));      /* what a directory's ".." says */
    if (!used || !inuse || !refs || !par || !dotdot) {
        kfree(used); kfree(inuse); kfree(refs); kfree(par); kfree(dotdot);
        return RPFS_ENOSPC;
    }

    for (u32 b = 0; b < sb->data_start; b++) bit_set(used, b);       /* superblock, bitmap, inode table */

    /* pass 1: inodes and their blocks */
    u8 buf[RPFS_BLOCK];
    u32 inodes_used = 0;
    for (u32 tb = 0; tb < sb->itable_blocks; tb++) {
        if (rd_block(fs, sb->itable_start + tb, buf)) { bad(verbose, &problems, "cannot read inode table block", tb); continue; }
        struct rpfs_inode *tbl = (struct rpfs_inode *)buf;
        for (u32 i = 0; i < 16; i++) {
            u32 ino = tb * 16 + i + 1;
            struct rpfs_inode *in = &tbl[i];
            if (in->mode == 0) continue;
            inodes_used++;
            inuse[ino] = 1;
            if (in->mode != RPFS_FILE && in->mode != RPFS_DIR) { bad(verbose, &problems, "inode has an invalid mode", ino); continue; }
            if (in->size > RPFS_MAX_FILE) bad(verbose, &problems, "inode size is too large", ino);
            if (in->mode == RPFS_DIR && (in->size == 0 || in->size % RPFS_BLOCK)) bad(verbose, &problems, "directory size is not a multiple of the block size", ino);

            u32 list[RPFS_NDIRECT + 1 + RPFS_PTRS];
            u32 n = 0;
            for (u32 k = 0; k < RPFS_NDIRECT; k++) if (in->blocks[k]) list[n++] = in->blocks[k];
            if (in->indirect) {
                list[n++] = in->indirect;
                u32 ptrs[RPFS_PTRS];
                if (in->indirect >= sb->data_start && in->indirect < total && rd_block(fs, in->indirect, ptrs) == 0) {
                    for (u32 k = 0; k < RPFS_PTRS; k++) if (ptrs[k]) list[n++] = ptrs[k];
                }
            }
            for (u32 k = 0; k < n; k++) {
                u32 blk = list[k];
                if (blk < sb->data_start || blk >= total) { bad(verbose, &problems, "block pointer out of range", ino); continue; }
                if (bit_get(used, blk)) { bad(verbose, &problems, "block used twice", blk); continue; }
                bit_set(used, blk);
            }
        }
    }

    /* pass 2: directory contents */
    for (u32 ino = 1; ino <= ni; ino++) {
        if (!inuse[ino]) continue;
        struct rpfs_inode din;
        if (rd_inode(fs, ino, &din) || din.mode != RPFS_DIR) continue;
        int saw_dot = 0, saw_dotdot = 0;
        for (u32 b = 0; b < din.size / RPFS_BLOCK; b++) {
            u32 blk; int dirty = 0;
            if (bmap(fs, &din, b, 0, &blk, &dirty) || !blk || blk >= total) continue;
            if (rd_block(fs, blk, buf)) { bad(verbose, &problems, "cannot read directory block", ino); continue; }
            struct rpfs_dirent *e = (struct rpfs_dirent *)buf;
            for (u32 i = 0; i < RPFS_BLOCK / sizeof(*e); i++) {
                if (!e[i].ino) continue;
                u32 t = e[i].ino;
                if (e[i].name[0] == 0) { bad(verbose, &problems, "directory entry with an empty name", ino); continue; }
                if (!strncmp(e[i].name, ".", 28)) {
                    saw_dot = 1;
                    if (t != ino) bad(verbose, &problems, "\".\" does not point at its own directory", ino);
                    continue;
                }
                if (!strncmp(e[i].name, "..", 28)) {
                    saw_dotdot = 1;
                    if (t > ni || !inuse[t]) bad(verbose, &problems, "\"..\" points at an unused inode", ino);
                    else dotdot[ino] = t;
                    continue;
                }
                if (t > ni || !inuse[t]) { bad(verbose, &problems, "entry points at an unused inode", ino); continue; }
                if (refs[t] < 0xFFFF) refs[t]++;
                par[t] = ino;
            }
        }
        if (!saw_dot || !saw_dotdot) bad(verbose, &problems, "directory lacks \".\" or \"..\"", ino);
    }

    /* pass 3: reference counts and reachability */
    for (u32 ino = 1; ino <= ni; ino++) {
        if (!inuse[ino]) continue;
        if (ino == RPFS_ROOT) {
            if (refs[ino] != 0) bad(verbose, &problems, "root directory is listed inside another directory", ino);
            if (dotdot[ino] != RPFS_ROOT) bad(verbose, &problems, "root's \"..\" must point at itself", ino);
            continue;
        }
        if (refs[ino] != 1) { bad(verbose, &problems, refs[ino] ? "inode is listed more than once" : "orphan inode (listed nowhere)", ino); continue; }
        struct rpfs_inode in;
        if (!rd_inode(fs, ino, &in) && in.mode == RPFS_DIR && dotdot[ino] != par[ino])
            bad(verbose, &problems, "directory's \"..\" does not match its parent", ino);
        u32 cur = ino;
        int steps = 0;
        while (cur != RPFS_ROOT && steps < 64 && par[cur]) { cur = par[cur]; steps++; }
        if (cur != RPFS_ROOT) bad(verbose, &problems, "inode is not reachable from the root", ino);
    }

    /* pass 4: block bitmap vs. blocks actually in use */
    u32 free_count = 0;
    for (u32 bb = 0; bb < sb->bitmap_blocks; bb++) {
        if (rd_block(fs, sb->bitmap_start + bb, buf)) { bad(verbose, &problems, "cannot read bitmap block", bb); continue; }
        for (u32 bit = 0; bit < RPFS_BLOCK * 8; bit++) {
            u32 blk = bb * RPFS_BLOCK * 8 + bit;
            if (blk >= total) break;
            int marked = bit_get(buf, bit), real = bit_get(used, blk);
            if (!marked) free_count++;
            if (marked && !real) bad(verbose, &problems, "block is marked used but nothing owns it (leaked)", blk);
            if (!marked && real) bad(verbose, &problems, "block is in use but marked free in the bitmap", blk);
        }
    }

    /* pass 5: superblock counters */
    if (free_count != sb->free_blocks) bad(verbose, &problems, "superblock free-block count is wrong", sb->free_blocks);
    if (ni - inodes_used != sb->free_inodes) bad(verbose, &problems, "superblock free-inode count is wrong", sb->free_inodes);

    kfree(used); kfree(inuse); kfree(refs); kfree(par); kfree(dotdot);
    return problems;
}
