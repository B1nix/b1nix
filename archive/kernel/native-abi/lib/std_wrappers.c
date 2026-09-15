/*
 * ARCHIVED — not built. The kernel libc calls that went through native
 * syscalls and had no caller left (see ../README.md).
 */

/* ── kernel/lib/stdlib.c ── */
void abort(void)
{
	/* Print message then exit */
	const char *msg = "abort() called\n";
	syscall_dispatch(SYS_WRITE, (u64)(usize)msg, strlen(msg), 0, 0, 0, 0);
	syscall_dispatch(SYS_EXIT, 1, 0, 0, 0, 0, 0);
	while (1);
}

/* ── kernel/lib/stdlib.c ── */
void exit(int status)
{
	syscall_dispatch(SYS_EXIT, (u64)status, 0, 0, 0, 0, 0);
	while (1);
}

/* ── kernel/lib/stdio.c ── */
int putchar(int c)
{
	char ch = (char)c;
	syscall_dispatch(SYS_WRITE, 1, (u64)(usize)&ch, 1, 0, 0, 0);
	return c;
}

/* ── kernel/lib/stdio.c ── */
int puts(const char *s)
{
	syscall_dispatch(SYS_WRITE, 1, (u64)(usize)s, strlen(s), 0, 0, 0);
	putchar('\n');
	return 0;
}

/* ── kernel/lib/stdio.c ── */
int printf(const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	int len = vsnprintf_impl(buf, sizeof(buf), fmt, args);
	va_end(args);
	syscall_dispatch(SYS_WRITE, 1, (u64)(usize)buf, len, 0, 0, 0);
	return len;
}
