/* rpfs_test.c - `fstest`: exercises RPFS Sequel on a private 1 MB RAM disk (never touches real disks) */
#include "rpfs.h"
#include "heap.h"
#include "string.h"
#include "console.h"

static int step(const char *name, int ok) {
    console_set_color(COL_GREY); puts("  "); puts(name); puts(": ");
    console_set_color(ok ? COL_GREEN : COL_RED); puts(ok ? "ok\n" : "FAILED\n");
    return ok;
}

struct count { u32 n, dirs; };
static int count_cb(const char *name, const struct rpfs_stat *st, void *ctx) {
    (void)name;
    struct count *c = ctx;
    c->n++;
    if (st->mode == RPFS_DIR) c->dirs++;
    return 0;
}
static u32 count_entries(struct rpfs *fs, const char *path) {
    struct count c = { 0, 0 };
    return rpfs_list(fs, path, count_cb, &c) == 0 ? c.n : 0xFFFFFFFFu;
}

static void pattern(u8 *buf, u32 n, u32 seed, u32 off) {
    for (u32 i = 0; i < n; i++) buf[i] = (u8)(seed + (off + i) * 7 + ((off + i) >> 8));
}
static int pattern_ok(const u8 *buf, u32 n, u32 seed, u32 off) {
    for (u32 i = 0; i < n; i++) if (buf[i] != (u8)(seed + (off + i) * 7 + ((off + i) >> 8))) return 0;
    return 1;
}

/* write `size` pattern bytes, then check stat and read back in awkward 777-byte pieces */
static int roundtrip(struct rpfs *fs, const char *path, u32 size, u32 seed) {
    u8 *buf = kmalloc(size ? size : 1);
    if (!buf) return 0;
    pattern(buf, size, seed, 0);
    int ok = rpfs_write(fs, path, buf, size, 0) == 0;
    struct rpfs_stat st;
    ok = ok && rpfs_stat(fs, path, &st) == 0 && st.size == size && st.mode == RPFS_FILE;
    u8 piece[777];
    for (u32 off = 0; ok && off < size; off += sizeof(piece)) {
        u32 got = 0, want = size - off < sizeof(piece) ? size - off : (u32)sizeof(piece);
        ok = rpfs_read(fs, path, off, piece, sizeof(piece), &got) == 0 && got == want && pattern_ok(piece, got, seed, off);
    }
    kfree(buf);
    return ok;
}

static void name_n(char *out, const char *prefix, u32 n) {
    u32 i = 0;
    while (*prefix) out[i++] = *prefix++;
    out[i++] = (char)('0' + (n / 100) % 10);
    out[i++] = (char)('0' + (n / 10) % 10);
    out[i++] = (char)('0' + n % 10);
    out[i] = 0;
}

int rpfs_selftest(void) {
    int pass = 1, ok;
    struct blockdev *dev = ramdisk_create("fstest", 2048, 0);       /* 1 MB */
    struct rpfs *fs = kmalloc(sizeof(*fs));
    if (!dev || !fs) { puts("  could not allocate a test RAM disk\n"); if (dev) ramdisk_destroy(dev); kfree(fs); return 0; }
    u32 total, free0, inodes, ifree0;
    struct rpfs_stat tmp;

    pass &= step("format a 1 MB RAM disk and mount it", rpfs_format(dev, "TEST") == 0 && rpfs_mount(fs, dev) == 0);
    rpfs_df(fs, &total, &free0, &inodes, &ifree0);
    pass &= step("fresh filesystem passes fsck and has an empty root", rpfs_fsck(fs, 1) == 0 && count_entries(fs, "/") == 0);

    ok = rpfs_mkdir(fs, "/docs") == 0 && rpfs_mkdir(fs, "/docs/notes") == 0 && rpfs_mkdir(fs, "/docs/notes/deep") == 0;
    ok &= count_entries(fs, "/") == 1 && count_entries(fs, "/docs") == 1;
    pass &= step("nested directories (mkdir, listing)", ok);

    ok = rpfs_mkdir(fs, "/docs") == RPFS_EEXIST && rpfs_mkdir(fs, "/nope/x") == RPFS_ENOENT
      && rpfs_mkdir(fs, "/docs/") == RPFS_EEXIST && rpfs_mkdir(fs, "/bad\x01name") == RPFS_EINVAL
      && rpfs_mkdir(fs, "/this_name_is_much_too_long_for_rpfs") == RPFS_EINVAL && rpfs_mkdir(fs, "/..") == RPFS_EINVAL;
    pass &= step("mkdir rejects duplicates, missing parents and bad names", ok);

    const char *msg = "Hello, RPFS Sequel!";
    u8 rb[64]; u32 got = 0;
    ok = rpfs_write(fs, "/docs/hello.txt", msg, 19, 0) == 0
      && rpfs_read(fs, "/docs/hello.txt", 0, rb, sizeof(rb), &got) == 0 && got == 19 && !memcmp(rb, msg, 19);
    ok &= rpfs_read(fs, "/docs/hello.txt", 7, rb, 4, &got) == 0 && got == 4 && !memcmp(rb, "RPFS", 4);
    ok &= rpfs_read(fs, "/docs/hello.txt", 100, rb, 4, &got) == 0 && got == 0;
    pass &= step("write and read a small file (incl. offset reads and reads past the end)", ok);

    ok = 1;
    static const u32 sizes[] = { 0, 1, 1023, 1024, 1025, 2048, 4097, 12 * 1024, 12 * 1024 + 1, 40000, 100000 };
    for (u32 i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char p[24]; name_n(p, "/f", i);
        if (!roundtrip(fs, p, sizes[i], 31 + i)) ok = 0;
    }
    pass &= step("files of 0 B to 100 KB (block edges, direct/indirect switch) round-trip exactly", ok);

    u8 *big = kmalloc(RPFS_MAX_FILE + 1);
    ok = big != 0;
    if (ok) {
        pattern(big, RPFS_MAX_FILE, 77, 0);
        ok = rpfs_write(fs, "/max", big, RPFS_MAX_FILE, 0) == 0;
        ok &= rpfs_write(fs, "/max", "x", 1, 1) == RPFS_EFBIG;               /* one byte over the limit */
        ok &= rpfs_write(fs, "/toobig", big, RPFS_MAX_FILE + 1, 0) == RPFS_EFBIG && rpfs_stat(fs, "/toobig", &tmp) == RPFS_ENOENT;
        u8 *back = kmalloc(RPFS_MAX_FILE);
        ok &= back && rpfs_read(fs, "/max", 0, back, RPFS_MAX_FILE, &got) == 0 && got == RPFS_MAX_FILE && !memcmp(back, big, RPFS_MAX_FILE);
        kfree(back);
    }
    kfree(big);
    pass &= step("largest file (268 KB) works; one byte more is refused cleanly", ok);
    pass &= step("fsck is clean after all that", rpfs_fsck(fs, 1) == 0);

    {   /* append across block boundaries, in odd-sized pieces */
        u8 piece[333];
        ok = 1;
        for (u32 i = 0; i < 20; i++) {
            pattern(piece, sizeof(piece), 5, i * 333);
            if (rpfs_write(fs, "/appended", piece, sizeof(piece), 1) != 0) ok = 0;
        }
        struct rpfs_stat st;
        ok &= rpfs_stat(fs, "/appended", &st) == 0 && st.size == 20 * 333;
        u8 *all = kmalloc(20 * 333);
        ok &= all && rpfs_read(fs, "/appended", 0, all, 20 * 333, &got) == 0 && got == 20 * 333 && pattern_ok(all, got, 5, 0);
        kfree(all);
        pass &= step("20 appends of 333 bytes build one continuous file", ok);
    }

    {   u32 f1, d;
        rpfs_df(fs, &d, &f1, &d, &d);
        ok = rpfs_write(fs, "/max", "tiny", 4, 0) == 0;
        u32 f2; rpfs_df(fs, &d, &f2, &d, &d);
        struct rpfs_stat st;
        ok &= f2 > f1 + 200 && rpfs_stat(fs, "/max", &st) == 0 && st.size == 4;
        pass &= step("overwriting a big file with a tiny one frees its blocks", ok); }

    ok = rpfs_rename(fs, "/appended", "/docs/moved.txt") == 0 && rpfs_stat(fs, "/appended", &tmp) == RPFS_ENOENT
      && rpfs_stat(fs, "/docs/moved.txt", &tmp) == 0
      && rpfs_rename(fs, "/docs/moved.txt", "/docs/hello.txt") == RPFS_EEXIST
      && rpfs_rename(fs, "/docs", "/docs2") == RPFS_EISDIR
      && rpfs_rename(fs, "/missing", "/x") == RPFS_ENOENT;
    pass &= step("rename moves files between directories and refuses to overwrite", ok);

    ok = rpfs_chdir(fs, "/docs") == 0 && !strcmp(fs->cwd_path, "/docs");
    u8 rb2[32];
    ok &= rpfs_read(fs, "notes/../hello.txt", 0, rb2, sizeof(rb2), &got) == 0 && got == 19;
    ok &= rpfs_chdir(fs, "notes/deep") == 0 && !strcmp(fs->cwd_path, "/docs/notes/deep");
    ok &= rpfs_chdir(fs, "../..") == 0 && !strcmp(fs->cwd_path, "/docs");
    ok &= rpfs_chdir(fs, "./hello.txt") == RPFS_ENOTDIR && rpfs_chdir(fs, "nothing") == RPFS_ENOENT;
    ok &= rpfs_chdir(fs, "/") == 0 && !strcmp(fs->cwd_path, "/") && rpfs_chdir(fs, "..") == 0 && !strcmp(fs->cwd_path, "/");
    pass &= step("current directory: relative paths, \".\" and \"..\", errors", ok);

    ok = rpfs_rmdir(fs, "/docs") == RPFS_ENOTEMPTY && rpfs_unlink(fs, "/docs") == RPFS_EISDIR
      && rpfs_rmdir(fs, "/docs/hello.txt") == RPFS_ENOTDIR && rpfs_unlink(fs, "/docs/notes") == RPFS_EISDIR
      && rpfs_rmdir(fs, "/") == RPFS_EINVAL;
    rpfs_chdir(fs, "/docs/notes/deep");
    ok &= rpfs_rmdir(fs, "/docs/notes/deep") == RPFS_EINVAL;                      /* can't remove where we stand */
    rpfs_chdir(fs, "/");
    ok &= rpfs_rmdir(fs, "/docs/notes/deep") == 0 && rpfs_rmdir(fs, "/docs/notes") == 0
       && rpfs_unlink(fs, "/docs/hello.txt") == 0 && rpfs_unlink(fs, "/docs/moved.txt") == 0 && rpfs_rmdir(fs, "/docs") == 0;
    pass &= step("rm / rmdir refuse the wrong kind of target, then clean up properly", ok);

    {   rpfs_mkdir(fs, "/many");
        char p[32];
        ok = 1;
        for (u32 i = 0; i < 100; i++) { name_n(p, "/many/file", i); if (rpfs_write(fs, p, "x", 1, 0) != 0) ok = 0; }
        ok &= count_entries(fs, "/many") == 100;
        ok &= rpfs_fsck(fs, 1) == 0;
        struct rpfs_stat st;
        ok &= rpfs_stat(fs, "/many", &st) == 0 && st.size >= 4 * RPFS_BLOCK;      /* needed several directory blocks */
        for (u32 i = 0; i < 100; i += 2) { name_n(p, "/many/file", i); if (rpfs_unlink(fs, p) != 0) ok = 0; }
        ok &= count_entries(fs, "/many") == 50;
        for (u32 i = 0; i < 100; i += 2) { name_n(p, "/many/new", i); if (rpfs_write(fs, p, "y", 1, 0) != 0) ok = 0; }   /* reuse freed slots */
        ok &= count_entries(fs, "/many") == 100;
        pass &= step("a directory with 100 entries spans several blocks; slots are reused", ok); }

    {   u32 fb, d, fi;
        rpfs_df(fs, &d, &fb, &d, &fi);
        u8 *chunk = kmalloc(100 * 1024);
        char p[24];
        int err = 0; u32 files = 0;
        if (chunk) { pattern(chunk, 100 * 1024, 9, 0);
            for (u32 i = 0; i < 30 && !err; i++) { name_n(p, "/fill", i); err = rpfs_write(fs, p, chunk, 100 * 1024, 0); if (!err) files++; else files++; } }
        kfree(chunk);
        u32 fb_full; rpfs_df(fs, &d, &fb_full, &d, &d);
        pass &= step("filling the disk ends with \"no space left\" (and fsck still passes)", chunk && err == RPFS_ENOSPC && fb_full == 0 && rpfs_fsck(fs, 1) == 0);
        for (u32 i = 0; i < files; i++) { name_n(p, "/fill", i); rpfs_unlink(fs, p); }
        u32 fb_back; rpfs_df(fs, &d, &fb_back, &d, &d);
        pass &= step("deleting the fill files gives every block back", fb_back == fb && rpfs_fsck(fs, 1) == 0); }

    {   u32 d, fi;
        rpfs_df(fs, &d, &d, &d, &fi);
        rpfs_mkdir(fs, "/inodes");
        char p[32];
        u32 made = 0; int err = 0;
        for (u32 i = 0; i < 400 && !err; i++) { name_n(p, "/inodes/e", i); err = rpfs_write(fs, p, 0, 0, 0); if (!err) made++; }
        u32 fi_now; rpfs_df(fs, &d, &d, &d, &fi_now);
        pass &= step("running out of inodes reports \"no space\" cleanly", err == RPFS_ENOSPC && fi_now == 0 && rpfs_fsck(fs, 1) == 0);
        for (u32 i = 0; i < made; i++) { name_n(p, "/inodes/e", i); rpfs_unlink(fs, p); }
        rpfs_rmdir(fs, "/inodes");
        u32 fi_back; rpfs_df(fs, &d, &d, &d, &fi_back);
        pass &= step("...and all inodes come back after cleanup", fi_back == fi); }

    rpfs_chdir(fs, "/many");
    rpfs_unmount(fs);
    ok = rpfs_read(fs, "/many/new000", 0, rb, 1, &got) == RPFS_ENOFS && rpfs_mkdir(fs, "/x") == RPFS_ENOFS && rpfs_fsck(fs, 0) == RPFS_ENOFS;
    ok &= rpfs_mount(fs, dev) == 0 && !strcmp(fs->cwd_path, "/")
       && rpfs_read(fs, "/many/new000", 0, rb, 1, &got) == 0 && got == 1 && rb[0] == 'y'
       && roundtrip(fs, "/f009", 40000, 40) && rpfs_fsck(fs, 1) == 0;
    pass &= step("unmount, remount: data survives, operations on an unmounted fs fail with \"no filesystem\"", ok);

    {   /* corrupt the bitmap on purpose: the first data block (root directory's) is marked free */
        u8 blk[RPFS_BLOCK];
        u32 bm = fs->sb.bitmap_start, first = fs->sb.data_start;
        ok = dev->read(dev, bm * RPFS_SECTORS, RPFS_SECTORS, blk) == 0;
        u8 saved = blk[first / 8];
        blk[first / 8] &= (u8)~(1u << (first % 8));
        ok &= dev->write(dev, bm * RPFS_SECTORS, RPFS_SECTORS, blk) == 0;
        console_set_color(COL_GREY);
        ok &= rpfs_fsck(fs, 0) > 0;
        blk[first / 8] = saved;
        ok &= dev->write(dev, bm * RPFS_SECTORS, RPFS_SECTORS, blk) == 0 && rpfs_fsck(fs, 1) == 0;
        pass &= step("fsck detects a deliberately corrupted bitmap (and is clean again once repaired)", ok); }

    rpfs_unmount(fs);
    {   u8 zero[RPFS_BLOCK]; memset(zero, 0, sizeof(zero));
        dev->write(dev, 0, RPFS_SECTORS, zero);                       /* wipe the superblock */
        pass &= step("mounting a blank disk is refused", rpfs_mount(fs, dev) == RPFS_ECORRUPT && !rpfs_probe(dev, 0)); }

    kfree(fs);
    ramdisk_destroy(dev);
    console_set_color(pass ? COL_GREEN : COL_RED);
    puts(pass ? "RPFS Sequel self-test PASSED\n" : "RPFS Sequel self-test FAILED\n");
    return pass;
}
