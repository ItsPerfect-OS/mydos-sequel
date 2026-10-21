#ifndef FSCMDS_H
#define FSCMDS_H
/* Shell commands for RPFS Sequel and the disk layer. Each has the usual (argc, argv) signature. */
void fs_autostart(void);                 /* probe disks, mount an RPFS disk, or fall back to a RAM disk */
const char *fs_prompt_dir(void);         /* current directory for the prompt, or NULL if nothing is mounted */
struct rpfs *fs_current(void);           /* the mounted filesystem, or NULL */

void cmd_disks(int argc, char **argv);
void cmd_format(int argc, char **argv);
void cmd_mount(int argc, char **argv);
void cmd_umount(int argc, char **argv);
void cmd_ls(int argc, char **argv);
void cmd_cat(int argc, char **argv);
void cmd_write(int argc, char **argv);
void cmd_append(int argc, char **argv);
void cmd_mkdir(int argc, char **argv);
void cmd_rm(int argc, char **argv);
void cmd_rmdir(int argc, char **argv);
void cmd_mv(int argc, char **argv);
void cmd_cp(int argc, char **argv);
void cmd_cd(int argc, char **argv);
void cmd_pwd(int argc, char **argv);
void cmd_df(int argc, char **argv);
void cmd_fsck(int argc, char **argv);
void cmd_fstest(int argc, char **argv);
void cmd_run(int argc, char **argv);
void cmd_install(int argc, char **argv);
void cmd_proctest(int argc, char **argv);
#endif
