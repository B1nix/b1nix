/* telinit — request an init runlevel change (M39).
 *
 * b1nix's PID 1 (/bin/init) is an in-kernel task, so it cannot install a
 * userspace signal handler the way SysV init does. Instead, init exposes a tiny
 * control file: telinit writes the requested runlevel to /run/initctl and init
 * polls that file from its supervisor loop, switching runlevel (stopping the
 * services not valid in the new level, starting the ones that are). A
 * best-effort SIGHUP to PID 1 nudges init to re-check promptly.
 *
 * Usage:
 *   telinit 0|1|2|3|4|5|6   switch to numeric runlevel (0 halt, 6 reboot)
 *   telinit S|s            switch to single-user
 *   telinit q|Q            re-read /etc/inittab (reload)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>

#define INITCTL "/run/initctl"

static int valid_runlevel(char c) {
	return (c >= '0' && c <= '6') || c == 'S' || c == 's' || c == 'q' ||
	       c == 'Q';
}

int main(int argc, char **argv) {
	if (argc != 2 || argv[1][0] == '\0' || argv[1][1] != '\0' ||
	    !valid_runlevel(argv[1][0])) {
		fprintf(stderr, "usage: telinit 0123456SsQq\n");
		return 1;
	}

	char level = argv[1][0];
	if (level == 's')
		level = 'S';
	if (level == 'q')
		level = 'Q';

	/* Make sure /run exists (it is a tmpfs mount point in the initramfs). */
	(void)mkdir("/run", 0755);

	int fd = open(INITCTL, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		perror("telinit: open " INITCTL);
		return 1;
	}
	char buf[2] = {level, '\n'};
	if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
		perror("telinit: write");
		close(fd);
		return 1;
	}
	close(fd);

	/* Nudge PID 1 to re-read the control file without waiting for its poll
	 * interval. Harmless if the kernel init task does not act on the signal. */
	(void)kill(1, SIGHUP);

	return 0;
}
