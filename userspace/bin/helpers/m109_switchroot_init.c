/* M109: the initramfs /init of the switchroot smoke instance.
 *
 * This is the boot Linux describes for an initramfs: the kernel keeps the RAM
 * filesystem as /, PID 1 mounts the real root somewhere below it, and hands
 * over with switch_root — which moves that mount onto / and execs the new init
 * there. Nothing here is simulated: the checks below are the preconditions
 * BusyBox's switch_root itself enforces, and the marker for the switch is
 * printed by the process running in the new root, not by this one.
 *
 * It runs only on the boot whose kernel cmdline selects it (init=/init with
 * root=initramfs); every other boot mounts a real root and never loads it.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#define RAMFS_MAGIC 0x858458f6
#define TMPFS_MAGIC 0x01021994

#define NEWROOT "/newroot"
#define BUSYBOX NEWROOT "/opt/busybox/bin/busybox"

/* The shell that runs as init in the new root. It reports on two files: the
 * witness this process wrote into the new root before the switch (which must
 * be there) and the one it wrote into the initramfs (which must not, because
 * that filesystem is no longer /). */
#define NEWINIT_SCRIPT                                                         \
  "if [ -f /m109-newroot-witness ] && [ ! -e /m109-initramfs-witness ]; then "  \
  "echo 'M109-SMOKE: ok switch-root'; else "                                    \
  "echo 'M109-SMOKE: FAIL switch-root'; fi; "                                   \
  "if [ \"$$\" = \"1\" ]; then echo 'M109-SMOKE: ok switch-root-init-pid1'; "    \
  "else echo 'M109-SMOKE: FAIL switch-root-init-pid1'; fi; "                     \
  "echo 'M109-SMOKE: done-switchroot'"

static void marker(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}

static int write_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  ssize_t n = write(fd, text, strlen(text));
  close(fd);
  return n == (ssize_t)strlen(text) ? 0 : -1;
}

int main(void) {
  struct statfs sf;

  /* switch_root refuses to run unless / is a RAM filesystem — the one thing
   * that makes deleting its contents safe. */
  if (statfs("/", &sf) == 0 &&
      ((unsigned)sf.f_type == RAMFS_MAGIC ||
       (unsigned)sf.f_type == TMPFS_MAGIC))
    marker("M109-SMOKE: ok initramfs-root-statfs");
  else
    marker("M109-SMOKE: FAIL initramfs-root-statfs");

  /* switch_root also insists on a regular /init, which is this program. */
  struct stat st;
  if (stat("/init", &st) != 0 || !S_ISREG(st.st_mode))
    marker("M109-SMOKE: FAIL switchroot-init-file");

  mkdir(NEWROOT, 0755);
  /* The real root is whichever device carries it.
   *
   * It used to be /dev/ram0 without qualification, which was true only while
   * the image travelled inside the boot image as a RAM disk. Served off a disk
   * — which is how the instances boot now, and how a real machine boots — it is
   * a block device with the same contents and a different name, so try the
   * candidates in turn rather than naming one. */
  static const char *const roots[] = { "/dev/ram0", "/dev/vda", "/dev/vdb",
                                       "/dev/sata0" };
  int mounted = 0;

  for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]) && !mounted; i++) {
    if (mount(roots[i], NEWROOT, "ext4", 0, NULL) == 0)
      mounted = 1;
  }
  if (!mounted) {
    marker("M109-SMOKE: FAIL switchroot-mount-newroot");
    marker("M109-SMOKE: done-switchroot");
    return 1;
  }
  marker("M109-SMOKE: ok switchroot-mount-newroot");

  write_file("/m109-initramfs-witness", "old");
  if (write_file(NEWROOT "/m109-newroot-witness", "new") != 0) {
    marker("M109-SMOKE: FAIL switchroot-witness");
    marker("M109-SMOKE: done-switchroot");
    return 1;
  }

  if (access(BUSYBOX, X_OK) != 0) {
    marker("M109-SMOKE: FAIL switchroot-no-busybox");
    marker("M109-SMOKE: done-switchroot");
    return 1;
  }

  char *const argv[] = {(char *)"switch_root", (char *)NEWROOT,
                        (char *)"/bin/sh",     (char *)"-c",
                        (char *)NEWINIT_SCRIPT, NULL};
  execv(BUSYBOX, argv);

  marker("M109-SMOKE: FAIL switchroot-exec");
  marker("M109-SMOKE: done-switchroot");
  return 1;
}
