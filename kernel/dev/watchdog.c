/* /dev/watchdog — a software watchdog (M107).
 *
 * b1nix has no watchdog hardware on any board it runs on, so this is a real
 * kernel-timer watchdog rather than a driver for a chip that is not there: the
 * device arms on open, every write (or WDIOC_KEEPALIVE) resets the deadline,
 * and if the deadline passes the machine is reset exactly as a hardware
 * watchdog would reset it. That is the behaviour BusyBox `watchdog` depends
 * on; nothing about it is simulated away.
 *
 * Magic close is honoured: writing 'V' before close disarms, any other close
 * leaves the watchdog running (WDIOF_MAGICCLOSE).
 */

#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/io.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/vfs.h>
#include <b1nix/watchdog.h>
#include <string.h>

/* Linux <linux/watchdog.h> ioctl numbers. struct watchdog_info is 40 bytes. */
#define WDIOC_GETSUPPORT     0x80285700
#define WDIOC_GETSTATUS      0x80045701
#define WDIOC_GETBOOTSTATUS  0x80045702
#define WDIOC_GETTEMP        0x80045703
#define WDIOC_SETOPTIONS     0x80045704
#define WDIOC_KEEPALIVE      0x80045705
#define WDIOC_SETTIMEOUT     0xc0045706
#define WDIOC_GETTIMEOUT     0x80045707
#define WDIOC_SETPRETIMEOUT  0xc0045708
#define WDIOC_GETPRETIMEOUT  0x80045709
#define WDIOC_GETTIMELEFT    0x8004570a

#define WDIOF_SETTIMEOUT    0x0080
#define WDIOF_MAGICCLOSE    0x0100
#define WDIOF_KEEPALIVEPING 0x8000

#define WDIOS_DISABLECARD 0x0001
#define WDIOS_ENABLECARD  0x0002

#define WD_MIN_TIMEOUT 1
#define WD_MAX_TIMEOUT 600
#define WD_DEFAULT_TIMEOUT 60

/* The scheduler tick is 100 Hz. */
#define WD_TICKS_PER_SEC 100

struct watchdog_info_k {
  u32 options;
  u32 firmware_version;
  u8 identity[32];
};

static volatile int g_armed;
static volatile int g_expect_close;
static volatile u32 g_timeout = WD_DEFAULT_TIMEOUT;
static volatile u64 g_deadline_tick;
static volatile int g_fired;

static void wd_ping(void) {
  g_deadline_tick =
      scheduler_get_uptime_ticks() + (u64)g_timeout * WD_TICKS_PER_SEC;
}

static void wd_arm(void) {
  if (!g_armed) {
    g_armed = 1;
    g_expect_close = 0;
  }
  wd_ping();
}

static void wd_disarm(void) {
  g_armed = 0;
  g_expect_close = 0;
}

/* Called from the timer tick on the boot CPU. */
void watchdog_tick(void) {
  if (!g_armed || g_fired)
    return;
  if (scheduler_get_uptime_ticks() < g_deadline_tick)
    return;
  g_fired = 1;
  console_write("watchdog: timeout expired, resetting the machine\n");
  interrupts_disable();
  /* Pulse the 8042 reset line, the same path SYS_REBOOT's restart takes. */
  while (inb(0x64) & 0x02)
    ;
  outb(0x64, 0xFE);
  arch_halt();
}

/* A write keeps the dog alive; a 'V' anywhere in the buffer arms magic close. */
static isize wd_write(struct vfs_node *node, u64 offset, const char *buf,
                      usize size, int flags) {
  (void)node;
  (void)offset;
  (void)flags;
  if (!g_armed)
    wd_arm();
  for (usize i = 0; i < size; i++) {
    if (buf[i] == 'V')
      g_expect_close = 1;
    else
      g_expect_close = 0;
  }
  wd_ping();
  return (isize)size;
}

static isize wd_read(struct vfs_node *node, u64 offset, char *buf, usize size,
                     int flags) {
  (void)node;
  (void)offset;
  (void)buf;
  (void)size;
  (void)flags;
  return 0; /* a watchdog is write-only; EOF is what Linux reports too */
}

static int wd_ioctl(struct vfs_node *node, u64 request, void *arg) {
  (void)node;
  switch (request) {
  case WDIOC_GETSUPPORT: {
    struct watchdog_info_k info;
    memset(&info, 0, sizeof(info));
    info.options = WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING;
    info.firmware_version = 1;
    strncpy((char *)info.identity, "b1nix software watchdog",
            sizeof(info.identity) - 1);
    if (!arg || syscall_copyout(arg, &info, sizeof(info)) < 0)
      return -EFAULT;
    return 0;
  }
  case WDIOC_GETSTATUS:
  case WDIOC_GETBOOTSTATUS: {
    int v = 0; /* this boot was not caused by a watchdog reset */
    if (!arg || syscall_copyout(arg, &v, sizeof(v)) < 0)
      return -EFAULT;
    return 0;
  }
  case WDIOC_GETTEMP:
    return -EOPNOTSUPP; /* no thermal sensor behind this device */
  case WDIOC_SETOPTIONS: {
    int v;
    if (!arg || syscall_copyin(&v, arg, sizeof(v)) < 0)
      return -EFAULT;
    if (v & WDIOS_DISABLECARD)
      wd_disarm();
    if (v & WDIOS_ENABLECARD)
      wd_arm();
    return 0;
  }
  case WDIOC_KEEPALIVE:
    if (!g_armed)
      wd_arm();
    else
      wd_ping();
    return 0;
  case WDIOC_SETTIMEOUT: {
    int v;
    if (!arg || syscall_copyin(&v, arg, sizeof(v)) < 0)
      return -EFAULT;
    if (v < WD_MIN_TIMEOUT || v > WD_MAX_TIMEOUT)
      return -EINVAL;
    g_timeout = (u32)v;
    wd_ping();
    return syscall_copyout(arg, &v, sizeof(v)) < 0 ? -EFAULT : 0;
  }
  case WDIOC_GETTIMEOUT: {
    int v = (int)g_timeout;
    if (!arg || syscall_copyout(arg, &v, sizeof(v)) < 0)
      return -EFAULT;
    return 0;
  }
  case WDIOC_GETTIMELEFT: {
    u64 now = scheduler_get_uptime_ticks();
    int v = 0;
    if (g_armed && g_deadline_tick > now)
      v = (int)((g_deadline_tick - now) / WD_TICKS_PER_SEC);
    if (!arg || syscall_copyout(arg, &v, sizeof(v)) < 0)
      return -EFAULT;
    return 0;
  }
  case WDIOC_SETPRETIMEOUT:
  case WDIOC_GETPRETIMEOUT:
    return -EOPNOTSUPP; /* no pre-timeout interrupt to raise */
  default:
    return -ENOTTY;
  }
}

/* Opening the device arms it; the VFS has no per-device open hook, so the arm
 * happens on the first write/ioctl instead — which is what every watchdog
 * daemon does immediately after opening. Closing with the magic 'V' pending
 * disarms. */
static void wd_release(struct vfs_node *node) {
  (void)node;
  if (g_expect_close)
    wd_disarm();
}

void watchdog_register_nodes(void) {
  struct vfs_node *n = vfs_add_node("/dev/watchdog", VFS_DEVICE, 0, 0, 0);
  if (!n || IS_ERR(n))
    return;
  n->inode->mode = 0600;
  n->inode->read_cb = wd_read;
  n->inode->write_cb = wd_write;
  n->inode->ioctl_cb = wd_ioctl;
  n->inode->release_cb = wd_release;
  struct vfs_node *n0 = vfs_add_node("/dev/watchdog0", VFS_DEVICE, 0, 0, 0);
  if (n0 && !IS_ERR(n0)) {
    n0->inode->mode = 0600;
    n0->inode->read_cb = wd_read;
    n0->inode->write_cb = wd_write;
    n0->inode->ioctl_cb = wd_ioctl;
    n0->inode->release_cb = wd_release;
  }
}

void watchdog_init(void) { watchdog_register_nodes(); }
