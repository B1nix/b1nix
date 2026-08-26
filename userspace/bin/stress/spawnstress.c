/* Process creation, execution and reaping, at speed and from many threads.
 *
 * Forking from a multithreaded process is the hard case and the one the
 * browser does constantly: the child gets one thread and a full copy of an
 * address space whose locks may have been held by threads that no longer
 * exist. So the children here do real work — they verify a mapping they
 * inherited — rather than exiting immediately, and their exit status is what
 * says whether the copy was sound.
 *
 * Reaping is checked as strictly as spawning. An orphan whose parent has gone
 * must be adopted and collected by PID 1, and a status that never arrives is a
 * process table entry that is never freed.
 */
#include "stress.h"
#include <pthread.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>

#define INHERIT_LEN (64 * 4096)

static unsigned long long g_deadline;
static volatile int g_fail;
static unsigned long long g_forks, g_execs;

static unsigned char *g_inherited;

static void *worker(void *arg)
{
	unsigned long tid = (unsigned long)(uintptr_t)arg;
	long rounds = scaled(120);

	for (long i = 0; i < rounds && !g_fail; i++) {
		if (now_ms() > g_deadline)
			break;

		pid_t pid = fork();
		if (pid < 0) {
			if (errno == EAGAIN || errno == ENOMEM) {
				usleep(1000);
				continue;
			}
			fprintf(stderr, "SPAWNSTRESS: FAIL fork: %s\n", strerror(errno));
			g_fail = 1;
			break;
		}
		if (pid == 0) {
			/* The inherited mapping must still hold what the parent put in it,
			 * and writing to it must not reach the parent. */
			size_t bad = pat_check(g_inherited, 0xA11CE, INHERIT_LEN);
			if (bad != (size_t)-1)
				_exit(2);
			pat_fill(g_inherited, 0xB0B, INHERIT_LEN);
			if (pat_check(g_inherited, 0xB0B, INHERIT_LEN) != (size_t)-1)
				_exit(3);
			_exit((int)(tid % 7) + 10);
		}

		__atomic_add_fetch(&g_forks, 1, __ATOMIC_RELAXED);
		int st = 0;
		if (waitpid(pid, &st, 0) != pid) {
			fprintf(stderr, "SPAWNSTRESS: FAIL waitpid(%d): %s\n", (int)pid, strerror(errno));
			g_fail = 1;
			break;
		}
		if (!WIFEXITED(st)) {
			fprintf(stderr, "SPAWNSTRESS: FAIL child %d did not exit normally (status 0x%x)\n",
			        (int)pid, st);
			g_fail = 1;
			break;
		}
		int want = (int)(tid % 7) + 10;
		if (WEXITSTATUS(st) != want) {
			fprintf(stderr, "SPAWNSTRESS: FAIL child %d exited %d, expected %d%s\n",
			        (int)pid, WEXITSTATUS(st), want,
			        WEXITSTATUS(st) == 2 ? " (inherited mapping was wrong)" :
			        WEXITSTATUS(st) == 3 ? " (private write did not take)" : "");
			g_fail = 1;
			break;
		}

		/* Every few rounds, replace the child with another program: fork alone
		 * never exercises the loader. */
		if ((i % 8) == 0) {
			pid_t e = fork();
			if (e == 0) {
				execl("/bin/true", "true", (char *)0);
				execl("/bin/busybox", "busybox", "true", (char *)0);
				_exit(127);
			}
			if (e > 0) {
				int est = 0;
				waitpid(e, &est, 0);
				__atomic_add_fetch(&g_execs, 1, __ATOMIC_RELAXED);
				if (WIFEXITED(est) && WEXITSTATUS(est) == 127) {
					fprintf(stderr, "SPAWNSTRESS: FAIL exec found no /bin/true\n");
					g_fail = 1;
					break;
				}
				if (!WIFEXITED(est) || WEXITSTATUS(est) != 0) {
					fprintf(stderr, "SPAWNSTRESS: FAIL exec'd child status 0x%x\n", est);
					g_fail = 1;
					break;
				}
			}
		}
	}
	return 0;
}

/* A grandchild whose parent exits first. PID 1 must adopt and reap it; the
 * evidence available to us is that the intermediate child returns at once and
 * the grandchild's own marker file appears. */
static int orphan_round(void)
{
	char path[128];
	snprintf(path, sizeof(path), "/tmp/soak-orphan-%d", (int)getpid());
	unlink(path);

	pid_t mid = fork();
	if (mid < 0)
		return 0;
	if (mid == 0) {
		pid_t g = fork();
		if (g == 0) {
			usleep(50000);
			int fd = open(path, O_CREAT | O_WRONLY, 0644);
			if (fd >= 0) {
				write(fd, "x", 1);
				close(fd);
			}
			_exit(0);
		}
		_exit(0);
	}
	int st = 0;
	waitpid(mid, &st, 0);

	for (int i = 0; i < 100; i++) {
		if (access(path, F_OK) == 0) {
			unlink(path);
			return 0;
		}
		usleep(20000);
	}
	fprintf(stderr, "SPAWNSTRESS: FAIL orphaned grandchild never ran\n");
	return -1;
}

int main(void)
{
	int threads = (int)env_long("SOAK_THREADS", sysconf(_SC_NPROCESSORS_ONLN));
	if (threads < 1)
		threads = 1;
	if (threads > 16)
		threads = 16;
	g_deadline = now_ms() + budget_ms();

	g_inherited = mmap(0, INHERIT_LEN, PROT_READ | PROT_WRITE,
	                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (g_inherited == MAP_FAILED) {
		fprintf(stderr, "SPAWNSTRESS: FAIL mmap: %s\n", strerror(errno));
		return 1;
	}
	pat_fill(g_inherited, 0xA11CE, INHERIT_LEN);

	printf("SPAWNSTRESS: start %d threads\n", threads);
	fflush(stdout);
	unsigned long long t0 = now_ms();

	pthread_t th[16];
	int made = 0;
	for (int i = 0; i < threads; i++) {
		if (pthread_create(&th[i], 0, worker, (void *)(uintptr_t)i) != 0) {
			fprintf(stderr, "SPAWNSTRESS: FAIL pthread_create %d\n", i);
			g_fail = 1;
			break;
		}
		made++;
	}
	for (int i = 0; i < made; i++)
		pthread_join(th[i], 0);

	if (!g_fail && orphan_round() != 0)
		g_fail = 1;

	/* The parent's own copy must have survived every child's writes. */
	if (!g_fail && pat_check(g_inherited, 0xA11CE, INHERIT_LEN) != (size_t)-1) {
		fprintf(stderr, "SPAWNSTRESS: FAIL a child's write reached the parent\n");
		g_fail = 1;
	}

	unsigned long long ms = now_ms() - t0;
	if (g_fail) {
		printf("SPAWNSTRESS: FAIL after %llu forks, %llu execs, ms=%llu\n", g_forks, g_execs, ms);
		return 1;
	}
	printf("SPAWNSTRESS: ok %llu forks, %llu execs, %d threads, ms=%llu\n",
	       g_forks, g_execs, made, ms);
	return 0;
}
