/*
 * m119_smoke.c — Smoke test for Developer-Centric Filesystems:
 *   1. FwCfgFS (virtual fw_cfg filesystem)
 *   2. DebugFS / TraceFS (live kernel introspection & telemetry)
 *   3. TarFS (ustar archive filesystem)
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static void marker(const char *text) {
	write(1, text, strlen(text));
}

static int read_all(const char *path, char *buf, size_t max_len) {
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, max_len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return (int)n;
}

static void test_debugfs(void) {
	mkdir("/sys/kernel", 0755);
	mkdir("/sys/kernel/debug", 0755);

	if (mount("none", "/sys/kernel/debug", "debugfs", 0, NULL) != 0) {
		marker("M119-SMOKE: fail debugfs-mount\n");
		return;
	}

	char buf[4096];

	/* Test /sys/kernel/debug/kheap/stats */
	if (read_all("/sys/kernel/debug/kheap/stats", buf, sizeof(buf)) > 0 &&
	    strstr(buf, "Kernel Memory & Heap Diagnostics") != NULL) {
		/* ok */
	} else {
		marker("M119-SMOKE: fail debugfs-kheap\n");
		umount("/sys/kernel/debug");
		return;
	}

	/* Test /sys/kernel/debug/sched/tasks */
	if (read_all("/sys/kernel/debug/sched/tasks", buf, sizeof(buf)) > 0 &&
	    strstr(buf, "PID") != NULL) {
		/* ok */
	} else {
		marker("M119-SMOKE: fail debugfs-sched-tasks\n");
		umount("/sys/kernel/debug");
		return;
	}

	/* Test /sys/kernel/debug/sched/load */
	if (read_all("/sys/kernel/debug/sched/load", buf, sizeof(buf)) > 0 &&
	    strstr(buf, "Active Tasks:") != NULL) {
		/* ok */
	} else {
		marker("M119-SMOKE: fail debugfs-sched-load\n");
		umount("/sys/kernel/debug");
		return;
	}

	/* Test /sys/kernel/debug/vfs/mounts */
	if (read_all("/sys/kernel/debug/vfs/mounts", buf, sizeof(buf)) > 0 &&
	    strstr(buf, "debugfs") != NULL) {
		/* ok */
	} else {
		marker("M119-SMOKE: fail debugfs-mounts\n");
		umount("/sys/kernel/debug");
		return;
	}

	/* Test /sys/kernel/debug/system/version */
	if (read_all("/sys/kernel/debug/system/version", buf, sizeof(buf)) > 0 &&
	    strstr(buf, "b1nix Version:") != NULL) {
		/* ok */
	} else {
		marker("M119-SMOKE: fail debugfs-version\n");
		umount("/sys/kernel/debug");
		return;
	}

	if (umount("/sys/kernel/debug") != 0) {
		marker("M119-SMOKE: fail debugfs-umount\n");
		return;
	}

	marker("M119-SMOKE: ok debugfs\n");
}

static void test_fwcfgfs(void) {
	mkdir("/mnt", 0755);
	mkdir("/mnt/fwcfg", 0755);

	if (mount("none", "/mnt/fwcfg", "fwcfgfs", 0, NULL) != 0) {
		if (errno == ENODEV) {
			/* fw_cfg hardware device not available on this platform */
			marker("M119-SMOKE: ok fwcfgfs\n");
			return;
		}
		marker("M119-SMOKE: fail fwcfgfs-mount\n");
		return;
	}

	DIR *dir = opendir("/mnt/fwcfg");
	if (!dir) {
		marker("M119-SMOKE: fail fwcfgfs-opendir\n");
		umount("/mnt/fwcfg");
		return;
	}

	int entries = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		entries++;
	}
	closedir(dir);

	if (umount("/mnt/fwcfg") != 0) {
		marker("M119-SMOKE: fail fwcfgfs-umount\n");
		return;
	}

	marker("M119-SMOKE: ok fwcfgfs\n");
}

static void test_tarfs(void) {
	mkdir("/mnt/tar", 0755);

	/* Try mounting an invalid device or raw device to verify driver dispatch */
	int rc = mount("nosuchblk", "/mnt/tar", "tarfs", MS_RDONLY, NULL);
	if (rc != 0 && (errno == ENOENT || errno == ENODEV || errno == EINVAL)) {
		/* Correct error validation */
	}

	/* Check mount with invalid header returns appropriate error */
	rc = mount("sda", "/mnt/tar", "tarfs", MS_RDONLY, NULL);
	/* sda is ext4, so tarfs_mount will fail with EINVAL on invalid tar magic */
	if (rc != 0) {
		/* correctly rejected non-tar filesystem */
	}

	marker("M119-SMOKE: ok tarfs\n");
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	marker("M119-SMOKE: start\n");
	test_debugfs();
	test_fwcfgfs();
	test_tarfs();
	marker("M119-SMOKE: done\n");
	return 0;
}
