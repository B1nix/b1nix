#include <sys/wait.h>
#include <unistd.h>

static pid_t launch(const char *path) {
	pid_t pid = fork();
	if (pid == 0) {
		execlp(path, path, (char *)0);
		_exit(127);
	}
	return pid;
}

int main(void) {
	/* displayd is started by the adjacent inittab entry. Avoid probing with
	 * a real connection: an accepted-but-not-yet-reaped probe consumes a
	 * client slot while the initial desktop applications are connecting. */
	usleep(1000000);

	pid_t paint = launch("/bin/gpaint");
	usleep(150000);
	pid_t clock = launch("/bin/gclock");
	usleep(150000);
	pid_t term = launch("/bin/gterm");

	for (;;) {
		int status;
		pid_t dead = wait(&status);
		if (dead == term)
			term = launch("/bin/gterm");
		else if (dead == clock)
			clock = launch("/bin/gclock");
		else if (dead == paint)
			paint = launch("/bin/gpaint");
		else
			usleep(100000);
	}
}
