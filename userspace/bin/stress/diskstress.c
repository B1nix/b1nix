/* Filesystem and block-cache churn, with everything read back.
 *
 * The write path is the one place where a bug survives a reboot, so every file
 * written here is fsynced, read back and compared; the pattern is derived from
 * the file's own index, so a file holding another file's bytes is reported as
 * such rather than as "wrong data".
 *
 * The directory half is deliberate too. A readdir cursor that treated its
 * resume cookie as an upper bound once listed exactly one entry per directory,
 * and every caller that asked "is it empty" got a plausible answer — so a
 * directory is filled with a known number of entries and the count that comes
 * back out is checked, not merely that the walk terminated.
 */
#include "stress.h"
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>

static const char *g_dir = "/tmp/soak-disk";
static unsigned long long g_deadline;
static unsigned long long g_bytes;

static int fail(const char *what)
{
	fprintf(stderr, "DISKSTRESS: FAIL %s: %s\n", what, strerror(errno));
	return -1;
}

/* One file: written in pieces, flushed, closed, reopened and verified. */
static int file_round(unsigned long idx, size_t len, unsigned char *buf, unsigned char *back)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/f%lu.bin", g_dir, idx);

	pat_fill(buf, idx, len);

	int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0)
		return fail("open for write");

	size_t off = 0;
	while (off < len) {
		size_t chunk = len - off;
		if (chunk > 8192)
			chunk = 8192;
		ssize_t n = write(fd, buf + off, chunk);
		if (n < 0) {
			close(fd);
			return fail("write");
		}
		off += (size_t)n;
	}
	if (fsync(fd) != 0) {
		close(fd);
		return fail("fsync");
	}
	close(fd);
	g_bytes += len;

	struct stat st;
	if (stat(path, &st) != 0)
		return fail("stat");
	if ((size_t)st.st_size != len) {
		fprintf(stderr, "DISKSTRESS: FAIL %s size %lld, wrote %zu\n",
		        path, (long long)st.st_size, len);
		return -1;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return fail("open for read");
	off = 0;
	while (off < len) {
		ssize_t n = read(fd, back + off, len - off);
		if (n < 0) {
			close(fd);
			return fail("read");
		}
		if (n == 0)
			break;
		off += (size_t)n;
	}
	close(fd);
	if (off != len) {
		fprintf(stderr, "DISKSTRESS: FAIL %s read back %zu of %zu bytes\n", path, off, len);
		return -1;
	}
	size_t bad = pat_check(back, idx, len);
	if (bad != (size_t)-1) {
		/* Say whether the wrong byte belongs to a neighbouring file: that
		 * separates a block handed to two inodes from data that was simply
		 * never written. */
		const char *whose = "unknown";
		for (unsigned long o = idx > 4 ? idx - 4 : 0; o < idx + 4; o++) {
			if (o != idx && back[bad] == pat(o, bad)) {
				whose = "a neighbouring file";
				break;
			}
		}
		fprintf(stderr, "DISKSTRESS: FAIL %s off %zu: 0x%02x want 0x%02x (%s)\n",
		        path, bad, back[bad], pat(idx, bad), whose);
		return -1;
	}

	/* Rename, then unlink through the new name: two directory writes, and the
	 * old name must be gone. */
	char path2[256];
	snprintf(path2, sizeof(path2), "%s/f%lu.done", g_dir, idx);
	if (rename(path, path2) != 0)
		return fail("rename");
	if (access(path, F_OK) == 0) {
		fprintf(stderr, "DISKSTRESS: FAIL %s still present after rename\n", path);
		return -1;
	}
	if (unlink(path2) != 0)
		return fail("unlink");
	return 0;
}

/* A file written through a shared mapping, then read through the ordinary read
 * path. Page cache and file both have to end up holding the same bytes. */
static int mmap_round(unsigned long idx)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/m%lu.bin", g_dir, idx);
	size_t len = 64 * 4096;

	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0)
		return fail("mmap open");
	if (ftruncate(fd, (off_t)len) != 0) {
		close(fd);
		return fail("ftruncate");
	}
	unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return fail("mmap");
	}
	/* A freshly extended file reads as zero; anything else here is a page
	 * handed over with somebody's data still in it. */
	for (size_t i = 0; i < len; i++) {
		if (p[i]) {
			fprintf(stderr, "DISKSTRESS: FAIL new file page not zero at %zu: 0x%02x\n", i, p[i]);
			munmap(p, len);
			close(fd);
			return -1;
		}
	}
	pat_fill(p, idx ^ 0x5eed, len);
	if (msync(p, len, MS_SYNC) != 0) {
		munmap(p, len);
		close(fd);
		return fail("msync");
	}
	munmap(p, len);

	unsigned char *back = malloc(len);
	if (!back) {
		close(fd);
		return 0;
	}
	if (pread(fd, back, len, 0) != (ssize_t)len) {
		free(back);
		close(fd);
		return fail("pread after msync");
	}
	size_t bad = pat_check(back, idx ^ 0x5eed, len);
	free(back);
	close(fd);
	if (bad != (size_t)-1) {
		fprintf(stderr, "DISKSTRESS: FAIL mapped write not visible to read at %zu\n", bad);
		return -1;
	}
	unlink(path);
	return 0;
}

/* Fill a directory, list it, count what comes back. */
static int dir_round(unsigned long idx, int entries)
{
	char dir[256];
	snprintf(dir, sizeof(dir), "%s/d%lu", g_dir, idx);
	mkdir(dir, 0755);

	for (int i = 0; i < entries; i++) {
		char p[320];
		snprintf(p, sizeof(p), "%s/e%03d", dir, i);
		int fd = open(p, O_CREAT | O_WRONLY, 0644);
		if (fd < 0)
			return fail("dir entry create");
		close(fd);
	}

	DIR *d = opendir(dir);
	if (!d)
		return fail("opendir");
	int seen = 0;
	struct dirent *de;
	while ((de = readdir(d)) != 0) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		seen++;
	}
	closedir(d);

	for (int i = 0; i < entries; i++) {
		char p[320];
		snprintf(p, sizeof(p), "%s/e%03d", dir, i);
		unlink(p);
	}
	rmdir(dir);

	if (seen != entries) {
		fprintf(stderr, "DISKSTRESS: FAIL directory with %d entries listed %d\n", entries, seen);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1)
		g_dir = argv[1];
	g_deadline = now_ms() + budget_ms();

	/* mkdir -p, by hand: the image mounts /tmp but nothing below it, and a
	 * caller naming a deeper path should not have to create it first. */
	{
		char tmp[512];
		snprintf(tmp, sizeof(tmp), "%s", g_dir);
		for (char *q = tmp + 1; *q; q++) {
			if (*q != '/')
				continue;
			*q = 0;
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
				fail("mkdir work directory");
				return 1;
			}
			*q = '/';
		}
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
			fail("mkdir work directory");
			return 1;
		}
	}

	long files = scaled(60);
	printf("DISKSTRESS: start dir=%s files=%ld\n", g_dir, files);
	fflush(stdout);
	unsigned long long t0 = now_ms();

	/* One buffer pair, reused: the point is the filesystem, and allocating a
	 * megabyte per file would make this a heap test as well. */
	size_t maxlen = 512 * 1024;
	unsigned char *buf = malloc(maxlen), *back = malloc(maxlen);
	if (!buf || !back) {
		fprintf(stderr, "DISKSTRESS: FAIL out of memory for buffers\n");
		return 1;
	}

	unsigned long seed = 20260823UL;
	int rc = 0;
	long done = 0;
	for (long i = 0; i < files && now_ms() < g_deadline; i++) {
		unsigned long r = rnd(&seed);
		size_t len;
		switch (r % 4) {
		case 0: len = 1 + r % 4096; break;
		case 1: len = 4096 + r % 32768; break;
		case 2: len = 65536 + r % 131072; break;
		default: len = 262144 + r % 262144; break;
		}
		if (len > maxlen)
			len = maxlen;
		if (file_round((unsigned long)i, len, buf, back) != 0) {
			rc = 1;
			break;
		}
		done++;
		if ((i % 8) == 0 && mmap_round((unsigned long)i) != 0) {
			rc = 1;
			break;
		}
		if ((i % 16) == 0 && dir_round((unsigned long)i, 64) != 0) {
			rc = 1;
			break;
		}
	}

	free(buf);
	free(back);
	sync();
	unsigned long long ms = now_ms() - t0;
	if (rc) {
		printf("DISKSTRESS: FAIL after %ld files, %llu KiB, ms=%llu\n", done, g_bytes / 1024, ms);
		return 1;
	}
	printf("DISKSTRESS: ok %ld files, %llu KiB written and verified, ms=%llu\n",
	       done, g_bytes / 1024, ms);
	return 0;
}
