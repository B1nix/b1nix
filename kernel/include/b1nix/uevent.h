#ifndef B1NIX_UEVENT_H
#define B1NIX_UEVENT_H

#include <b1nix/types.h>

/*
 * Hot-plug announcements (M109).
 *
 * A device appearing or leaving is not something userspace can poll for: mdev
 * and udev are written against the kernel's NETLINK_KOBJECT_UEVENT broadcast
 * and nothing else. This is the one place in b1nix that formats that message,
 * so every producer emits the same wire format:
 *
 *   "<action>@<devpath>\0"
 *   "ACTION=<action>\0"
 *   "DEVPATH=<devpath>\0"
 *   "SUBSYSTEM=<subsystem>\0"
 *   ["DEVTYPE=<devtype>\0"]
 *   "SEQNUM=<n>\0"
 *   ["DEVNAME=<name>\0" "MAJOR=<n>\0" "MINOR=<n>\0"]
 *
 * DEVPATH is the /sys path with the mount point stripped: userspace prepends
 * its own /sys, so leaving it on produces /sys/sys/... and every lookup fails.
 * SEQNUM is a global monotonic counter — udev drops a message without one.
 *
 * DEVTYPE is what a device calls itself inside its subsystem — "disk" or
 * "partition" for a block device, "drm_minor" for a DRM node. It is optional
 * on the wire and it is not optional in practice for anything udev serves: an
 * sd_device built from a netlink message is SEALED, so libsystemd answers
 * sd_device_get_devtype() out of the message alone and never falls back to the
 * `uevent` file under /sys. Without it systemd-udevd's
 * block_device_is_whole_disk() gets ENOENT and abandons the event — "Failed to
 * get whole disk device: No such file or directory" — so the device is never
 * tagged and no .device unit can activate.
 *
 * `devtype`, `devname`, `major` and `minor` are optional: pass NULL and -1 for a device
 * that has no node in /dev. When they are given, mdev names the node it
 * creates after DEVNAME rather than after the last path component, which for
 * /sys/dev/block/8:0 would otherwise be the useless "8:0".
 *
 * Broadcasting with no listener bound is deliberately harmless, so a driver
 * may announce from anywhere, including before the network stack is up.
 */
void uevent_post(const char *action, const char *devpath, const char *subsystem,
                 const char *devtype, const char *devname, int major,
                 int minor);

/* The write half of a device's `uevent` file, which is how a coldplug replay
 * works: `udevadm trigger` walks /sys and writes "add" to every uevent file it
 * finds, and a device whose file refuses the write is one no replay can reach.
 * The action is parsed and validated here so that every subsystem answers the
 * same set of verbs; `len` is returned on success, a negative errno otherwise.
 */
isize uevent_store_write(const char *buf, usize len, const char *devpath,
                         const char *subsystem, const char *devtype,
                         const char *devname, int major, int minor);

/* The last sequence number handed out. Monotonic for the boot. */
u64 uevent_seqnum(void);

#endif
