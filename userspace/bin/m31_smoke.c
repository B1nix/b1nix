/* M31 user security smoke.
 * Written against standard POSIX APIs.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <grp.h>

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
 * with the expected $6$ prefix. Confirms the file is shipped and that the
 * password hashes use standard SHA-512 crypt — the format musl's crypt(3)
 * verifies (the old in-house $b1$ format went away with the musl migration;
 * dropbear/login/su all go through musl crypt now). */
static int test_shadow_readable(void) {
  int fd = open("/etc/shadow", O_RDONLY);
  if (fd < 0) { fail("shadow-open"); return -1; }
  char buf[2048];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) { fail("shadow-read"); return -1; }
  buf[n] = '\0';
  if (!strstr(buf, "root:$6$")) { fail("shadow-root-entry"); return -1; }
  if (!strstr(buf, "user:$6$")) { fail("shadow-user-entry"); return -1; }
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

  int pid = fork();
  if (pid < 0) { fail("setuid-fork"); return -1; }

  if (pid == 0) {
    /* Child: drop to uid 1000 first. */
    int rc = setuid(1000);
    if (rc != 0) {
      emit("M31-SEC: FAIL setuid-drop (cannot drop)\n");
      _exit(2);
    }
    char *argv2[] = {"/bin/m31-setuid", NULL};
    execve("/bin/m31-setuid", argv2, NULL);
    _exit(3);
  }

  /* Parent: wait + check status. */
  int status = 0;
  waitpid(pid, &status, 0);
  int code = WEXITSTATUS(status);
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
  int pid = fork();
  if (pid < 0) { fail("uid-fork"); return -1; }

  if (pid == 0) {
    int rc = setuid(1000);
    if (rc != 0) _exit(2);
    /* Now try to setuid(0) — must fail since euid is no longer 0. */
    int rc2 = setuid(0);
    _exit(rc2 == 0 ? 1 : 0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  int code = WEXITSTATUS(status);
  if (code == 0) {
    ok("uid-denial");
    return 0;
  }
  fail("uid-denial");
  return -1;
}

/* getuid/geteuid round-trip — sanity that the syscalls are wired. */
static int test_uid_syscalls(void) {
  uid_t uid = getuid();
  uid_t euid = geteuid();
  if (uid != euid) { fail("uid-eq-euid-at-start"); return -1; }
  ok("uid-syscalls");
  return 0;
}

/* Verify unprivileged user cannot read /etc/shadow. */
static int test_shadow_denied_for_user(void) {
  int pid = fork();
  if (pid < 0) { fail("shadow-denied-fork"); return -1; }
  if (pid == 0) {
    int rc = setuid(1000);
    if (rc != 0) _exit(2);
    int fd = open("/etc/shadow", O_RDONLY);
    if (fd >= 0) {
      close(fd);
      _exit(1);
    }
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  int code = WEXITSTATUS(status);
  if (code == 0) {
    ok("shadow-denied-user");
    return 0;
  }
  fail("shadow-denied-user");
  return -1;
}

/* Test seteuid/setegid behavior. */
static int test_seteuid_setegid(void) {
  int rc = seteuid(1000);
  if (rc != 0) { fail("seteuid-1000"); return -1; }
  if (geteuid() != 1000) { fail("seteuid-get"); return -1; }
  
  rc = seteuid(0);
  if (rc != 0) { fail("seteuid-0"); return -1; }
  if (geteuid() != 0) { fail("seteuid-get-0"); return -1; }
  
  ok("seteuid-setegid");
  return 0;
}

/* Test getgroups/setgroups. */
static int test_groups_syscalls(void) {
  gid_t list[3] = { 5, 10, 1000 };
  int rc = setgroups(3, list);
  if (rc != 0) { fail("setgroups"); return -1; }
  
  gid_t read_list[32];
  int count = getgroups(32, read_list);
  if (count != 3) { fail("getgroups-count"); return -1; }
  
  int has_5 = 0, has_10 = 0, has_1000 = 0;
  for (int i = 0; i < count; i++) {
    if (read_list[i] == 5) has_5 = 1;
    if (read_list[i] == 10) has_10 = 1;
    if (read_list[i] == 1000) has_1000 = 1;
  }
  if (!has_5 || !has_10 || !has_1000) { fail("getgroups-verify"); return -1; }
  
  ok("groups-syscalls");
  return 0;
}

/* Test sticky bit behavior on /tmp. */
static int test_sticky_bit(void) {
  unlink("/tmp/sticky_test"); // Clean up just in case
  int pid = fork();
  if (pid < 0) { fail("sticky-fork1"); return -1; }
  if (pid == 0) {
    if (setuid(1000) != 0) _exit(2);
    int fd = open("/tmp/sticky_test", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) _exit(3);
    write(fd, "test", 4);
    close(fd);
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (WEXITSTATUS(status) != 0) { fail("sticky-create"); return -1; }
  
  pid = fork();
  if (pid < 0) { fail("sticky-fork2"); return -1; }
  if (pid == 0) {
    if (setuid(1001) != 0) _exit(2);
    int rc = unlink("/tmp/sticky_test");
    _exit(rc == 0 ? 1 : 0);
  }
  waitpid(pid, &status, 0);
  if (WEXITSTATUS(status) != 0) { fail("sticky-unlink-denied"); return -1; }
  
  pid = fork();
  if (pid < 0) { fail("sticky-fork3"); return -1; }
  if (pid == 0) {
    if (setuid(1000) != 0) _exit(2);
    int rc = unlink("/tmp/sticky_test");
    _exit(rc == 0 ? 0 : 1);
  }
  waitpid(pid, &status, 0);
  if (WEXITSTATUS(status) != 0) { fail("sticky-unlink-allowed"); return -1; }
  
  ok("sticky-bit");
  return 0;
}

/* Test that unprivileged user cannot write to /etc/passwd. */
static int test_passwd_write_denied(void) {
  int pid = fork();
  if (pid < 0) { fail("passwd-write-fork"); return -1; }
  if (pid == 0) {
    if (setuid(1000) != 0) _exit(2);
    int fd = open("/etc/passwd", O_WRONLY);
    if (fd >= 0) {
      close(fd);
      _exit(1);
    }
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (WEXITSTATUS(status) != 0) { fail("passwd-write-denied"); return -1; }
  ok("passwd-write-denied");
  return 0;
}

/* Test basic commands execution. */
static int test_commands(void) {
  int pid = fork();
  if (pid == 0) {
    char *argv[] = { "/bin/whoami", NULL };
    execve("/bin/whoami", argv, NULL);
    _exit(1);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (WEXITSTATUS(status) != 0) { fail("cmd-whoami"); return -1; }

  pid = fork();
  if (pid == 0) {
    char *argv[] = { "/bin/id", NULL };
    execve("/bin/id", argv, NULL);
    _exit(1);
  }
  waitpid(pid, &status, 0);
  if (WEXITSTATUS(status) != 0) { fail("cmd-id"); return -1; }

  pid = fork();
  if (pid == 0) {
    char *argv[] = { "/bin/groups", NULL };
    execve("/bin/groups", argv, NULL);
    _exit(1);
  }
  waitpid(pid, &status, 0);
  if (WEXITSTATUS(status) != 0) { fail("cmd-groups"); return -1; }

  ok("commands-exec");
  return 0;
}

int main(void) {
  emit("M31-SEC: start\n");
  if (test_uid_syscalls() != 0)          return 1;
  if (test_seteuid_setegid() != 0)       return 1;
  if (test_groups_syscalls() != 0)       return 1;
  if (test_shadow_readable() != 0)       return 1;
  if (test_shadow_denied_for_user() != 0)return 1;
  if (test_setuid_elevate() != 0)        return 1;
  if (test_setuid_denied() != 0)         return 1;
  if (test_passwd_write_denied() != 0)   return 1;
  if (test_sticky_bit() != 0)            return 1;
  if (test_commands() != 0)              return 1;
  emit("M31-SEC: done\n");
  return 0;
}
