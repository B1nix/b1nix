/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fanotify(7) — filesystem access notification, and the veto that goes with it
 * (M126).
 *
 * inotify watches a directory and says what changed in it. fanotify answers a
 * different question: what is this machine DOING to its files, and may it?
 * A monitor marks a whole mount, is told every open and every read on it, and
 * — with a permission event — can refuse one before it happens. That is how an
 * antivirus scanner, a file-integrity monitor and a hierarchical storage
 * manager are written, and none of them can be written with inotify.
 *
 * The constants are Linux's UAPI values; they are ABI.
 */
#ifndef B1NIX_FANOTIFY_H
#define B1NIX_FANOTIFY_H

#include <b1nix/types.h>

/* fanotify_init(2) flags. */
#define FAN_CLOEXEC 0x00000001
#define FAN_NONBLOCK 0x00000002
#define FAN_CLASS_NOTIF 0x00000000
#define FAN_CLASS_CONTENT 0x00000004
#define FAN_CLASS_PRE_CONTENT 0x00000008
#define FAN_UNLIMITED_QUEUE 0x00000010
#define FAN_UNLIMITED_MARKS 0x00000020
#define FAN_REPORT_TID 0x00000100
#define FAN_REPORT_FID 0x00000200
#define FAN_REPORT_DIR_FID 0x00000400
#define FAN_REPORT_NAME 0x00000800

/* Events. */
#define FAN_ACCESS 0x00000001
#define FAN_MODIFY 0x00000002
#define FAN_ATTRIB 0x00000004
#define FAN_CLOSE_WRITE 0x00000008
#define FAN_CLOSE_NOWRITE 0x00000010
#define FAN_OPEN 0x00000020
#define FAN_MOVED_FROM 0x00000040
#define FAN_MOVED_TO 0x00000080
#define FAN_CREATE 0x00000100
#define FAN_DELETE 0x00000200
#define FAN_DELETE_SELF 0x00000400
#define FAN_MOVE_SELF 0x00000800
#define FAN_OPEN_EXEC 0x00001000
#define FAN_Q_OVERFLOW 0x00004000
#define FAN_OPEN_PERM 0x00010000
#define FAN_ACCESS_PERM 0x00020000
#define FAN_OPEN_EXEC_PERM 0x00040000
#define FAN_EVENT_ON_CHILD 0x08000000
#define FAN_ONDIR 0x40000000

#define FAN_CLOSE (FAN_CLOSE_WRITE | FAN_CLOSE_NOWRITE)
#define FAN_MOVE (FAN_MOVED_FROM | FAN_MOVED_TO)

/* fanotify_mark(2) flags. */
#define FAN_MARK_ADD 0x00000001
#define FAN_MARK_REMOVE 0x00000002
#define FAN_MARK_DONT_FOLLOW 0x00000004
#define FAN_MARK_ONLYDIR 0x00000008
#define FAN_MARK_MOUNT 0x00000010
#define FAN_MARK_IGNORED_MASK 0x00000020
#define FAN_MARK_IGNORED_SURV_MODIFY 0x00000040
#define FAN_MARK_FLUSH 0x00000080
#define FAN_MARK_FILESYSTEM 0x00000100

/* Responses to a permission event. */
#define FAN_ALLOW 0x01
#define FAN_DENY 0x02

#define FANOTIFY_METADATA_VERSION 3

struct fanotify_event_metadata {
  u32 event_len;
  u8 vers;
  u8 reserved;
  u16 metadata_len;
  u64 mask;
  i32 fd;
  i32 pid;
};

struct fanotify_response {
  i32 fd;
  u32 response;
};

/* The system-call hook: fanotify_init(2) and fanotify_mark(2). */
int fanotify_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 *ret);

struct vfs_node;

/*
 * The VFS telling the notification layer that something happened. `mask` is in
 * the inotify bit space, which every hook site already speaks; fanotify
 * translates. `node` is the object, and for a permission event the return says
 * whether the access may proceed.
 *
 * Returns 0 to allow and -EPERM when a monitor refused. Only the permission
 * hooks can refuse; the plain notification form always returns 0.
 */
int fanotify_notify(struct vfs_node *node, u32 in_mask);
int fanotify_permission(struct vfs_node *node, u32 in_mask);

/* Is anyone watching at all? The hot paths ask this before building anything;
 * a machine with no fanotify monitor pays one relaxed load. */
int fanotify_active(void);

#endif
