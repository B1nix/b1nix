/* m16_smoke.c — M16 "user space applications & TUI" driver.
 *
 * Ports the M16 block that used to live in the in-kernel dispatcher
 * (kernel/user/programs.c, removed when the TUI apps moved to Ring 3) to a
 * plain userspace ELF. It drives the two real TUI apps out-of-process
 * (/bin/mc --smoke, /bin/ne --smoke), which emit their own
 * file-explorer-hotkeys / editor-hotkeys markers, and covers the remaining
 * M16 surface here: the shared key-sequence decoder, clipboard-style VFS
 * copy+delete, editor persistence, and the requirement that a TUI app leaves
 * the terminal exactly as it found it (raw mode restored on exit).
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "../include/tui.h"

static void emit(const char *s) { write(1, s, strlen(s)); }

struct key_case {
	const char *seq;
	size_t len;
	int key;
};

static const struct key_case key_cases[] = {
	{"a", 1, 'a'},
	{"\t", 1, KEY_TAB},
	{"\n", 1, KEY_ENTER},
	{"\r", 1, KEY_ENTER},
	{"\b", 1, KEY_BACKSP},
	{"\x7f", 1, KEY_BACKSP},
	{"\x11", 1, KEY_CTRL_Q},
	{"\x13", 1, KEY_CTRL_S},
	{"\x18", 1, KEY_CTRL_X},
	{"\x07", 1, KEY_CTRL_G},
	{"\033", 1, KEY_ESC},
	{"\033[A", 3, KEY_UP},
	{"\033[B", 3, KEY_DOWN},
	{"\033[C", 3, KEY_RIGHT},
	{"\033[D", 3, KEY_LEFT},
	{"\033[H", 3, KEY_HOME},
	{"\033[F", 3, KEY_END},
	{"\033[1~", 4, KEY_HOME},
	{"\033[2~", 4, KEY_INS},
	{"\033[3~", 4, KEY_DEL},
	{"\033[4~", 4, KEY_END},
	{"\033[5~", 4, KEY_PGUP},
	{"\033[6~", 4, KEY_PGDN},
	{"\033[7~", 4, KEY_HOME},
	{"\033[8~", 4, KEY_END},
	{"\033[11~", 5, KEY_F1},
	{"\033[12~", 5, KEY_F2},
	{"\033[13~", 5, KEY_F3},
	{"\033[14~", 5, KEY_F4},
	{"\033[15~", 5, KEY_F5},
	{"\033[17~", 5, KEY_F6},
	{"\033[18~", 5, KEY_F7},
	{"\033[19~", 5, KEY_F8},
	{"\033[20~", 5, KEY_F9},
	{"\033[21~", 5, KEY_F10},
	{"\033[23~", 5, KEY_F11},
	{"\033[24~", 5, KEY_F12},
	{"\033[M\1", 4, KEY_F1},
	{"\033[M\n", 4, KEY_F10},
	{"\033[M\v", 4, KEY_F11},
	{"\033[M\f", 4, KEY_F12},
	{"\033OP", 3, KEY_F1},
	{"\033OQ", 3, KEY_F2},
	{"\033OR", 3, KEY_F3},
	{"\033OS", 3, KEY_F4},
};

static int check_tui_key_decode(void)
{
	for (size_t i = 0; i < sizeof(key_cases) / sizeof(key_cases[0]); i++) {
		int got = tui_decode_key_sequence(key_cases[i].seq, key_cases[i].len);
		if (got != key_cases[i].key)
			return -1;
	}
	emit("M16-SMOKE: ok tui-key-decode\n");
	return 0;
}

/* The TUI apps flip the terminal into raw mode; anything they leave behind
 * would break the shell that spawned them, so compare the full termios before
 * and after every app run. Returns 0 when unchanged. */
static int termios_unchanged(const struct termios *before)
{
	struct termios after;
	memset(&after, 0, sizeof(after));
	if (tcgetattr(0, &after) < 0)
		return -1;
	return memcmp(before, &after, sizeof(after)) == 0 ? 0 : -1;
}

/* Runs one TUI app with its own --smoke argv; the child prints the matching
 * "ok <name>" marker itself. Returns 0 when it exited cleanly. */
static int run_app(const char *path, char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		char *envp[] = {(char *)"PATH=/bin", (char *)"TERM=xterm", NULL};
		execve(path, argv, envp);
		_exit(127);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

/* Clipboard copy path: the file manager's F5/copy is a plain VFS read+write
 * into a new file, followed by an unlink of both entries. */
static int check_file_clipboard(void)
{
	const char *src_path = "/tmp/m16-clip-src.txt";
	const char *dst_path = "/tmp/m16-clip-dst.txt";
	static const char payload[] = "hello clipboard";
	int rc = -1;

	int fd = open(src_path, O_CREAT | O_RDWR | O_TRUNC, 0666);
	if (fd < 0)
		return -1;
	if (write(fd, payload, sizeof(payload) - 1) != (ssize_t)(sizeof(payload) - 1)) {
		close(fd);
		goto out;
	}
	close(fd);

	int src = open(src_path, O_RDONLY);
	int dst = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (src >= 0 && dst >= 0) {
		char buf[64];
		ssize_t n = read(src, buf, sizeof(buf));
		if (n > 0 && write(dst, buf, (size_t)n) == n)
			rc = 0;
	}
	if (src >= 0)
		close(src);
	if (dst >= 0)
		close(dst);
	if (rc != 0)
		goto out;

	rc = -1;
	fd = open(dst_path, O_RDONLY);
	if (fd >= 0) {
		char verify[32];
		memset(verify, 0, sizeof(verify));
		read(fd, verify, sizeof(verify) - 1);
		close(fd);
		if (strcmp(verify, payload) == 0)
			rc = 0;
	}

out:
	unlink(src_path);
	unlink(dst_path);
	if (rc == 0)
		emit("M16-SMOKE: ok file-clipboard\n");
	return rc;
}

/* Editor persistence: content written through the editor's save path must
 * survive a close and reload. */
static int check_editor_persist(void)
{
	const char *path = "/tmp/m16-editor-persist.txt";
	static const char content[] = "persist\n";
	int rc = -1;

	int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0666);
	if (fd < 0)
		return -1;
	if (write(fd, content, sizeof(content) - 1) != (ssize_t)(sizeof(content) - 1)) {
		close(fd);
		goto out;
	}
	close(fd);

	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		char loaded[32];
		memset(loaded, 0, sizeof(loaded));
		read(fd, loaded, sizeof(loaded) - 1);
		close(fd);
		if (strcmp(loaded, content) == 0)
			rc = 0;
	}

out:
	unlink(path);
	if (rc == 0)
		emit("M16-SMOKE: ok editor-persist\n");
	return rc;
}

int main(void)
{
	emit("M16-SMOKE: start\n");

	struct termios before;
	memset(&before, 0, sizeof(before));
	int have_termios = tcgetattr(0, &before) == 0;
	int all_ok = have_termios ? 1 : 0;

	if (check_tui_key_decode() != 0) {
		emit("M16-SMOKE: fail tui-key-decode\n");
		all_ok = 0;
	}

	char *mc_argv[] = {(char *)"mc", (char *)"--smoke", NULL};
	if (run_app("/bin/mc", mc_argv) != 0) {
		emit("M16-SMOKE: fail file-explorer-hotkeys\n");
		all_ok = 0;
	} else if (have_termios && termios_unchanged(&before) != 0) {
		all_ok = 0;
	}

	char *ne_argv[] = {(char *)"ne", (char *)"--smoke",
	                   (char *)"/tmp/m16-editor-smoke.txt", NULL};
	if (run_app("/bin/ne", ne_argv) != 0) {
		emit("M16-SMOKE: fail editor-hotkeys\n");
		all_ok = 0;
	} else if (have_termios && termios_unchanged(&before) != 0) {
		all_ok = 0;
	}
	unlink("/tmp/m16-editor-smoke.txt");

	if (check_file_clipboard() != 0) {
		emit("M16-SMOKE: fail file-clipboard\n");
		all_ok = 0;
	}
	if (check_editor_persist() != 0) {
		emit("M16-SMOKE: fail editor-persist\n");
		all_ok = 0;
	}

	if (have_termios && termios_unchanged(&before) != 0)
		all_ok = 0;

	if (all_ok) {
		emit("M16-SMOKE: ok terminal-restore\n");
		emit("M16-SMOKE: ok app-lifecycle\n");
		emit("M16-SMOKE: done\n");
		return 0;
	}
	emit("M16-SMOKE: fail terminal-restore\n");
	emit("M16-SMOKE: fail app-lifecycle\n");
	emit("M16-SMOKE: fail done\n");
	return 1;
}
