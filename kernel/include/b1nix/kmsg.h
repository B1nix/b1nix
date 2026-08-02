#ifndef B1NIX_KMSG_H
#define B1NIX_KMSG_H

#include <b1nix/types.h>

/*
 * Structured kernel log records — M107 (kernel/dev/kmsg.c).
 *
 * klog's ring is a flat character transcript: no record boundaries, no
 * sequence numbers, no timestamps, no priorities. `klogd`, `syslogd` and
 * `logread` all want records, so this is a second, record-shaped view of the
 * same stream: console output is folded into one record per line, and the
 * kernel can emit a record directly with a real priority.
 *
 * Both /dev/kmsg and /proc/kmsg read from it, in the Linux wire format
 *
 *     <priority>,<sequence>,<microseconds>,-;<message>\n
 *
 * one record per read(), and syslog(2)/klogctl serves its READ actions from it
 * as well.
 */

/* syslog(2) / klogctl actions. */
#define SYSLOG_ACTION_CLOSE         0
#define SYSLOG_ACTION_OPEN          1
#define SYSLOG_ACTION_READ          2
#define SYSLOG_ACTION_READ_ALL      3
#define SYSLOG_ACTION_READ_CLEAR    4
#define SYSLOG_ACTION_CLEAR         5
#define SYSLOG_ACTION_CONSOLE_OFF   6
#define SYSLOG_ACTION_CONSOLE_ON    7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD   9
#define SYSLOG_ACTION_SIZE_BUFFER   10

/* Create /dev/kmsg. Safe to call again after the root filesystem is switched. */
void kmsg_init(void);
void kmsg_register_nodes(void);

/* Fold one console character into the record ring (called from console_putc).
 * A record is closed on '\n' or when the line buffer fills. */
void kmsg_putc(char ch);

/* Emit a complete record with an explicit syslog priority (0..7). */
void kmsg_emit(int priority, const char *text);

/* syslog(2)/klogctl. `ubuf` is a user pointer; returns the byte count read, a
 * size for the SIZE_* actions, 0 for the accepted no-ops, or -errno. */
isize kmsg_syslog(int type, char *ubuf, int len);

/* Backing callbacks for /proc/kmsg, which is the same stream. */
struct vfs_node;
struct b1nix_pollfd;
isize kmsg_proc_read(struct vfs_node *node, u64 offset, char *buf, usize size,
                     int flags);
int kmsg_proc_poll(struct vfs_node *node, struct b1nix_pollfd *pfd);

#endif
