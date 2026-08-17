/* Does /proc/self/fd tell a process the truth about its own descriptors?
 *
 * A multi-process program closes what it does not want a helper to inherit by
 * enumerating /proc/self/fd after forking and closing everything outside its
 * mapping. That makes the listing load-bearing: if it omits a descriptor the
 * process holds, the descriptor survives into the helper; if it names one the
 * process does not hold, the close hits an unrelated number; and if the child
 * is shown its PARENT's table, both happen at once. Chromium's zygote failed to
 * answer its parent right after this step, so the listing is worth checking on
 * its own.
 *
 * Checked in the child, where it matters, and across exec, since a helper is a
 * fresh image of the same binary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>

/* Collect the descriptor numbers the listing reports, excluding the one the
 * enumeration itself is using. */
static int list_fds(int *out, int max, int skip)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	int n = 0;

	if (!d)
		return -1;
	while ((e = readdir(d)) != 0 && n < max) {
		int fd;
		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		fd = atoi(e->d_name);
		if (fd == dirfd(d) || fd == skip)
			continue;
		out[n++] = fd;
	}
	closedir(d);
	return n;
}

static int has(const int *v, int n, int want)
{
	for (int i = 0; i < n; i++)
		if (v[i] == want)
			return 1;
	return 0;
}

static int report(const char *what, const int *v, int n, int expect_hi,
                  int forbid)
{
	int bad = 0;

	if (n < 0) {
		printf("FD-VIS: FAIL %s (cannot read /proc/self/fd, errno %d)\n", what,
		       errno);
		return 1;
	}
	if (!has(v, n, 0) || !has(v, n, 1) || !has(v, n, 2)) {
		printf("FD-VIS: FAIL %s (standard descriptors missing from listing)\n",
		       what);
		bad = 1;
	}
	if (expect_hi >= 0 && !has(v, n, expect_hi)) {
		printf("FD-VIS: FAIL %s (holds %d, listing does not show it)\n", what,
		       expect_hi);
		bad = 1;
	}
	if (forbid >= 0 && has(v, n, forbid)) {
		printf("FD-VIS: FAIL %s (listing shows %d, which was closed)\n", what,
		       forbid);
		bad = 1;
	}
	if (!bad)
		printf("FD-VIS: ok %s (%d descriptors)\n", what, n);
	return bad;
}

int main(int argc, char **argv)
{
	int v[64];
	int n;
	int bad = 0;

	/* The exec'd half: it was handed one descriptor at a known number and had
	 * another closed before exec. Both must be reflected. */
	if (argc > 1 && strcmp(argv[1], "execd") == 0) {
		setvbuf(stdout, 0, _IONBF, 0);
		n = list_fds(v, 64, -1);
		return report("after-exec", v, n, 7, 6);
	}

	/* Unbuffered, because half of what this test says is said by a child that
	 * either replaces its image or exits outright, and both discard a buffer
	 * that has not been flushed. The first version of this test printed
	 * nothing from the child at all and looked like a child that had crashed. */
	setvbuf(stdout, 0, _IONBF, 0);

	printf("FD-VIS: start\n");

	{
		int keep = open("/dev/null", O_RDONLY);
		int drop = open("/dev/null", O_RDONLY);
		pid_t pid;
		int status = 0;

		if (keep < 0 || drop < 0) {
			printf("FD-VIS: FAIL setup (open errno %d)\n", errno);
			return 1;
		}

		/* The parent's own view, as a baseline. */
		n = list_fds(v, 64, -1);
		bad += report("parent", v, n, keep, -1);

		pid = fork();
		if (pid < 0) {
			printf("FD-VIS: FAIL fork (errno %d)\n", errno);
			return 1;
		}
		if (pid == 0) {
			/* The child closes one descriptor and moves the other to a fixed
			 * number, exactly as a launcher does before handing over. */
			close(drop);
			if (keep != 7) {
				dup2(keep, 7);
				close(keep);
			}
			n = list_fds(v, 64, -1);
			if (report("in-child", v, n, 7, drop) != 0)
				_exit(1);
			fflush(stdout);
			execl(argv[0], argv[0], "execd", (char *)0);
			printf("FD-VIS: FAIL exec (errno %d)\n", errno);
			fflush(stdout);
			_exit(127);
		}

		waitpid(pid, &status, 0);
		if (status != 0)
			bad++;
		close(keep);
		close(drop);
	}

	printf("FD-VIS: done (%d failed)\n", bad);
	return bad ? 1 : 0;
}
