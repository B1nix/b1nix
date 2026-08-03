/* M108 smoke: BusyBox owns su, passwd and init.
 *
 * Every marker is emitted only after the operation ran AND its result was
 * checked against something this test knows independently — a uid it did not
 * print itself, a shell that announces its own name, a hash re-read out of
 * /etc/shadow, a PAM verdict from the module that owns the file. There is no
 * unconditional "ok" print anywhere below, and no marker is reachable without
 * the corresponding BusyBox applet actually doing the work.
 *
 * Default mode (runs on the posix instance, as root, under openrc-init):
 *
 *   setuid-layout    /bin/su and /bin/passwd are symlinks onto
 *                    /opt/busybox/bin/busybox-suid; that file is mode 4755
 *                    root, and the multicall ELF every other applet resolves to
 *                    (/opt/busybox/bin/busybox) is NOT setuid. This is the
 *                    privilege boundary the rest of the test relies on, so it
 *                    is checked rather than assumed.
 *   su-uid-and-shell root's `su -l m108user -c ...` really becomes uid 1002 and
 *                    really execs THAT ACCOUNT'S login shell: /bin/m108shell
 *                    prints a line only it can print, and `id -u` inside it
 *                    reports 1002. Neither value is supplied by this process.
 *   su-password-auth an UNPRIVILEGED process (this test drops to uid 1002)
 *                    su's to m108peer with the correct password and comes out
 *                    as uid 1003 — which is only possible through the setuid
 *                    bit plus a successful /etc/shadow crypt(3) comparison.
 *   su-wrong-password  the same call with a wrong password fails and does NOT
 *                    reach uid 1003.
 *   pam-accepts-initial  pam_unix.so authenticates m108pw's shipped password,
 *                    establishing the baseline the next two markers move.
 *   passwd-writes-sha512  BusyBox `passwd` rewrites m108pw's /etc/shadow field;
 *                    the field is re-read and must have changed and still be a
 *                    SHA-512 "$6$" crypt string (the scheme pam_unix.so reads).
 *   passwd-pam-accepts-new  the PAM path accepts the password BusyBox wrote.
 *   passwd-pam-rejects-old  and rejects the one it replaced.
 *   su-accepts-passwd-hash  and BusyBox `su` authenticates that same new
 *                    password — the round trip closes in both directions.
 *
 * `m108_smoke initcheck` mode (runs on the bbinit instance, from /etc/inittab,
 * where PID 1 is /opt/busybox/bin/init):
 *
 *   bbinit-pid1      PID 1 is the BusyBox multicall ELF running as `init`
 *                    (/proc/1/cmdline and /proc/1/exe, not a self-report).
 *   bbinit-openrc-runlevels  OpenRC's `default` runlevel ran under BusyBox
 *                    init: its local.d hook left a file only that path writes.
 *   bbinit-shell     a shell starts and runs a command under BusyBox init.
 *   bbinit-reaps-orphan  a grandchild orphaned mid-flight is re-parented to
 *                    PID 1 (it reports getppid()==1 itself) and then vanishes
 *                    from /proc — i.e. PID 1 waited on it. An init that does
 *                    not reap would leave the zombie there forever.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <security/pam_appl.h>
#include <security/pam_constants.h>

#define BB_PLAIN "/opt/busybox/bin/busybox"
#define BB_SUID  "/opt/busybox/bin/busybox-suid"

#define PAM_SERVICE "m108-smoke"

#define USER_CALLER "m108user"
#define UID_CALLER  1002
#define USER_PEER   "m108peer"
#define UID_PEER    1003
#define PASS_PEER   "M108peer!"
#define USER_PW     "m108pw"
#define PASS_PW_OLD "M108old!"
#define PASS_PW_NEW "M108new!"

static int failures;

static void emit(const char *s) { (void)!write(1, s, strlen(s)); }

static void ok(const char *name)
{
	emit("M108-SMOKE: ok ");
	emit(name);
	emit("\n");
}

/* Never include a password (or a hash) in a failure message: this goes to the
 * serial console and into the committed smoke logs. */
static void fail(const char *name, const char *why)
{
	failures++;
	emit("M108-SMOKE: FAIL ");
	emit(name);
	emit(" (");
	emit(why);
	emit(")\n");
}

/* ── /etc/shadow ──────────────────────────────────────────────────────────
 * Read a user's crypt(3) field. Only used to observe that `passwd` changed
 * something and what scheme it wrote — never to authenticate. */
static int shadow_hash(const char *user, char *out, size_t outsz)
{
	FILE *f;
	char line[512];
	size_t ulen = strlen(user);
	int found = 0;

	f = fopen("/etc/shadow", "r");
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *colon;
		if (strncmp(line, user, ulen) != 0 || line[ulen] != ':')
			continue;
		colon = strchr(line + ulen + 1, ':');
		if (colon != NULL)
			*colon = '\0';
		line[strcspn(line, "\r\n")] = '\0';
		snprintf(out, outsz, "%s", line + ulen + 1);
		found = 1;
		break;
	}
	fclose(f);
	return found ? 0 : -1;
}

/* ── PAM ─────────────────────────────────────────────────────────────────
 * Same shape as the M104 smoke: a scripted conversation feeding one password,
 * so the verdict comes from pam_unix.so's own crypt(3) comparison. */
static int m108_conv(int n, const struct pam_message **msg,
                     struct pam_response **resp, void *data)
{
	struct pam_response *r;
	const char *password = (const char *)data;
	int i;

	if (n <= 0 || n > PAM_MAX_NUM_MSG)
		return PAM_CONV_ERR;
	r = calloc((size_t)n, sizeof(*r));
	if (r == NULL)
		return PAM_BUF_ERR;
	for (i = 0; i < n; i++) {
		r[i].resp_retcode = 0;
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
		case PAM_PROMPT_ECHO_ON:
			r[i].resp = strdup(password);
			break;
		default:
			r[i].resp = NULL;
			break;
		}
	}
	*resp = r;
	return PAM_SUCCESS;
}

static int pam_check(const char *user, const char *password)
{
	pam_handle_t *pamh = NULL;
	struct pam_conv conv;
	int r, auth_r;

	conv.conv = m108_conv;
	conv.appdata_ptr = (void *)(uintptr_t)password;

	r = pam_start(PAM_SERVICE, user, &conv, &pamh);
	if (r != PAM_SUCCESS)
		return r;
	auth_r = pam_authenticate(pamh, 0);
	pam_end(pamh, auth_r);
	return auth_r;
}

/* ── running a BusyBox applet ────────────────────────────────────────────
 * Runs `argv` with `stdin_text` on its standard input, optionally after
 * dropping to `as_uid`/`as_gid`, and collects everything it wrote. stdin is a
 * pipe rather than a tty on purpose: libbb's bb_ask_noecho() reads the fd
 * directly and merely tries (and is allowed to fail) to turn echo off, so the
 * password prompt works without a pty — and, more importantly, the test never
 * has to type a password onto a shared console.
 *
 * Returns the child's wait status, or -1 if it could not be run. */
static int run_capture(char *const argv[], const char *stdin_text,
                       uid_t as_uid, gid_t as_gid, char *out, size_t outsz)
{
	int in_pipe[2], out_pipe[2];
	pid_t pid;
	size_t used = 0;
	int status = -1;

	out[0] = '\0';
	if (pipe(in_pipe) != 0)
		return -1;
	if (pipe(out_pipe) != 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		close(in_pipe[0]); close(in_pipe[1]);
		close(out_pipe[0]); close(out_pipe[1]);
		return -1;
	}
	if (pid == 0) {
		close(in_pipe[1]);
		close(out_pipe[0]);
		if (dup2(in_pipe[0], STDIN_FILENO) < 0 ||
		    dup2(out_pipe[1], STDOUT_FILENO) < 0 ||
		    dup2(out_pipe[1], STDERR_FILENO) < 0)
			_exit(126);
		close(in_pipe[0]);
		close(out_pipe[1]);
		if (as_uid != (uid_t)-1) {
			/* Supplementary groups first, then the primary group,
			 * then the user: any other order silently keeps some of
			 * root's authority, and an "unprivileged" caller that is
			 * not actually unprivileged would make the su test
			 * meaningless. A partial drop is a hard error, never a
			 * quietly-weaker test. */
			if (setgroups(0, NULL) != 0)
				_exit(125);
			if (setgid(as_gid) != 0)
				_exit(125);
			if (setuid(as_uid) != 0)
				_exit(125);
			if (getuid() != as_uid || geteuid() != as_uid)
				_exit(125);
		}
		execv(argv[0], argv);
		_exit(127);
	}

	close(in_pipe[0]);
	close(out_pipe[1]);
	if (stdin_text != NULL)
		(void)!write(in_pipe[1], stdin_text, strlen(stdin_text));
	close(in_pipe[1]);

	for (;;) {
		ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
		if (n <= 0)
			break;
		used += (size_t)n;
		if (used >= outsz - 1)
			break;
	}
	out[used] = '\0';
	close(out_pipe[0]);

	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return status;
}

static int exited_zero(int status)
{
	return status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* True when `text` contains `needle` as a whole line (so "1003" does not match
 * inside "11003" or a diagnostic that happens to mention the number). */
static int has_line(const char *text, const char *needle)
{
	size_t nlen = strlen(needle);
	const char *p = text;

	while (*p != '\0') {
		const char *end = strchr(p, '\n');
		size_t len = (end != NULL) ? (size_t)(end - p) : strlen(p);
		while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' '))
			len--;
		if (len == nlen && strncmp(p, needle, nlen) == 0)
			return 1;
		if (end == NULL)
			break;
		p = end + 1;
	}
	return 0;
}

/* ── phase 1: where the setuid bit lives ─────────────────────────────── */
static void check_setuid_layout(void)
{
	static const char *names[] = { "/bin/su", "/bin/passwd" };
	char link[256];
	struct stat st;
	size_t i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		ssize_t n = readlink(names[i], link, sizeof(link) - 1);
		if (n <= 0) {
			fail("setuid-layout", "not a symlink onto busybox");
			return;
		}
		link[n] = '\0';
		if (strcmp(link, BB_SUID) != 0) {
			fail("setuid-layout", "does not resolve to busybox-suid");
			return;
		}
	}

	if (stat(BB_SUID, &st) != 0) {
		fail("setuid-layout", "busybox-suid missing");
		return;
	}
	if (st.st_uid != 0) {
		fail("setuid-layout", "busybox-suid not owned by root");
		return;
	}
	if ((st.st_mode & S_ISUID) == 0) {
		fail("setuid-layout", "busybox-suid has no setuid bit");
		return;
	}
	if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		fail("setuid-layout", "busybox-suid is group/world writable");
		return;
	}
	/* The other half of the boundary: the copy the remaining ~190 applet
	 * symlinks point at must NOT be setuid. */
	if (stat(BB_PLAIN, &st) != 0) {
		fail("setuid-layout", "busybox missing");
		return;
	}
	if ((st.st_mode & (S_ISUID | S_ISGID)) != 0) {
		fail("setuid-layout", "plain busybox is setuid");
		return;
	}
	ok("setuid-layout");
}

/* ── phase 2: su as root changes uid and runs the account's shell ────── */
static void check_su_uid_and_shell(void)
{
	/* "-l" rather than a lone "-": identical login semantics, but it cannot
	 * be confused with an option terminator by getopt's argument
	 * permutation, so the command below is unambiguous. */
	char *argv[] = { (char *)"/bin/su", (char *)"-l", (char *)USER_CALLER,
	                 (char *)"-c", (char *)"/bin/id -u", NULL };
	char out[1024];
	int status = run_capture(argv, NULL, (uid_t)-1, (gid_t)-1, out, sizeof(out));

	if (!exited_zero(status)) {
		fail("su-uid-and-shell", "su exited non-zero");
		return;
	}
	/* Only /bin/m108shell prints this, and only /etc/passwd's entry for
	 * m108user points su at it — so seeing it means su execed the account's
	 * own login shell. m108shell is an ELF precisely so the login '-' prefix
	 * in argv[0] survives the exec and can be asserted here; a #!/bin/sh
	 * script's $0 is its own path on b1nix as on Linux. */
	if (strstr(out, "m108shell-argv0=-m108shell") == NULL) {
		fail("su-uid-and-shell", "account's login shell did not run");
		return;
	}
	if (!has_line(out, "1002")) {
		fail("su-uid-and-shell", "uid did not become 1002");
		return;
	}
	ok("su-uid-and-shell");
}

/* ── phase 3: unprivileged su, with a password, through the setuid bit ── */
static int su_as_caller(const char *target, const char *password, char *out,
                        size_t outsz)
{
	char *argv[] = { (char *)"/bin/su", (char *)target,
	                 (char *)"-c", (char *)"/bin/id -u", NULL };
	char stdin_text[128];

	snprintf(stdin_text, sizeof(stdin_text), "%s\n", password);
	return run_capture(argv, stdin_text, (uid_t)UID_CALLER, (gid_t)UID_CALLER,
	                   out, outsz);
}

static void check_su_password(void)
{
	char out[1024];
	int status;

	status = su_as_caller(USER_PEER, PASS_PEER, out, sizeof(out));
	if (!exited_zero(status)) {
		fail("su-password-auth", "su with the correct password failed");
	} else if (!has_line(out, "1003")) {
		fail("su-password-auth", "uid did not become 1003");
	} else {
		ok("su-password-auth");
	}

	status = su_as_caller(USER_PEER, "definitely not the password",
	                      out, sizeof(out));
	if (status < 0) {
		fail("su-wrong-password", "could not run su");
	} else if (exited_zero(status)) {
		fail("su-wrong-password", "wrong password was accepted");
	} else if (has_line(out, "1003")) {
		fail("su-wrong-password", "reached the target uid anyway");
	} else {
		ok("su-wrong-password");
	}
}

/* ── phase 4: passwd writes a hash the PAM path accepts ─────────────── */
static void check_passwd(void)
{
	char before[256], after[256], out[1024];
	char *argv[] = { (char *)"/bin/passwd", (char *)USER_PW, NULL };
	char stdin_text[128];
	int status, r;

	if (pam_check(USER_PW, PASS_PW_OLD) != PAM_SUCCESS) {
		fail("pam-accepts-initial", "shipped password not accepted by PAM");
		return;
	}
	ok("pam-accepts-initial");

	if (shadow_hash(USER_PW, before, sizeof(before)) != 0) {
		fail("passwd-writes-sha512", "no /etc/shadow entry to change");
		return;
	}

	/* BusyBox passwd asks for the new password twice; run as root, so it
	 * does not ask for the old one. */
	snprintf(stdin_text, sizeof(stdin_text), "%s\n%s\n",
	         PASS_PW_NEW, PASS_PW_NEW);
	status = run_capture(argv, stdin_text, (uid_t)-1, (gid_t)-1,
	                     out, sizeof(out));
	if (!exited_zero(status)) {
		fail("passwd-writes-sha512", "passwd exited non-zero");
		return;
	}
	if (shadow_hash(USER_PW, after, sizeof(after)) != 0) {
		fail("passwd-writes-sha512", "shadow entry disappeared");
		return;
	}
	if (strcmp(before, after) == 0) {
		fail("passwd-writes-sha512", "shadow entry unchanged");
		return;
	}
	if (strncmp(after, "$6$", 3) != 0) {
		fail("passwd-writes-sha512", "new hash is not a SHA-512 crypt");
		return;
	}
	ok("passwd-writes-sha512");

	r = pam_check(USER_PW, PASS_PW_NEW);
	if (r != PAM_SUCCESS)
		fail("passwd-pam-accepts-new", "PAM rejected the new password");
	else
		ok("passwd-pam-accepts-new");

	r = pam_check(USER_PW, PASS_PW_OLD);
	if (r == PAM_SUCCESS)
		fail("passwd-pam-rejects-old", "PAM still accepts the old password");
	else if (r != PAM_AUTH_ERR)
		fail("passwd-pam-rejects-old", "unexpected PAM code");
	else
		ok("passwd-pam-rejects-old");

	/* And the other direction: BusyBox su must authenticate the very hash
	 * BusyBox passwd just wrote. */
	{
		char sout[1024];
		char *su_argv[] = { (char *)"/bin/su", (char *)USER_PW,
		                    (char *)"-c", (char *)"/bin/id -u", NULL };
		char su_stdin[128];
		int s;

		snprintf(su_stdin, sizeof(su_stdin), "%s\n", PASS_PW_NEW);
		s = run_capture(su_argv, su_stdin, (uid_t)UID_CALLER,
		                (gid_t)UID_CALLER, sout, sizeof(sout));
		if (!exited_zero(s))
			fail("su-accepts-passwd-hash", "su rejected the new password");
		else if (!has_line(sout, "1004"))
			fail("su-accepts-passwd-hash", "uid did not become 1004");
		else
			ok("su-accepts-passwd-hash");
	}
}

/* ── initcheck mode: BusyBox init as PID 1 ──────────────────────────── */

/* /proc/1/cmdline is NUL-separated; return its argv[0]. */
static int pid1_argv0(char *out, size_t outsz)
{
	int fd = open("/proc/1/cmdline", O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, out, outsz - 1);
	close(fd);
	if (n <= 0)
		return -1;
	out[n] = '\0';
	return 0;
}

static void check_bbinit_pid1(void)
{
	char argv0[256], exe[256];
	ssize_t n;

	if (pid1_argv0(argv0, sizeof(argv0)) != 0) {
		fail("bbinit-pid1", "cannot read /proc/1/cmdline");
		return;
	}
	if (strstr(argv0, "init") == NULL) {
		fail("bbinit-pid1", "PID 1 is not running as init");
		return;
	}
	/* argv[0] alone would be forgeable; /proc/1/exe is the file the kernel
	 * actually loaded. It must be the BusyBox multicall ELF. */
	n = readlink("/proc/1/exe", exe, sizeof(exe) - 1);
	if (n <= 0) {
		fail("bbinit-pid1", "cannot read /proc/1/exe");
		return;
	}
	exe[n] = '\0';
	if (strstr(exe, "busybox") == NULL) {
		fail("bbinit-pid1", "PID 1 is not the BusyBox multicall ELF");
		return;
	}
	ok("bbinit-pid1");
}

static void check_bbinit_openrc(void)
{
	/* Written by /etc/local.d/00-smoke.start, which only runs as part of
	 * OpenRC's `local` service in the default runlevel — so its presence
	 * means /etc/inittab's `openrc default` really completed under BusyBox
	 * init, not merely that inittab listed it. */
	if (access("/tmp/m108-openrc-local-ran", F_OK) != 0)
		fail("bbinit-openrc-runlevels", "openrc default did not run");
	else
		ok("bbinit-openrc-runlevels");
}

static void check_bbinit_shell(void)
{
	char *argv[] = { (char *)"/bin/sh", (char *)"-c",
	                 (char *)"echo m108-shell-alive-$((20 + 3))", NULL };
	char out[256];
	int status = run_capture(argv, NULL, (uid_t)-1, (gid_t)-1,
	                         out, sizeof(out));

	if (!exited_zero(status))
		fail("bbinit-shell", "shell exited non-zero");
	else if (strstr(out, "m108-shell-alive-23") == NULL)
		fail("bbinit-shell", "shell did not evaluate the command");
	else
		ok("bbinit-shell");
}

static void check_bbinit_reaps_orphan(void)
{
	int fds[2];
	pid_t child, grandchild = -1;
	char path[64];
	int status, i;
	ssize_t n;

	if (pipe(fds) != 0) {
		fail("bbinit-reaps-orphan", "pipe failed");
		return;
	}

	child = fork();
	if (child < 0) {
		close(fds[0]); close(fds[1]);
		fail("bbinit-reaps-orphan", "fork failed");
		return;
	}
	if (child == 0) {
		pid_t g = fork();
		if (g == 0) {
			/* Outlive our parent so we are re-parented, then report
			 * the pid AND the new parent we observe, and exit. */
			pid_t me = getpid();
			for (i = 0; i < 200; i++) {
				if (getppid() == 1)
					break;
				usleep(20000);
			}
			if (getppid() == 1)
				(void)!write(fds[1], &me, sizeof(me));
			_exit(0);
		}
		_exit(g < 0 ? 1 : 0); /* orphan the grandchild immediately */
	}
	close(fds[1]);
	waitpid(child, &status, 0);

	n = read(fds[0], &grandchild, sizeof(grandchild));
	close(fds[0]);
	if (n != (ssize_t)sizeof(grandchild) || grandchild <= 1) {
		fail("bbinit-reaps-orphan", "orphan was not re-parented to PID 1");
		return;
	}

	/* This process is not the grandchild's parent, so it cannot wait for it:
	 * the only way /proc/<pid> can disappear is PID 1 reaping the zombie. */
	snprintf(path, sizeof(path), "/proc/%d", (int)grandchild);
	for (i = 0; i < 250; i++) {
		if (access(path, F_OK) != 0) {
			ok("bbinit-reaps-orphan");
			return;
		}
		usleep(20000);
	}
	fail("bbinit-reaps-orphan", "orphan stayed a zombie; PID 1 never reaped it");
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "initcheck") == 0) {
		emit("M108-SMOKE: start-init\n");
		check_bbinit_pid1();
		check_bbinit_openrc();
		check_bbinit_shell();
		check_bbinit_reaps_orphan();
		emit("M108-SMOKE: done-init\n");
		return failures != 0;
	}

	emit("M108-SMOKE: start\n");
	if (geteuid() != 0) {
		/* Not a pass and not a silent skip: the whole point is the
		 * privileged path, so say so loudly and fail. */
		fail("setuid-layout", "test must run as root");
		emit("M108-SMOKE: done\n");
		return 1;
	}
	check_setuid_layout();
	check_su_uid_and_shell();
	check_su_password();
	check_passwd();
	emit("M108-SMOKE: done\n");
	return failures != 0;
}
