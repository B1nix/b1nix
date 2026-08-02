#pragma once

/*
 * M73: inotify — filesystem change notification. An inotify fd carries a queue
 * of events; a watch (added via inotify_add_watch) ties a path to a mask, and
 * VFS mutations call vfs_inotify_notify() which enqueues matching events. The fd
 * is pollable and read() returns Linux struct inotify_event records.
 *
 * Bounded first implementation: IN_MODIFY (write to a watched file), IN_ATTRIB,
 * IN_CREATE / IN_DELETE (entry added/removed in a watched directory),
 * IN_DELETE_SELF. The common no-watch path is a single atomic load.
 */

#include <b1nix/types.h>

struct vfs_node;
struct vfs_handle;

/* inotify event masks (Linux ABI values). */
#define IN_ACCESS        0x00000001
#define IN_MODIFY        0x00000002
#define IN_ATTRIB        0x00000004
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN          0x00000020
#define IN_MOVED_FROM    0x00000040
#define IN_MOVED_TO      0x00000080
#define IN_CREATE        0x00000100
#define IN_DELETE        0x00000200
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800
#define IN_UNMOUNT       0x00002000
#define IN_Q_OVERFLOW    0x00004000
#define IN_IGNORED       0x00008000
#define IN_ISDIR         0x40000000
#define IN_ALL_EVENTS    0x00000fff

/* Watch-add modifiers (Linux ABI). */
#define IN_ONLYDIR       0x01000000
#define IN_DONT_FOLLOW   0x02000000
#define IN_EXCL_UNLINK   0x04000000
#define IN_MASK_ADD      0x20000000
#define IN_ONESHOT       0x80000000

/* inotify_init1 / fd flags (Linux ABI). */
#define IN_CLOEXEC  0x00080000
#define IN_NONBLOCK 0x00000800

/* Linux struct inotify_event (variable-length name). */
struct inotify_event {
  int wd;
  u32 mask;
  u32 cookie;
  u32 len;
  char name[]; /* NUL-terminated, len bytes incl. padding */
};

/* Syscall backends. */
int vfs_inotify_init1(int flags);
int vfs_inotify_add_watch(int fd, const char *user_path, u32 mask);
int vfs_inotify_rm_watch(int fd, int wd);

/* VFS hook: a mutation occurred on `node` (the file itself, or the parent
 * directory for create/delete with `name` set). Cheap no-op when no inotify
 * instances are active. */
void vfs_inotify_notify(struct vfs_node *node, u32 mask, const char *name);
/* Rename: the two halves share a cookie so a watcher can pair them, which is
 * the whole point of IN_MOVED_FROM/IN_MOVED_TO. */
void vfs_inotify_notify_move(struct vfs_node *old_dir, const char *old_name,
                             struct vfs_node *new_dir, const char *new_name,
                             int is_dir);
