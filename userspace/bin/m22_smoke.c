/*
 * m22_smoke — utility smoke tests (pwd, ls, cp, ln, readlink, lstat,
 * grep, date, uname, id, whoami, ps, head, tail, wc, uuidgen, tree,
 * sha384sum, vmstat, path-norm, parent-perms, POSIX compliance).
 * Ported from deleted kernel/user/programs.c m22_smoke_main() to
 * POSIX API.  Emits the same M22-SMOKE markers so smoke.sh checks
 * remain unchanged.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void marker(const char *t) { write(1, t, strlen(t)); }

/* Run an external command; emit M22-SMOKE: ok/fail <label>. */
static int m22_run(const char *label, const char *path, char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0) {
    marker("M22-SMOKE: fail "); marker(label); marker("\n");
    return 1;
  }
  if (pid == 0) {
    execve(path, argv, NULL);
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    marker("M22-SMOKE: fail "); marker(label); marker("\n");
    return 1;
  }
  marker("M22-SMOKE: ok "); marker(label); marker("\n");
  return 0;
}

static int m22_check_parent_enforcement(void) {
  /* Creating a file in a non-existent parent must fail. */
  int fd = open("/tmp/m22-missing/file", O_CREAT | O_WRONLY, 0666);
  if (fd >= 0) { close(fd); marker("M22-SMOKE: fail parent-perms\n"); return 1; }
  if (mkdir("/tmp/m22-missing/dir", 0755) == 0) {
    rmdir("/tmp/m22-missing/dir");
    marker("M22-SMOKE: fail parent-perms\n"); return 1;
  }
  marker("M22-SMOKE: ok parent-perms\n");
  return 0;
}

static int m22_check_symlink_stat(void) {
  struct stat st, lst;
  if (stat("/tmp/m22dir/m22.link", &st) != 0 ||
      lstat("/tmp/m22dir/m22.link", &lst) != 0) {
    marker("M22-SMOKE: fail lstat\n"); return 1;
  }
  /* stat() follows symlink → should NOT be S_IFLNK. */
  if (S_ISLNK(st.st_mode)) {
    marker("M22-SMOKE: fail lstat\n"); return 1;
  }
  /* lstat() should report S_IFLNK. */
  if (!S_ISLNK(lst.st_mode)) {
    marker("M22-SMOKE: fail lstat\n"); return 1;
  }
  marker("M22-SMOKE: ok lstat\n");
  return 0;
}

static int m22_check_posix_compliance(void) {
  int fails = 0;

  /* 1. MAP_FIXED */
  void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (p == MAP_FAILED) { marker("m22: MAP_FIXED init fail\n"); return 1; }
  void *target = (char *)p + 4096;
  void *r = mmap(target, 4096, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  int fixed_adjacent = (r == target);
  if (!fixed_adjacent) { fails++; }
  void *r2 = mmap(p, 4096, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  int fixed_same = (r2 == p);
  if (!fixed_same) { fails++; }
  munmap(p, 8192);

  /* 2. mprotect alignment */
  void *pp = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (pp == MAP_FAILED) { marker("m22: mprotect init fail\n"); return 1; }
  /* A misaligned address must be rejected with EINVAL. Go through syscall()
   * rather than the libc wrapper: musl's mprotect() rounds the address down to
   * a page boundary before the syscall, so the wrapper can never surface the
   * kernel's alignment check. */
  int mprot_bad =
      (syscall(SYS_mprotect, (long)((char *)pp + 1), 4096L, (long)PROT_READ) !=
       -1);
  int mprot_good = (mprotect(pp, 4096, PROT_READ) != 0);
  munmap(pp, 4096);
  if (mprot_bad || mprot_good) fails++;

  /* 3. fork COW isolation */
  volatile char *cow = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (cow == MAP_FAILED) { marker("m22: COW init fail\n"); return 1; }
  cow[0] = 'P';
  pid_t cpid = fork();
  if (cpid == 0) {
    _exit(cow[0] == 'P' && cow[0] != 'C' ? 0 : 3);
  } else if (cpid > 0) {
    int st = 0;
    waitpid(cpid, &st, 0);
    if (WEXITSTATUS(st) != 0) fails++;
    if (cow[0] != 'P') fails++;
  } else {
    fails++;
  }
  munmap((void *)cow, 4096);

  if (fails == 0) {
    marker("M22-SMOKE: ok posix-compliance\n");
  } else {
    char d[128];
    snprintf(d, sizeof(d),
             "M22-SMOKE: detail posix-compliance mapfixed=%d,%d mprot=%d,%d\n",
             fixed_adjacent, fixed_same, mprot_bad, mprot_good);
    marker(d);
    marker("M22-SMOKE: fail posix-compliance\n");
  }
  return fails ? 1 : 0;
}

int main(void) {
  marker("M22-SMOKE: start\n");

  /* Prepare test data. */
  int fd = open("/tmp/m22.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (fd >= 0) {
    write(fd, "beta\nalpha\nalpha\n", 17);
    close(fd);
  }

  int failures = 0;

  failures += m22_run("pwd", "/bin/pwd", (char *[]){"/bin/pwd", NULL});

  /* /bin/mkdir must create the directory itself — clear any leftover from a
   * previous run so the tool is exercised, not short-circuited by EEXIST. */
  rmdir("/tmp/m22dir");
  failures += m22_run("mkdir", "/bin/mkdir", (char *[]){"/bin/mkdir", "/tmp/m22dir", NULL});
  failures += m22_check_parent_enforcement();

  failures += m22_run("ls", "/bin/ls", (char *[]){"/bin/ls", "/tmp", NULL});

  fd = open("/tmp/m22_grep.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (fd >= 0) { write(fd, "beta\n", 5); close(fd); }
  failures += m22_run("grep", "/bin/grep",
                       (char *[]){"/bin/grep", "beta", "/tmp/m22_grep.txt", NULL});

  failures += m22_run("cp", "/bin/cp",
                       (char *[]){"/bin/cp", "/tmp/m22.txt", "/tmp/m22dir/copy.txt", NULL});

  failures += m22_run("ln-s", "/bin/ln",
                       (char *[]){"/bin/ln", "-s", "/tmp/m22.txt", "/tmp/m22dir/m22.link", NULL});

  failures += m22_run("readlink", "/bin/readlink",
                       (char *[]){"/bin/readlink", "/tmp/m22dir/m22.link", NULL});
  failures += m22_check_symlink_stat();

  failures += m22_run("cat", "/bin/cat",
                       (char *[]){"/bin/cat", "/tmp/m22.txt", NULL});

  failures += m22_run("cat-link", "/bin/cat",
                       (char *[]){"/bin/cat", "/tmp/m22dir/m22.link", NULL});

  /* Path normalization: double slash, .., . */
  failures += m22_run("path-norm", "/bin/cat",
                       (char *[]){"/bin/cat", "/tmp//m22dir/../m22dir/./m22.link", NULL});

  failures += m22_run("head", "/bin/head",
                       (char *[]){"/bin/head", "-n", "10", "/tmp/m22.txt", NULL});

  failures += m22_run("tail", "/bin/tail",
                       (char *[]){"/bin/tail", "-n", "10", "/tmp/m22.txt", NULL});

  failures += m22_run("wc", "/bin/wc",
                       (char *[]){"/bin/wc", "/tmp/m22.txt", NULL});

  failures += m22_run("date", "/bin/date",
                       (char *[]){"/bin/date", NULL});

  failures += m22_run("uname", "/bin/uname",
                       (char *[]){"/bin/uname", "-a", NULL});

  failures += m22_run("id", "/bin/id",
                       (char *[]){"/bin/id", NULL});

  failures += m22_run("whoami", "/bin/whoami",
                       (char *[]){"/bin/whoami", NULL});

  failures += m22_run("ps", "/bin/ps",
                       (char *[]){"/bin/ps", NULL});

  failures += m22_run("uuidgen", "/bin/uuidgen",
                       (char *[]){"/bin/uuidgen", NULL});

  failures += m22_run("tree", "/bin/tree",
                       (char *[]){"/bin/tree", "/etc", NULL});

  failures += m22_run("sha384sum", "/bin/sha384sum",
                       (char *[]){"/bin/sha384sum", "/tmp/m22.txt", NULL});

  failures += m22_run("vmstat", "/bin/vmstat",
                       (char *[]){"/bin/vmstat", NULL});

  failures += m22_check_posix_compliance();

  /* Cleanup. */
  unlink("/tmp/m22.txt");
  unlink("/tmp/m22_grep.txt");
  unlink("/tmp/m22dir/copy.txt");
  unlink("/tmp/m22dir/m22.link");
  rmdir("/tmp/m22dir");

  marker(failures ? "M22-SMOKE: fail\n" : "M22-SMOKE: done\n");
  return failures ? 1 : 0;
}
