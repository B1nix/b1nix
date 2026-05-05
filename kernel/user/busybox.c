#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <b1nix/syscall.h>

/* ── Utility: cat — concatenate files ── */
static int cat_main(int argc, const char **argv)
{
	if (argc < 2) {
		/* Read from stdin (use console as fallback for now) */
		char c;
		while (read(0, &c, 1) > 0) {
			putchar(c);
		}
		return 0;
	}
	
	for (int i = 1; i < argc; i++) {
		u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)argv[i], 0, 0, 0);
		if (fd == (u64)-1) {
			printf("cat: %s: No such file or directory\n", argv[i]);
			continue;
		}
		
		char buf[256];
		while (1) {
			u64 n = syscall_dispatch(SYS_READ, fd, (u64)(usize)buf, sizeof(buf) - 1, 0);
			if (n == 0 || n == (u64)-1) break;
			buf[n] = '\0';
			printf("%s", buf);
		}
		syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
	}
	return 0;
}

/* ── Utility: echo — print text ── */
static int echo_main(int argc, const char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (i > 1) putchar(' ');
		printf("%s", argv[i]);
	}
	putchar('\n');
	return 0;
}

/* ── Utility: true — return success ── */
static int true_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

/* ── Utility: false — return failure ── */
static int false_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	return 1;
}

/* ── Utility: yes — print y forever ── */
static int yes_main(int argc, const char **argv)
{
	const char *str = (argc > 1) ? argv[1] : "y";
	while (1) {
		printf("%s\n", str);
	}
	return 0;
}

/* ── Utility: sleep — delay for seconds ── */
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

/* ── Utility: whoami — print effective user name ── */
static int whoami_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	u64 uid = syscall_dispatch(SYS_GETUID, 0, 0, 0, 0);
	
	const char *names[] = {"root", "daemon", "bin", "user"};
	if (uid < 4) {
		printf("%s\n", names[uid]);
	} else {
		printf("user-%d\n", (int)uid);
	}
	return 0;
}

/* ── Utility: id — print user/group identity ── */
static int id_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	u64 uid = syscall_dispatch(SYS_GETUID, 0, 0, 0, 0);
	u64 euid = syscall_dispatch(SYS_GETEUID, 0, 0, 0, 0);
	u64 gid = syscall_dispatch(SYS_GETGID, 0, 0, 0, 0);
	u64 egid = syscall_dispatch(SYS_GETEGID, 0, 0, 0, 0);
	
	printf("uid=%d euid=%d gid=%d egid=%d\n", (int)uid, (int)euid, (int)gid, (int)egid);
	return 0;
}

/* ── Utility: clear — clear terminal ── */
static int clear_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0);
	return 0;
}

/* ── BusyBox-style dispatcher ── */
struct bb_app {
	const char *name;
	int (*main)(int argc, const char **argv);
};

static struct bb_app bb_apps[] = {
	{"cat",     cat_main},
	{"echo",    echo_main},
	{"true",    true_main},
	{"false",   false_main},
	{"yes",     yes_main},
	{"sleep",   sleep_main},
	{"whoami",  whoami_main},
	{"id",      id_main},
	{"clear",   clear_main},
	{0, 0},
};

int busybox_main(int argc, const char **argv)
{
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
	
	/* Check symlink-style invocation (argv[0] == command name) */
	for (struct bb_app *app = bb_apps; app->name; app++) {
		if (strcmp(app->name, cmd) == 0) {
			return app->main(argc - 1, argv + 1);
		}
	}

	printf("busybox: %s: applet not found\n", cmd);
	return 1;
}
