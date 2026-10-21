/* fscmds.c - shell front end for the disk layer and RPFS Sequel */
#include "fscmds.h"
#include "rpfs.h"
#include "blockdev.h"
#include "console.h"
#include "heap.h"
#include "string.h"
#include "rtc.h"
#include "embedded.h"
#include "process.h"

#define TEXT_MAX 128

static struct rpfs *cur;                               /* the mounted filesystem (struct reused across mounts) */

static void err(const char *msg) { console_set_color(COL_RED); puts(msg); putc('\n'); }
static void fail(const char *what, const char *arg, int e) {
    console_set_color(COL_RED);
    puts(what); if (arg) { putc(' '); puts(arg); } puts(": "); puts(rpfs_strerror(e)); putc('\n');
}
static int need_fs(void) {
    if (cur && cur->mounted) return 1;
    err("No filesystem mounted. Try `disks`, then `mount <disk>`.");
    return 0;
}

static void put_pad_dec(u32 v, u32 width) {            /* right-aligned decimal */
    u32 digits = 0, t = v;
    do { digits++; t /= 10; } while (t);
    for (; digits < width; digits++) putc(' ');
    put_dec(v);
}
static void put_dec2(u32 v) { putc((char)('0' + (v / 10) % 10)); putc((char)('0' + v % 10)); }
static void put_date(u32 t) {
    if (!t) { puts("----------------"); return; }
    u32 y, mo, d, h, mi, s;
    rtc_split(t, &y, &mo, &d, &h, &mi, &s);
    put_dec(y); putc('-'); put_dec2(mo); putc('-'); put_dec2(d); putc(' '); put_dec2(h); putc(':'); put_dec2(mi);
}
static void put_size(u32 sectors) {                    /* disk size from sector count */
    u32 kb = sectors / 2;
    if (kb >= 1024) { put_dec(kb / 1024); puts(" MB"); } else { put_dec(kb); puts(" KB"); }
}

/* ---------------- startup ---------------- */

static int is_boot_sector(struct blockdev *d) {        /* sector 0 ends in 0x55 0xAA: looks bootable */
    u8 s[SECTOR_SIZE];
    return d->read(d, 0, 1, s) == 0 && s[510] == 0x55 && s[511] == 0xAA;
}

static int do_mount(struct blockdev *d) {
    if (!cur) { cur = kmalloc(sizeof(*cur)); if (!cur) return RPFS_ENOSPC; cur->mounted = 0; }
    return rpfs_mount(cur, d);
}

void fs_autostart(void) {
    int n = ata_init();
    struct blockdev *ram = ramdisk_create("ram0", 4096, 1);               /* 2 MB scratch disk */
    console_set_color(n ? COL_GREEN : COL_GREY);
    puts("Disks: "); put_dec((u32)n); puts(n == 1 ? " ATA disk" : " ATA disks"); puts(ram ? " + RAM disk ram0.\n" : ".\n");

    for (int i = 0; i < blockdev_count(); i++) {                           /* prefer a real disk that already holds RPFS */
        struct blockdev *d = blockdev_get(i);
        char label[17];
        if (d->kind != BD_ATA || !rpfs_probe(d, label)) continue;
        if (do_mount(d) == 0) {
            console_set_color(COL_GREEN);
            puts("RPFS Sequel: mounted "); puts(d->name); puts(" (\""); puts(label); puts("\").\n");
            return;
        }
    }
    for (int i = 0; i < blockdev_count(); i++) {                           /* hint about unformatted data disks */
        struct blockdev *d = blockdev_get(i);
        if (d->kind != BD_ATA) continue;
        console_set_color(COL_YELLOW);
        puts(d->name); puts(is_boot_sector(d) ? " looks like a boot disk (left alone).\n"
                                              : " has no RPFS filesystem: `format` it, then `mount` it.\n");
    }
    if (ram && rpfs_format(ram, "RAM") == 0 && do_mount(ram) == 0) {
        console_set_color(COL_YELLOW);
        puts("RPFS Sequel: using RAM disk ram0 (contents are lost at reboot).\n");
        int n = embedded_install(cur);
        if (n > 0) { console_set_color(COL_GREEN); puts("Installed "); put_dec((u32)n); puts(" programs in /bin (try `hello`).\n"); }
    } else {
        err("RPFS Sequel: no usable storage.");
    }
}

const char *fs_prompt_dir(void) { return (cur && cur->mounted) ? cur->cwd_path : 0; }
struct rpfs *fs_current(void) { return (cur && cur->mounted) ? cur : 0; }

int embedded_install(struct rpfs *fs) {
    int r = rpfs_mkdir(fs, "/bin");
    if (r && r != RPFS_EEXIST) return r;
    int n = 0;
    for (unsigned i = 0; i < embedded_count; i++) {
        r = rpfs_write(fs, embedded_files[i].path, embedded_files[i].data, embedded_files[i].size, 0);
        if (r) return r;
        n++;
    }
    return n;
}

/* ---------------- disks ---------------- */

void cmd_disks(int argc, char **argv) {
    (void)argc; (void)argv;
    console_set_color(COL_CYAN);
    puts("name  size     type  contents\n");
    for (int i = 0; i < blockdev_count(); i++) {
        struct blockdev *d = blockdev_get(i);
        char label[17];
        console_set_color(COL_YELLOW); puts(d->name);
        for (u32 p = (u32)strlen(d->name); p < 6; p++) putc(' ');
        console_set_color(COL_GREY);
        put_size(d->sectors);
        puts(d->sectors / 2 >= 1024 ? "   " : "  ");
        for (u32 p = 0; p < 2; p++) putc(' ');
        puts(d->kind == BD_ATA ? "ATA   " : "RAM   ");
        if (rpfs_probe(d, label)) { console_set_color(COL_GREEN); puts("RPFS Sequel \""); puts(label); puts("\""); }
        else if (is_boot_sector(d)) puts("boot disk (has a boot signature)");
        else puts("no RPFS filesystem");
        if (cur && cur->mounted && cur->dev == d) { console_set_color(COL_CYAN); puts("  <- mounted"); }
        putc('\n');
        if (d->model[0]) { console_set_color(COL_GREY); puts("      "); puts(d->model); putc('\n'); }
    }
}

void cmd_format(int argc, char **argv) {
    const char *name = 0, *label = "RPFS";
    int force = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) force = 1;
        else if (!name) name = argv[i];
        else label = argv[i];
    }
    if (!name) { err("usage: format <disk> [label] [-f]    (see `disks`)"); return; }
    struct blockdev *d = blockdev_find(name);
    if (!d) { err("No such disk. See `disks`."); return; }
    if (cur && cur->mounted && cur->dev == d) { err("That disk is mounted. `umount` it first."); return; }
    if (!force && is_boot_sector(d)) {
        err("Refusing: that disk has a boot signature and may be your boot disk. Use -f to override.");
        return;
    }
    int r = rpfs_format(d, label);
    if (r) { fail("format", name, r); return; }
    console_set_color(COL_GREEN);
    puts("Formatted "); puts(name); puts(" as RPFS Sequel (\""); puts(label); puts("\"). Use `mount "); puts(name); puts("`.\n");
}

void cmd_mount(int argc, char **argv) {
    if (argc < 2) { err("usage: mount <disk>    (see `disks`)"); return; }
    struct blockdev *d = blockdev_find(argv[1]);
    if (!d) { err("No such disk. See `disks`."); return; }
    int r = do_mount(d);
    if (r) { fail("mount", argv[1], r); return; }
    console_set_color(COL_GREEN);
    puts("Mounted "); puts(d->name); puts(" at /.\n");
}

void cmd_umount(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!need_fs()) return;
    rpfs_unmount(cur);
    console_set_color(COL_GREEN); puts("Unmounted.\n");
}

/* ---------------- browsing ---------------- */

static void print_row(const char *name, const struct rpfs_stat *st) {
    console_set_color(COL_GREY);
    putc(st->mode == RPFS_DIR ? 'd' : '-'); putc(' ');
    put_pad_dec(st->size, 7); putc(' ');
    put_date(st->mtime); putc(' ');
    console_set_color(st->mode == RPFS_DIR ? COL_CYAN : COL_WHITE);
    puts(name);
    if (st->mode == RPFS_DIR) putc('/');
    putc('\n');
}
static int ls_cb(const char *name, const struct rpfs_stat *st, void *ctx) {
    print_row(name, st);
    (*(u32 *)ctx)++;
    return 0;
}

void cmd_ls(int argc, char **argv) {
    if (!need_fs()) return;
    const char *path = argc > 1 ? argv[1] : ".";
    struct rpfs_stat st;
    int r = rpfs_stat(cur, path, &st);
    if (r) { fail("ls", path, r); return; }
    if (st.mode == RPFS_FILE) { print_row(path, &st); return; }
    u32 n = 0;
    r = rpfs_list(cur, path, ls_cb, &n);
    if (r) { fail("ls", path, r); return; }
    console_set_color(COL_GREY);
    put_dec(n); puts(n == 1 ? " entry\n" : " entries\n");
}

void cmd_cat(int argc, char **argv) {
    if (!need_fs()) return;
    if (argc < 2) { err("usage: cat <file>"); return; }
    u8 buf[256];
    u32 off = 0, got = 0;
    char last = '\n';
    console_set_color(COL_WHITE);
    for (;;) {
        int r = rpfs_read(cur, argv[1], off, buf, sizeof(buf), &got);
        if (r) { fail("cat", argv[1], r); return; }
        if (got == 0) break;
        for (u32 i = 0; i < got; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\t' || (c >= 32 && c < 127)) putc(c); else putc('.');
            last = c;
        }
        off += got;
    }
    if (last != '\n') putc('\n');
}

/* ---------------- changing things ---------------- */

static u32 unescape(char *s) {                         /* \n \t \\ in command arguments */
    char *o = s, *start = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            if (*s == 'n') *o++ = '\n';
            else if (*s == 't') *o++ = '\t';
            else if (*s == '\\') *o++ = '\\';
            else { *o++ = '\\'; *o++ = *s; }
        } else *o++ = *s;
    }
    *o = 0;
    return (u32)(o - start);
}

static void do_write(int argc, char **argv, int append) {
    if (!need_fs()) return;
    if (argc < 2) { err(append ? "usage: append <file> <text...>" : "usage: write <file> [text...]   (\\n and \\t work in text)"); return; }
    char text[TEXT_MAX];
    u32 n = 0;
    for (int i = 2; i < argc; i++) {
        if (i > 2 && n < TEXT_MAX - 1) text[n++] = ' ';
        for (const char *p = argv[i]; *p && n < TEXT_MAX - 1; p++) text[n++] = *p;
    }
    text[n] = 0;
    u32 len = unescape(text);
    int r = rpfs_write(cur, argv[1], text, len, append);
    if (r) fail(append ? "append" : "write", argv[1], r);
}
void cmd_write(int argc, char **argv)  { do_write(argc, argv, 0); }
void cmd_append(int argc, char **argv) { do_write(argc, argv, 1); }

#define SIMPLE_CMD(fn, usage, call, name) \
    void fn(int argc, char **argv) { \
        if (!need_fs()) return; \
        if (argc < 2) { err(usage); return; } \
        int r = call(cur, argv[1]); \
        if (r) fail(name, argv[1], r); \
    }
SIMPLE_CMD(cmd_mkdir, "usage: mkdir <dir>",   rpfs_mkdir,  "mkdir")
SIMPLE_CMD(cmd_rm,    "usage: rm <file>",     rpfs_unlink, "rm")
SIMPLE_CMD(cmd_rmdir, "usage: rmdir <dir>",   rpfs_rmdir,  "rmdir")

/* If `dst` is an existing directory, the copy/move goes inside it under src's name. */
static int resolve_dest(const char *src, const char *dst, char *out) {
    struct rpfs_stat st;
    u32 len = (u32)strlen(dst);
    if (len >= RPFS_PATH_MAX - RPFS_NAME_MAX - 2) return RPFS_EINVAL;
    strcpy(out, dst);
    if (rpfs_stat(cur, dst, &st) == 0 && st.mode == RPFS_DIR) {
        u32 sl = (u32)strlen(src);
        while (sl > 1 && src[sl - 1] == '/') sl--;
        u32 b = sl;
        while (b > 0 && src[b - 1] != '/') b--;
        if (sl - b == 0 || sl - b > RPFS_NAME_MAX) return RPFS_EINVAL;
        if (len > 0 && out[len - 1] != '/') out[len++] = '/';
        for (u32 i = b; i < sl; i++) out[len++] = src[i];
        out[len] = 0;
    }
    return 0;
}

void cmd_mv(int argc, char **argv) {
    if (!need_fs()) return;
    if (argc < 3) { err("usage: mv <file> <new name or directory>"); return; }
    char dst[RPFS_PATH_MAX];
    int r = resolve_dest(argv[1], argv[2], dst);
    if (!r) r = rpfs_rename(cur, argv[1], dst);
    if (r) fail("mv", argv[1], r);
}

void cmd_cp(int argc, char **argv) {
    if (!need_fs()) return;
    if (argc < 3) { err("usage: cp <file> <new name or directory>"); return; }
    struct rpfs_stat st;
    int r = rpfs_stat(cur, argv[1], &st);
    if (r) { fail("cp", argv[1], r); return; }
    if (st.mode == RPFS_DIR) { fail("cp", argv[1], RPFS_EISDIR); return; }
    char dst[RPFS_PATH_MAX];
    r = resolve_dest(argv[1], argv[2], dst);
    if (r) { fail("cp", argv[2], r); return; }
    u8 *buf = kmalloc(st.size ? st.size : 1);
    if (!buf) { fail("cp", argv[1], RPFS_ENOSPC); return; }
    u32 got = 0;
    r = st.size ? rpfs_read(cur, argv[1], 0, buf, st.size, &got) : 0;
    if (!r) r = rpfs_write(cur, dst, buf, got, 0);
    kfree(buf);
    if (r) fail("cp", argv[1], r);
}

void cmd_cd(int argc, char **argv) {
    if (!need_fs()) return;
    const char *path = argc > 1 ? argv[1] : "/";
    int r = rpfs_chdir(cur, path);
    if (r) fail("cd", path, r);
}

void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!need_fs()) return;
    console_set_color(COL_WHITE); puts(cur->cwd_path); putc('\n');
}

/* ---------------- information and checking ---------------- */

static void put_blocks_kb(u32 blocks) { put_dec(blocks); puts(" KB"); }   /* 1 block = 1 KB */

void cmd_df(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!need_fs()) return;
    u32 total, freeb, inodes, ifree;
    rpfs_df(cur, &total, &freeb, &inodes, &ifree);
    console_set_color(COL_CYAN); puts("RPFS Sequel on "); puts(cur->dev->name); puts(" (\""); puts(cur->sb.label); puts("\")\n");
    console_set_color(COL_GREY);
    puts("  data space: "); put_blocks_kb(total); puts(" total, ");
    put_blocks_kb(total - freeb); puts(" used, "); put_blocks_kb(freeb); puts(" free\n");
    puts("  inodes:     "); put_dec(inodes); puts(" total, "); put_dec(inodes - ifree); puts(" used, "); put_dec(ifree); puts(" free\n");
    puts("  max file size: 268 KB, name length: up to 27 characters\n");
}

void cmd_fsck(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!need_fs()) return;
    console_set_color(COL_GREY); puts("Checking...\n");
    int p = rpfs_fsck(cur, 1);
    if (p < 0) { fail("fsck", 0, p); return; }
    if (p == 0) { console_set_color(COL_GREEN); puts("fsck: filesystem is clean.\n"); }
    else { console_set_color(COL_RED); puts("fsck: "); put_dec((u32)p); puts(p == 1 ? " problem found.\n" : " problems found.\n"); }
}

void cmd_fstest(int argc, char **argv) {
    (void)argc; (void)argv;
    console_set_color(COL_CYAN); puts("RPFS Sequel self-test (on a private RAM disk):\n");
    rpfs_selftest();
}

/* ---------------- programs ---------------- */

void cmd_run(int argc, char **argv) {
    if (!need_fs()) return;
    if (argc < 2) { err("usage: run <program> [arguments]   (looks in the current directory, then /bin; .exe is optional)"); return; }
    if (!process_shell_run(argc - 1, argv + 1)) { console_set_color(COL_RED); puts("run: no such program: "); puts(argv[1]); putc('\n'); }
}

void cmd_install(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!need_fs()) return;
    int n = embedded_install(cur);
    if (n < 0) { fail("install", "/bin", n); return; }
    console_set_color(COL_GREEN); puts("Installed "); put_dec((u32)n); puts(" programs into /bin.\n");
}

void cmd_proctest(int argc, char **argv) {
    (void)argc; (void)argv;
    console_set_color(COL_CYAN); puts("Program loader self-test (on a private RAM disk):\n");
    process_selftest();
}
