/*
 * m123_smoke — namespaces complete enough for containers (M123).
 *
 * Every check runs in a forked child, so a namespace one check enters never
 * leaks into the next, and reports through a pipe what it saw. Each marker is
 * printed only after the property was observed from where it can be observed
 * — a translation from both sides of the namespace, a refusal with its errno.
 *
 *   userns-unpriv          an unprivileged task creates a user namespace, is
 *                          the overflow uid until it maps itself, then root
 *                          with every capability
 *   userns-map-rules       a map is written once; an unprivileged task maps
 *                          only its own id, and a gid map only after
 *                          setgroups is denied; setgroups then fails
 *   userns-ids             stat, getuid and a created file translate in both
 *                          directions; an unmapped id is EINVAL
 *   userns-no-host-power   root in a user namespace cannot rename the host,
 *                          mount in the host's mount namespace or signal init
 *   userns-owned           but it can in the UTS and mount namespaces it owns:
 *                          sethostname, a tmpfs mount; a block filesystem stays
 *                          refused
 *   userns-locked-mount    a read-only mount inherited from outside stays
 *                          read-only
 *   userns-nsfs            NS_GET_OWNER_UID, NS_GET_PARENT, NS_GET_NSTYPE
 *   userns-setns           root joins a child user namespace by its handle
 *   userns-threaded        unshare(CLONE_NEWUSER) in a threaded process is
 *                          EINVAL
 *   pidns-clone-init       clone(CLONE_NEWPID) makes the child pid 1 with no
 *                          parent in sight
 *   pidns-orphans          an orphan inside is adopted by the namespace's init
 *   pidns-init-signals     init ignores an unhandled SIGTERM from inside;
 *                          SIGKILL from outside kills it
 *   pidns-init-exit        when init exits, the rest of the namespace dies
 *   pidns-proc             a proc mount lists the namespace's own pids;
 *                          NSpid shows both numbers
 *   pidns-userns-clone     clone(CLONE_NEWUSER|CLONE_NEWPID) unprivileged
 *   ipcns-sysv             SysV keys and ids do not cross IPC namespaces
 *   ipcns-mqueue           nor do POSIX queue names; an mqueue mount lists
 *                          the namespace's own queues
 *   mqueue-semantics       priority order, O_NONBLOCK, timeouts, mq_notify
 *   shm-rmid-attached      IPC_RMID on an attached segment defers the free
 *   cgroupns-root          /proc/self/cgroup is "/" at the namespace root;
 *                          outside it names the real path
 *   timens-offsets         a time namespace shifts MONOTONIC and BOOTTIME for
 *                          its members (vDSO and system call), not REALTIME
 *   timens-locked          offsets are fixed once a task has entered
 *   ns-links               /proc/<pid>/ns entries are "kind:[inode]" links
 *                          whose handles stat to that inode
 *   ns-handle-keeps-alive  an open handle keeps a namespace whose last task
 *                          is gone, and setns into it still works
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <mqueue.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef CLONE_NEWTIME
#define CLONE_NEWTIME 0x00000080
#endif
#define NS_GET_USERNS 0xb701
#define NS_GET_PARENT 0xb702
#define NS_GET_NSTYPE 0xb703
#define NS_GET_OWNER_UID 0xb704

#define UNPRIV_UID 1000
#define UNPRIV_GID 1000
#define CAP_SYS_ADMIN_BIT 21

static int g_fail;

static void marker(const char *s) { write(1, s, strlen(s)); }

static void ok(const char *name) {
  char b[128];
  snprintf(b, sizeof(b), "M123-SMOKE: ok %s\n", name);
  marker(b);
}

static void fail(const char *name, const char *why) {
  char b[512];
  snprintf(b, sizeof(b), "M123-SMOKE: FAIL %s %s\n", name, why);
  marker(b);
  g_fail = 1;
}

/* ── plumbing ─────────────────────────────────────────────────────────────── */

/* A check runs in a child that reports "" for success or a reason. */
typedef void (*check_fn)(int report_fd);

static void reportf(int fd, const char *fmt, ...) {
  char b[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  write(fd, b, strlen(b));
}

static void run(const char *name, check_fn fn) {
  int p[2];
  if (pipe(p) != 0) {
    fail(name, "pipe");
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    fail(name, "fork");
    return;
  }
  if (pid == 0) {
    close(p[0]);
    fn(p[1]);
    close(p[1]);
    _exit(0);
  }
  close(p[1]);
  char buf[512];
  size_t n = 0;
  for (;;) {
    ssize_t r = read(p[0], buf + n, sizeof(buf) - 1 - n);
    if (r <= 0)
      break;
    n += (size_t)r;
    if (n == sizeof(buf) - 1)
      break;
  }
  buf[n] = '\0';
  close(p[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  if (n > 0)
    fail(name, buf);
  else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
    fail(name, "check process died");
  else
    ok(name);
}

static int write_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY);
  if (fd < 0)
    return -errno;
  ssize_t w = write(fd, text, strlen(text));
  int e = errno;
  close(fd);
  return w == (ssize_t)strlen(text) ? 0 : -e;
}

static int read_file(const char *path, char *buf, size_t len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -errno;
  ssize_t n = read(fd, buf, len - 1);
  close(fd);
  if (n < 0)
    return -errno;
  buf[n] = '\0';
  return (int)n;
}

static int drop_to_unpriv(void) {
  if (setgroups(0, 0) != 0 || setgid(UNPRIV_GID) != 0 ||
      setuid(UNPRIV_UID) != 0)
    return -errno;
  return 0;
}

/* Enter a new user namespace as the unprivileged user, mapping it to root. */
static int enter_userns_as_root(void) {
  int rc = drop_to_unpriv();
  if (rc)
    return rc;
  if (unshare(CLONE_NEWUSER) != 0)
    return -errno;
  if ((rc = write_file("/proc/self/setgroups", "deny")) != 0)
    return rc;
  char m[64];
  snprintf(m, sizeof(m), "0 %d 1", UNPRIV_UID);
  if ((rc = write_file("/proc/self/uid_map", m)) != 0)
    return rc;
  snprintf(m, sizeof(m), "0 %d 1", UNPRIV_GID);
  if ((rc = write_file("/proc/self/gid_map", m)) != 0)
    return rc;
  return 0;
}

struct cap_hdr {
  unsigned version;
  int pid;
};
struct cap_data {
  unsigned effective, permitted, inheritable;
};

static int has_cap(int bit) {
  struct cap_hdr h = {0x20080522u, 0};
  struct cap_data d[2];
  memset(d, 0, sizeof(d));
  if (syscall(SYS_capget, &h, d) != 0)
    return -1;
  return (d[bit / 32].effective >> (bit % 32)) & 1;
}

static ino_t ns_ino(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 ? st.st_ino : 0;
}

/* ── user namespaces ──────────────────────────────────────────────────────── */

static void check_userns_unpriv(int out) {
  if (drop_to_unpriv() != 0)
    return reportf(out, "drop privileges errno=%d", errno);
  if (has_cap(CAP_SYS_ADMIN_BIT) != 0)
    return reportf(out, "unprivileged task holds CAP_SYS_ADMIN");
  if (unshare(CLONE_NEWUSER) != 0)
    return reportf(out, "unshare(CLONE_NEWUSER) errno=%d", errno);
  if (getuid() != 65534 || geteuid() != 65534)
    return reportf(out, "unmapped uid reads %d, want 65534", (int)getuid());
  if (has_cap(CAP_SYS_ADMIN_BIT) != 1)
    return reportf(out, "creator lacks CAP_SYS_ADMIN in its namespace");
  int rc;
  if ((rc = write_file("/proc/self/setgroups", "deny")) != 0)
    return reportf(out, "setgroups deny rc=%d", rc);
  if ((rc = write_file("/proc/self/uid_map", "0 1000 1")) != 0)
    return reportf(out, "uid_map rc=%d", rc);
  if ((rc = write_file("/proc/self/gid_map", "0 1000 1")) != 0)
    return reportf(out, "gid_map rc=%d", rc);
  if (getuid() != 0 || getgid() != 0)
    return reportf(out, "mapped ids read %d/%d", (int)getuid(), (int)getgid());
  char m[128];
  if (read_file("/proc/self/uid_map", m, sizeof(m)) <= 0)
    return reportf(out, "cannot read uid_map back");
  unsigned a, b, c;
  if (sscanf(m, "%u %u %u", &a, &b, &c) != 3 || a != 0 || b != 1000 || c != 1)
    return reportf(out, "uid_map reads back \"%s\"", m);
}

static void check_userns_map_rules(int out) {
  if (drop_to_unpriv() != 0)
    return reportf(out, "drop privileges");
  if (unshare(CLONE_NEWUSER) != 0)
    return reportf(out, "unshare errno=%d", errno);
  /* Another uid than one's own is refused without CAP_SETUID outside. */
  if (write_file("/proc/self/uid_map", "0 0 1") != -EPERM)
    return reportf(out, "mapping root as an unprivileged task was not EPERM");
  /* A gid map before setgroups is denied is refused. */
  if (write_file("/proc/self/gid_map", "0 1000 1") != -EPERM)
    return reportf(out, "gid_map before setgroups deny was not EPERM");
  if (write_file("/proc/self/uid_map", "0 1000 1") != 0)
    return reportf(out, "own uid map refused");
  if (write_file("/proc/self/uid_map", "0 1000 1") != -EPERM)
    return reportf(out, "a second uid_map write was not EPERM");
  if (write_file("/proc/self/setgroups", "deny") != 0 ||
      write_file("/proc/self/gid_map", "0 1000 1") != 0)
    return reportf(out, "gid map after setgroups deny refused");
  if (write_file("/proc/self/setgroups", "allow") != -EPERM)
    return reportf(out, "setgroups re-allowed");
  gid_t g = 0;
  if (setgroups(1, &g) == 0 || errno != EPERM)
    return reportf(out, "setgroups(2) with setgroups denied errno=%d", errno);
}

static void check_userns_ids(int out) {
  const char *path = "/tmp/m123-owned";
  unlink(path);
  pid_t kid = fork();
  if (kid == 0) {
    if (enter_userns_as_root() != 0)
      _exit(10);
    struct stat st;
    if (stat("/", &st) != 0 || st.st_uid != 65534)
      _exit(11); /* host root is not mapped */
    int fd = open(path, O_CREAT | O_WRONLY | O_EXCL, 0644);
    if (fd < 0)
      _exit(12);
    close(fd);
    if (stat(path, &st) != 0 || st.st_uid != 0 || st.st_gid != 0)
      _exit(13); /* own file reads as root inside */
    if (setuid(5) == 0 || errno != EINVAL)
      _exit(14); /* unmapped uid */
    if (chown(path, 5, -1) == 0 || errno != EINVAL)
      _exit(15);
    _exit(0);
  }
  int st = 0;
  waitpid(kid, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
    return reportf(out, "inside step %d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  struct stat hs;
  if (stat(path, &hs) != 0 || hs.st_uid != UNPRIV_UID ||
      hs.st_gid != UNPRIV_GID)
    return reportf(out, "file made inside is %d:%d outside, want 1000:1000",
                   (int)hs.st_uid, (int)hs.st_gid);
  unlink(path);
}

static void check_userns_no_host_power(int out) {
  if (enter_userns_as_root() != 0)
    return reportf(out, "enter");
  if (sethostname("m123-host", 9) == 0 || errno != EPERM)
    return reportf(out, "sethostname in the host UTS namespace errno=%d",
                   errno);
  if (mount("none", "/mnt", "tmpfs", 0, 0) == 0 || errno != EPERM)
    return reportf(out, "mount in the host mount namespace errno=%d", errno);
  if (kill(1, SIGTERM) == 0 || errno != EPERM)
    return reportf(out, "kill(1) errno=%d", errno);
}

static void check_userns_owned(int out) {
  if (enter_userns_as_root() != 0)
    return reportf(out, "enter");
  if (unshare(CLONE_NEWUTS | CLONE_NEWNS) != 0)
    return reportf(out, "unshare UTS|NS errno=%d", errno);
  if (sethostname("m123-own", 8) != 0)
    return reportf(out, "sethostname in own namespace errno=%d", errno);
  if (mount("none", "/mnt", "tmpfs", 0, 0) != 0)
    return reportf(out, "tmpfs mount in own namespace errno=%d", errno);
  if (umount("/mnt") != 0)
    return reportf(out, "umount errno=%d", errno);
  if (mount("/dev/vda", "/mnt", "ext4", MS_RDONLY, 0) == 0 || errno != EPERM)
    return reportf(out, "ext4 mount from a user namespace errno=%d", errno);
}

static void check_userns_locked_mount(int out) {
  const char *dir = "/tmp/m123-ro";
  mkdir(dir, 0755);
  /* Our own private copy of the mount table, so the read-only mount made
   * here is visible to the child and to nobody else. */
  if (unshare(CLONE_NEWNS) != 0)
    return reportf(out, "unshare NS errno=%d", errno);
  if (mount("none", dir, "tmpfs", MS_RDONLY, 0) != 0)
    return reportf(out, "read-only tmpfs errno=%d", errno);
  pid_t kid = fork();
  if (kid == 0) {
    if (enter_userns_as_root() != 0 || unshare(CLONE_NEWNS) != 0)
      _exit(10);
    if (mount(0, dir, 0, MS_REMOUNT | MS_BIND, 0) == 0)
      _exit(11); /* lifted the read-only flag */
    if (errno != EPERM)
      _exit(12);
    if (mount(0, dir, 0, MS_REMOUNT | MS_BIND | MS_RDONLY, 0) != 0)
      _exit(13); /* keeping it is allowed */
    _exit(0);
  }
  int st = 0;
  waitpid(kid, &st, 0);
  umount(dir);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
    reportf(out, "inside step %d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

static void check_userns_nsfs(int out) {
  ino_t init_user = ns_ino("/proc/self/ns/user");
  int go[2], ready[2];
  if (pipe(go) || pipe(ready))
    return reportf(out, "pipe");
  pid_t kid = fork();
  if (kid == 0) {
    close(go[1]);
    close(ready[0]);
    /* Inside: the owner is the namespace's own root, the type is a user
     * namespace, and the parent is out of reach — handing it out would be a
     * way out of the namespace. */
    int r = 0;
    if (enter_userns_as_root() != 0)
      r = 1;
    int fd = r ? -1 : open("/proc/self/ns/user", O_RDONLY);
    uid_t owner = (uid_t)-1;
    if (!r && (fd < 0 || ioctl(fd, NS_GET_OWNER_UID, &owner) != 0 ||
               owner != 0))
      r = 2;
    if (!r && ioctl(fd, NS_GET_NSTYPE) != CLONE_NEWUSER)
      r = 3;
    if (!r && (ioctl(fd, NS_GET_PARENT) >= 0 || errno != EPERM))
      r = 4;
    write(ready[1], &r, sizeof(r));
    char c;
    read(go[0], &c, 1);
    _exit(0);
  }
  close(go[0]);
  close(ready[1]);
  int r = -1;
  read(ready[0], &r, sizeof(r));
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/ns/user", (int)kid);
  int fd = open(path, O_RDONLY);
  uid_t owner = (uid_t)-1;
  int got_owner = fd >= 0 ? ioctl(fd, NS_GET_OWNER_UID, &owner) : -1;
  int pfd = fd >= 0 ? ioctl(fd, NS_GET_PARENT) : -1;
  struct stat ps;
  int parent_ok = pfd >= 0 && fstat(pfd, &ps) == 0 && ps.st_ino == init_user;
  int root_parent = pfd >= 0 ? ioctl(pfd, NS_GET_PARENT) : 0;
  int root_errno = errno;
  int uts = open("/proc/self/ns/uts", O_RDONLY);
  int ufd = uts >= 0 ? ioctl(uts, NS_GET_USERNS) : -1;
  int userns_ok = ufd >= 0 && fstat(ufd, &ps) == 0 && ps.st_ino == init_user;
  write(go[1], "g", 1);
  waitpid(kid, 0, 0);
  if (r != 0)
    return reportf(out, "inside step %d", r);
  if (got_owner != 0 || owner != UNPRIV_UID)
    return reportf(out, "outside NS_GET_OWNER_UID %d", (int)owner);
  if (!parent_ok)
    return reportf(out, "NS_GET_PARENT did not give the initial namespace");
  if (root_parent >= 0 || root_errno != EPERM)
    return reportf(out, "NS_GET_PARENT of the initial namespace errno=%d",
                   root_errno);
  if (!userns_ok)
    return reportf(out, "NS_GET_USERNS of the UTS namespace");
}

static void check_userns_setns(int out) {
  int go[2], ready[2];
  if (pipe(go) || pipe(ready))
    return reportf(out, "pipe");
  pid_t kid = fork();
  if (kid == 0) {
    close(go[1]);
    close(ready[0]);
    char c = enter_userns_as_root() == 0 ? 'y' : 'n';
    write(ready[1], &c, 1);
    read(go[0], &c, 1);
    _exit(0);
  }
  close(go[0]);
  close(ready[1]);
  char c = 0;
  read(ready[0], &c, 1);
  if (c != 'y') {
    kill(kid, SIGKILL);
    waitpid(kid, 0, 0);
    return reportf(out, "child could not enter");
  }
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/ns/user", (int)kid);
  int fd = open(path, O_RDONLY);
  int rc = fd >= 0 ? setns(fd, CLONE_NEWUSER) : -1;
  int e = errno;
  uid_t u = getuid();
  int cap = has_cap(CAP_SYS_ADMIN_BIT);
  write(go[1], "g", 1);
  waitpid(kid, 0, 0);
  if (rc != 0)
    return reportf(out, "setns errno=%d", e);
  /* Host root (kernel uid 0) is not mapped in the child's namespace. */
  if (u != 65534 || cap != 1)
    return reportf(out, "after setns uid=%d cap=%d", (int)u, cap);
}

static void *idle_thread(void *arg) {
  (void)arg;
  pause();
  return 0;
}

static void check_userns_threaded(int out) {
  pthread_t t;
  if (pthread_create(&t, 0, idle_thread, 0) != 0)
    return reportf(out, "pthread_create");
  if (unshare(CLONE_NEWUSER) == 0 || errno != EINVAL)
    return reportf(out, "threaded unshare(CLONE_NEWUSER) errno=%d", errno);
}

/* ── PID namespaces ───────────────────────────────────────────────────────── */

static pid_t clone_flags(unsigned long flags) {
  return (pid_t)syscall(SYS_clone, flags | SIGCHLD, 0, 0, 0, 0);
}

static void check_pidns_clone_init(int out) {
  int p[2];
  pipe(p);
  pid_t kid = clone_flags(CLONE_NEWPID);
  if (kid < 0)
    return reportf(out, "clone(CLONE_NEWPID) errno=%d", errno);
  if (kid == 0) {
    int ids[2] = {getpid(), getppid()};
    write(p[1], ids, sizeof(ids));
    _exit(0);
  }
  int ids[2] = {-1, -1};
  read(p[0], ids, sizeof(ids));
  int st = 0;
  pid_t w = waitpid(kid, &st, 0);
  if (ids[0] != 1 || ids[1] != 0)
    return reportf(out, "child pid %d ppid %d, want 1 and 0", ids[0], ids[1]);
  if (w != kid)
    return reportf(out, "waitpid returned %d, want %d", (int)w, (int)kid);
}

static void check_pidns_orphans(int out) {
  int p[2];
  pipe(p);
  pid_t kid = clone_flags(CLONE_NEWPID);
  if (kid == 0) {
    /* init: make a child that makes a grandchild and exits. */
    pid_t a = fork();
    if (a == 0) {
      pid_t b = fork();
      if (b == 0) {
        for (int i = 0; i < 200 && getppid() != 1; i++)
          usleep(10000);
        int pp = getppid();
        write(p[1], &pp, sizeof(pp));
        _exit(0);
      }
      _exit(0);
    }
    waitpid(a, 0, 0);
    /* The orphan is ours now: reap it. */
    int st = 0;
    pid_t w = wait(&st);
    int got = w > 0 ? 1 : 0;
    write(p[1], &got, sizeof(got));
    _exit(0);
  }
  int pp = -1, reaped = 0;
  read(p[0], &pp, sizeof(pp));
  read(p[0], &reaped, sizeof(reaped));
  waitpid(kid, 0, 0);
  if (pp != 1)
    return reportf(out, "orphan's parent is %d, want the namespace init", pp);
  if (!reaped)
    return reportf(out, "namespace init could not reap the orphan");
}

static void check_pidns_init_signals(int out) {
  int p[2];
  pipe(p);
  pid_t kid = clone_flags(CLONE_NEWPID);
  if (kid == 0) {
    pid_t c = fork();
    if (c == 0) {
      int rc = kill(1, SIGTERM); /* unhandled: ignored, but not an error */
      _exit(rc == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(c, &st, 0);
    usleep(50000);
    int alive = 1; /* we are still running, so the SIGTERM was ignored */
    int sent = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    int r[2] = {alive, sent};
    write(p[1], r, sizeof(r));
    pause();
    _exit(0);
  }
  int r[2] = {0, 0};
  read(p[0], r, sizeof(r));
  kill(kid, SIGKILL);
  int st = 0;
  waitpid(kid, &st, 0);
  if (!r[0] || !r[1])
    return reportf(out, "inside: alive=%d kill-ok=%d", r[0], r[1]);
  if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL)
    return reportf(out, "SIGKILL from outside did not kill init (st=0x%x)", st);
}

static void check_pidns_init_exit(int out) {
  int p[2];
  pipe(p);
  pid_t kid = clone_flags(CLONE_NEWPID);
  if (kid == 0) {
    pid_t c = fork();
    if (c == 0) {
      for (;;)
        pause();
    }
    int started = 1;
    write(p[1], &started, sizeof(started));
    _exit(0); /* init leaves: the member must die */
  }
  int dummy;
  read(p[0], &dummy, sizeof(dummy));
  waitpid(kid, 0, 0);
  /* The member is number 2 in the namespace and was created after `kid`:
   * no live task with that NSpid may remain once init is gone. */
  for (int tries = 0; tries < 100; tries++) {
    int survivors = 0;
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d))) {
      if (e->d_name[0] < '0' || e->d_name[0] > '9')
        continue;
      char path[64], buf[2048];
      snprintf(path, sizeof(path), "/proc/%s/status", e->d_name);
      if (read_file(path, buf, sizeof(buf)) <= 0)
        continue;
      char *ns = strstr(buf, "NSpid:");
      char *state = strstr(buf, "State:");
      if (!ns || !state || strstr(state, "Z (zombie)") == state)
        continue;
      int a = 0, b = 0;
      if (sscanf(ns, "NSpid:\t%d\t%d", &a, &b) == 2 && b == 2 && a > kid)
        survivors++;
    }
    if (d)
      closedir(d);
    if (!survivors)
      return;
    usleep(20000);
  }
  reportf(out, "a namespace member outlived its init");
}

static void check_pidns_proc(int out) {
  int p[2], go[2];
  pipe(p);
  pipe(go);
  pid_t kid = clone_flags(CLONE_NEWPID | CLONE_NEWNS);
  if (kid == 0) {
    char r[256] = "";
    mount(0, "/", 0, MS_REC | MS_PRIVATE, 0);
    if (mount("proc", "/proc", "proc", 0, 0) != 0) {
      snprintf(r, sizeof(r), "mount proc errno=%d", errno);
    } else {
      int n = 0, bad = 0;
      DIR *d = opendir("/proc");
      struct dirent *e;
      while (d && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
          continue;
        n++;
        if (atoi(e->d_name) != 1)
          bad = 1;
      }
      if (d)
        closedir(d);
      char link[64] = "";
      ssize_t l = readlink("/proc/self", link, sizeof(link) - 1);
      if (l > 0)
        link[l] = 0;
      char st[2048] = "";
      read_file("/proc/self/status", st, sizeof(st));
      char *ns = strstr(st, "NSpid:");
      int a = -1;
      if (n != 1 || bad)
        snprintf(r, sizeof(r), "proc lists %d pid dirs (bad=%d)", n, bad);
      else if (!strstr(link, "1"))
        snprintf(r, sizeof(r), "/proc/self -> %s", link);
      else if (!ns || sscanf(ns, "NSpid:\t%d", &a) != 1 || a != 1)
        snprintf(r, sizeof(r), "NSpid inside: %.40s", ns ? ns : "(none)");
      umount("/proc");
    }
    write(p[1], r, strlen(r) + 1);
    char c;
    read(go[0], &c, 1);
    _exit(0);
  }
  char r[256] = "";
  ssize_t n = read(p[0], r, sizeof(r) - 1);
  if (n > 0)
    r[n] = 0;
  /* Seen from outside, the same task has both numbers. */
  char path[64], st[2048] = "";
  snprintf(path, sizeof(path), "/proc/%d/status", (int)kid);
  read_file(path, st, sizeof(st));
  write(go[1], "g", 1);
  waitpid(kid, 0, 0);
  if (r[0])
    return reportf(out, "%s", r);
  char *ns = strstr(st, "NSpid:");
  int a = 0, b = 0;
  if (!ns || sscanf(ns, "NSpid:\t%d\t%d", &a, &b) != 2 || a != kid || b != 1)
    return reportf(out, "outside NSpid: %.40s", ns ? ns : "(none)");
}

static void check_pidns_userns_clone(int out) {
  if (drop_to_unpriv() != 0)
    return reportf(out, "drop");
  int p[2];
  pipe(p);
  pid_t kid = clone_flags(CLONE_NEWUSER | CLONE_NEWPID);
  if (kid < 0)
    return reportf(out, "clone errno=%d", errno);
  if (kid == 0) {
    int r[3] = {getpid(), (int)getuid(), has_cap(CAP_SYS_ADMIN_BIT)};
    write(p[1], r, sizeof(r));
    _exit(0);
  }
  int r[3] = {0, 0, 0};
  read(p[0], r, sizeof(r));
  waitpid(kid, 0, 0);
  if (r[0] != 1 || r[1] != 65534 || r[2] != 1)
    return reportf(out, "child pid %d uid %d cap %d", r[0], r[1], r[2]);
}

/* ── IPC namespaces ───────────────────────────────────────────────────────── */

static void check_ipcns_sysv(int out) {
  key_t key = 0x12301230;
  int host = shmget(key, 4096, IPC_CREAT | 0600);
  if (host < 0)
    return reportf(out, "host shmget errno=%d", errno);
  pid_t kid = fork();
  if (kid == 0) {
    if (unshare(CLONE_NEWIPC) != 0)
      _exit(10);
    if (shmget(key, 0, 0) >= 0 || errno != ENOENT)
      _exit(11); /* the host's key must not be found */
    struct shmid_ds ds;
    if (shmctl(host, IPC_STAT, &ds) == 0)
      _exit(12); /* nor its id */
    int own = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600);
    if (own < 0)
      _exit(13);
    char buf[4096];
    int lines = 0;
    if (read_file("/proc/sysvipc/shm", buf, sizeof(buf)) > 0)
      for (char *q = buf; (q = strchr(q, '\n')); q++)
        lines++;
    shmctl(own, IPC_RMID, 0);
    _exit(lines == 2 ? 0 : 14); /* header + our own segment */
  }
  int st = 0;
  waitpid(kid, &st, 0);
  struct shmid_ds ds;
  int still = shmctl(host, IPC_STAT, &ds) == 0;
  shmctl(host, IPC_RMID, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
    return reportf(out, "inside step %d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  if (!still)
    return reportf(out, "host segment disappeared");
}

static void check_ipcns_mqueue(int out) {
  struct mq_attr attr = {0, 4, 64, 0, {0}};
  mqd_t q = mq_open("/m123q", O_CREAT | O_RDWR, 0600, &attr);
  if (q == (mqd_t)-1)
    return reportf(out, "host mq_open errno=%d", errno);
  const char *dir = "/tmp/m123-mq";
  mkdir(dir, 0755);
  pid_t kid = fork();
  if (kid == 0) {
    if (unshare(CLONE_NEWIPC | CLONE_NEWNS) != 0)
      _exit(10);
    mount(0, "/", 0, MS_REC | MS_PRIVATE, 0);
    if (mq_open("/m123q", O_RDWR) != (mqd_t)-1 || errno != ENOENT)
      _exit(11);
    mqd_t own = mq_open("/m123own", O_CREAT | O_RDWR, 0600, &attr);
    if (own == (mqd_t)-1)
      _exit(12);
    if (mount("mqueue", dir, "mqueue", 0, 0) != 0)
      _exit(13);
    struct stat st;
    int sees_own = stat("/tmp/m123-mq/m123own", &st) == 0;
    int sees_host = stat("/tmp/m123-mq/m123q", &st) == 0;
    umount(dir);
    mq_unlink("/m123own");
    _exit(sees_own && !sees_host ? 0 : 14);
  }
  int st = 0;
  waitpid(kid, &st, 0);
  mq_close(q);
  mq_unlink("/m123q");
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
    return reportf(out, "inside step %d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

static volatile sig_atomic_t g_notified;
static void on_usr1(int s) {
  (void)s;
  g_notified = 1;
}

static void check_mqueue_semantics(int out) {
  mq_unlink("/m123sem");
  struct mq_attr attr = {0, 3, 32, 0, {0}};
  mqd_t q = mq_open("/m123sem", O_CREAT | O_EXCL | O_RDWR | O_NONBLOCK, 0600,
                    &attr);
  if (q == (mqd_t)-1)
    return reportf(out, "mq_open errno=%d", errno);
  char buf[32];
  unsigned prio;
  if (mq_receive(q, buf, sizeof(buf), &prio) >= 0 || errno != EAGAIN)
    return reportf(out, "empty nonblocking receive errno=%d", errno);
  struct sigevent ev;
  memset(&ev, 0, sizeof(ev));
  ev.sigev_notify = SIGEV_SIGNAL;
  ev.sigev_signo = SIGUSR1;
  signal(SIGUSR1, on_usr1);
  if (mq_notify(q, &ev) != 0)
    return reportf(out, "mq_notify errno=%d", errno);
  if (mq_send(q, "low", 3, 1) || mq_send(q, "high", 4, 9) ||
      mq_send(q, "mid", 3, 5))
    return reportf(out, "mq_send errno=%d", errno);
  for (int i = 0; i < 100 && !g_notified; i++)
    usleep(1000);
  if (!g_notified)
    return reportf(out, "mq_notify signal never arrived");
  if (mq_send(q, "x", 1, 0) == 0 || errno != EAGAIN)
    return reportf(out, "full nonblocking send errno=%d", errno);
  struct pollfd pfd = {q, POLLIN, 0};
  if (poll(&pfd, 1, 0) != 1 || !(pfd.revents & POLLIN))
    return reportf(out, "poll did not report POLLIN");
  const char *want[3] = {"high", "mid", "low"};
  for (int i = 0; i < 3; i++) {
    ssize_t n = mq_receive(q, buf, sizeof(buf), &prio);
    if (n < 0 || (size_t)n != strlen(want[i]) || memcmp(buf, want[i], n))
      return reportf(out, "receive %d got %zd", i, n);
  }
  if (mq_receive(q, buf, 8, &prio) >= 0 || errno != EMSGSIZE)
    return reportf(out, "short buffer errno=%d", errno);
  struct mq_attr a2 = {0, 0, 0, 0, {0}};
  mq_setattr(q, &a2, 0); /* clear O_NONBLOCK */
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += 50000000;
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000;
  }
  if (mq_timedreceive(q, buf, sizeof(buf), &prio, &ts) >= 0 ||
      errno != ETIMEDOUT)
    return reportf(out, "timed receive errno=%d", errno);
  mq_close(q);
  if (mq_unlink("/m123sem") != 0)
    return reportf(out, "unlink errno=%d", errno);
}

static void check_shm_rmid_attached(int out) {
  key_t key = 0x12301231;
  int id = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600);
  if (id < 0)
    return reportf(out, "shmget errno=%d", errno);
  char *p = shmat(id, 0, 0);
  if (p == (char *)-1)
    return reportf(out, "shmat errno=%d", errno);
  strcpy(p, "still-here");
  if (shmctl(id, IPC_RMID, 0) != 0)
    return reportf(out, "IPC_RMID errno=%d", errno);
  if (shmget(key, 0, 0) >= 0)
    return reportf(out, "removed key still found");
  if (strcmp(p, "still-here") != 0)
    return reportf(out, "attached memory lost");
  if (shmdt(p) != 0)
    return reportf(out, "shmdt errno=%d", errno);
  struct shmid_ds ds;
  if (shmctl(id, IPC_STAT, &ds) == 0)
    return reportf(out, "segment outlived its last detach");
}

/* ── cgroup namespaces ────────────────────────────────────────────────────── */

static void check_cgroupns_root(int out) {
  const char *mnt = "/tmp/m123-cg";
  mkdir(mnt, 0755);
  if (unshare(CLONE_NEWNS) != 0)
    return reportf(out, "unshare NS errno=%d", errno);
  mount(0, "/", 0, MS_REC | MS_PRIVATE, 0);
  if (mount("cgroup2", mnt, "cgroup2", 0, 0) != 0)
    return reportf(out, "mount cgroup2 errno=%d", errno);
  char sub[128];
  snprintf(sub, sizeof(sub), "%s/m123sub", mnt);
  mkdir(sub, 0755);
  int p[2];
  pipe(p);
  pid_t kid = fork();
  if (kid == 0) {
    char procs[160], me[16];
    snprintf(procs, sizeof(procs), "%s/cgroup.procs", sub);
    snprintf(me, sizeof(me), "%d", (int)getpid());
    char r[256] = "";
    if (write_file(procs, me) != 0) {
      snprintf(r, sizeof(r), "join sub-cgroup");
    } else if (unshare(CLONE_NEWCGROUP) != 0) {
      snprintf(r, sizeof(r), "unshare CGROUP errno=%d", errno);
    } else {
      char cg[256] = "";
      read_file("/proc/self/cgroup", cg, sizeof(cg));
      if (strcmp(cg, "0::/\n") != 0)
        snprintf(r, sizeof(r), "inside /proc/self/cgroup is \"%s\"", cg);
    }
    write(p[1], r, strlen(r));
    close(p[1]);
    pause();
    _exit(0);
  }
  close(p[1]);
  char r[256] = "";
  ssize_t n = read(p[0], r, sizeof(r) - 1);
  if (n > 0)
    r[n] = 0;
  char path[64], cg[256] = "";
  snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)kid);
  read_file(path, cg, sizeof(cg));
  kill(kid, SIGKILL);
  waitpid(kid, 0, 0);
  rmdir(sub);
  umount(mnt);
  if (n > 0)
    return reportf(out, "%s", r);
  if (strcmp(cg, "0::/m123sub\n") != 0)
    return reportf(out, "outside view \"%s\"", cg);
}

/* ── time namespaces ──────────────────────────────────────────────────────── */

static long long ts_ns(const struct timespec *t) {
  return (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
}

static void check_timens_offsets(int out) {
  if (unshare(CLONE_NEWTIME) != 0)
    return reportf(out, "unshare TIME errno=%d", errno);
  if (write_file("/proc/self/timens_offsets",
                 "monotonic 86400 0\nboottime 172800 0\n") != 0)
    return reportf(out, "write timens_offsets errno=%d", errno);
  int p[2];
  pipe(p);
  struct timespec m0, b0, r0;
  clock_gettime(CLOCK_MONOTONIC, &m0);
  clock_gettime(CLOCK_BOOTTIME, &b0);
  clock_gettime(CLOCK_REALTIME, &r0);
  pid_t kid = fork();
  if (kid == 0) {
    struct timespec v[4];
    clock_gettime(CLOCK_MONOTONIC, &v[0]);            /* through the vDSO */
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &v[1]); /* and the call */
    clock_gettime(CLOCK_BOOTTIME, &v[2]);
    clock_gettime(CLOCK_REALTIME, &v[3]);
    write(p[1], v, sizeof(v));
    _exit(0);
  }
  struct timespec v[4];
  read(p[0], v, sizeof(v));
  waitpid(kid, 0, 0);
  long long day = 86400LL * 1000000000LL, slack = 5LL * 1000000000LL;
  long long dm = ts_ns(&v[0]) - ts_ns(&m0), ds = ts_ns(&v[1]) - ts_ns(&m0);
  long long db = ts_ns(&v[2]) - ts_ns(&b0), dr = ts_ns(&v[3]) - ts_ns(&r0);
  if (dm < day || dm > day + slack)
    return reportf(out, "vDSO monotonic shift %lld ns", dm);
  if (ds < day || ds > day + slack)
    return reportf(out, "syscall monotonic shift %lld ns", ds);
  if (db < 2 * day || db > 2 * day + slack)
    return reportf(out, "boottime shift %lld ns", db);
  if (dr < 0 || dr > slack)
    return reportf(out, "realtime moved by %lld ns", dr);
  /* The creator itself stays in its original namespace. */
  struct timespec m1;
  clock_gettime(CLOCK_MONOTONIC, &m1);
  if (ts_ns(&m1) - ts_ns(&m0) > slack)
    return reportf(out, "the unsharing task's own clock moved");
}

static void check_timens_locked(int out) {
  if (unshare(CLONE_NEWTIME) != 0)
    return reportf(out, "unshare TIME errno=%d", errno);
  pid_t kid = fork();
  if (kid == 0)
    _exit(0);
  waitpid(kid, 0, 0);
  if (write_file("/proc/self/timens_offsets", "monotonic 5 0\n") != -EACCES)
    return reportf(out, "offsets writable after a task entered");
}

/* ── handles ──────────────────────────────────────────────────────────────── */

static void check_ns_links(int out) {
  const char *kinds[] = {"user", "ipc", "cgroup", "time", "uts", "mnt",
                         "pid",  "net", "pid_for_children",
                         "time_for_children"};
  for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    char path[64], link[64];
    snprintf(path, sizeof(path), "/proc/self/ns/%s", kinds[i]);
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n <= 0)
      return reportf(out, "readlink %s errno=%d", path, errno);
    link[n] = 0;
    char kind[32];
    unsigned long ino = 0;
    if (sscanf(link, "%31[a-z]:[%lu]", kind, &ino) != 2)
      return reportf(out, "%s -> \"%s\"", path, link);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_ino != ino)
      return reportf(out, "%s stat inode %lu, link %lu", path,
                     (unsigned long)st.st_ino, ino);
  }
  if (ns_ino("/proc/self/ns/user") != 4026531837UL)
    return reportf(out, "initial user namespace inode");
}

static void check_ns_handle_keeps_alive(int out) {
  int p[2];
  pipe(p);
  pid_t kid = fork();
  if (kid == 0) {
    unshare(CLONE_NEWUTS);
    sethostname("m123-kept", 9);
    write(p[1], "r", 1);
    pause();
    _exit(0);
  }
  char c;
  read(p[0], &c, 1);
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/ns/uts", (int)kid);
  int fd = open(path, O_RDONLY);
  kill(kid, SIGKILL);
  waitpid(kid, 0, 0);
  if (fd < 0)
    return reportf(out, "open handle errno=%d", errno);
  if (setns(fd, CLONE_NEWUTS) != 0)
    return reportf(out, "setns into a memberless namespace errno=%d", errno);
  char host[64] = "";
  gethostname(host, sizeof(host));
  if (strcmp(host, "m123-kept") != 0)
    return reportf(out, "hostname in kept namespace \"%s\"", host);
}

int main(void) {
  marker("M123-SMOKE: start\n");
  run("userns-unpriv", check_userns_unpriv);
  run("userns-map-rules", check_userns_map_rules);
  run("userns-ids", check_userns_ids);
  run("userns-no-host-power", check_userns_no_host_power);
  run("userns-owned", check_userns_owned);
  run("userns-locked-mount", check_userns_locked_mount);
  run("userns-nsfs", check_userns_nsfs);
  run("userns-setns", check_userns_setns);
  run("userns-threaded", check_userns_threaded);
  run("pidns-clone-init", check_pidns_clone_init);
  run("pidns-orphans", check_pidns_orphans);
  run("pidns-init-signals", check_pidns_init_signals);
  run("pidns-init-exit", check_pidns_init_exit);
  run("pidns-proc", check_pidns_proc);
  run("pidns-userns-clone", check_pidns_userns_clone);
  run("ipcns-sysv", check_ipcns_sysv);
  run("ipcns-mqueue", check_ipcns_mqueue);
  run("mqueue-semantics", check_mqueue_semantics);
  run("shm-rmid-attached", check_shm_rmid_attached);
  run("cgroupns-root", check_cgroupns_root);
  run("timens-offsets", check_timens_offsets);
  run("timens-locked", check_timens_locked);
  run("ns-links", check_ns_links);
  run("ns-handle-keeps-alive", check_ns_handle_keeps_alive);
  marker(g_fail ? "M123-SMOKE: done with failures\n" : "M123-SMOKE: done\n");
  return 0;
}
