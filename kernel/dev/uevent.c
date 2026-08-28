/* Hot-plug announcements over NETLINK_KOBJECT_UEVENT (M109).
 *
 * The transport already existed (kernel/net/netlink.c); what was missing was a
 * producer the device layer could call. A device that appears after boot is
 * invisible to userspace unless the kernel says so — `mdev -d` sits on this
 * socket and creates or removes the node in /dev for every message it reads.
 *
 * The format is the kernel's own and every part of it is load-bearing: mdev
 * matches on SUBSYSTEM before it looks at anything else, and udev refuses a
 * message with no SEQNUM. See b1nix/uevent.h for the exact layout.
 */

#include <b1nix/errno.h>
#include <b1nix/netlink.h>
#include <b1nix/uevent.h>
#include <stdio.h>
#include <string.h>

/* One counter for the whole kernel: a listener uses SEQNUM to order events and
 * to notice that it missed one, so two producers handing out overlapping
 * numbers would be worse than no counter at all. */
static volatile u64 g_uevent_seqnum;

u64 uevent_seqnum(void) {
  return __atomic_load_n(&g_uevent_seqnum, __ATOMIC_ACQUIRE);
}

/* Append "<text>\0" to the message. Returns the new length, unchanged when the
 * property does not fit — a truncated key=value is worse than a missing one. */
static usize uevent_put(char *msg, usize cap, usize pos, const char *text) {
  usize len = strlen(text);
  if (pos + len + 1 > cap)
    return pos;
  memcpy(msg + pos, text, len + 1);
  return pos + len + 1;
}

void uevent_post(const char *action, const char *devpath, const char *subsystem,
                 const char *devtype, const char *devname, int major,
                 int minor) {
  if (!action || !devpath)
    return;
  if (!subsystem)
    subsystem = "unknown";

  u64 seq = __atomic_add_fetch(&g_uevent_seqnum, 1ull, __ATOMIC_ACQ_REL);

  char msg[512];
  char line[192];
  usize pos = 0;

  /* The summary line, NUL-terminated inside the payload like every property
   * that follows it. */
  int n = snprintf(line, sizeof(line), "%s@%s", action, devpath);
  if (n < 0)
    return;
  pos = uevent_put(msg, sizeof(msg), pos, line);

  snprintf(line, sizeof(line), "ACTION=%s", action);
  pos = uevent_put(msg, sizeof(msg), pos, line);
  snprintf(line, sizeof(line), "DEVPATH=%s", devpath);
  pos = uevent_put(msg, sizeof(msg), pos, line);
  snprintf(line, sizeof(line), "SUBSYSTEM=%s", subsystem);
  pos = uevent_put(msg, sizeof(msg), pos, line);
  /* DEVTYPE before SEQNUM, in the kernel's own order. A device built from this
   * message is sealed — libsystemd will not read the `uevent` file to fill a
   * property the message left out — so an absent DEVTYPE is not a cosmetic
   * gap. It is the entire answer to "is this a whole disk", and udevd abandons
   * the event without it. */
  if (devtype && devtype[0]) {
    snprintf(line, sizeof(line), "DEVTYPE=%s", devtype);
    pos = uevent_put(msg, sizeof(msg), pos, line);
  }
  snprintf(line, sizeof(line), "SEQNUM=%lu", (unsigned long)seq);
  pos = uevent_put(msg, sizeof(msg), pos, line);

  if (devname && devname[0]) {
    snprintf(line, sizeof(line), "DEVNAME=%s", devname);
    pos = uevent_put(msg, sizeof(msg), pos, line);
  }
  if (major >= 0) {
    snprintf(line, sizeof(line), "MAJOR=%d", major);
    pos = uevent_put(msg, sizeof(msg), pos, line);
  }
  if (minor >= 0) {
    snprintf(line, sizeof(line), "MINOR=%d", minor);
    pos = uevent_put(msg, sizeof(msg), pos, line);
  }

  netlink_uevent_broadcast(msg, pos);
}

isize uevent_store_write(const char *buf, usize len, const char *devpath,
                         const char *subsystem, const char *devtype,
                         const char *devname, int major, int minor) {
  if (!buf || len == 0 || !devpath)
    return -EINVAL;

  /* The first word is the action; udev appends a synthetic-event UUID after
   * it, which nothing here needs. */
  char action[16];
  usize n = 0;
  while (n < len && n < sizeof(action) - 1 && buf[n] != ' ' && buf[n] != '\n' &&
         buf[n] != '\0')
    n++;
  memcpy(action, buf, n);
  action[n] = '\0';

  static const char *const known[] = {"add",    "remove", "change", "move",
                                      "online", "offline", "bind",  "unbind"};
  int ok = 0;
  for (usize i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (strcmp(action, known[i]) == 0) {
      ok = 1;
      break;
    }
  }
  if (!ok)
    return -EINVAL;

  uevent_post(action, devpath, subsystem, devtype, devname, major, minor);
  return (isize)len;
}
