/* Shared memory between processes, checked from both ends.
 *
 * A mapping that is meant to be shared and is quietly private looks perfect
 * from inside one process: every write reads back correctly, and only the
 * other end knows the data never arrived. That fault shipped once already —
 * an in-memory file was mapped privately per process and a compositor's
 * clients drew into nothing — so both directions are verified here, over both
 * of the ways this system creates shared memory.
 *
 *   memfd_create + mmap(MAP_SHARED)  — what Wayland clients and Chromium use.
 *   shm_open under /dev/shm          — the POSIX spelling of the same thing.
 *
 * The two processes take turns rather than racing: a race would make a missing
 * write indistinguishable from a late one.
 */
#include "stress.h"
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>

#define REGION (256 * 4096)

/* A tiny handshake in the first cache line of its own shared page, so the
 * region under test carries only the pattern. */
struct sync_page {
	volatile int turn;   /* 0 = parent writes, 1 = child writes */
	volatile int child_bad;
	volatile size_t child_bad_off;
};

static int wait_turn(volatile int *turn, int want, int ms)
{
	unsigned long long end = now_ms() + (unsigned long long)ms;
	while (*turn != want) {
		if (now_ms() > end)
			return -1;
		usleep(200);
	}
	return 0;
}

static int run_pair(int fd, const char *what)
{
	if (ftruncate(fd, REGION + 4096) != 0) {
		fprintf(stderr, "SHMSTRESS: FAIL ftruncate %s: %s\n", what, strerror(errno));
		return -1;
	}
	unsigned char *base = mmap(0, REGION + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		fprintf(stderr, "SHMSTRESS: FAIL mmap %s: %s\n", what, strerror(errno));
		return -1;
	}
	struct sync_page *sp = (struct sync_page *)base;
	unsigned char *region = base + 4096;
	sp->turn = 0;
	sp->child_bad = 0;

	long rounds = scaled(12);
	if (rounds > 64)
		rounds = 64;

	pid_t pid = fork();
	if (pid < 0) {
		munmap(base, REGION + 4096);
		fprintf(stderr, "SHMSTRESS: FAIL fork: %s\n", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		for (long r = 0; r < rounds; r++) {
			if (wait_turn(&sp->turn, 1, 10000) != 0)
				_exit(4);
			size_t bad = pat_check(region, (unsigned long)r * 2, REGION);
			if (bad != (size_t)-1) {
				sp->child_bad = 1;
				sp->child_bad_off = bad;
				sp->turn = 0;
				_exit(5);
			}
			pat_fill(region, (unsigned long)r * 2 + 1, REGION);
			sp->turn = 0;
		}
		_exit(0);
	}

	int rc = 0;
	for (long r = 0; r < rounds; r++) {
		pat_fill(region, (unsigned long)r * 2, REGION);
		sp->turn = 1;
		if (wait_turn(&sp->turn, 0, 10000) != 0) {
			fprintf(stderr, "SHMSTRESS: FAIL %s: the other process never answered in round %ld\n", what, r);
			rc = -1;
			break;
		}
		if (sp->child_bad) {
			fprintf(stderr, "SHMSTRESS: FAIL %s: child saw the wrong bytes at %zu in round %ld"
			                " (the mapping is not shared parent->child)\n",
			        what, sp->child_bad_off, r);
			rc = -1;
			break;
		}
		size_t bad = pat_check(region, (unsigned long)r * 2 + 1, REGION);
		if (bad != (size_t)-1) {
			fprintf(stderr, "SHMSTRESS: FAIL %s: parent saw 0x%02x want 0x%02x at %zu in round %ld"
			                " (the mapping is not shared child->parent)\n",
			        what, region[bad], pat((unsigned long)r * 2 + 1, bad), bad, r);
			rc = -1;
			break;
		}
	}

	if (rc != 0)
		kill(pid, SIGKILL);
	int st = 0;
	waitpid(pid, &st, 0);
	if (rc == 0 && (!WIFEXITED(st) || WEXITSTATUS(st) != 0)) {
		fprintf(stderr, "SHMSTRESS: FAIL %s: child status 0x%x\n", what, st);
		rc = -1;
	}
	munmap(base, REGION + 4096);
	return rc;
}

int main(void)
{
	unsigned long long t0 = now_ms();
	printf("SHMSTRESS: start\n");
	fflush(stdout);

	int fd = memfd_create("shmstress", 0);
	if (fd < 0) {
		fprintf(stderr, "SHMSTRESS: FAIL memfd_create: %s\n", strerror(errno));
		return 1;
	}
	int rc = run_pair(fd, "memfd");
	close(fd);
	if (rc != 0) {
		printf("SHMSTRESS: FAIL ms=%llu\n", now_ms() - t0);
		return 1;
	}

	char name[64];
	snprintf(name, sizeof(name), "/soak-%d", (int)getpid());
	fd = shm_open(name, O_CREAT | O_RDWR, 0600);
	if (fd < 0) {
		/* Not every image mounts /dev/shm; that is a configuration fact, not a
		 * kernel failure, and it is said out loud rather than passed over. */
		printf("SHMSTRESS: skip shm_open (%s)\n", strerror(errno));
	} else {
		rc = run_pair(fd, "shm_open");
		close(fd);
		shm_unlink(name);
		if (rc != 0) {
			printf("SHMSTRESS: FAIL ms=%llu\n", now_ms() - t0);
			return 1;
		}
	}

	printf("SHMSTRESS: ok both directions shared, ms=%llu\n", now_ms() - t0);
	return 0;
}
