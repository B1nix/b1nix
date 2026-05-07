#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <b1nix/blk.h>
#include <b1nix/btrfs.h>
#include <b1nix/pci.h>
#include <b1nix/syscall.h>
#include <b1nix/posix.h>
#include <b1nix/dirent.h>
#include <b1nix/net.h>

/* ── Helper: open file, return fd or -1 ── */
static u64 bb_open(const char *path)
{
	return syscall_dispatch(SYS_OPEN, (u64)(usize)path, 0, 0, 0);
}

/* ── Helper: read file into buffer, return bytes read ── */
static usize bb_read_file(const char *path, char *buf, usize max)
{
	u64 fd = bb_open(path);
	if (fd == (u64)-1) return 0;
	u64 n = syscall_dispatch(SYS_READ, fd, (u64)(usize)buf, max - 1, 0);
	syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
	if (n == (u64)-1) return 0;
	buf[n] = '\0';
	return (usize)n;
}

/* ── Helper: write data to file (create/truncate) ── */
static int bb_write_file(const char *path, const char *data, usize len)
{
	/* Try create, then open */
	syscall_dispatch(SYS_CREATE, (u64)(usize)path, (u64)(usize)"", 0, 0);
	u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path, (u64)B1NIX_O_WRONLY | B1NIX_O_TRUNC, 0, 0);
	if (fd == (u64)-1) return -1;
	syscall_dispatch(SYS_WRITE, (u64)(usize)data, (u64)len, (u64)fd, 1);
	syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
	return 0;
}

/* ── Helper: resolve relative path against cwd ── */
static void bb_resolve(const char *rel, char *abs, usize abs_size)
{
	if (rel[0] == '/') {
		usize i = 0;
		while (rel[i] && i < abs_size - 1) { abs[i] = rel[i]; i++; }
		abs[i] = '\0';
		return;
	}
	char cwd[128];
	if (getcwd(cwd, sizeof(cwd)) < 0) {
		cwd[0] = '/'; cwd[1] = '\0';
	}
	usize cl = strlen(cwd);
	usize rl = strlen(rel);
	usize total = cl + rl + 2;
	if (total > abs_size) total = abs_size;
	memcpy(abs, cwd, cl);
	if (cl > 0 && abs[cl - 1] != '/') abs[cl++] = '/';
	memcpy(abs + cl, rel, rl);
	abs[cl + rl] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════
   FILE & DIRECTORY UTILITIES
   ═══════════════════════════════════════════════════════════════════ */

/* ── pwd — print working directory ── */
static int pwd_main(int argc, const char **argv)
{
	(void)argc; (void)argv;
	char cwd[256];
	if (getcwd(cwd, sizeof(cwd)) < 0) {
		printf("pwd: cannot get current directory\n");
		return 1;
	}
	printf("%s\n", cwd);
	return 0;
}

/* ── ls — list directory contents ── */
static int ls_main(int argc, const char **argv)
{
	int long_fmt = 0;
	int all_files = 0;
	const char *target = ".";
	
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			for (int j = 1; argv[i][j]; j++) {
				if (argv[i][j] == 'l') long_fmt = 1;
				if (argv[i][j] == 'a') all_files = 1;
			}
		} else {
			target = argv[i];
		}
	}
	
	char path[256];
	bb_resolve(target, path, sizeof(path));

	struct dirent entries[64];
	int count = (int)syscall_dispatch(SYS_READDIR, (u64)(usize)path,
	                                   (u64)(usize)entries, 64, 0);
	if (count < 0) {
		printf("ls: %s: No such file or directory\n", target);
		return 1;
	}

	for (int i = 0; i < count; i++) {
		if (!all_files && entries[i].name[0] == '.') continue;
		if (long_fmt) {
			char type = '-';
			if (entries[i].is_dir) type = 'd';
			else if (entries[i].type == 2) type = 'c';
			printf("%c  %8d  %s\n", type, (int)entries[i].size, entries[i].name);
		} else {
			printf("%s  ", entries[i].name);
		}
	}
	if (!long_fmt) printf("\n");
	return 0;
}

/* ── cp — copy file ── */
static int bb_copy_file(const char *src, const char *dst) {
	u64 fds = bb_open(src);
	if (fds == (u64)-1) return -1;
	syscall_dispatch(SYS_CREATE, (u64)(usize)dst, (u64)(usize)"", 0, 0);
	u64 fdd = syscall_dispatch(SYS_OPEN, (u64)(usize)dst, (u64)B1NIX_O_WRONLY | B1NIX_O_TRUNC, 0, 0);
	if (fdd == (u64)-1) { syscall_dispatch(SYS_CLOSE, fds, 0, 0, 0); return -1; }
	char buf[4096];
	while (1) {
		u64 n = syscall_dispatch(SYS_READ, fds, (u64)(usize)buf, sizeof(buf), 0);
		if (n == 0 || n == (u64)-1) break;
		syscall_dispatch(SYS_WRITE, (u64)(usize)buf, n, fdd, 1);
	}
	syscall_dispatch(SYS_CLOSE, fds, 0, 0, 0);
	syscall_dispatch(SYS_CLOSE, fdd, 0, 0, 0);
	return 0;
}

static void cp_recursive(const char *src, const char *dst) {
	struct b1nix_stat st;
	if (syscall_dispatch(SYS_LSTAT, (u64)(usize)src, (u64)(usize)&st, 0, 0) != 0) return;
	if ((st.st_mode & B1NIX_S_IFDIR) == B1NIX_S_IFDIR) {
		syscall_dispatch(SYS_MKDIR, (u64)(usize)dst, 0755, 0, 0);
		struct dirent entries[32];
		int count = (int)syscall_dispatch(SYS_READDIR, (u64)(usize)src, (u64)(usize)entries, 32, 0);
		for (int i = 0; i < count; i++) {
			if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) continue;
			char ssub[256], dsub[256];
			snprintf(ssub, sizeof(ssub), "%s/%s", src, entries[i].name);
			snprintf(dsub, sizeof(dsub), "%s/%s", dst, entries[i].name);
			cp_recursive(ssub, dsub);
		}
	} else {
		bb_copy_file(src, dst);
	}
}

static int cp_main(int argc, const char **argv)
{
	int recursive = 0;
	int start_idx = 1;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			if (strchr(argv[i], 'r') || strchr(argv[i], 'R')) recursive = 1;
			start_idx = i + 1;
		} else break;
	}
	if (argc - start_idx < 2) {
		printf("cp: missing operand\n");
		return 1;
	}
	char s[256], d[256];
	bb_resolve(argv[start_idx], s, sizeof(s));
	bb_resolve(argv[start_idx + 1], d, sizeof(d));
	if (recursive) cp_recursive(s, d);
	else bb_copy_file(s, d);
	return 0;
}

/* ── mv — move/rename file ── */
static int mv_main(int argc, const char **argv)
{
	if (argc < 3) {
		printf("mv: missing operand\nUsage: mv <src> <dst>\n");
		return 1;
	}

	char src[256], dst[256];
	bb_resolve(argv[1], src, sizeof(src));
	bb_resolve(argv[2], dst, sizeof(dst));

	if (syscall_dispatch(SYS_RENAME, (u64)(usize)src,
	                     (u64)(usize)dst, 0, 0) != 0) {
		printf("mv: cannot move %s to %s\n", argv[1], argv[2]);
		return 1;
	}
	return 0;
}

/* ── rm — remove file ── */
static void rm_recursive(const char *path) {
	struct b1nix_stat st;
	if (syscall_dispatch(SYS_LSTAT, (u64)(usize)path, (u64)(usize)&st, 0, 0) != 0) return;
	
	if ((st.st_mode & B1NIX_S_IFDIR) == B1NIX_S_IFDIR) {
		struct dirent entries[32];
		int count = (int)syscall_dispatch(SYS_READDIR, (u64)(usize)path, (u64)(usize)entries, 32, 0);
		for (int i = 0; i < count; i++) {
			if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) continue;
			char sub[256];
			snprintf(sub, sizeof(sub), "%s/%s", path, entries[i].name);
			rm_recursive(sub);
		}
		syscall_dispatch(SYS_RMDIR, (u64)(usize)path, 0, 0, 0);
	} else {
		syscall_dispatch(SYS_UNLINK, (u64)(usize)path, 0, 0, 0);
	}
}

static int rm_main(int argc, const char **argv)
{
	int recursive = 0;
	int force = 0;
	int start_idx = 1;

	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			for (int j = 1; argv[i][j]; j++) {
				if (argv[i][j] == 'r' || argv[i][j] == 'R') recursive = 1;
				if (argv[i][j] == 'f') force = 1;
			}
			start_idx = i + 1;
		} else break;
	}

	if (start_idx >= argc) {
		printf("rm: missing operand\n");
		return 1;
	}

	for (int i = start_idx; i < argc; i++) {
		char path[256];
		bb_resolve(argv[i], path, sizeof(path));
		if (recursive) {
			rm_recursive(path);
		} else {
			if (syscall_dispatch(SYS_UNLINK, (u64)(usize)path, 0, 0, 0) != 0 && !force) {
				printf("rm: cannot remove %s\n", argv[i]);
			}
		}
	}
	return 0;
}

/* ── mkdir — create directory ── */
static int mkdir_p(const char *path) {
	char tmp[256];
	strncpy(tmp, path, 256);
	char *p = tmp;
	if (*p == '/') p++;
	while (*p) {
		if (*p == '/') {
			*p = '\0';
			if (tmp[0] != '\0') {
				struct b1nix_stat st;
				if (syscall_dispatch(SYS_STAT, (u64)(usize)tmp, (u64)(usize)&st, 0, 0) == 0) {
					if (!((st.st_mode & B1NIX_S_IFDIR) == B1NIX_S_IFDIR)) return -1;
				} else {
					syscall_dispatch(SYS_MKDIR, (u64)(usize)tmp, 0755, 0, 0);
				}
			}
			*p = '/';
			while (*(p + 1) == '/') p++; /* Skip duplicate slashes */
		}
		p++;
	}
	struct b1nix_stat st;
	if (syscall_dispatch(SYS_STAT, (u64)(usize)tmp, (u64)(usize)&st, 0, 0) == 0) {
		if (!((st.st_mode & B1NIX_S_IFDIR) == B1NIX_S_IFDIR)) return -1;
		return 0;
	}
	return (int)syscall_dispatch(SYS_MKDIR, (u64)(usize)tmp, 0755, 0, 0);
}

static int mkdir_main(int argc, const char **argv)
{
	int parents = 0;
	int start_idx = 1;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			if (strchr(argv[i], 'p')) parents = 1;
			start_idx = i + 1;
		} else break;
	}

	if (start_idx >= argc) {
		printf("mkdir: missing operand\n");
		return 1;
	}

	for (int i = start_idx; i < argc; i++) {
		char path[256];
		bb_resolve(argv[i], path, sizeof(path));
		if (parents) {
			mkdir_p(path);
		} else {
			if (syscall_dispatch(SYS_MKDIR, (u64)(usize)path, 0755, 0, 0) != 0) {
				printf("mkdir: cannot create directory %s\n", argv[i]);
			}
		}
	}
	return 0;
}

/* ── rmdir — remove empty directory ── */
static int rmdir_main(int argc, const char **argv)
{
	if (argc < 2) {
		printf("rmdir: missing operand\n");
		return 1;
	}

	for (int i = 1; i < argc; i++) {
		char path[256];
		bb_resolve(argv[i], path, sizeof(path));
		if (syscall_dispatch(SYS_RMDIR, (u64)(usize)path, 0, 0, 0) != 0) {
			printf("rmdir: failed to remove %s\n", argv[i]);
		}
	}
	return 0;
}

/* ── chmod — change file mode ── */
static int chmod_main(int argc, const char **argv)
{
	if (argc < 3) {
		printf("chmod: missing operand\nUsage: chmod <mode> <file>\n");
		return 1;
	}

	/* Parse octal mode */
	const char *mode_str = argv[1];
	u16 mode = 0;
	for (int i = 0; mode_str[i]; i++) {
		if (mode_str[i] >= '0' && mode_str[i] <= '7') {
			mode = (u16)((mode << 3) | (mode_str[i] - '0'));
		}
	}

	for (int i = 2; i < argc; i++) {
		char path[256];
		bb_resolve(argv[i], path, sizeof(path));
		if (syscall_dispatch(SYS_CHMOD, (u64)(usize)path,
		                     (u64)mode, 0, 0) != 0) {
			printf("chmod: cannot access %s\n", argv[i]);
		}
	}
	return 0;
}

/* ── chown — change file owner ── */
static int chown_main(int argc, const char **argv)
{
	if (argc < 3) {
		printf("chown: missing operand\nUsage: chown <uid> <gid> <file>\n");
		return 1;
	}

	u16 uid = (u16)atoi(argv[1]);
	u16 gid = (u16)atoi(argv[2]);

	for (int i = 3; i < argc; i++) {
		char path[256];
		bb_resolve(argv[i], path, sizeof(path));
		if (syscall_dispatch(SYS_CHOWN, (u64)(usize)path,
		                     (u64)uid, (u64)gid, 0) != 0) {
			printf("chown: cannot access %s\n", argv[i]);
		}
	}
	return 0;
}

/* ── ln — create hard or symbolic links ── */
static int ln_main(int argc, const char **argv)
{
	int symbolic = 0;
	int argi = 1;

	if (argc > 1 && strcmp(argv[1], "-s") == 0) {
		symbolic = 1;
		argi = 2;
	}

	if (argc - argi < 2) {
		printf("ln: missing operand\nUsage: ln [-s] <target> <linkname>\n");
		return 1;
	}

	char dst[256];
	bb_resolve(argv[argi + 1], dst, sizeof(dst));

	if (symbolic) {
		if (syscall_dispatch(SYS_SYMLINK, (u64)(usize)argv[argi],
		                     (u64)(usize)dst, 0, 0) != 0) {
			printf("ln: cannot create symbolic link %s\n", argv[argi + 1]);
			return 1;
		}
		return 0;
	}

	char src[256];
	bb_resolve(argv[argi], src, sizeof(src));
	if (syscall_dispatch(SYS_LINK, (u64)(usize)src, (u64)(usize)dst, 0, 0) != 0) {
		printf("ln: cannot create link %s\n", argv[argi + 1]);
		return 1;
	}
	return 0;
}

/* ── readlink — print symlink target ── */
static int readlink_main(int argc, const char **argv)
{
	if (argc < 2) {
		printf("readlink: missing operand\nUsage: readlink <path>\n");
		return 1;
	}

	char path[256];
	char target[256];
	bb_resolve(argv[1], path, sizeof(path));
	long n = (long)syscall_dispatch(SYS_READLINK, (u64)(usize)path,
	                                (u64)(usize)target, sizeof(target) - 1, 0);
	if (n < 0) {
		printf("readlink: %s: Invalid argument\n", argv[1]);
		return 1;
	}
	target[n] = '\0';
	printf("%s\n", target);
	return 0;
}

/* ── touch — update file timestamps or create empty file ── */
static int touch_main(int argc, const char **argv)
{
	if (argc < 2) {
		printf("touch: missing operand\n");
		return 1;
	}

	for (int i = 1; i < argc; i++) {
		char path[256];
		bb_resolve(argv[i], path, sizeof(path));
		
		/* Try to create file if it doesn't exist. In B1NIX, SYS_CREATE is often used. */
		syscall_dispatch(SYS_CREATE, (u64)(usize)path, (u64)(usize)"", 0, 0);
		
		/* Open and close to update timestamp (if kernel supports it) */
		u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path, (u64)B1NIX_O_WRONLY, 0, 0);
		if (fd != (u64)-1) {
			syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
		} else {
			printf("touch: cannot touch %s\n", argv[i]);
		}
	}
	return 0;
}

/* ── basename — strip directory and suffix from filenames ── */
static int basename_main(int argc, const char **argv)
{
	if (argc < 2) {
		printf("basename: missing operand\n");
		return 1;
	}

	const char *path = argv[1];
	const char *base = strrchr(path, '/');
	if (base) {
		printf("%s\n", base + 1);
	} else {
		printf("%s\n", path);
	}
	return 0;
}

/* ── dirname — strip last component from file name ── */
static int dirname_main(int argc, const char **argv)
{
	if (argc < 2) {
		printf("dirname: missing operand\n");
		return 1;
	}

	char path[256];
	strncpy(path, argv[1], sizeof(path));
	path[sizeof(path) - 1] = '\0';

	char *last = strrchr(path, '/');
	if (!last) {
		printf(".\n");
	} else if (last == path) {
		printf("/\n");
	} else {
		*last = '\0';
		printf("%s\n", path);
	}
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
   SYSTEM UTILITIES
   ═══════════════════════════════════════════════════════════════════ */

/* ── ps — process status ── */
static int ps_main(int argc, const char **argv)
{
	(void)argc; (void)argv;
	syscall_dispatch(SYS_PS, 0, 0, 0, 0);
	return 0;
}

/* ── kill — send signal to process ── */
static int kill_main(int argc, const char **argv)
{
	if (argc < 2) {
		printf("kill: missing operand\nUsage: kill <pid> [sig]\n");
		return 1;
	}

	int pid = atoi(argv[1]);
	int sig = 15; /* SIGTERM default */
	if (argc > 2) sig = atoi(argv[2]);

	if (syscall_dispatch(SYS_KILL, (u64)pid, (u64)sig, 0, 0) != 0) {
		printf("kill: failed to send signal %d to %d\n", sig, pid);
		return 1;
	}
	return 0;
}

/* ── date — print system date/time ── */
static int date_main(int argc, const char **argv)
{
	(void)argc; (void)argv;
	long seconds = time();

	/* Simple conversion: seconds since boot */
	long days = seconds / 86400;
	long hours = (seconds % 86400) / 3600;
	long mins = (seconds % 3600) / 60;
	long secs = seconds % 60;

	printf("Uptime: %ld day%s %02ld:%02ld:%02ld\n",
	       days, days == 1 ? "" : "s", hours, mins, secs);
	return 0;
}

/* ── uname — print system information ── */
static int uname_main(int argc, const char **argv)
{
	struct b1nix_utsname uts;
	if (uname(&uts) < 0) {
		printf("uname: error\n");
		return 1;
	}

	int all = (argc < 2);
	int show_s = 0, show_n = 0, show_r = 0, show_v = 0, show_m = 0;

	if (!all) {
		for (int i = 1; i < argc; i++) {
			if (strcmp(argv[i], "-s") == 0) show_s = 1;
			else if (strcmp(argv[i], "-n") == 0) show_n = 1;
			else if (strcmp(argv[i], "-r") == 0) show_r = 1;
			else if (strcmp(argv[i], "-v") == 0) show_v = 1;
			else if (strcmp(argv[i], "-m") == 0) show_m = 1;
			else if (strcmp(argv[i], "-a") == 0)
				show_s = show_n = show_r = show_v = show_m = 1;
		}
	}

	if (all || show_s) printf("%s ", uts.sysname);
	if (all || show_n) printf("%s ", uts.nodename);
	if (all || show_r) printf("%s ", uts.release);
	if (all || show_v) printf("%s ", uts.version);
	if (all || show_m) printf("%s", uts.machine);
	printf("\n");
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
   TEXT UTILITIES
   ═══════════════════════════════════════════════════════════════════ */

/* ── cat — concatenate files ── */
static int cat_main(int argc, const char **argv)
{
	if (argc < 2) {
		char c;
		while (read(0, &c, 1) > 0) putchar(c);
		return 0;
	}

	for (int i = 1; i < argc; i++) {
		char buf[256];
		usize n = bb_read_file(argv[i], buf, sizeof(buf));
		if (n == 0 && argv[i][0] != '\0') {
			printf("cat: %s: No such file or directory\n", argv[i]);
			continue;
		}
		printf("%s", buf);
	}
	return 0;
}

/* ── head — output first N lines ── */
static int head_main(int argc, const char **argv)
{
	int nlines = 10;
	const char *file = 0;
	int file_idx = 1;

	if (argc > 1 && argv[1][0] == '-') {
		nlines = atoi(argv[1] + 1);
		file_idx = 2;
	}
	if (argc > file_idx) file = argv[file_idx];

	char buf[4096];
	usize total;

	if (file) {
		total = bb_read_file(file, buf, sizeof(buf));
		if (total == 0) {
			printf("head: %s: No such file\n", file);
			return 1;
		}
	} else {
		/* Read from stdin */
		total = 0;
		while (total < sizeof(buf) - 1) {
			char c;
			if (read(0, &c, 1) <= 0) break;
			buf[total++] = c;
		}
		buf[total] = '\0';
	}

	int lines = 0;
	for (usize i = 0; i < total && lines < nlines; i++) {
		putchar(buf[i]);
		if (buf[i] == '\n') lines++;
	}
	return 0;
}

/* ── tail — output last N lines ── */
static int tail_main(int argc, const char **argv)
{
	int nlines = 10;
	const char *file = 0;
	int file_idx = 1;

	if (argc > 1 && argv[1][0] == '-') {
		nlines = atoi(argv[1] + 1);
		file_idx = 2;
	}
	if (argc > file_idx) file = argv[file_idx];

	char buf[4096];
	usize total;

	if (file) {
		total = bb_read_file(file, buf, sizeof(buf));
		if (total == 0) {
			printf("tail: %s: No such file\n", file);
			return 1;
		}
	} else {
		total = 0;
		while (total < sizeof(buf) - 1) {
			char c;
			if (read(0, &c, 1) <= 0) break;
			buf[total++] = c;
		}
		buf[total] = '\0';
	}

	/* Count total lines and find start of last N */
	int line_starts[512];
	int lc = 0;
	line_starts[lc++] = 0;
	for (usize i = 0; i < total; i++) {
		if (buf[i] == '\n' && i + 1 < total) {
			if (lc < 512) line_starts[lc++] = (int)(i + 1);
		}
	}

	int start_idx = (lc > nlines) ? line_starts[lc - nlines - 1] : 0;
	for (usize i = (usize)start_idx; i < total; i++) {
		putchar(buf[i]);
	}
	return 0;
}

/* ── grep — search for pattern in files ── */
static int grep_main(int argc, const char **argv)
{
	int quiet = 0;
	int start_idx = 1;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			if (strchr(argv[i], 'q')) quiet = 1;
			start_idx = i + 1;
		} else break;
	}

	if (argc - start_idx < 1) {
		printf("grep: missing pattern\n");
		return 1;
	}

	const char *pattern = argv[start_idx];
	const char *file = (argc - start_idx > 1) ? argv[start_idx + 1] : 0;

	char buf[4096];
	usize total = 0;
	if (file) {
		total = bb_read_file(file, buf, sizeof(buf));
		if (total == 0) return 1;
	} else {
		while (total < sizeof(buf) - 1) {
			char c;
			if (read(0, &c, 1) <= 0) break;
			buf[total++] = c;
		}
		buf[total] = '\0';
	}
	
	usize plen = strlen(pattern);
	usize line_start = 0;
	int found = 0;

	for (usize i = 0; i <= total; i++) {
		if (buf[i] == '\n' || buf[i] == '\0') {
			for (usize j = line_start; j + plen <= i; j++) {
				int match = 1;
				for (usize k = 0; k < plen; k++) {
					if (buf[j + k] != pattern[k]) { match = 0; break; }
				}
				if (match) {
					found = 1;
					if (!quiet) {
						for (usize p = line_start; p < i; p++) putchar(buf[p]);
						putchar('\n');
					}
					break;
				}
			}
			line_start = i + 1;
			if (buf[i] == '\0') break;
		}
	}
	return found ? 0 : 1;
}

/* ── find — walk directory tree ── */
static void find_recurse(const char *base, const char *name_pat, int *found)
{
	struct dirent entries[32];
	char path[256];
	usize bl = strlen(base);

	int count = (int)syscall_dispatch(SYS_READDIR, (u64)(usize)base,
	                                   (u64)(usize)entries, 32, 0);
	if (count < 0) return;

	for (int i = 0; i < count; i++) {
		if (strcmp(entries[i].name, ".") == 0 ||
		    strcmp(entries[i].name, "..") == 0) continue;

		/* Build full path */
		memcpy(path, base, bl);
		if (bl > 0 && path[bl - 1] != '/') path[bl++] = '/';
		usize nl = strlen(entries[i].name);
		memcpy(path + bl, entries[i].name, nl + 1);

		/* Check name match */
		if (!name_pat || strcmp(entries[i].name, name_pat) == 0) {
			printf("%s\n", path);
			(*found)++;
		}

		/* Recurse into directories */
		if (entries[i].is_dir) {
			find_recurse(path, name_pat, found);
		}
	}
}

static int find_main(int argc, const char **argv)
{
	const char *start = ".";
	const char *name = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
			name = argv[++i];
		} else if (argv[i][0] != '-') {
			start = argv[i];
		}
	}

	char base[256];
	bb_resolve(start, base, sizeof(base));

	int found = 0;
	find_recurse(base, name, &found);
	return 0;
}

/* ── wc — word/line/char count ── */
static int wc_main(int argc, const char **argv)
{
	const char *file = (argc > 1) ? argv[1] : 0;

	char buf[4096];
	usize total;

	if (file) {
		total = bb_read_file(file, buf, sizeof(buf));
		if (total == 0) {
			printf("wc: %s: No such file\n", file);
			return 1;
		}
	} else {
		total = 0;
		while (total < sizeof(buf) - 1) {
			char c;
			if (read(0, &c, 1) <= 0) break;
			buf[total++] = c;
		}
		buf[total] = '\0';
	}

	int lines = 0, words = 0;
	int in_word = 0;

	for (usize i = 0; i < total; i++) {
		if (buf[i] == '\n') lines++;
		if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t') {
			in_word = 0;
		} else if (!in_word) {
			in_word = 1;
			words++;
		}
	}

	printf("%d %d %d %s\n", lines, words, (int)total,
	       file ? file : "");
	return 0;
}

/* ── sort — sort lines alphabetically ── */
static void sort_swap(char **a, char **b)
{
	char *t = *a; *a = *b; *b = t;
}

static int sort_main(int argc, const char **argv)
{
	const char *file = (argc > 1) ? argv[1] : 0;

	char buf[4096];
	usize total;

	if (file) {
		total = bb_read_file(file, buf, sizeof(buf));
		if (total == 0) return 0;
	} else {
		total = 0;
		while (total < sizeof(buf) - 1) {
			char c;
			if (read(0, &c, 1) <= 0) break;
			buf[total++] = c;
		}
		buf[total] = '\0';
	}

	/* Collect line pointers */
	char *lines[256];
	int lc = 0;
	lines[lc++] = buf;
	for (usize i = 0; i < total && lc < 256; i++) {
		if (buf[i] == '\n') {
			buf[i] = '\0';
			if (i + 1 < total) lines[lc++] = &buf[i + 1];
		}
	}

	/* Bubble sort (simple, good for small data) */
	for (int i = 0; i < lc - 1; i++) {
		for (int j = 0; j < lc - 1 - i; j++) {
			if (strcmp(lines[j], lines[j + 1]) > 0) {
				sort_swap(&lines[j], &lines[j + 1]);
			}
		}
	}

	for (int i = 0; i < lc; i++) {
		printf("%s\n", lines[i]);
	}
	return 0;
}

/* ── uniq — report or omit repeated lines ── */
static int uniq_main(int argc, const char **argv)
{
	const char *file = (argc > 1) ? argv[1] : 0;

	char buf[4096];
	usize total;

	if (file) {
		total = bb_read_file(file, buf, sizeof(buf));
		if (total == 0) return 0;
	} else {
		total = 0;
		while (total < sizeof(buf) - 1) {
			char c;
			if (read(0, &c, 1) <= 0) break;
			buf[total++] = c;
		}
		buf[total] = '\0';
	}

	/* Collect line pointers */
	char *lines[256];
	int lc = 0;
	lines[lc++] = buf;
	for (usize i = 0; i < total && lc < 256; i++) {
		if (buf[i] == '\n') {
			buf[i] = '\0';
			if (i + 1 < total) lines[lc++] = &buf[i + 1];
		}
	}

	if (lc == 0) return 0;
	puts(lines[0]);
	for (int i = 1; i < lc; i++) {
		if (strcmp(lines[i], lines[i - 1]) != 0) {
			puts(lines[i]);
		}
	}
	return 0;
}

/* ── printf — formatted output ── */
static int printf_main(int argc, const char **argv)
{
	if (argc < 2) return 0;

	const char *fmt = argv[1];
	int argi = 2;

	for (int i = 0; fmt[i]; i++) {
		if (fmt[i] == '%') {
			i++;
			if (fmt[i] == 's' && argi < argc) {
				printf("%s", argv[argi++]);
			} else if (fmt[i] == 'd' && argi < argc) {
				printf("%d", atoi(argv[argi++]));
			} else if (fmt[i] == '%') {
				putchar('%');
			}
		} else if (fmt[i] == '\\') {
			i++;
			if (fmt[i] == 'n') putchar('\n');
			else if (fmt[i] == 't') putchar('\t');
			else putchar(fmt[i]);
		} else {
			putchar(fmt[i]);
		}
	}
	return 0;
}

/* ── test/[ — evaluate expression ── */
static int test_main(int argc, const char **argv)
{
	int arg_idx = 1;
	if (argc > 0 && strcmp(argv[0], "[") == 0) {
		if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) return 1;
		argc--;
	}

	if (arg_idx >= argc) return 1;

	/* -f <file> */
	if (strcmp(argv[arg_idx], "-f") == 0 && arg_idx + 1 < argc) {
		char path[256];
		bb_resolve(argv[arg_idx + 1], path, sizeof(path));
		struct b1nix_stat st;
		if (syscall_dispatch(SYS_STAT, (u64)(usize)path, (u64)(usize)&st, 0, 0) == 0) {
			return (st.st_mode & B1NIX_S_IFDIR) ? 1 : 0;
		}
		return 1;
	}

	/* -d <dir> */
	if (strcmp(argv[arg_idx], "-d") == 0 && arg_idx + 1 < argc) {
		char path[256];
		bb_resolve(argv[arg_idx + 1], path, sizeof(path));
		struct b1nix_stat st;
		if (syscall_dispatch(SYS_STAT, (u64)(usize)path, (u64)(usize)&st, 0, 0) == 0) {
			return (st.st_mode & B1NIX_S_IFDIR) ? 0 : 1;
		}
		return 1;
	}

	/* string == string */
	if (argc - arg_idx >= 3) {
		if (strcmp(argv[arg_idx + 1], "=") == 0 || strcmp(argv[arg_idx + 1], "==") == 0) {
			return strcmp(argv[arg_idx], argv[arg_idx + 2]) == 0 ? 0 : 1;
		}
		if (strcmp(argv[arg_idx + 1], "!=") == 0) {
			return strcmp(argv[arg_idx], argv[arg_idx + 2]) != 0 ? 0 : 1;
		}
	}

	/* -n string (non-empty) */
	if (strcmp(argv[arg_idx], "-n") == 0 && arg_idx + 1 < argc) {
		return (argv[arg_idx + 1][0] != '\0') ? 0 : 1;
	}

	/* -z string (empty) */
	if (strcmp(argv[arg_idx], "-z") == 0 && arg_idx + 1 < argc) {
		return (argv[arg_idx + 1][0] == '\0') ? 0 : 1;
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
   FILESYSTEM UTILITIES
   ═══════════════════════════════════════════════════════════════════ */

/* ── mount — mount filesystem ── */
static int mount_main(int argc, const char **argv)
{
	if (argc < 2) {
		struct b1nix_mount_entry entries[8];
		long count = (long)syscall_dispatch(SYS_MOUNTS, (u64)(usize)entries,
		                                    8, 0, 0);
		if (count < 0) {
			printf("mount: cannot read mount table\n");
			return 1;
		}
		for (long i = 0; i < count && i < 8; i++) {
			printf("%s on %s type %s", entries[i].source, entries[i].target,
			       entries[i].fstype);
			if (entries[i].flags) printf(" flags=%lu", (unsigned long)entries[i].flags);
			printf("\n");
		}
		if (count > 8) {
			printf("mount: %ld more mount(s) not shown\n", count - 8);
		}
		return 0;
	}

	const char *source = argv[1];
	const char *target = (argc > 2) ? argv[2] : "/mnt";
	const char *fstype = (argc > 3) ? argv[3] : "ext2";

	char tgt[256];
	bb_resolve(target, tgt, sizeof(tgt));

	int rc;
	if (strcmp(fstype, "btrfs") == 0) {
		rc = btrfs_mount_root(source, tgt);
	} else {
		rc = (int)syscall_dispatch(SYS_MOUNT, (u64)(usize)source,
		                           (u64)(usize)tgt, (u64)(usize)fstype, 0);
	}
	if (rc != 0) {
		printf("mount: cannot mount %s on %s (type %s)\n",
		       source, target, fstype);
		return 1;
	}
	return 0;
}

/* ── df — disk free ── */
static int df_main(int argc, const char **argv)
{
	(void)argc; (void)argv;

	/* Use memory info as approximation */
	syscall_dispatch(SYS_MEM, 0, 0, 0, 0);
	return 0;
}

static void print_size(u64 bytes)
{
	if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
		printf("%dG", (int)(bytes / (1024ULL * 1024ULL * 1024ULL)));
	} else if (bytes >= 1024ULL * 1024ULL) {
		printf("%dM", (int)(bytes / (1024ULL * 1024ULL)));
	} else if (bytes >= 1024ULL) {
		printf("%dK", (int)(bytes / 1024ULL));
	} else {
		printf("%dB", (int)bytes);
	}
}

static const char *mountpoint_for_source(const char *source,
                                         struct b1nix_mount_entry *entries,
                                         long count)
{
	for (long i = 0; i < count; i++) {
		if (strcmp(entries[i].source, source) == 0) {
			return entries[i].target;
		}
	}
	return "";
}

#ifndef __aarch64__
static const char *storage_subclass_name(u8 subclass)
{
	switch (subclass) {
	case 0x01: return "IDE";
	case 0x04: return "RAID";
	case 0x06: return "SATA/AHCI";
	case 0x07: return "SAS";
	case 0x08: return "NVMe";
	case 0x80: return "storage";
	default: return "storage";
	}
}

static const char *storage_vendor_name(u16 vendor)
{
	switch (vendor) {
	case 0x1002: return "AMD";
	case 0x1022: return "AMD";
	case 0x106b: return "Apple";
	case 0x10ec: return "Realtek";
	case 0x1179: return "Toshiba";
	case 0x144d: return "Samsung";
	case 0x15b7: return "SanDisk";
	case 0x1c5c: return "SK hynix";
	case 0x1d0f: return "Amazon";
	case 0x8086: return "Intel";
	default: return "unknown";
	}
}

static int print_storage_class(u8 subclass)
{
	int printed = 0;
	for (u8 idx = 0; idx < 8; idx++) {
		struct pci_device_info pci;
		if (!pci_find_class(0x01, subclass, idx, &pci)) break;
		printf("pci        -     -      -   %s %s vendor 0x%04x device 0x%04x prog_if 0x%02x\n",
		       storage_vendor_name(pci.vendor_id),
		       storage_subclass_name(pci.subclass),
		       pci.vendor_id, pci.device_id, pci.prog_if);
		printed++;
	}
	return printed;
}

static int print_storage_controllers(void)
{
	int printed = 0;
	printed += print_storage_class(0x08); /* NVMe */
	printed += print_storage_class(0x06); /* SATA/AHCI */
	printed += print_storage_class(0x04); /* RAID/RST/VMD-style */
	printed += print_storage_class(0x01); /* IDE */
	printed += print_storage_class(0x07); /* SAS */
	printed += print_storage_class(0x80); /* Other */
	return printed;
}
#endif

static int lsblk_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	struct b1nix_mount_entry mounts[16];
	long mount_count = (long)syscall_dispatch(SYS_MOUNTS, (u64)(usize)mounts, 16, 0, 0);
	if (mount_count < 0) mount_count = 0;

	printf("NAME       SIZE  BLKSZ  RO  MOUNTPOINT\n");
	usize count = blk_count();
	if (count == 0) {
		printf("(no block devices)\n");
#ifndef __aarch64__
		if (print_storage_controllers() > 0) {
			printf("note: storage controller found, but no block driver registered a disk\n");
		}
#endif
		return 0;
	}

	for (usize i = 0; i < count; i++) {
		struct block_device *dev = blk_at(i);
		if (!dev) continue;
		u64 bytes = dev->block_count * (u64)dev->block_size;
		printf("%-10s ", dev->name);
		print_size(bytes);
		printf("  %5d  %s   %s\n",
		       (int)dev->block_size,
		       dev->write_blocks ? "rw" : "ro",
		       mountpoint_for_source(dev->name, mounts, mount_count));
	}
#ifndef __aarch64__
	print_storage_controllers();
#endif
	return 0;
}

/* ── sync — flush filesystem caches ── */
static int sync_main(int argc, const char **argv)
{
	(void)argc; (void)argv;
	sync();
	printf("sync: filesystems synchronized\n");
	return 0;
}

/* ── hexdump — hex dump of file ── */
static int hexdump_main(int argc, const char **argv)
{
	if (argc < 2) {
		printf("hexdump: missing file\nUsage: hexdump <file>\n");
		return 1;
	}

	char buf[512];
	usize total = bb_read_file(argv[1], buf, sizeof(buf));
	if (total == 0) {
		printf("hexdump: %s: No such file or empty\n", argv[1]);
		return 1;
	}

	for (usize offset = 0; offset < total; offset += 16) {
		/* Offset */
		printf("%04x  ", (int)offset);

		/* Hex bytes */
		for (usize j = 0; j < 16; j++) {
			if (offset + j < total) {
				printf("%02x ", (u8)buf[offset + j]);
			} else {
				printf("   ");
			}
			if (j == 7) putchar(' ');
		}

		/* ASCII representation */
		printf(" |");
		for (usize j = 0; j < 16 && offset + j < total; j++) {
			char c = buf[offset + j];
			putchar((c >= 32 && c < 127) ? c : '.');
		}
		printf("|\n");
	}
	return 0;
}

/* ── echo — print text ── */
static int echo_main(int argc, const char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (i > 1) putchar(' ');
		printf("%s", argv[i]);
	}
	putchar('\n');
	return 0;
}

/* ── true — return success ── */
static int true_main(int argc, const char **argv)
{
	(void)argc; (void)argv;
	return 0;
}

/* ── false — return failure ── */
static int false_main(int argc, const char **argv)
{
	(void)argc; (void)argv;
	return 1;
}

/* ── yes — print string forever ── */
static int yes_main(int argc, const char **argv)
{
	const char *str = (argc > 1) ? argv[1] : "y";
	while (1) printf("%s\n", str);
	return 0;
}

/* ── sleep — delay for seconds ── */
static int sleep_main(int argc, const char **argv)
{
	if (argc < 2) {
		printf("sleep: missing operand\n");
		return 1;
	}
	int seconds = atoi(argv[1]);
	sleep(seconds);
	return 0;
}

/* ── whoami — print effective user name ── */
static int whoami_main(int argc, const char **argv)
{
	(void)argc; (void)argv;
	u64 uid = syscall_dispatch(SYS_GETUID, 0, 0, 0, 0);

	const char *names[] = {"root", "daemon", "bin", "user"};
	if (uid < 4) {
		printf("%s\n", names[uid]);
	} else {
		printf("user-%d\n", (int)uid);
	}
	return 0;
}

/* ── id — print user/group identity ── */
static int id_main(int argc, const char **argv)
{
	(void)argc; (void)argv;
	u64 uid = syscall_dispatch(SYS_GETUID, 0, 0, 0, 0);
	u64 euid = syscall_dispatch(SYS_GETEUID, 0, 0, 0, 0);
	u64 gid = syscall_dispatch(SYS_GETGID, 0, 0, 0, 0);
	u64 egid = syscall_dispatch(SYS_GETEGID, 0, 0, 0, 0);

	printf("uid=%d euid=%d gid=%d egid=%d\n",
	       (int)uid, (int)euid, (int)gid, (int)egid);
	return 0;
}

/* ── clear — clear terminal ── */
static int clear_main(int argc, const char **argv)
{
	(void)argc; (void)argv;
	syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
   NETWORK UTILITIES (M23)
   ═══════════════════════════════════════════════════════════════════ */

/* ── Helper: check if network is available ── */
static int net_is_available(void)
{
	/* Network is available if virtio-net was initialized at boot.
	   Individual operations will fail gracefully if not. */
#ifdef __aarch64__
	return 0;
#else
	return net_is_ready();
#endif
}

/* ── ifconfig — network interface configuration ── */
static int ifconfig_main(int argc, const char **argv)
{
	(void)argc; (void)argv;

	syscall_dispatch(SYS_NET_INFO, 0, 0, 0, 0);
	return net_is_available() ? 0 : 1;
}

/* ── ping — send ICMP echo requests ── */
static int ping_main(int argc, const char **argv)
{
	if (argc < 2) {
		printf("ping: missing host\nUsage: ping <ip-address>\n");
		return 1;
	}

	if (!net_is_available()) {
		printf("ping: network unavailable\n");
		return 1;
	}

	/* Use SYS_NET_PING */
	syscall_dispatch(SYS_NET_PING, (u64)(usize)argv[1], 0, 0, 0);
	return 0;
}

/* ── nc (netcat) — TCP/UDP network tool ── */
static int nc_main(int argc, const char **argv)
{
	if (argc < 3) {
		printf("nc: usage: nc <host> <port> [-u]\n");
		return 1;
	}

	const char *host = argv[1];
	const char *port_str = argv[2];
	int use_udp = 0;

	if (argc > 3 && strcmp(argv[3], "-u") == 0) use_udp = 1;

	if (!net_is_available()) {
		printf("nc: network unavailable\n");
		return 1;
	}

	u16 port = (u16)atoi(port_str);

	/* Parse IP address */
	struct b1nix_sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = B1NIX_AF_INET;
	addr.sin_port = (u16)((port >> 8) | (port << 8)); /* host to network */

	/* Parse dotted decimal IP */
	u32 ip = 0;
	int shift = 0;
	const char *p = host;
	while (*p) {
		if (*p == '.') {
			shift++;
			p++;
			continue;
		}
		u32 octet = 0;
		while (*p >= '0' && *p <= '9') {
			octet = octet * 10 + (u32)(*p - '0');
			p++;
		}
		ip |= (octet << (shift * 8));
		if (*p == '.') { shift++; p++; }
	}
	addr.sin_addr = ip;

	int sock_type = use_udp ? B1NIX_SOCK_DGRAM : B1NIX_SOCK_STREAM;
	int fd = socket(B1NIX_AF_INET, sock_type, 0);
	if (fd < 0) {
		printf("nc: socket failed\n");
		return 1;
	}

	if (connect(fd, &addr, sizeof(addr)) < 0) {
		printf("nc: connect failed\n");
		close(fd);
		return 1;
	}

	printf("nc: connected to %s:%d (%s)\n", host, port,
	       use_udp ? "udp" : "tcp");

	if (use_udp) {
		/* UDP: read stdin, send, recv response */
		char buf[1024];
		usize total = 0;
		while (total < sizeof(buf) - 1) {
			char c;
			if (read(0, &c, 1) <= 0) break;
			buf[total++] = c;
			if (c == '\n') break;
		}
		if (total > 0) {
			send(fd, buf, total, 0);
			/* Try to receive response */
			char rbuf[512];
			long n = recv(fd, rbuf, sizeof(rbuf) - 1, 0);
			if (n > 0) {
				rbuf[n] = '\0';
				printf("%s", rbuf);
			}
		}
	} else {
		/* TCP: interactive send/receive until EOF */
		while (1) {
			char c;
			if (read(0, &c, 1) <= 0) break;
			send(fd, &c, 1, 0);

			/* Check for response */
			char rbuf[256];
			long n = recv(fd, rbuf, sizeof(rbuf) - 1, 0);
			if (n > 0) {
				rbuf[n] = '\0';
				printf("%s", rbuf);
			}
		}
	}

	close(fd);
	return 0;
}

/* ── wget — simple HTTP client ── */
static int wget_main(int argc, const char **argv)
{
	if (argc < 2) {
		printf("wget: missing URL\nUsage: wget <http://host/path>\n");
		return 1;
	}

	if (!net_is_available()) {
		printf("wget: network unavailable\n");
		return 1;
	}

	const char *url = argv[1];

	/* Parse URL: http://host[:port]/path */
	const char *host_start = url;
	if (strncmp(url, "http://", 7) == 0) {
		host_start = url + 7;
	}

	/* Extract host */
	char host[128];
	int hi = 0;
	u16 port = 80;
	const char *path_start = "/";

	while (*host_start && *host_start != ':' && *host_start != '/' && hi < 127) {
		host[hi++] = *host_start++;
	}
	host[hi] = '\0';

	if (*host_start == ':') {
		host_start++;
		port = (u16)atoi(host_start);
		while (*host_start && *host_start != '/') host_start++;
	}
	if (*host_start == '/') path_start = host_start;
	if (path_start[0] == '\0') path_start = "/";

	/* Resolve hostname via DNS */
	printf("wget: resolving %s...\n", host);
	syscall_dispatch(SYS_NET_DNS, (u64)(usize)host, 0, 0, 0);

	/* DNS resolution is async via console. For now use QEMU user-mode
	   networking default gateway (10.0.2.2) as HTTP proxy */
	u32 server_ip_raw = (10) | (0 << 8) | (2 << 16) | (2 << 24);

	/* Build HTTP request */
	char request[1024];
	int req_len = snprintf(request, sizeof(request),
		"GET %s HTTP/1.0\r\n"
		"Host: %s\r\n"
		"User-Agent: b1nix-wget/0.1\r\n"
		"Accept: */*\r\n"
		"Connection: close\r\n"
		"\r\n",
		path_start, host);

	/* Parse IP for socket */
	struct b1nix_sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = B1NIX_AF_INET;
	addr.sin_port = (u16)((port >> 8) | (port << 8)); /* host to network */
	addr.sin_addr = server_ip_raw;

	int fd = socket(B1NIX_AF_INET, B1NIX_SOCK_STREAM, 0);
	if (fd < 0) {
		printf("wget: socket failed\n");
		return 1;
	}

	printf("wget: connecting to %s:%d...\n", host, port);
	if (connect(fd, &addr, sizeof(addr)) < 0) {
		printf("wget: connect failed\n");
		close(fd);
		return 1;
	}

	printf("wget: sending request...\n");
	send(fd, request, req_len, 0);

	/* Receive response */
	printf("wget: receiving response...\n");
	char buf[512];
	int total = 0;
	int header_done = 0;

	while (1) {
		long n = recv(fd, buf, sizeof(buf) - 1, 0);
		if (n <= 0) break;
		buf[n] = '\0';

		if (!header_done) {
			/* Find end of headers */
			char *body = strstr(buf, "\r\n\r\n");
			if (body) {
				header_done = 1;
				body += 4;
				printf("%s", body);
				total += (int)strlen(body);
			}
		} else {
			printf("%s", buf);
			total += (int)n;
		}
	}

	printf("\nwget: received %d bytes\n", total);
	close(fd);
	return 0;
}

/* ── dmesg — print kernel log buffer ── */
static int dmesg_main(int argc, const char **argv)
{
	(void)argc; (void)argv;

	char buf[2048];
	long n = (long)syscall_dispatch(SYS_DMESG, (u64)(usize)buf, sizeof(buf), 0, 0);
	if (n < 0) {
		printf("dmesg: error reading kernel log\n");
		return 1;
	}
	if (n > 0) {
		printf("%s", buf);
		if (buf[n - 1] != '\n') putchar('\n');
	}
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
   BUSYBOX DISPATCHER
   ═══════════════════════════════════════════════════════════════════ */

struct bb_app {
	const char *name;
	int (*main)(int argc, const char **argv);
};

static struct bb_app bb_apps[] = {
	/* File/directory */
	{"pwd",     pwd_main},
	{"ls",      ls_main},
	{"cp",      cp_main},
	{"mv",      mv_main},
	{"rm",      rm_main},
	{"mkdir",   mkdir_main},
	{"rmdir",   rmdir_main},
	{"chmod",   chmod_main},
	{"chown",   chown_main},
	{"ln",      ln_main},
	{"readlink", readlink_main},
	{"touch",    touch_main},
	{"basename", basename_main},
	{"dirname",  dirname_main},
	/* System */
	{"ps",      ps_main},
	{"kill",    kill_main},
	{"date",    date_main},
	{"uname",   uname_main},
	/* Text */
	{"cat",     cat_main},
	{"echo",    echo_main},
	{"printf",  printf_main},
	{"head",    head_main},
	{"tail",    tail_main},
	{"grep",    grep_main},
	{"find",    find_main},
	{"wc",      wc_main},
	{"sort",    sort_main},
	{"uniq",    uniq_main},
	/* Filesystem */
	{"mount",   mount_main},
	{"df",      df_main},
	{"lsblk",   lsblk_main},
	{"sync",    sync_main},
	{"hexdump", hexdump_main},
	/* Network (M23) */
	{"ifconfig", ifconfig_main},
	{"ping",     ping_main},
	{"nc",       nc_main},
	{"wget",     wget_main},
	/* Diagnostics (M24) */
	{"dmesg",    dmesg_main},
	/* Misc */
	{"true",    true_main},
	{"false",   false_main},
	{"test",    test_main},
	{"[",       test_main},
	{"yes",     yes_main},
	{"sleep",   sleep_main},
	{"whoami",  whoami_main},
	{"id",      id_main},
	{"clear",   clear_main},
	{0, 0},
};

int busybox_main(int argc, const char **argv)
{
	const char *invoked = argv && argc > 0 ? argv[0] : "";
	const char *base = strrchr(invoked, '/');
	if (base) invoked = base + 1;

	if (invoked && invoked[0] != '\0' && strcmp(invoked, "busybox") != 0) {
		for (struct bb_app *app = bb_apps; app->name; app++) {
			if (strcmp(app->name, invoked) == 0) {
				return app->main(argc, argv);
			}
		}
		printf("busybox: %s: applet not found\n", invoked);
		return 1;
	}

	if (argc < 2) {
		printf("BusyBox v1.0 (b1nix) multi-call binary\n");
		printf("Usage: busybox [command] [arguments...]\n");
		printf("\nCurrently defined functions:\n");
		for (struct bb_app *app = bb_apps; app->name; app++) {
			printf("  %s\n", app->name);
		}
		return 0;
	}

	const char *cmd = argv[1];
	for (struct bb_app *app = bb_apps; app->name; app++) {
		if (strcmp(app->name, cmd) == 0) {
			return app->main(argc - 1, argv + 1);
		}
	}
	printf("busybox: %s: applet not found\n", cmd);
	return 1;
}
