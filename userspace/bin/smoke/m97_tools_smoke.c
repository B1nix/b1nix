/*
 * M97 — GNU-free build tools.
 *
 * Numbered M97 ("GNU-Free ISO: Limine bootloader + BSD build tools") because
 * that is the milestone this covers. It was M98 for a while, which is the
 * driver-infrastructure milestone — whose own checks are in-kernel and emit
 * M98-DRV-SMOKE, so two unrelated things answered to the same number in the
 * same log.
 *
 * b1nix used to carry GNU Make (GPLv3) as /bin/make. It is now bmake (the
 * portable NetBSD make, BSD 3-clause) with samurai (a 0BSD reimplementation of
 * Ninja) alongside it as /bin/samu and /bin/ninja. This test proves the
 * replacements really drive a build on the target rather than merely existing:
 * it writes a Makefile and a build.ninja into /tmp, runs each tool on it, and
 * checks the file the recipe was supposed to produce. It also asserts that
 * /bin/make is not GNU Make, so a re-introduced GPL binary fails the suite
 * instead of passing silently.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

static void marker(const char *s) { write(1, s, strlen(s)); }

static int write_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  size_t len = strlen(text);
  ssize_t n = write(fd, text, len);
  close(fd);
  return (n == (ssize_t)len) ? 0 : -1;
}

/* Run argv, capturing stdout+stderr into out[]. Returns the exit status, or
 * -1 if the child could not be started or did not exit normally. */
static int run_capture(char *const argv[], char *out, size_t outsz) {
  int pipefd[2];
  if (out && outsz)
    out[0] = '\0';
  if (pipe(pipefd) < 0)
    return -1;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], 1);
    dup2(pipefd[1], 2);
    close(pipefd[1]);
    execv(argv[0], argv);
    _exit(127);
  }

  close(pipefd[1]);
  size_t used = 0;
  for (;;) {
    char buf[256];
    ssize_t n = read(pipefd[0], buf, sizeof(buf));
    if (n <= 0)
      break;
    if (out && used + (size_t)n < outsz) {
      memcpy(out + used, buf, (size_t)n);
      used += (size_t)n;
      out[used] = '\0';
    }
  }
  close(pipefd[0]);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0)
    return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Did the tool produce the file its recipe promised, with the right content? */
static int built_ok(const char *path, const char *want) {
  char buf[128];
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return 0;
  buf[n] = '\0';
  return strstr(buf, want) != NULL;
}

int main(void) {
  char out[1024];
  int rc;

  marker("M97-TOOLS: start\n");

  /* ── /bin/make is bmake, and it is not GNU Make ───────────────────────── */
  /* An empty regular file, not /dev/null: bmake opens its makefile as a file
   * and a character device is not one. */
  if (write_file("/tmp/m98-empty.mk", "") != 0) {
    marker("M97-TOOLS: fail make-write-empty\n");
    return 1;
  }
  {
    char *const argv[] = {(char *)"/bin/make", (char *)"-f", (char *)"/tmp/m98-empty.mk",
                          (char *)"-V", (char *)"MAKE_VERSION", NULL};
    rc = run_capture(argv, out, sizeof(out));
    /* -V is a bmake extension: it prints the value of a variable. GNU Make
     * treats -V as an unknown option and fails, so a passing run here is
     * already evidence of which make this is. MAKE_VERSION is bmake's own
     * date-stamped version, so the output has to carry a digit. */
    int has_digit = 0;
    for (const char *p = out; *p; p++)
      if (*p >= '0' && *p <= '9') {
        has_digit = 1;
        break;
      }
    if (rc == 0 && has_digit) {
      marker("M97-TOOLS: ok make-is-bmake\n");
    } else {
      char diag[256];
      snprintf(diag, sizeof(diag), "M97-TOOLS-DEBUG: make -V rc=%d out=[%.160s]\n", rc, out);
      marker(diag);
      marker("M97-TOOLS: fail make-is-bmake\n");
    }
  }
  {
    char *const argv[] = {(char *)"/bin/make", (char *)"--version", NULL};
    run_capture(argv, out, sizeof(out));
    if (strstr(out, "GNU Make") == NULL)
      marker("M97-TOOLS: ok make-not-gnu\n");
    else
      marker("M97-TOOLS: fail make-not-gnu\n");
  }

  /* ── bmake actually interprets a Makefile ─────────────────────────────── */
  unlink("/tmp/m98-make.out");
  if (write_file("/tmp/m98.mk",
                 "GREETING = b1nix-bmake-built\n"
                 "all: /tmp/m98-make.out\n"
                 "/tmp/m98-make.out:\n"
                 "\techo ${GREETING} > /tmp/m98-make.out\n") != 0) {
    marker("M97-TOOLS: fail make-write-makefile\n");
    return 1;
  }
  {
    char *const argv[] = {(char *)"/bin/make", (char *)"-f", (char *)"/tmp/m98.mk",
                          (char *)"all", NULL};
    rc = run_capture(argv, out, sizeof(out));
    /* The recipe has no @ prefix, so a run that actually executes it echoes the
     * command line first. Both the echo and the resulting file must be there. */
    if (rc == 0 && strstr(out, "b1nix-bmake-built") != NULL &&
        built_ok("/tmp/m98-make.out", "b1nix-bmake-built"))
      marker("M97-TOOLS: ok make-build\n");
    else
      marker("M97-TOOLS: fail make-build\n");
  }
  /* A second run must NOT re-run the recipe: the target is newer than its
   * prerequisites. That timestamp logic is the whole reason to ship a make
   * rather than a shell script. bmake stays silent in this case, so the proof
   * is the absence of the echoed recipe. */
  {
    char *const argv[] = {(char *)"/bin/make", (char *)"-f", (char *)"/tmp/m98.mk",
                          (char *)"all", NULL};
    rc = run_capture(argv, out, sizeof(out));
    if (rc == 0 && strstr(out, "b1nix-bmake-built") == NULL)
      marker("M97-TOOLS: ok make-uptodate\n");
    else
      marker("M97-TOOLS: fail make-uptodate\n");
  }

  /* ── samurai runs, as /bin/samu and under the /bin/ninja alias ─────────── */
  {
    char *const argv[] = {(char *)"/bin/samu", (char *)"--version", NULL};
    rc = run_capture(argv, out, sizeof(out));
    if (rc == 0 && out[0] >= '0' && out[0] <= '9')
      marker("M97-TOOLS: ok samu-version\n");
    else
      marker("M97-TOOLS: fail samu-version\n");
  }
  {
    char *const argv[] = {(char *)"/bin/ninja", (char *)"--version", NULL};
    rc = run_capture(argv, out, sizeof(out));
    if (rc == 0)
      marker("M97-TOOLS: ok ninja-alias\n");
    else
      marker("M97-TOOLS: fail ninja-alias\n");
  }

  /* ── samurai actually executes a build graph ──────────────────────────── */
  unlink("/tmp/m98-ninja.out");
  if (write_file("/tmp/build.ninja",
                 "rule gen\n"
                 "  command = echo b1nix-samu-built > $out\n"
                 "  description = GEN $out\n"
                 "build /tmp/m98-ninja.out: gen\n") != 0) {
    marker("M97-TOOLS: fail samu-write-graph\n");
    return 1;
  }
  {
    char *const argv[] = {(char *)"/bin/samu", (char *)"-C", (char *)"/tmp",
                          (char *)"/tmp/m98-ninja.out", NULL};
    rc = run_capture(argv, out, sizeof(out));
    if (rc == 0 && built_ok("/tmp/m98-ninja.out", "b1nix-samu-built"))
      marker("M97-TOOLS: ok samu-build\n");
    else
      marker("M97-TOOLS: fail samu-build\n");
  }
  /* Re-running a satisfied graph must be a no-op ("nothing to do"). */
  {
    char *const argv[] = {(char *)"/bin/samu", (char *)"-C", (char *)"/tmp",
                          (char *)"/tmp/m98-ninja.out", NULL};
    rc = run_capture(argv, out, sizeof(out));
    if (rc == 0 && strstr(out, "nothing to do") != NULL)
      marker("M97-TOOLS: ok samu-uptodate\n");
    else
      marker("M97-TOOLS: fail samu-uptodate\n");
  }

  marker("M97-TOOLS: done\n");
  return 0;
}
