/*
 * m58_smoke — verifies the standalone JavaScript interpreter /bin/js (Duktape).
 *
 * For each case it spawns `/bin/js -e "<code>"` with stdout redirected to a
 * temp file, waits for it, then reads the file back and compares the EXACT
 * bytes against the expected output. A marker is only emitted when real JS ran
 * and produced the correct result — no unconditional "ok".
 *
 * Cases:
 *   eval  — arithmetic + a String method: print(2 + 3 * 4); print("ab".toUpperCase())
 *   json  — JSON.parse / JSON.stringify round-trip
 *   print — multi-arg print() argument joining (the C binding itself)
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static void marker(const char *text) {
	write(1, text, strlen(text));
}

/* Spawn `/bin/js -e CODE` with stdout redirected to out_path. Returns the
 * child's exit code, or -1 on spawn/wait failure. */
static int run_js(const char *code, const char *out_path) {
	int pid = fork();
	if (pid == 0) {
		int fd = open(out_path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
		if (fd < 0)
			_exit(127);
		if (fd != 1) {
			dup2(fd, 1);
			close(fd);
		}
		char *const argv[] = { "/bin/js", "-e", (char *)code, NULL };
		execve("/bin/js", argv, NULL);
		_exit(127);
	} else if (pid > 0) {
		int status = 0;
		if (waitpid(pid, &status, 0) == pid && WIFEXITED(status))
			return WEXITSTATUS(status);
	}
	return -1;
}

/* Read up to cap-1 bytes of out_path into buf, NUL-terminate. Returns length
 * or -1. */
static int read_all(const char *path, char *buf, int cap) {
	int fd = open(path, O_RDONLY);
	int total = 0, n;
	if (fd < 0)
		return -1;
	while (total < cap - 1 &&
	       (n = read(fd, buf + total, cap - 1 - total)) > 0)
		total += n;
	close(fd);
	buf[total] = '\0';
	return total;
}

/* Run a JS snippet and assert its stdout equals `expect` exactly. Emits the
 * pass marker only on an exact match. */
static int check_case(const char *name, const char *code, const char *expect) {
	char out[1024];
	char msg[256];
	const char *tmp = "/tmp/m58_js.out";

	int rc = run_js(code, tmp);
	if (rc != 0) {
		snprintf(msg, sizeof(msg),
		         "M58-SMOKE: FAIL %s (js exit=%d)\n", name, rc);
		marker(msg);
		return 1;
	}
	if (read_all(tmp, out, sizeof(out)) < 0) {
		snprintf(msg, sizeof(msg),
		         "M58-SMOKE: FAIL %s (no output)\n", name);
		marker(msg);
		return 1;
	}
	if (strcmp(out, expect) != 0) {
		snprintf(msg, sizeof(msg),
		         "M58-SMOKE: FAIL %s (got \"%s\" want \"%s\")\n",
		         name, out, expect);
		marker(msg);
		return 1;
	}
	snprintf(msg, sizeof(msg), "M58-SMOKE: ok %s\n", name);
	marker(msg);
	return 0;
}

int main(void) {
	int fails = 0;

	marker("M58-SMOKE: start\n");

	/* eval: integer arithmetic (2+3*4 = 14) and a real String method. */
	fails += check_case("eval",
	                     "print(2 + 3 * 4); print(\"ab\".toUpperCase());",
	                     "14\nAB\n");

	/* json: parse an object, mutate it, stringify it back. */
	fails += check_case("json",
	                     "var o = JSON.parse('{\"a\":1,\"b\":[2,3]}');"
	                     "o.c = o.b[0] + o.b[1];"
	                     "print(JSON.stringify(o));",
	                     "{\"a\":1,\"b\":[2,3],\"c\":5}\n");

	/* print: the native binding joins multiple args with a single space. */
	fails += check_case("print",
	                     "print('x', 1 + 1, true);",
	                     "x 2 true\n");

	if (fails == 0)
		marker("M58-SMOKE: done\n");
	else
		marker("M58-SMOKE: FAILED\n");
	return fails ? 1 : 0;
}
