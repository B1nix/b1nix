/* M31 user security smoke. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "syscall.h"

static void emit(const char *s) {
  write(1, s, strlen(s));
}

static void ok(const char *name) {
  char buf[128];
  int n = 0;
  const char *p = "M31-SEC: ok ";
  while (*p) buf[n++] = *p++;
  while (*name) buf[n++] = *name++;
  buf[n++] = '\n';
  write(1, buf, n);
}

static void fail(const char *name) {
  char buf[128];
  int n = 0;
  const char *p = "M31-SEC: FAIL ";
  while (*p) buf[n++] = *p++;
  while (*name) buf[n++] = *name++;
  buf[n++] = '\n';
  write(1, buf, n);
}

/* Read /etc/shadow and verify it has at least the root + user entries
 * with the expected $b1$ prefix. Confirms the file is shipped and that
 * the password hashes use the b1nix-crypt format. */
static int test_shadow_readable(void) {
  int fd = open("/etc/shadow", O_RDONLY);
  if (fd < 0) { fail("shadow-open"); return -1; }
  char buf[2048];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) { fail("shadow-read"); return -1; }
  buf[n] = '\0';
  if (!strstr(buf, "root:$b1$")) { fail("shadow-root-entry"); return -1; }
  if (!strstr(buf, "user:$b1$")) { fail("shadow-user-entry"); return -1; }
  ok("shadow-format");
  return 0;
}

/* Verify the setuid execve path: fork → child setuid(1000) → exec
 * /bin/m31-setuid (initramfs INITRAMFS_SETUID, owner uid 0). The new
 * task's euid should become 0 even though the calling task is uid 1000.
 * The setuid binary exits 0 if its euid is root, 1 otherwise. */
static int test_setuid_elevate(void) {
  struct stat st;
  if (stat("/bin/m31-setuid", &st) == 0) {
    char dbg[128];
    snprintf(dbg, sizeof(dbg), "M31-DBG: m31-setuid mode=%o uid=%d gid=%d size=%d\n", (int)st.st_mode, (int)st.st_uid, (int)st.st_gid, (int)st.st_size);
    emit(dbg);
  } else {
    emit("M31-DBG: stat failed\n");
  }

  int pid = (int)syscall(SYS_FORK);
  if (pid < 0) { fail("setuid-fork"); return -1; }

  if (pid == 0) {
    /* Child: drop to uid 1000 first. */
    long rc = syscall(SYS_SETUID, 1000);
    if (rc != 0) {
      emit("M31-SEC: FAIL setuid-drop (cannot drop)\n");
      _exit(2);
    }
    const char *argv2[] = {"/bin/m31-setuid", 0};
    long exec_rc = syscall(SYS_EXECVE, "/bin/m31-setuid", argv2, 0);
    if (exec_rc < 0) {
      _exit((int)-exec_rc);
    }
    _exit(3);
  }

  /* Parent: wait + check status. */
  int status = 0;
  syscall(SYS_WAITPID, pid, &status, 0);
  /* WIFEXITED + WEXITSTATUS — b1nix encodes normal exit as
   * (code & 0xFF) << 8. See scheduler_waitpid. */
  int code = (status >> 8) & 0xff;
  if (code == 0) {
    ok("setuid-elevate");
    return 0;
  }
  char err_msg[64];
  snprintf(err_msg, sizeof(err_msg), "setuid-elevate-failed-code-%d", code);
  fail(err_msg);
  return -1;
}

/* Verify that an unprivileged task cannot raise its own uid back up. */
static int test_setuid_denied(void) {
  int pid = (int)syscall(SYS_FORK);
  if (pid < 0) { fail("uid-fork"); return -1; }

  if (pid == 0) {
    long rc = syscall(SYS_SETUID, 1000);
    if (rc != 0) _exit(2);
    /* Now try to setuid(0) — must fail since euid is no longer 0. */
    long rc2 = syscall(SYS_SETUID, 0);
    _exit(rc2 == 0 ? 1 : 0);
  }
  int status = 0;
  syscall(SYS_WAITPID, pid, &status, 0);
  int code = (status >> 8) & 0xff;
  if (code == 0) {
    ok("uid-denial");
    return 0;
  }
  fail("uid-denial");
  return -1;
}

/* getuid/geteuid round-trip — sanity that the syscalls are wired. */
static int test_uid_syscalls(void) {
  long uid = syscall(SYS_GETUID);
  long euid = syscall(SYS_GETEUID);
  if (uid != euid) { fail("uid-eq-euid-at-start"); return -1; }
  ok("uid-syscalls");
  return 0;
}

int main(void) {
  emit("M31-SEC: start\n");
  if (test_uid_syscalls() != 0)    return 1;
  if (test_shadow_readable() != 0) return 1;
  if (test_setuid_elevate() != 0)  return 1;
  if (test_setuid_denied() != 0)   return 1;
  emit("M31-SEC: done\n");
  return 0;
}
