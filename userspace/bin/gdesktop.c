#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct app {
	const char *path;
	pid_t pid;
	time_t started;
};

static void launch(struct app *a) {
	a->started = time(0);
	a->pid = fork();
	if (a->pid == 0) {
		execlp(a->path, a->path, (char *)0);
		_exit(127);
	}
}

/* Respawn, but back off if the app just died moments after launching — a
 * crash-looping app must not flood the desktop with windows. */
static void respawn(struct app *a) {
	if (time(0) - a->started < 2)
		sleep(2);
	launch(a);
}

int main(void) {
	/* displayd is started by the adjacent inittab entry. Give it a moment to
	 * create the listening socket before the apps connect. */
	usleep(1000000);

	struct app apps[] = {
	    {"/bin/gpaint", 0, 0},
	    {"/bin/gclock", 0, 0},
	    {"/bin/gterm", 0, 0},
	};
	const int n = (int)(sizeof(apps) / sizeof(apps[0]));

	for (int i = 0; i < n; i++) {
		launch(&apps[i]);
		usleep(150000);
	}

	for (;;) {
		int status;
		pid_t dead = wait(&status);
		if (dead <= 0) {
			usleep(100000);
			continue;
		}
		for (int i = 0; i < n; i++)
			if (apps[i].pid == dead) {
				respawn(&apps[i]);
				break;
			}
	}
}
