/* When does a pipe read end?
 *
 * A browser thread was found blocked in pipe_read while every other thread of
 * the process waited on a futex — the shape of one thread holding everything
 * up. A read that should have ended and did not is one way to get there: if the
 * last write end is gone, read(2) must return 0, not block forever. The cases
 * differ in who held that write end and how it went away, and inheritance
 * across fork/exec is the one a multi-process program exercises constantly.
 *
 * Each case bounds itself with alarm(), so a kernel that blocks where it should
 * report EOF fails the test instead of hanging the run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

static volatile int timed_out;

static void on_alarm(int sig)
{
	(void)sig;
	timed_out = 1;
}

static void arm(unsigned sec)
{
	timed_out = 0;
	signal(SIGALRM, on_alarm);
	alarm(sec);
}

static void disarm(void)
{
	alarm(0);
}

/* The plain case: the only write end is closed by this process. */
static int case_self_close(void)
{
	int fd[2];
	char buf[16];
	int n;

	if (pipe(fd) != 0)
		return printf("PIPE-EOF: FAIL self-close (pipe errno %d)\n", errno), 1;

	close(fd[1]);
	arm(5);
	n = (int)read(fd[0], buf, sizeof(buf));
	disarm();
	close(fd[0]);

	if (timed_out || n != 0)
		return printf("PIPE-EOF: FAIL self-close (read returned %d, errno %d%s)\n",
		              n, errno, timed_out ? ", blocked" : ""), 1;

	printf("PIPE-EOF: ok self-close\n");
	return 0;
}

/* The write end lives on in a child, which then exits. The reader must see the
 * end only once BOTH copies are gone — and must see it then. */
static int case_child_exits(void)
{
	int fd[2];
	char buf[16];
	pid_t pid;
	int n, status = 0;

	if (pipe(fd) != 0)
		return printf("PIPE-EOF: FAIL child-exit (pipe errno %d)\n", errno), 1;

	pid = fork();
	if (pid < 0)
		return printf("PIPE-EOF: FAIL child-exit (fork errno %d)\n", errno), 1;
	if (pid == 0) {
		close(fd[0]);
		/* Hold it briefly, so the parent is already blocked in read when the
		 * last write end disappears: the wake has to come from the close, not
		 * from the read finding it already gone. */
		usleep(200 * 1000);
		_exit(0); /* exit, without closing explicitly: teardown must do it */
	}

	close(fd[1]); /* the parent's own copy */

	arm(5);
	n = (int)read(fd[0], buf, sizeof(buf));
	disarm();
	waitpid(pid, &status, 0);
	close(fd[0]);

	if (timed_out || n != 0)
		return printf("PIPE-EOF: FAIL child-exit (read returned %d, errno %d%s)\n",
		              n, errno, timed_out ? ", blocked" : ""), 1;

	printf("PIPE-EOF: ok child-exit\n");
	return 0;
}

/* The same, but the child replaces its image first: the write end has to
 * survive exec and still be released when that image exits. This is what a
 * program launching helpers relies on. */
static int case_child_execs(void)
{
	int fd[2];
	char buf[16];
	pid_t pid;
	int n, status = 0;

	if (pipe(fd) != 0)
		return printf("PIPE-EOF: FAIL child-exec (pipe errno %d)\n", errno), 1;

	pid = fork();
	if (pid < 0)
		return printf("PIPE-EOF: FAIL child-exec (fork errno %d)\n", errno), 1;
	if (pid == 0) {
		close(fd[0]);
		/* /bin/true neither reads nor writes it; it simply exits, which is the
		 * point — the descriptor is released by process teardown after exec. */
		execl("/bin/true", "true", (char *)0);
		_exit(127);
	}

	close(fd[1]);

	arm(5);
	n = (int)read(fd[0], buf, sizeof(buf));
	disarm();
	waitpid(pid, &status, 0);
	close(fd[0]);

	if (timed_out || n != 0)
		return printf("PIPE-EOF: FAIL child-exec (read returned %d, errno %d%s)\n",
		              n, errno, timed_out ? ", blocked" : ""), 1;

	printf("PIPE-EOF: ok child-exec\n");
	return 0;
}

/* Data first, end after: a read already blocked must be woken by the write,
 * and the following read must report the end rather than block again. */
static int case_data_then_eof(void)
{
	int fd[2];
	char buf[16];
	pid_t pid;
	int n, status = 0;

	if (pipe(fd) != 0)
		return printf("PIPE-EOF: FAIL data-then-eof (pipe errno %d)\n", errno), 1;

	pid = fork();
	if (pid < 0)
		return printf("PIPE-EOF: FAIL data-then-eof (fork errno %d)\n", errno), 1;
	if (pid == 0) {
		close(fd[0]);
		usleep(150 * 1000);
		if (write(fd[1], "abc", 3) != 3)
			_exit(1);
		usleep(150 * 1000);
		_exit(0);
	}

	close(fd[1]);

	arm(5);
	n = (int)read(fd[0], buf, sizeof(buf));
	if (n != 3) {
		disarm();
		waitpid(pid, &status, 0);
		return printf("PIPE-EOF: FAIL data-then-eof (first read %d, errno %d%s)\n",
		              n, errno, timed_out ? ", blocked" : ""), 1;
	}
	n = (int)read(fd[0], buf, sizeof(buf));
	disarm();
	waitpid(pid, &status, 0);
	close(fd[0]);

	if (timed_out || n != 0)
		return printf("PIPE-EOF: FAIL data-then-eof (second read %d, errno %d%s)\n",
		              n, errno, timed_out ? ", blocked" : ""), 1;

	printf("PIPE-EOF: ok data-then-eof\n");
	return 0;
}

int main(void)
{
	int bad = 0;

	printf("PIPE-EOF: start\n");
	bad += case_self_close();
	bad += case_child_exits();
	bad += case_child_execs();
	bad += case_data_then_eof();
	printf("PIPE-EOF: done (%d failed)\n", bad);
	return bad ? 1 : 0;
}
