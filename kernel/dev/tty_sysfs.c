// SPDX-License-Identifier: GPL-2.0-only
/*
 * Terminals in /sys, in Linux's shape.
 *
 * The VTs and the serial ports used to be plain directories directly under
 * /sys/class/tty. udev resolves a device's subsystem from the `subsystem` link
 * in its directory, and with none there sd-device fell back on the path:
 * anything under /sys/class/ is the subsystem "subsystem". So `udevadm trigger
 * --subsystem-match=tty` matched nothing, the rule in 99-systemd.rules that
 * tags terminals for systemd never ran, and no terminal ever got a .device
 * unit -- serial-getty@ttyS0, BindsTo=dev-ttyS0.device, could not start on any
 * b1nix system. On Linux every class entry is a link into /sys/devices, and
 * /sys/dev/char/<maj>:<min> is a link to the same directory; this is that.
 */
#include <b1nix/bootinfo.h>
#include <b1nix/mm.h>
#include <b1nix/tty_sysfs.h>
#include <b1nix/uevent.h>
#include <stdio.h>
#include <string.h>

struct tty_sysfs_dev {
  char name[16];
  char devpath[96];
  int major;
  int minor;
};

/* What has been published, so the console list names only real terminals. */
#define TTY_SYSFS_MAX 16
static struct tty_sysfs_dev *g_ttys[TTY_SYSFS_MAX];
static int g_ntty;

static isize tty_sysfs_dev_show(void *ctx, char *buf, usize cap) {
  const struct tty_sysfs_dev *t = ctx;
  return (isize)snprintf(buf, cap, "%d:%d\n", t->major, t->minor);
}

static isize tty_sysfs_uevent_show(void *ctx, char *buf, usize cap) {
  const struct tty_sysfs_dev *t = ctx;
  return (isize)snprintf(buf, cap, "MAJOR=%d\nMINOR=%d\nDEVNAME=%s\n",
                         t->major, t->minor, t->name);
}

/* Writing "add" re-announces the device: that is how `udevadm trigger`
 * replays the devices a manager started too late to hear about. A tty
 * carries no DEVTYPE on Linux either. */
static isize tty_sysfs_uevent_store(void *ctx, const char *buf, usize len) {
  const struct tty_sysfs_dev *t = ctx;
  return uevent_store_write(buf, len, t->devpath, "tty", 0, t->name, t->major,
                            t->minor);
}

struct sysfs_dir *tty_sysfs_publish(const char *parent, const char *name,
                                    int major, int minor) {
  if (!parent || !name || g_ntty >= TTY_SYSFS_MAX)
    return 0;
  struct tty_sysfs_dev *t = kzalloc(sizeof(*t));
  if (!t)
    return 0;
  strncpy(t->name, name, sizeof(t->name) - 1);
  t->major = major;
  t->minor = minor;
  snprintf(t->devpath, sizeof(t->devpath), "/devices/%s/tty/%s", parent, name);

  /* /sys/devices/<parent...>/tty/<name>, one directory per component. */
  struct sysfs_dir *d = sysfs_reg_dir(0, "devices");
  int depth = 1;
  char comp[32];
  const char *p = parent;
  while (d && *p) {
    usize n = 0;
    while (p[n] && p[n] != '/' && n < sizeof(comp) - 1)
      n++;
    memcpy(comp, p, n);
    comp[n] = '\0';
    d = sysfs_reg_dir(d, comp);
    depth++;
    p += n;
    if (*p == '/')
      p++;
  }
  d = d ? sysfs_reg_dir(d, "tty") : 0;
  d = d ? sysfs_reg_dir(d, name) : 0;
  if (!d) {
    kfree(t);
    return 0;
  }
  depth += 2;

  (void)sysfs_reg_attr(d, "dev", 0444, tty_sysfs_dev_show, 0, t, 0);
  (void)sysfs_reg_attr(d, "uevent", 0644, tty_sysfs_uevent_show,
                       tty_sysfs_uevent_store, t, 0);

  /* Relative, with exactly one "../" per component between the link and
   * /sys: libudev splices these by counting rather than with realpath (see
   * the DRM minors in drm.c). */
  char target[160];
  usize off = 0;
  for (int i = 0; i < depth && off + 3 < sizeof(target); i++)
    off += (usize)snprintf(target + off, sizeof(target) - off, "../");
  snprintf(target + off, sizeof(target) - off, "class/tty");
  (void)sysfs_reg_link(d, "subsystem", target);

  snprintf(target, sizeof(target), "../..%s", t->devpath);
  struct sysfs_dir *cls = sysfs_reg_dir(sysfs_reg_dir(0, "class"), "tty");
  if (cls)
    (void)sysfs_reg_link(cls, name, target);
  char majmin[24];
  snprintf(majmin, sizeof(majmin), "%d:%d", major, minor);
  struct sysfs_dir *chr = sysfs_reg_dir(sysfs_reg_dir(0, "dev"), "char");
  if (chr)
    (void)sysfs_reg_link(chr, majmin, target);

  g_ttys[g_ntty++] = t;
  return d;
}

static int tty_sysfs_known(const char *name) {
  for (int i = 0; i < g_ntty; i++)
    if (strcmp(g_ttys[i]->name, name) == 0)
      return 1;
  return 0;
}

/* Linux lists the enabled consoles with the preferred one -- the last
 * console= on the command line, the one /dev/console is -- last, which is
 * the command line's own order. systemd-getty-generator reads this file to
 * decide where a serial getty goes; without it, a machine booted with
 * console=ttyS0 got none. With no console= the console is the VT. Rendered
 * per read: hvc0 is published only once virtio-console has probed. */
static isize tty_sysfs_console_active_show(void *ctx, char *buf, usize cap) {
  (void)ctx;
  const char *cl = bootinfo_cmdline();
  usize off = 0;
  int any = 0;

  buf[0] = '\0';
  while (cl && *cl) {
    while (*cl == ' ')
      cl++;
    const char *w = cl;
    while (*cl && *cl != ' ')
      cl++;
    if ((usize)(cl - w) <= 8 || strncmp(w, "console=", 8) != 0)
      continue;
    char name[16];
    usize n = 0;
    for (const char *c = w + 8; c < cl && *c != ',' && n < sizeof(name) - 1; c++)
      name[n++] = *c;
    name[n] = '\0';
    if (!tty_sysfs_known(name))
      continue;
    off += (usize)snprintf(buf + off, cap - off, "%s%s", any ? " " : "", name);
    any = 1;
    if (off + 1 >= cap)
      break;
  }
  if (!any)
    off = (usize)snprintf(buf, cap, "tty0");
  off += (usize)snprintf(buf + off, cap - off, "\n");
  return (isize)off;
}

void tty_sysfs_publish_console(void) {
  struct sysfs_dir *d = tty_sysfs_publish("virtual", "console", 5, 1);
  if (d)
    (void)sysfs_reg_attr(d, "active", 0444, tty_sysfs_console_active_show, 0,
                         0, 0);
}
