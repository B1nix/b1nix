/* SPDX-License-Identifier: GPL-2.0-only */
/* iso9660 smoke: the imported Linux isofs, through every way a live medium is
 * mounted.
 *
 * The image (/isofs-test.img, built by tools/image/mk-isofs-test-image.sh with
 * xorriso, Rock Ridge and Joliet both present) holds only content this program
 * can recompute, so every comparison is against a value derived here, never
 * against a second read through the same driver. A marker is printed only
 * after every check behind it passed.
 *
 *   fsmount-loop   the image on a loop device (512-byte sectors), mounted with
 *                  the new mount API (fsopen/fsconfig/fsmount/move_mount), as
 *                  util-linux mount(8) does it: Rock Ridge long and mixed-case
 *                  names, a nested directory, a symlink, a 5 MiB file read in
 *                  odd-sized pieces and at scattered offsets, a zisofs file
 *                  read back inflated, statfs's magic, /proc/mounts' "ro", and
 *                  a refused write
 *   mount-loop     the same, through mount(2) with no MS_RDONLY: an ISO 9660
 *                  mount is read-only whatever the caller asked
 *   loop-on-iso    a file ON the mounted ISO attached to a second loop device
 *                  and read through it, which is how a live system reaches the
 *                  root image it carries (/live/root.img)
 *   isofs-alias    the type also answers to its driver name, "isofs"
 *   cdrom          the boot CD on the AHCI ATAPI drive (2048-byte sectors),
 *                  found as the block device whose logical block size is
 *                  2048 and mounted with BusyBox mount -t iso9660 -o ro as an
 *                  initramfs does: Limine's config found by its Rock Ridge
 *                  name and the kernel ELF read to its last section header.
 *                  x86_64 only -- the aarch64 machine has no CD drive, which
 *                  is said as a skip
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_fsopen
#define SYS_fsopen 430
#endif
#ifndef SYS_fsconfig
#define SYS_fsconfig 431
#endif
#ifndef SYS_fsmount
#define SYS_fsmount 432
#endif
#ifndef SYS_move_mount
#define SYS_move_mount 429
#endif
#define FSCONFIG_SET_FLAG 0
#define FSCONFIG_SET_STRING 1
#define FSCONFIG_CMD_CREATE 6
#define MOVE_MOUNT_F_EMPTY_PATH 4

#define LOOP_SET_FD 0x4C00
#define LOOP_CLR_FD 0x4C01
#define LOOP_CTL_GET_FREE 0x4C82

#define ISOFS_SUPER_MAGIC 0x9660

#define IMG "/isofs-test.img"
#define LONG_NAME                                                              \
  "a-file-name-far-longer-than-iso9660-or-joliet-can-hold-rock-ridge-only.txt"
#define BIG_SIZE (5u * 1024u * 1024u)
#define ZLINES 65536u
#define ZLINE_LEN 21u /* "zisofs line %08d\n" */

static int failures;

static void ok(const char *name) { printf("ISOFS-SMOKE: ok %s\n", name); }
static void fail(const char *name, const char *why) {
  printf("ISOFS-SMOKE: FAIL %s (%s)\n", name, why);
  failures++;
}
static void skip(const char *name, const char *why) {
  printf("ISOFS-SMOKE: skip %s (%s)\n", name, why);
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static int read_all(const char *path, char *buf, size_t cap, size_t *len) {
  int fd = open(path, O_RDONLY);
  size_t got = 0;

  if (fd < 0)
    return -1;
  for (;;) {
    ssize_t r = read(fd, buf + got, cap - got);
    if (r < 0) {
      close(fd);
      return -1;
    }
    if (r == 0)
      break;
    got += (size_t)r;
    if (got == cap)
      break;
  }
  close(fd);
  *len = got;
  return 0;
}

static int file_is(const char *path, const char *want) {
  char buf[256];
  size_t n = 0;

  if (read_all(path, buf, sizeof(buf), &n) != 0)
    return 0;
  return n == strlen(want) && memcmp(buf, want, n) == 0;
}

static uint32_t big_word(uint32_t k) { return k * 2654435761u; }

/* Check `len` bytes that sit at byte `off` of big.bin. */
static int big_matches(const unsigned char *p, uint64_t off, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uint64_t at = off + i;
    uint32_t w = big_word((uint32_t)(at / 4));
    unsigned char want = (unsigned char)(w >> (8 * (at % 4)));
    if (p[i] != want)
      return 0;
  }
  return 1;
}

static int dir_has(const char *dir, const char *name, unsigned char type) {
  DIR *d = opendir(dir);
  struct dirent *e;
  int hit = 0;

  if (!d)
    return 0;
  while ((e = readdir(d)))
    if (strcmp(e->d_name, name) == 0 &&
        (e->d_type == DT_UNKNOWN || e->d_type == type)) {
      hit = 1;
      break;
    }
  closedir(d);
  return hit;
}

/* The /proc/mounts row whose mount point is `mnt`: its type and options. */
static int mounts_row(const char *mnt, char *type, size_t tcap, char *opts,
                      size_t ocap) {
  FILE *f = fopen("/proc/mounts", "r");
  char line[512];
  int found = 0;

  if (!f)
    return 0;
  while (fgets(line, sizeof(line), f)) {
    char src[256], dir[256], t[64], o[256];
    if (sscanf(line, "%255s %255s %63s %255s", src, dir, t, o) != 4)
      continue;
    if (strcmp(dir, mnt) != 0)
      continue;
    snprintf(type, tcap, "%s", t);
    snprintf(opts, ocap, "%s", o);
    found = 1; /* the last row wins: that is the mount on top */
  }
  fclose(f);
  return found;
}

static int opts_have(const char *opts, const char *word) {
  size_t wl = strlen(word);
  const char *p = opts;

  while (*p) {
    const char *c = strchr(p, ',');
    size_t l = c ? (size_t)(c - p) : strlen(p);
    if (l == wl && strncmp(p, word, wl) == 0)
      return 1;
    if (!c)
      break;
    p = c + 1;
  }
  return 0;
}

/* ── the content checks, shared by every way of mounting the image ───────── */

static int verify_image(const char *mnt, const char *label) {
  char path[512], why[256], buf[256];
  struct stat st;
  int bad = 0;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      snprintf(why, sizeof(why), __VA_ARGS__);                                 \
      fail(label, why);                                                        \
      bad = 1;                                                                 \
    }                                                                          \
  } while (0)

  snprintf(path, sizeof(path), "%s/hello.txt", mnt);
  CHECK(file_is(path, "isofs test volume\n"), "hello.txt does not read back");

  /* Rock Ridge: a 74-character name, found by lookup AND listed by readdir
   * under the same spelling. Plain ISO 9660 would show "A_FILE_N.TXT;1" and
   * Joliet would cut it at 64 characters. */
  snprintf(path, sizeof(path), "%s/" LONG_NAME, mnt);
  CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode),
        "stat of the Rock Ridge long name failed (errno %d)", errno);
  CHECK(file_is(path, "a name only Rock Ridge can hold\n"),
        "the long-named file does not read back");
  CHECK(dir_has(mnt, LONG_NAME, DT_REG), "readdir does not list the long name");
  CHECK(dir_has(mnt, "MixedCase.Name", DT_REG),
        "readdir does not list MixedCase.Name with its case");
  snprintf(path, sizeof(path), "%s/MixedCase.Name", mnt);
  CHECK(file_is(path, "case kept\n"), "MixedCase.Name does not read back");

  /* Three directory levels, each a lookup of its own. */
  snprintf(path, sizeof(path), "%s/dir/nested", mnt);
  CHECK(stat(path, &st) == 0 && S_ISDIR(st.st_mode), "dir/nested is not a directory");
  CHECK(dir_has(path, "deeper", DT_DIR), "readdir of dir/nested lacks deeper");
  snprintf(path, sizeof(path), "%s/dir/nested/deeper/leaf.txt", mnt);
  CHECK(file_is(path, "three levels down\n"), "the nested leaf does not read back");

  /* The Rock Ridge symlink: its target as recorded, and followed. */
  snprintf(path, sizeof(path), "%s/link", mnt);
  CHECK(lstat(path, &st) == 0 && S_ISLNK(st.st_mode), "link is not a symlink");
  {
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n >= 0)
      buf[n] = '\0';
    CHECK(n >= 0 && strcmp(buf, "dir/nested/deeper/leaf.txt") == 0,
          "readlink gave '%s'", n >= 0 ? buf : "(error)");
  }
  CHECK(file_is(path, "three levels down\n"), "reading through the symlink failed");

  /* 5 MiB in 65537-byte reads: every read straddles a 2048-byte block and a
   * page, and every word of the file is distinct. */
  snprintf(path, sizeof(path), "%s/big.bin", mnt);
  CHECK(stat(path, &st) == 0 && st.st_size == (off_t)BIG_SIZE,
        "big.bin is %lld bytes, not %u", (long long)st.st_size, BIG_SIZE);
  {
    int fd = open(path, O_RDONLY);
    unsigned char *chunk = malloc(65537);
    uint64_t total = 0;
    int match = fd >= 0 && chunk;

    while (match) {
      ssize_t r = read(fd, chunk, 65537);
      if (r < 0) {
        match = 0;
        break;
      }
      if (r == 0)
        break;
      if (!big_matches(chunk, total, (size_t)r))
        match = 0;
      total += (uint64_t)r;
    }
    CHECK(match && total == BIG_SIZE,
          "big.bin streamed %llu bytes, content %s", (unsigned long long)total,
          match ? "right" : "wrong");
    /* And scattered: the last bytes, a run across the 4 MiB mark, a few
     * unaligned spots, in an order no read-ahead would guess. */
    static const uint64_t offs[] = {BIG_SIZE - 7, 4u * 1024 * 1024 - 3001,
                                    2047, 1234567, 3 * 2048 + 1, 0};
    for (unsigned i = 0; match && i < sizeof(offs) / sizeof(offs[0]); i++) {
      size_t want = (size_t)((BIG_SIZE - offs[i]) < 6000 ? BIG_SIZE - offs[i] : 6000);
      ssize_t r = pread(fd, chunk, want, (off_t)offs[i]);
      CHECK(r == (ssize_t)want && big_matches(chunk, offs[i], want),
            "big.bin pread at %llu came back wrong", (unsigned long long)offs[i]);
    }
    if (fd >= 0)
      close(fd);
    free(chunk);
  }

  /* zisofs: stored compressed, read back as the original text. */
  snprintf(path, sizeof(path), "%s/zlines.txt", mnt);
  CHECK(stat(path, &st) == 0 && st.st_size == (off_t)(ZLINES * ZLINE_LEN),
        "zlines.txt is %lld bytes, not %u (zisofs size not decoded)",
        (long long)st.st_size, ZLINES * ZLINE_LEN);
  {
    size_t cap = ZLINES * ZLINE_LEN + 1, n = 0;
    char *z = malloc(cap);
    int match = z && read_all(path, z, cap, &n) == 0 && n == ZLINES * ZLINE_LEN;
    for (unsigned i = 0; match && i < ZLINES; i++) {
      char want[32];
      snprintf(want, sizeof(want), "zisofs line %08u\n", i);
      if (memcmp(z + (size_t)i * ZLINE_LEN, want, ZLINE_LEN) != 0)
        match = 0;
    }
    CHECK(match, "zlines.txt did not inflate to the original lines");
    free(z);
  }

  {
    struct statfs sf;
    CHECK(statfs(mnt, &sf) == 0 && (unsigned long)sf.f_type == ISOFS_SUPER_MAGIC,
          "statfs f_type is 0x%lx, not 0x9660", (unsigned long)sf.f_type);
  }
  {
    char type[64] = "", opts[256] = "";
    CHECK(mounts_row(mnt, type, sizeof(type), opts, sizeof(opts)) &&
              strcmp(type, "iso9660") == 0 && opts_have(opts, "ro"),
          "/proc/mounts row is '%s %s', want iso9660 and ro", type, opts);
  }
  {
    snprintf(path, sizeof(path), "%s/new-file", mnt);
    errno = 0;
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    int e = errno;
    if (fd >= 0)
      close(fd);
    CHECK(fd < 0 && e == EROFS, "creating a file gave fd %d errno %d, not EROFS",
          fd, e);
  }
#undef CHECK
  return bad ? -1 : 0;
}

/* ── loop device ─────────────────────────────────────────────────────────── */

static int loop_attach(char *dev, size_t cap) {
  int img = open(IMG, O_RDONLY);
  int ctl, idx, loop;

  if (img < 0)
    return -1;
  ctl = open("/dev/loop-control", O_RDWR);
  idx = ctl >= 0 ? ioctl(ctl, LOOP_CTL_GET_FREE, 0) : -1;
  if (ctl >= 0)
    close(ctl);
  if (idx < 0) {
    close(img);
    return -1;
  }
  snprintf(dev, cap, "/dev/loop%d", idx);
  loop = open(dev, O_RDONLY);
  if (loop < 0 || ioctl(loop, LOOP_SET_FD, img) != 0) {
    if (loop >= 0)
      close(loop);
    close(img);
    return -1;
  }
  close(img);
  return loop;
}

static void t_fsmount(const char *dev) {
  const char *mnt = "/tmp/isofs-fsmount";
  char why[160];
  int fsfd, mfd;

  mkdir(mnt, 0755);
  fsfd = (int)syscall(SYS_fsopen, "iso9660", 0);
  if (fsfd < 0) {
    snprintf(why, sizeof(why), "fsopen(iso9660) errno %d", errno);
    fail("fsmount-loop", why);
    return;
  }
  if (syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_STRING, "source", dev, 0) != 0 ||
      syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_FLAG, "ro", NULL, 0) != 0 ||
      syscall(SYS_fsconfig, fsfd, FSCONFIG_CMD_CREATE, NULL, NULL, 0) != 0) {
    snprintf(why, sizeof(why), "fsconfig errno %d", errno);
    fail("fsmount-loop", why);
    close(fsfd);
    return;
  }
  mfd = (int)syscall(SYS_fsmount, fsfd, 0, 0);
  if (mfd < 0) {
    snprintf(why, sizeof(why), "fsmount errno %d", errno);
    fail("fsmount-loop", why);
    close(fsfd);
    return;
  }
  /* Before it has a place, the descriptor already reaches the filesystem:
   * lookups go through the imported driver, not through a tree built for a
   * path. */
  {
    int fd = openat(mfd, "dir/nested/deeper/leaf.txt", O_RDONLY);
    char b[32] = {0};
    ssize_t n = fd >= 0 ? read(fd, b, sizeof(b) - 1) : -1;
    if (fd >= 0)
      close(fd);
    if (n != 18 || strcmp(b, "three levels down\n") != 0) {
      fail("fsmount-loop", "openat through the detached mount did not read the leaf");
      close(mfd);
      close(fsfd);
      return;
    }
  }
  if (syscall(SYS_move_mount, mfd, "", AT_FDCWD, mnt, MOVE_MOUNT_F_EMPTY_PATH) != 0) {
    snprintf(why, sizeof(why), "move_mount errno %d", errno);
    fail("fsmount-loop", why);
    close(mfd);
    close(fsfd);
    return;
  }
  close(mfd);
  close(fsfd);
  int rc = verify_image(mnt, "fsmount-loop");
  if (umount(mnt) != 0) {
    snprintf(why, sizeof(why), "umount errno %d", errno);
    fail("fsmount-loop", why);
    rc = -1;
  }
  rmdir(mnt);
  if (rc == 0)
    ok("fsmount-loop");
}

/* A loop device over big.bin on the mounted image: the block device's bytes
 * have to be the file's, at the start, across the 4 MiB mark and at the end. */
static void t_loop_on_iso(const char *mnt) {
  char path[256], dev[32], why[160];
  unsigned char buf[6000];
  int img, ctl, idx, loop;

  snprintf(path, sizeof(path), "%s/big.bin", mnt);
  img = open(path, O_RDONLY);
  ctl = open("/dev/loop-control", O_RDWR);
  idx = ctl >= 0 ? ioctl(ctl, LOOP_CTL_GET_FREE, 0) : -1;
  if (ctl >= 0)
    close(ctl);
  if (img < 0 || idx < 0) {
    fail("loop-on-iso", img < 0 ? "big.bin did not open" : "no free loop device");
    if (img >= 0)
      close(img);
    return;
  }
  snprintf(dev, sizeof(dev), "/dev/loop%d", idx);
  loop = open(dev, O_RDONLY);
  if (loop < 0 || ioctl(loop, LOOP_SET_FD, img) != 0) {
    snprintf(why, sizeof(why), "attaching big.bin to %s failed (errno %d)", dev, errno);
    fail("loop-on-iso", why);
    if (loop >= 0)
      close(loop);
    close(img);
    return;
  }
  close(img);

  static const uint64_t offs[] = {0, 4u * 1024 * 1024 - 2048, BIG_SIZE - sizeof(buf)};
  int good = 1;
  for (unsigned i = 0; good && i < sizeof(offs) / sizeof(offs[0]); i++) {
    int fd = open(dev, O_RDONLY);
    ssize_t r = fd >= 0 ? pread(fd, buf, sizeof(buf), (off_t)offs[i]) : -1;
    if (fd >= 0)
      close(fd);
    if (r != (ssize_t)sizeof(buf) || !big_matches(buf, offs[i], sizeof(buf))) {
      snprintf(why, sizeof(why), "%s at %llu read %zd bytes, content %s", dev,
               (unsigned long long)offs[i], r, r > 0 ? "wrong" : "missing");
      fail("loop-on-iso", why);
      good = 0;
    }
  }
  ioctl(loop, LOOP_CLR_FD, 0);
  close(loop);
  if (good)
    ok("loop-on-iso");
}

static void t_mount2(const char *dev) {
  const char *mnt = "/tmp/isofs-mount";
  char why[160];

  mkdir(mnt, 0755);
  /* No MS_RDONLY: a CD mounts read-only regardless, and verify_image checks
   * that it was recorded so and that a write is refused. */
  if (mount(dev, mnt, "iso9660", 0, NULL) != 0) {
    snprintf(why, sizeof(why), "mount(2) errno %d", errno);
    fail("mount-loop", why);
    rmdir(mnt);
    return;
  }
  int rc = verify_image(mnt, "mount-loop");
  t_loop_on_iso(mnt);
  if (umount(mnt) != 0) {
    snprintf(why, sizeof(why), "umount errno %d", errno);
    fail("mount-loop", why);
    rc = -1;
  }
  rmdir(mnt);
  if (rc == 0)
    ok("mount-loop");
}

static void t_alias(const char *dev) {
  const char *mnt = "/tmp/isofs-alias";
  char why[160], type[64] = "", opts[256] = "";

  mkdir(mnt, 0755);
  if (mount(dev, mnt, "isofs", MS_RDONLY, NULL) != 0) {
    snprintf(why, sizeof(why), "mount -t isofs errno %d", errno);
    fail("isofs-alias", why);
    rmdir(mnt);
    return;
  }
  int good = file_is("/tmp/isofs-alias/hello.txt", "isofs test volume\n") &&
             mounts_row(mnt, type, sizeof(type), opts, sizeof(opts)) &&
             opts_have(opts, "ro");
  umount(mnt);
  rmdir(mnt);
  if (good)
    ok("isofs-alias");
  else
    fail("isofs-alias", "the isofs-typed mount did not serve the image read-only");
}

/* ── the boot CD ─────────────────────────────────────────────────────────── */

static int run(char *const argv[]) {
  pid_t pid = fork();
  int status = 0;

  if (pid < 0)
    return -1;
  if (pid == 0) {
    execv(argv[0], argv);
    _exit(127);
  }
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
    return -1;
  return WEXITSTATUS(status);
}

/* The CD drive: the block device with 2048-byte logical blocks. Found by
 * that property rather than by name, which is the drive's business. */
static int find_cdrom(char *dev, size_t cap) {
  DIR *d = opendir("/sys/block");
  struct dirent *e;
  int found = 0;

  if (!d)
    return 0;
  while (!found && (e = readdir(d))) {
    char path[300], v[32];
    size_t n = 0;
    struct stat st;

    if (e->d_name[0] == '.')
      continue;
    snprintf(path, sizeof(path), "/sys/block/%s/queue/logical_block_size",
             e->d_name);
    if (read_all(path, v, sizeof(v) - 1, &n) != 0)
      continue;
    v[n] = '\0';
    if (strtoul(v, 0, 10) != 2048)
      continue;
    snprintf(dev, cap, "/dev/%s", e->d_name);
    found = stat(dev, &st) == 0 && S_ISBLK(st.st_mode);
  }
  closedir(d);
  return found;
}

static void t_cdrom(void) {
  const char *mnt = "/tmp/isofs-cdrom";
  char why[200], buf[8192], dev[64];
  struct stat st;
  size_t n = 0;

  if (!find_cdrom(dev, sizeof(dev))) {
    skip("cdrom", "no CD drive in this machine");
    return;
  }
  /* The device itself first: the primary volume descriptor at 32 KiB, read
   * raw, carries the ISO 9660 signature. */
  {
    int fd = open(dev, O_RDONLY);
    ssize_t r = fd >= 0 ? pread(fd, buf, 2048, 16 * 2048) : -1;
    int e = errno;
    if (fd >= 0)
      close(fd);
    if (r != 2048 || memcmp(buf + 1, "CD001", 5) != 0) {
      snprintf(why, sizeof(why), "raw read of %s's volume descriptor: %zd (errno %d)",
               dev, r, e);
      fail("cdrom", why);
      return;
    }
  }
  mkdir(mnt, 0755);
  /* What an initramfs runs to find its live medium. */
  char *const argv[] = {(char *)"/bin/mount", (char *)"-t", (char *)"iso9660",
                        (char *)"-o", (char *)"ro", dev, (char *)mnt, 0};
  int rc = run(argv);
  if (rc != 0) {
    snprintf(why, sizeof(why), "mount -t iso9660 -o ro %s exited %d", dev, rc);
    fail("cdrom", why);
    rmdir(mnt);
    return;
  }
  int bad = 0;
  /* limine.conf: an 11-character lower-case name that exists only in Rock
   * Ridge (and Joliet); ISO 9660 itself spells it LIMINE.CON;1. Its entries
   * name the kernel, which is the next file read. */
  if (read_all("/tmp/isofs-cdrom/boot/limine/limine.conf", buf, sizeof(buf) - 1, &n) != 0) {
    snprintf(why, sizeof(why), "boot/limine/limine.conf unreadable (errno %d)", errno);
    fail("cdrom", why);
    bad = 1;
  } else {
    buf[n] = '\0';
    if (!strstr(buf, "path: boot():/boot/kernel.elf")) {
      fail("cdrom", "limine.conf does not name /boot/kernel.elf");
      bad = 1;
    }
  }
  /* The kernel, read whole through 2048-byte sectors: an ELF of this
   * machine's kind whose section headers -- at the end of the file -- name
   * .text in the string table they point at. A block read from the wrong
   * place anywhere along the way breaks the chain. */
  if (!bad) {
    int fd = open("/tmp/isofs-cdrom/boot/kernel.elf", O_RDONLY);
    Elf64_Ehdr eh;
    unsigned long long total = 0;
    if (fd < 0 || fstat(fd, &st) != 0 || pread(fd, &eh, sizeof(eh), 0) != sizeof(eh) ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
#if defined(__x86_64__)
        eh.e_machine != EM_X86_64
#else
        eh.e_machine != EM_AARCH64
#endif
    ) {
      fail("cdrom", "boot/kernel.elf is not this machine's ELF");
      bad = 1;
    } else {
      ssize_t r;
      while ((r = read(fd, buf, sizeof(buf))) > 0)
        total += (unsigned long long)r;
      uint64_t sh_end = eh.e_shoff + (uint64_t)eh.e_shnum * eh.e_shentsize;
      Elf64_Shdr strsh;
      char names[4096];
      int found = 0;
      if (r < 0 || total != (unsigned long long)st.st_size || sh_end > total ||
          eh.e_shstrndx >= eh.e_shnum ||
          pread(fd, &strsh, sizeof(strsh),
                (off_t)(eh.e_shoff + (uint64_t)eh.e_shstrndx * eh.e_shentsize)) !=
              sizeof(strsh)) {
        snprintf(why, sizeof(why),
                 "kernel.elf read %llu of %lld bytes, section headers end at %llu",
                 total, (long long)st.st_size, (unsigned long long)sh_end);
        fail("cdrom", why);
        bad = 1;
      } else {
        size_t want = strsh.sh_size < sizeof(names) ? strsh.sh_size : sizeof(names);
        if (pread(fd, names, want, (off_t)strsh.sh_offset) == (ssize_t)want)
          for (size_t i = 0; i + 6 <= want; i++)
            if (memcmp(names + i, ".text", 6) == 0) {
              found = 1;
              break;
            }
        if (!found) {
          fail("cdrom", "kernel.elf's section name table has no .text");
          bad = 1;
        }
      }
    }
    if (fd >= 0)
      close(fd);
  }
  {
    char type[64] = "", opts[256] = "";
    if (!mounts_row(mnt, type, sizeof(type), opts, sizeof(opts)) ||
        strcmp(type, "iso9660") != 0 || !opts_have(opts, "ro")) {
      snprintf(why, sizeof(why), "/proc/mounts row is '%s %s'", type, opts);
      fail("cdrom", why);
      bad = 1;
    }
  }
  if (umount(mnt) != 0) {
    snprintf(why, sizeof(why), "umount errno %d", errno);
    fail("cdrom", why);
    bad = 1;
  }
  rmdir(mnt);
  if (!bad)
    ok("cdrom");
}

int main(void) {
  char dev[32];
  int loop = loop_attach(dev, sizeof(dev));

  if (loop < 0) {
    if (access(IMG, F_OK) != 0)
      skip("loop", "no " IMG " in this machine (no xorriso at build time)");
    else
      fail("loop", "could not attach " IMG " to a loop device");
  } else {
    t_fsmount(dev);
    t_mount2(dev);
    t_alias(dev);
    ioctl(loop, LOOP_CLR_FD, 0);
    close(loop);
  }
  t_cdrom();

  if (failures == 0)
    printf("ISOFS-SMOKE: done\n");
  else
    printf("ISOFS-SMOKE: done with %d failure(s)\n", failures);
  return failures ? 1 : 0;
}
