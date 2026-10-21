/* shell.c - MyDOS Sequel command shell */
#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "string.h"
#include "timer.h"
#include "io.h"
#include "memmap.h"
#include "pmm.h"
#include "heap.h"
#include "paging.h"
#include "fscmds.h"
#include "process.h"

#define LINE_MAX 128
#define ARGV_MAX 16

struct command {
    const char *name;
    const char *help;
    void (*fn)(int argc, char **argv);
};

static void cmd_help(int argc, char **argv);

static void cmd_clear(int argc, char **argv) { (void)argc; (void)argv; console_clear(); }

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) { if (i > 1) putc(' '); puts(argv[i]); }
    putc('\n');
}

static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    u32 t = timer_ticks(), hz = timer_hz();
    u32 s = t / hz;
    puts("up "); put_dec(s / 60); puts(" min "); put_dec(s % 60); puts(" s  (");
    put_dec(t); puts(" ticks @ "); put_dec(hz); puts(" Hz)\n");
}

static void cmd_about(int argc, char **argv) {
    (void)argc; (void)argv;
    console_set_color(COL_CYAN); puts("MyDOS Sequel 0.9 (x86-64)\n");
    console_set_color(COL_GREY); puts("BIOS boot, 64-bit long mode, written from scratch.\n");
}

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("Rebooting...\n");
    for (int i = 0; i < 100000 && inb(0x64) != 0xFF && (inb(0x64) & 0x02); i++) {}
    outb(0x64, 0xFE);                        /* pulse the CPU reset line via the keyboard controller */
    for (volatile int i = 0; i < 2000000; i++) {}
    /* No controller (or it ignored us): force a triple fault with an empty IDT. */
    static const struct { u16 limit; u64 base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile("cli; lidt %0; int3" :: "m"(null_idt));
    for (;;) __asm__ volatile("hlt");
}

static void cmd_meminfo(int argc, char **argv) {
    (void)argc; (void)argv;
    memmap_print();
    putc('\n');
    pmm_print();
    putc('\n');
    heap_print();
    putc('\n');
    paging_print();
}

static void cmd_pmmtest(int argc, char **argv) {
    (void)argc; (void)argv;
    console_set_color(COL_CYAN); puts("Frame allocator self-test:\n");
    pmm_selftest();
}

static void cmd_heaptest(int argc, char **argv) {
    (void)argc; (void)argv;
    console_set_color(COL_CYAN); puts("Kernel heap self-test:\n");
    heap_selftest();
}

static void cmd_pagetest(int argc, char **argv) {
    (void)argc; (void)argv;
    console_set_color(COL_CYAN); puts("Paging self-test:\n");
    paging_selftest();
}

static void cmd_crash(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "";
    console_set_color(COL_YELLOW);
    if (!strcmp(what, "null")) {
        puts("Writing to address 0 (NULL)... the system will halt.\n");
        __asm__ volatile("movl $1, (%0)" :: "r"((uptr)0) : "memory");
    } else if (!strcmp(what, "ro")) {
        puts("Writing to kernel code at 0x100000... the system will halt.\n");
        __asm__ volatile("movl $1, (%0)" :: "r"((uptr)0x100000) : "memory");
    } else if (!strcmp(what, "unmapped")) {
        puts("Reading unmapped address 0x7FFFDEAD0000... the system will halt.\n");
        __asm__ volatile("movl (%0), %%eax" :: "r"((uptr)0x7FFFDEAD0000UL) : "eax", "memory");
    } else if (!strcmp(what, "div0")) {
        puts("Dividing by zero... the system will halt.\n");
        __asm__ volatile("xor %%edx, %%edx; mov $1, %%eax; xor %%ecx, %%ecx; div %%ecx" ::: "eax", "ecx", "edx");
    } else if (!strcmp(what, "ud")) {
        puts("Executing an invalid instruction... the system will halt.\n");
        __asm__ volatile("ud2");
    } else {
        puts("usage: crash null | ro | unmapped | div0 | ud   (deliberately faults; reboot afterwards)\n");
    }
}

static const struct command commands[] = {
    { "help",    "list commands",           cmd_help     },
    { "clear",   "clear the screen",        cmd_clear    },
    { "echo",    "print arguments",         cmd_echo     },
    { "uptime",  "time since boot",         cmd_uptime   },
    { "about",   "about MyDOS Sequel",      cmd_about    },
    { "reboot",  "restart the machine",     cmd_reboot   },
    { "disks",   "list disks",              cmd_disks    },
    { "format",  "format a disk as RPFS",   cmd_format   },
    { "mount",   "mount a disk",            cmd_mount    },
    { "umount",  "unmount the filesystem",  cmd_umount   },
    { "ls",      "list a directory",        cmd_ls       },
    { "cat",     "show a file",             cmd_cat      },
    { "write",   "write text to a file",    cmd_write    },
    { "append",  "append text to a file",   cmd_append   },
    { "mkdir",   "make a directory",        cmd_mkdir    },
    { "rm",      "delete a file",           cmd_rm       },
    { "rmdir",   "delete an empty dir",     cmd_rmdir    },
    { "mv",      "rename or move a file",   cmd_mv       },
    { "cp",      "copy a file",             cmd_cp       },
    { "cd",      "change directory",        cmd_cd       },
    { "pwd",     "print directory",         cmd_pwd      },
    { "df",      "filesystem usage",        cmd_df       },
    { "fsck",    "check the filesystem",    cmd_fsck     },
    { "fstest",  "filesystem self-test",    cmd_fstest   },
    { "run",     "run a program (.exe)",    cmd_run      },
    { "install", "copy demo programs /bin", cmd_install  },
    { "proctest","program loader test",     cmd_proctest },
    { "meminfo", "memory map, PMM, heap",   cmd_meminfo  },
    { "pmmtest", "frame allocator test",    cmd_pmmtest  },
    { "heaptest","kernel heap test",        cmd_heaptest },
    { "pagetest","paging test",             cmd_pagetest },
    { "crash",   "trigger a CPU fault",     cmd_crash    },
};
#define NCOMMANDS (sizeof(commands) / sizeof(commands[0]))

static void put_cmd(const struct command *c) {
    console_set_color(COL_YELLOW); puts(c->name);
    for (u32 pad = strlen(c->name); pad < 9; pad++) putc(' ');
    console_set_color(COL_GREY);   puts(c->help);
    for (u32 pad = strlen(c->help); pad < 25; pad++) putc(' ');
}

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    for (u32 i = 0; i < NCOMMANDS; i += 2) {
        put_cmd(&commands[i]);
        if (i + 1 < NCOMMANDS) put_cmd(&commands[i + 1]);
        putc('\n');
    }
    console_set_color(COL_GREY);
    puts("Files: write notes.txt Hello\\nWorld   then   cat notes.txt   (\\n = new line)\n");
}

static int read_line(char *line, int max) {
    int n = 0;
    for (;;) {
        char c = keyboard_getc();
        if (c == '\n') { putc('\n'); break; }
        if (c == '\b') { if (n > 0) { n--; putc('\b'); } continue; }
        if (c == 3)    { puts("^C\n"); n = 0; break; }          /* Ctrl+C cancels the line */
        if (c >= 32 && c < 127 && n < max - 1) { line[n++] = c; putc(c); }
    }
    line[n] = 0;
    return n;
}

static int tokenize(char *line, char **argv) {
    int argc = 0;
    while (*line && argc < ARGV_MAX) {
        while (*line == ' ') *line++ = 0;
        if (!*line) break;
        argv[argc++] = line;
        while (*line && *line != ' ') line++;
    }
    return argc;
}

static void execute(char *line) {
    char *argv[ARGV_MAX];
    int argc = tokenize(line, argv);
    if (argc == 0) return;
    for (u32 i = 0; i < NCOMMANDS; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) { commands[i].fn(argc, argv); return; }
    }
    if (process_shell_run(argc, argv)) return;                 /* not a built-in: maybe a program */
    console_set_color(COL_RED);
    puts("Unknown command: "); puts(argv[0]); puts("  (type 'help')\n");
}

void shell_run(void) {
    char line[LINE_MAX];
    for (;;) {
        console_set_color(COL_GREEN); puts("mydos");
        { const char *dir = fs_prompt_dir(); if (dir) { putc(':'); puts(dir); } }
        puts("> ");
        console_set_color(COL_WHITE);
        if (read_line(line, LINE_MAX) > 0) {
            console_set_color(COL_GREY);
            execute(line);
        }
    }
}
