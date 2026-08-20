#ifndef B1NIX_VFS_H
#define B1NIX_VFS_H

#include <b1nix/dirent.h>
#include <b1nix/posix.h>
#include <b1nix/types.h>
#include <b1nix/uidgid.h>

#define VFS_MAX_PATH 256
#define MS_RDONLY 1
/* mount(2)'s MS_MOVE: move an existing mount to another mountpoint, without
 * unmounting it. Same value as Linux's, since that is what userspace passes. */
#define MS_MOVE 8192

/* Standard permission bits */
#define VFS_IRUSR 0400 /* Owner read */
#define VFS_IWUSR 0200 /* Owner write */
#define VFS_IXUSR 0100 /* Owner execute */
#define VFS_IRGRP 0040 /* Group read */
#define VFS_IWGRP 0020 /* Group write */
#define VFS_IXGRP 0010 /* Group execute */
#define VFS_IROTH 0004 /* Others read */
#define VFS_IWOTH 0002 /* Others write */
#define VFS_IXOTH 0001 /* Others execute */

#define VFS_DEFAULT_PERMS (VFS_IRUSR | VFS_IWUSR | VFS_IRGRP | VFS_IROTH)

#define R_OK 4
#define W_OK 2
#define X_OK 1

struct vfs_node;
struct vfs_handle;
int vfs_check_access(struct vfs_node *node, int requested_access);

enum vfs_node_type {
  VFS_FILE = 1,
  VFS_DEVICE = 2,
  VFS_DIRECTORY = 3,
  VFS_SYMLINK = 4,
  VFS_SOCKET = 5,
  VFS_FIFO = 6,
};

/* inode->flags: the VFS owns inode->data and must kfree() it on release. */
#define VFS_NODE_OWNS_DATA 0x80000000u

/* inode->attr: the ext2/3/4 i_flags low byte, shared verbatim with userspace
 * through FS_IOC_GETFLAGS/FS_IOC_SETFLAGS (chattr/lsattr).
 *   enforced by the VFS:  IMMUTABLE, APPEND
 *   stored, not enforced: SECRM, UNRM, COMPR, SYNC, NODUMP, NOATIME
 * (b1nix writes metadata through the journal and never overwrites freed data,
 * so SECRM/UNRM/COMPR have nothing to act on; SYNC and NOATIME are recorded
 * for the tools that read them but do not change the writeback or atime
 * policy.) */
#define VFS_ATTR_SECRM     0x00000001u /* s */
#define VFS_ATTR_UNRM      0x00000002u /* u */
#define VFS_ATTR_COMPR     0x00000004u /* c */
#define VFS_ATTR_SYNC      0x00000008u /* S */
#define VFS_ATTR_IMMUTABLE 0x00000010u /* i */
#define VFS_ATTR_APPEND    0x00000020u /* a */
#define VFS_ATTR_NODUMP    0x00000040u /* d */
#define VFS_ATTR_NOATIME   0x00000080u /* A */
#define VFS_ATTR_USER_MASK 0x000000FFu

struct acl_entry {
  u16 tag;       /* ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_GROUP, ACL_MASK,
                    ACL_OTHER */
  u16 qualifier; /* UID or GID (for ACL_USER, ACL_GROUP) */
  u16 perms;     /* Permission bitmask */
};

#define ACL_MAX_ENTRIES 16

/* ACL tag types */
#define ACL_USER_OBJ 0x01
#define ACL_USER 0x02
#define ACL_GROUP_OBJ 0x04
#define ACL_GROUP 0x08
#define ACL_MASK 0x10
#define ACL_OTHER 0x20

struct vfs_node;

/* Extended-attribute entry: one name/value pair on an inode's xattr list. */
struct vfs_xattr {
  struct vfs_xattr *next;
  usize size;       /* value length in bytes */
  void *value;      /* kmalloc'd value buffer (may be NULL when size==0) */
  char name[256];   /* full attribute name, e.g. "user.foo" */
};

/* setxattr flags (Linux ABI). */
#define XATTR_CREATE  0x1 /* fail if the attribute already exists */
#define XATTR_REPLACE 0x2 /* fail if the attribute does not exist */
#define XATTR_NAME_MAX 255

/* Longest single path component, as Linux's NAME_MAX.
 *
 * This was 64 in a dozen separate places, so a directory entry held 63
 * characters — shorter than what ext4 stores (255) and shorter than what real
 * programs create: Chromium's cache filenames run past it, and the path
 * splitter used to copy them into a 64-byte caller buffer regardless, which is
 * memory corruption rather than a limit. One name for the limit, and the limit
 * is the filesystem's. */
#define VFS_NAME_MAX 256
#define XATTR_VALUE_MAX 4096

/* Name the holder of an inode rwlock a task is parked on (watchdog dump). */
void vfs_inode_chan_report(u64 chan, u64 payload_base, usize block_size);

struct vfs_inode {
  u64 ino;
  /* Device number (major << 8 | minor) of the filesystem this inode lives on —
   * what stat reports in st_dev and /proc/<pid>/maps prints in its device
   * column. Distinct from fs_id: fs_id identifies one MOUNT (it keys the inode
   * cache, so it must differ between a mount and a later re-mount of the same
   * disk), while dev identifies the DEVICE and is deliberately stable across
   * mounts. 0 until the mount stamps it; vfs_node_dev resolves it. */
  u32 dev;
  /* The device this node IS, for a character or block special file — major << 8
   * | minor, reported as st_rdev. Programs identify a device by these numbers
   * rather than by its path: seatd refuses to open a card whose major does not
   * say "DRM". Kept in its own field because inode->data already means "the
   * file's contents live in memory here", and the page-fault path copies from
   * it — a device number parked there is read as an address. */
  u64 rdev;
  /* Inode generation: bumped every time the filesystem hands this inode number
   * to a NEW file. A stored file handle carries it, so reusing the number for a
   * different file makes the old handle report ESTALE instead of opening the
   * impostor (this is what Linux's i_generation is for). 0 = the filesystem
   * does not track one. */
  u32 generation;
  enum vfs_node_type type;
  u32 flags;
  volatile int rw_lock; /* >0: readers, -1: writer, 0: free */
  /* Who last took rw_lock, and from where. A leaked inode lock is otherwise
   * anonymous: the watchdog can see a task parked on &inode->rw_lock but not
   * which caller took the lock and failed to drop it, and the holder is by
   * then usually gone. Two stores per acquire buys that name. */
  volatile u64 rw_owner;
  const void *rw_site;
  int refcount;         /* Internal references (e.g. open handles) */
  int nlink; /* Number of hard links (names pointing to this inode) */
  usize size;
  usize capacity;
  void *data;

  /* Ownership and permissions */
  u16 uid;
  u16 gid;
  u16 mode;

  /* ACL support */
  struct acl_entry acls[ACL_MAX_ENTRIES];
  int acl_count;

  /* Extended attributes (in-memory, per-inode list). Head of a singly-linked
   * list of name/value pairs; NULL when the inode has no xattrs. */
  struct vfs_xattr *xattrs;

  /* Timestamps */
  u64 atime;
  u64 mtime;
  u64 ctime;

  /* Named pipe (FIFO) backing buffer, non-NULL only while a VFS_FIFO node has
   * at least one opener. Allocated on first open and dropped when the last
   * reader and writer close, so a FIFO that nobody holds keeps no data — the
   * same lifetime an anonymous pipe has. */
  struct vfs_pipe *fifo;

  struct block_device *blk_dev;
  u32 fs_id; /* Unique ID for this filesystem instance */
  /* Dirty pages this inode currently has in the page cache. Flushing a file
   * means walking every cached page in the machine, so close(2) checks this
   * first and skips the walk when the file it is closing has nothing to
   * write. */
  u32 dirty_pages;
  /* Pages of this inode currently in the page cache. Truncating or
   * invalidating a file walks every cached page in the machine to find its
   * own; a file with none can skip the walk entirely, which is the common
   * case for the shared-memory regions a browser creates by the hundred. */
  u32 cached_pages;

  /* M56 file sealing (memfd). `seals` holds the active F_SEAL_* bitmask;
   * `seals_allowed` is set when the memfd was created with MFD_ALLOW_SEALING.
   * Both are 0 for ordinary files (which therefore behave as F_SEAL_SEAL-set:
   * F_ADD_SEALS is rejected). */
  u32 seals;
  u32 seals_allowed;

  /* Set by a filesystem whose readdir_cb already walks the in-memory child
   * list itself (procfs materialises a directory per task and then defers to
   * vfs_readdir_children). The VFS merges in-memory children over the
   * filesystem's own listing for every other directory — see vfs_getdents —
   * and this says "no need, and do not list them twice". */
  int readdir_lists_children;
  /* M109 inode attribute flags — what FS_IOC_GETFLAGS reports and
   * FS_IOC_SETFLAGS changes, i.e. what `lsattr` prints and `chattr` sets. The
   * values are the ext2/3/4 on-disk i_flags low byte (VFS_ATTR_* above), so a
   * filesystem that has the field on disk just copies it in and out.
   * IMMUTABLE and APPEND are enforced by the VFS; the rest are stored. */
  u32 attr;

  /* Callbacks (Inode Operations) */
  isize (*read_cb)(struct vfs_node *node, u64 offset, char *buffer, usize size,
                   int flags);
  isize (*readdir_cb)(struct vfs_node *node, usize offset, struct dirent *buf,
                      usize max_entries);
  /* Cursor-based readdir, for filesystems whose on-disk directory has no stable
   * "Nth live entry" numbering. `readdir_cb`'s offset is an index of live
   * entries, and that index is destroyed by an unlink: the entries after the
   * removed one all shift down by one, so a reader that resumes at its saved
   * index (every `rm -rf`, which deletes as it walks) SKIPS exactly as many
   * entries as it has deleted, and the final rmdir then fails ENOTEMPTY on a
   * directory that only looks empty. This variant takes an opaque cookie and
   * returns the one to resume from — for on-disk formats that is a byte
   * position, which an unlink elsewhere in the directory does not move. When a
   * filesystem provides it, the VFS uses it in preference. */
  isize (*readdir_at_cb)(struct vfs_node *node, u64 cookie, struct dirent *buf,
                         usize max_entries, u64 *next_cookie);
  /* On-demand child materialisation for synthetic dirs (procfs/sysfs) whose
   * children exist lazily. The path resolver calls this when find_child misses,
   * so a DIRECT lookup (e.g. musl ttyname's readlink("/proc/self/fd/N"), which
   * never runs readdir first) can still resolve. NULL on all real filesystems,
   * so their lookup path is unchanged. Must materialise `name` as a physical
   * child of `dir` (idempotent) and return 0 on success, <0 if no such child. */
  int (*lookup_cb)(struct vfs_node *dir, const char *name);
  /* Refresh the inode fields that a synthetic filesystem computes rather than
   * stores, immediately before a stat reads them. /proc/<pid>/task is the case
   * that needed it: its link count is two plus the number of live threads, and
   * Chromium's sandbox reads exactly that number to decide whether it is safe
   * to enter — a stored 2 says "fewer than . and .. alone", which fails its
   * check and kills the process. NULL everywhere else. */
  void (*getattr_cb)(struct vfs_node *node);
  isize (*write_cb)(struct vfs_node *node, u64 offset, const char *buffer,
                    usize size, int flags);
  int (*create_cb)(struct vfs_node *dir, const char *name,
                   const char *full_path, u32 mode);
  int (*mkdir_cb)(struct vfs_node *dir, const char *name, u32 mode);
  /* mknod: create a special file (only S_IFIFO is used today) as a real on-disk
   * inode. Filesystems that cannot represent one leave this NULL and vfs_mknod
   * reports -EOPNOTSUPP. The child VFS node is already linked into `dir` when
   * this runs, so the implementation can find_child(dir, name) to attach its
   * per-inode state, exactly as create_cb does. */
  int (*mknod_cb)(struct vfs_node *dir, const char *name, u32 mode);
  int (*unlink_cb)(struct vfs_node *dir, const char *name);
  int (*rmdir_cb)(struct vfs_node *dir, const char *name);
  int (*rename_cb)(struct vfs_node *old_dir, const char *old_name,
                   struct vfs_node *new_dir, const char *new_name);
  int (*link_cb)(struct vfs_node *target, struct vfs_node *dir,
                 const char *name);
  int (*symlink_cb)(struct vfs_node *dir, const char *name, const char *target);
  void (*release_cb)(struct vfs_node *node);
  int (*truncate_cb)(struct vfs_node *node, u64 length);
  int (*setattr_cb)(struct vfs_node *node);
  /* M109: persist inode->attr (the ext2 i_flags low byte) to the medium. A
   * filesystem with no such field on disk leaves this NULL, and
   * FS_IOC_SETFLAGS then reports EOPNOTSUPP rather than accepting flags it
   * would forget at umount. FS_IOC_GETFLAGS still answers for everyone. */
  int (*setflags_cb)(struct vfs_node *node, u32 attr);
  /* M109 FITRIM: hand every free range of this filesystem back to the device
   * as a discard. `node` is any node on the mount (fstrim passes the mount
   * point). Returns 0 and stores the number of bytes offered in *trimmed. */
  int (*fitrim_cb)(struct vfs_node *node, u64 start, u64 len, u64 minlen,
                   u64 *trimmed);
  int (*statfs_cb)(struct vfs_node *node, struct b1nix_statfs *st);
  int (*fsync_cb)(struct vfs_node *node);
  int (*poll_cb)(struct vfs_node *node, struct b1nix_pollfd *pfd);
  /* Called once per open(), after the handle exists, so a device can attach
   * per-descriptor state (h->private_data) and take over h->ops. Without it a
   * device node only ever sees `struct vfs_node *`, which is shared by every
   * open of the path — two readers of /dev/kmsg then shared one cursor and
   * each got half the log. A negative return fails the open. */
  int (*open_cb)(struct vfs_node *node, struct vfs_handle *h);
  /* Device ioctl hook, dispatched by vfs_ioctl before the legacy name-based
   * special cases. The callee validates/copies its own user arg. */
  int (*ioctl_cb)(struct vfs_node *node, u64 request, void *arg);
  /* Device-memory mmap hook (M47 /dev/fb0): translate a page-aligned file
   * offset to the physical base to map. sys_mmap maps the range with
   * VMM_SHARED and takes a pmm reference per page; the device must keep its
   * own reference so the frames outlive every user mapping. */
  int (*mmap_phys_cb)(struct vfs_node *node, u64 offset, usize length,
                      u64 *out_phys);
  int (*mmap_handle_phys_cb)(struct vfs_handle *handle, u64 offset,
                             usize length, u64 *out_phys);
  /* M100: per-page variant. mmap_handle_phys_cb answers with a single base and
   * sys_mmap then assumes the rest of the range is physically contiguous after
   * it — which a scatter-gather GEM buffer object is not. A device that backs
   * its mappings with discontiguous pages sets this instead; sys_mmap then asks
   * for one page at a time. The two are mutually exclusive; this one wins. */
  int (*mmap_handle_page_phys_cb)(struct vfs_handle *handle, u64 offset,
                                  u64 *out_phys);
  /* Mapping-lifetime hooks. Called once per VMA, including fork copies and
   * VMA splits, with a matching close on munmap/exec/exit. */
  void (*mmap_open_cb)(struct vfs_node *node);
  void (*mmap_close_cb)(struct vfs_node *node);
  void (*mmap_range_open_cb)(struct vfs_node *node, u64 offset, usize length);
  void (*mmap_range_close_cb)(struct vfs_node *node, u64 offset, usize length);
};

/* Give an inode a fresh generation (a create reusing an existing node). */
void vfs_inode_new_generation(struct vfs_inode *inode);

/* Inode cache API */
void icache_init(void);
struct vfs_inode *icache_get(u32 fs_id, u64 ino);
void icache_insert(u32 fs_id, u64 ino, struct vfs_inode *inode);
void icache_invalidate(u32 fs_id, u64 ino);
void icache_invalidate_fs(u32 fs_id);

struct vfs_node {
  char name[VFS_NAME_MAX];
  struct vfs_inode *inode;
  int refcount; /* References to this NAME (e.g. current directory) */
  int deleted;
  /* Directory-cursor sequence number, handed out in creation order from a
   * single global counter. readdir resumes at "the first child whose seq is
   * greater than the cookie", which is what makes the cursor survive deletion:
   * a positional index renumbers every surviving entry when one is removed, so
   * a reader that deletes as it walks (rm -rf) skips one entry per deletion and
   * the final rmdir fails ENOTEMPTY on a directory that only looks empty. */
  u64 dir_seq;

  struct vfs_node *parent;
  struct vfs_node *first_child;
  struct vfs_node *next_sibling;
};

/* A filesystem that needs no block device (procfs, sysfs, tmpfs, ...). Only
 * affects how /proc/filesystems labels the entry, exactly as Linux does. */
#define VFS_FS_NODEV 0x1

struct module;

struct vfs_fs {
  const char *name;
  struct vfs_node *(*mount)(const char *source, u64 flags, void *data);
  int (*umount)(struct vfs_node *root_node);
  u32 flags;
  /* Module this type came from, filled in by vfs_register_fs from the address
   * of the descriptor itself (NULL for a built-in filesystem). Every mount
   * holds a reference on it, so `rmmod isofs` with an ISO mounted is refused
   * instead of freeing code the mount is still calling into. */
  struct module *owner;
  struct vfs_fs *next;
};

/* Snapshot of the registered filesystem types, in registration order (newest
 * first, the order find_fs searches). Fills up to `max` entries and returns the
 * number written. Backs /proc/filesystems, which used to be a hardcoded list
 * and therefore could not show a filesystem that arrived with a module. */
struct vfs_fs_info {
  const char *name;
  u32 flags;
};
usize vfs_list_filesystems(struct vfs_fs_info *out, usize max);

u64 vfs_get_unix_time(void);
void vfs_init(void);
/* tmpfs/ramfs/devtmpfs: RAM-backed mounts (see kernel/fs/tmpfs.c). */
void tmpfs_init(void);
/* Sleeping mutex for filesystem-wide metadata (allocator bitmaps, superblock
 * counters). Legal to hold across block I/O — never a spinlock. */
void vfs_meta_lock_acquire(int *lock);
void vfs_meta_lock_release(int *lock);
void vfs_repopulate_after_root_mount(void);
void vfs_resolve_path(const char *path, char *out);
int vfs_get_node_path(struct vfs_node *node, char *buf, usize buf_len);
int vfs_node_is_readonly(struct vfs_node *node);
struct vfs_node *find_child(struct vfs_node *parent, const char *name);
struct vfs_node *vfs_find_node(const char *path);
struct vfs_node *vfs_add_node(const char *path, enum vfs_node_type type,
                              void *data, usize size, u32 flags);
struct vfs_node *vfs_node_get(struct vfs_node *node);
void vfs_node_put(struct vfs_node *node);
struct vfs_node *vfs_create_node(enum vfs_node_type type);
void vfs_attach_child(struct vfs_node *parent, struct vfs_node *child);
/* Unlink `child` from `parent`'s sibling list. Used by synthetic filesystems
 * whose tree changes at runtime (a module removing its /sys/module entry).
 * The caller still owns its reference on the child. */
void vfs_detach_child(struct vfs_node *parent, struct vfs_node *child);
/* Device number of the filesystem a node belongs to, resolved through its
 * ancestors when the node itself predates the mount stamp. */
u32 vfs_node_dev(struct vfs_node *node);
isize vfs_readdir_children(struct vfs_node *dir, usize offset,
                           struct dirent *buf, usize max_entries);

/* Permission-aware operations */
int vfs_open(const char *path);
int vfs_open_flags(const char *path, int flags);
/* open(2) with O_CREAT: `mode` is the requested permission, masked by umask. */
int vfs_open_flags_mode(const char *path, int flags, u16 mode);
isize vfs_read(int handle, char *buffer, usize size);
/* 1 when this descriptor's driver reads into userspace itself — see
 * vfs_file_ops.read_user. Asked before the read so the caller knows whether to
 * bounce; the answer cannot be inferred from a return value, because every
 * errno a probing read could produce is also one a real read produces. */
int vfs_read_is_direct(int handle);
isize vfs_read_user(int handle, void *user_buffer, usize size);
isize vfs_write(int handle, const char *buffer, usize size);
/* Positioned I/O: read/write at `offset` without touching the fd's own offset
 * (thread-safe pread/pwrite; non-seekable handles return ESPIPE). */
isize vfs_pread(int handle, char *buffer, usize size, u64 offset);
isize vfs_pwrite(int handle, const char *buffer, usize size, u64 offset);
/* Same, for kernel-internal users that hold a node rather than a descriptor
 * (the loop driver). They go through the page cache exactly as read()/write()
 * do, which inode->read_cb/write_cb do not. */
isize vfs_node_pread(struct vfs_node *node, char *buffer, usize size,
                     u64 offset);
isize vfs_node_pwrite(struct vfs_node *node, const char *buffer, usize size,
                      u64 offset);
int vfs_node_fsync(struct vfs_node *node);
int vfs_poll(int handle_idx, struct b1nix_pollfd *pfd);
void vfs_close(int handle);
/* Close a handle not (or no longer) reachable through an fd table. */
struct vfs_handle;
void vfs_close_handle(struct vfs_handle *h, int owner_pid);
int vfs_create(const char *path, u32 mode);
int vfs_mkdir(const char *path, u32 mode);
isize vfs_list(const char *dir_path, const char **names, usize max_names);
struct vfs_node *vfs_find_node_by_fd(int fd);
int vfs_stat(const char *path, struct b1nix_stat *st);
int vfs_lstat(const char *path, struct b1nix_stat *st);
int vfs_statfs(const char *path, struct b1nix_statfs *st);
isize vfs_lseek(int handle, isize offset, int whence);
int vfs_unlink(const char *path);
int vfs_link(const char *target, const char *link_path);
int vfs_symlink(const char *target, const char *link_path);
isize vfs_readlink(const char *path, char *buffer, usize size);
int vfs_rename(const char *old_path, const char *new_path);
int vfs_rmdir(const char *path);
int vfs_fstat(int fd, struct b1nix_stat *st);
int vfs_fd_abspath(int fd, char *buf, usize size);
/* M109: the namespace a /proc/<pid>/ns/<kind> descriptor pinned at open time,
 * or -EBADF / -EINVAL when the descriptor is not one. */
int vfs_fd_ns_pin(int fd, u32 *pin_out);
int vfs_fsync(int fd);
int vfs_mount(const char *source, const char *target, const char *fstype,
              u64 flags);
int vfs_umount(const char *target);
/* pivot_root(2): new_root becomes "/", the old root moves to put_old (which
 * must be a directory underneath new_root). */
int vfs_pivot_root(const char *new_root, const char *put_old);
/* Move the mount rooted at `source` to `target`, taking every mount nested
 * inside it along. Neither filesystem is unmounted, so open files on the moved
 * mount stay open — this is what switch_root does with the new root. */
int vfs_move_mount(const char *source, const char *target);
void vfs_register_fs(struct vfs_fs *fs);
/* Withdraw a filesystem type. A module that registered a filesystem must call
 * this from its exit path, or the VFS keeps a pointer into freed module text. */
void vfs_unregister_fs(struct vfs_fs *fs);
void vfs_set_currently_mounting_root(struct vfs_node *root);
isize vfs_mounts(struct b1nix_mount_entry *out, usize max_entries);
/* How many mount slots this machine was sized for — between MIN_MOUNTS and
 * MAX_MOUNTS, derived from RAM at vfs_init(). Callers sizing a buffer for
 * vfs_mounts() should ask for this rather than assume MAX_MOUNTS, and must
 * heap-allocate it. */
usize vfs_mount_capacity(void);
int vfs_sync(void);
isize vfs_getdents(int handle, struct dirent *buf, usize max_entries);
int vfs_pipe(int pipefd[2]);
/* tee(2): copy bytes between two pipes without consuming the source. */
isize vfs_pipe_tee(struct vfs_handle *in, struct vfs_handle *out, usize len);

/* mknod(2): only S_IFIFO (named pipes) and S_IFREG are creatable; character and
 * block special files have no userspace-creatable backing in b1nix and return
 * -EPERM, as they do for an unprivileged Linux process. */
int vfs_mknod(const char *path, u32 mode, u64 dev);

/* Open a VFS_FIFO node: allocates the shared buffer on first opener and applies
 * POSIX rendezvous semantics (a blocking O_RDONLY waits for a writer and vice
 * versa; O_NONBLOCK O_WRONLY without a reader fails with -ENXIO). Returns an fd
 * or -errno; the caller keeps its own reference to `node`. */
int vfs_fifo_open(struct vfs_node *node, int flags);
int vfs_dup(int oldfd);
int vfs_dup2(int oldfd, int newfd);
int vfs_ftruncate(int fd, u64 length);
int vfs_memfd_create(const char *name, u32 flags);
int vfs_fcntl(int fd, int cmd, u64 arg);
int vfs_ioctl(int fd, u64 request, void *arg);

/* M56 event-loop / IPC primitives (kernel/fs/eventpoll.c). */
int vfs_eventfd(unsigned int initval, int flags);
int vfs_timerfd_create(int clockid, int flags);
int vfs_timerfd_settime(int fd, int flags,
                        const struct b1nix_itimerspec *new_value,
                        struct b1nix_itimerspec *old_value);
int vfs_signalfd(int fd, u64 mask, int flags);
int vfs_epoll_create(int flags);
int vfs_epoll_ctl(int epfd, int op, int fd, struct b1nix_epoll_event *event);
int vfs_epoll_wait(int epfd, struct b1nix_epoll_event *events, int maxevents,
                   int timeout);
/* M56 file sealing — implemented in eventpoll.c, called from vfs_fcntl. */
int vfs_fcntl_add_seals(int fd, u32 seals);
int vfs_fcntl_get_seals(int fd);
void vfs_close_on_exec(void);
int vfs_socket(int domain, int type, int protocol);
int vfs_socketpair(int domain, int type, int protocol, int sv[2]);
int vfs_bind(int fd, const void *addr, usize addrlen);
int vfs_listen(int fd, int backlog);
int vfs_accept(int fd, void *addr, usize *addrlen);
int vfs_connect(int fd, const void *addr, usize addrlen);
isize vfs_socket_send(int fd, const void *buf, usize len, int flags);
isize vfs_socket_recv(int fd, void *buf, usize len, int flags);
isize vfs_socket_sendto(int fd, const void *buf, usize len, int flags,
                        const void *addr, usize addrlen);
isize vfs_socket_recvfrom(int fd, void *buf, usize len, int flags, void *addr,
                          usize *addrlen);
struct b1nix_ucred {
  int pid;
  u32 uid;
  u32 gid;
};
/* Descriptors carried by one control message. Eight was below what real
 * senders use: chromium's sandbox IPC passes up to sixteen, and Mojo attaches
 * many more to a single message — each of those became EINVAL, and the
 * descriptors being passed are the shared-memory regions the GPU process needs
 * before anything can be drawn. */
#define VFS_SCM_MAX_FDS 32
isize vfs_socket_sendmsg(int fd, const void *buf, usize len, int flags,
                         struct vfs_handle **handles, usize nhandles,
                         const struct b1nix_ucred *cred);
isize vfs_socket_recvmsg(int fd, void *buf, usize len, int flags,
                         int *received_fds, usize fd_capacity,
                         usize *received_count, struct b1nix_ucred *cred,
                         int *has_cred, int *control_truncated);
int vfs_setsockopt(int fd, int level, int optname, const void *optval,
                   usize optlen);
int vfs_getsockopt(int fd, int level, int optname, void *optval,
                   usize *optlen);
int vfs_getsockname(int fd, void *addr, usize *addrlen);
int vfs_getpeername(int fd, void *addr, usize *addrlen);
int vfs_shutdown(int fd, int how);
int vfs_socket_push_udp(u16 local_port_net, const void *data, usize len,
                        const void *src_ip, int src_is_v6, u16 src_port_net);
usize vfs_socket_last_srcaddr(int fd, void *addr, usize cap);

/* M32b pseudo-terminals (kernel/dev/pty.c). */
void pty_init(void);
int pty_open_master(int flags);    /* opening /dev/ptmx */
int pty_open_slave(int index, int flags); /* opening /dev/pts/N */
usize pty_fg_pgrp(int idx);
int pty_index_of(struct vfs_handle *h);
int pty_allocated(int idx);

extern void *vfs_poll_chan;

/* Permission management */
int vfs_chmod(const char *path, u16 mode);
int vfs_fchmod(int fd, u16 mode);
int vfs_utime(const char *path, u64 atime, u64 mtime);
int vfs_chown(const char *path, u16 uid, u16 gid);
/* lchown(2): the symlink itself is the target, not what it points at. */
int vfs_lchown(const char *path, u16 uid, u16 gid);
int vfs_fchown(int fd, u16 uid, u16 gid);
int vfs_fstatfs(int fd, struct b1nix_statfs *st);
int vfs_syncfs(int fd);
int vfs_get_node_perm(const struct vfs_node *node, const struct cred *cred,
                      u32 mask);
int vfs_set_acl(struct vfs_node *node, const struct acl_entry *acl);
int vfs_get_acl(struct vfs_node *node, struct acl_entry *out_acl,
                int max_entries);

/* Extended attributes (kernel/fs/vfs.c). Path-based; nofollow selects the
 * l*xattr (do-not-dereference-symlink) variant. Return values follow the
 * Linux getxattr/listxattr contract: a non-negative size, or -errno. */
isize vfs_setxattr(const char *path, const char *name, const void *value,
                   usize size, int flags, int nofollow);
isize vfs_getxattr(const char *path, const char *name, void *value,
                   usize size, int nofollow);
isize vfs_listxattr(const char *path, char *list, usize size, int nofollow);
isize vfs_removexattr(const char *path, const char *name, int nofollow);
void vfs_free_xattrs(struct vfs_inode *inode);

enum vfs_handle_kind {
  VFS_HANDLE_NONE = 0,
  VFS_HANDLE_NODE,
  VFS_HANDLE_PIPE_READ,
  VFS_HANDLE_PIPE_WRITE,
  VFS_HANDLE_SOCKET,
  VFS_HANDLE_PTY_MASTER,
  VFS_HANDLE_PTY_SLAVE,
  VFS_HANDLE_SERIAL_TTY,
  VFS_HANDLE_INPUT,
  /* M56 event-loop / IPC primitives. */
  VFS_HANDLE_EVENTFD,
  VFS_HANDLE_TIMERFD,
  VFS_HANDLE_SIGNALFD,
  VFS_HANDLE_EPOLL,
  /* M73: inotify filesystem-change notification. */
  VFS_HANDLE_INOTIFY
};

struct vfs_file_ops {
  isize (*read)(struct vfs_handle *h, char *buf, usize len);
  /* Reads straight into the caller's userspace buffer.
   *
   * The ordinary .read is handed a kernel bounce buffer and the syscall layer
   * copies it out. That is the right default, but it is wrong for a driver
   * whose implementation copies out itself — the imported DRM core's drm_read
   * writes through copy_to_user, and handing it a kernel address makes every
   * read fail with EFAULT, which is exactly how this was found. Set this
   * instead of .read in that case: it takes precedence and the syscall layer
   * does not bounce. The op owns the validation of the pointer it is given. */
  isize (*read_user)(struct vfs_handle *h, void *user_buf, usize len);
  isize (*write)(struct vfs_handle *h, const char *buf, usize len);
  int (*poll)(struct vfs_handle *h, struct b1nix_pollfd *pfd);
  isize (*lseek)(struct vfs_handle *h, isize offset, int whence);
  int (*close)(struct vfs_handle *h);
  void (*release)(struct vfs_handle *h);
  int (*getdents)(struct vfs_handle *h, struct dirent *buf, usize max_entries);
  int (*ioctl)(struct vfs_handle *h, u64 request, void *arg);
};

/* (There is no MAX_VFS_NODES. Nodes come from vfs_alloc_node/kzalloc and are
 * bounded by memory; node_count is a statistic, not a limit. A constant here
 * claiming otherwise was referenced by nothing and read as a cap that does not
 * exist.) */
/* Compile-time ceiling of the pipes[] pool. The pool search (pipe_pool_claim)
 * only walks g_resource_caps.max_pipes slots (M77), so the runtime cap is
 * adjustable via /proc/sys/kernel/pipe-max-count without reallocating. */
#define MAX_VFS_PIPES_CEIL 1024
/* Mount-table capacity.
 *
 * MAX_MOUNTS is now only the CEILING; the table is allocated at vfs_init() to a
 * size derived from RAM and reported by vfs_mount_capacity(). Sixty-four was a
 * global count shared by every mount namespace, so an ordinary boot (/, /proc,
 * /sys, /dev, /dev/pts, /dev/shm, /run, /tmp, a couple of cgroup mounts) spent a
 * third of it before userspace started, and anything that unshares its mount
 * namespace — a container, or `unshare -m` — multiplies the usage. Past the
 * limit mount(2) returns ENOMEM.
 *
 * FLOOR   MIN_MOUNTS (64) — the previous fixed size, so a 256 MiB guest is
 *         unchanged.
 * CEILING MAX_MOUNTS (4096) — ~2 MiB of table on the 4 GiB machine that
 *         reaches it, and more mounts than a namespace-heavy host keeps live.
 * `b1nix.max-mounts=N` states the capacity directly.
 *
 * The table is sized once and never reallocated: several path-resolution scans
 * read mounts[] WITHOUT the mount lock (see kernel/fs/vfs.c), and moving the
 * array under a lock-free reader would hand it a freed pointer. Growth would
 * need RCU; sizing from RAM at init buys the same headroom without it.
 *
 * Anything declaring a buffer of this size must heap-allocate it —
 * MAX_MOUNTS entries is far too much for a kernel stack. */
#define MIN_MOUNTS 64
#define MAX_MOUNTS 4096
/* Pipe capacity. Linux's is 64 KiB and userspace is written against that: bash
 * feeds a here-document through a pipe when it believes the document fits, and
 * with a 4 KiB pipe here it simply blocked forever on `read -rd '' config`
 * (neofetch's default config is bigger than that) — a hang with no error and no
 * way to see the cause from userspace. The buffer is allocated per slot on
 * first use, so the pool costs nothing until pipes are actually created. */
#define PIPE_BUFFER_SIZE 65536
#define TTY_INPUT_SIZE 256

struct vfs_pipe {
  int used;
  char *buffer; /* PIPE_BUFFER_SIZE bytes, allocated on this slot's first use */
  usize size;
  usize read_pos;
  usize write_pos;
  int readers;
  int writers;
  volatile int lock;
};

/* Datagram queue geometry for UDP, raw and netlink sockets. Linux bounds a
 * datagram socket by rmem (212992 bytes by default) rather than by a slot
 * count; sixteen slots of 2 KiB is 32 KiB per socket, the same order, and it is
 * the shape this fixed-size state can express. Eight slots meant a burst of
 * nine replies — or a netlink dump longer than 16 KiB — was silently dropped.
 * The slot size stays at 2 KiB: it is also the largest frame either NIC driver
 * delivers, so a larger slot could not be filled. */
#define SOCK_DGRAM_Q_SLOTS 16
#define SOCK_DGRAM_SLOT_MAX 2048

struct vfs_socket_state {
  /* The network namespace the socket was created in. A socket keeps it for
   * life — as on Linux, joining another namespace re-points what a task
   * creates NEXT, never what it already holds open. Delivery compares it
   * against the namespace of the interface a frame arrived on. */
  u32 netns;
  int domain;
  int type;
  int protocol;
  union {
    struct b1nix_sockaddr_in in;
    struct b1nix_sockaddr_in6 in6;
    struct b1nix_sockaddr_un un;
    struct b1nix_sockaddr_ll ll;
  } local, peer;
  int bound;
  int connected;
  int listening;
  /* AF_UNIX SOCK_DGRAM connected to /dev/log: musl syslog() connect()s + send()s
   * here. There is no userspace syslogd, so the kernel is the sink — datagrams
   * are forwarded to the serial console (see unix_connect / unix_send_control). */
  int syslog_sink;
  /* M32b socket options + shutdown state (SHUT_RD/WR half-close flags). */
  int so_reuseaddr;
  int so_keepalive;
  /* TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT, in seconds, as the socket
   * would apply them; the live values belong to the connection. */
  int tcp_keepalive_param[3];
  /* SO_PASSCRED: the receiver asked for the sender's credentials to be attached
   * to every message it gets, instead of only to messages whose sender chose to
   * send SCM_CREDENTIALS. Crashpad's handler relies on this to learn, from the
   * kernel rather than from the message body, which process is asking it for a
   * dump. AF_UNIX only. */
  int so_passcred;
  int tcp_nodelay;
  int ipv6_v6only;
  int so_error;
  int so_rcvbuf;
  int so_sndbuf;
  /* SO_RCVTIMEO / SO_SNDTIMEO, in milliseconds; 0 means "no timeout", which is
   * what a socket starts with. A blocking recv/send that reaches the deadline
   * with nothing transferred reports EAGAIN, as POSIX requires. */
  u64 so_rcvtimeo_ms;
  u64 so_sndtimeo_ms;
  int backlog;
  int shut_rd;
  int shut_wr;
  void *tcp_conn;
  void *unix_data;
  usize recv_len;
  /* Datagram queue for UDP, raw and netlink sockets: SOCK_DGRAM_Q_SLOTS
   * datagrams of at most SOCK_DGRAM_SLOT_MAX bytes each, inline in this
   * kzalloc'd state. */
  char udp_q_buf[SOCK_DGRAM_Q_SLOTS][SOCK_DGRAM_SLOT_MAX];
  usize udp_q_len[SOCK_DGRAM_Q_SLOTS];
  /* Where each queued datagram came from. recvfrom() has to report the
   * sender of the datagram it just handed back — musl's resolver drops any
   * reply whose reported source does not match the nameserver it queried, so
   * reporting the socket's last send target (which is what this used to do)
   * made every DNS answer look forged. */
  u8 udp_q_src_ip[SOCK_DGRAM_Q_SLOTS][16];
  u16 udp_q_src_port[SOCK_DGRAM_Q_SLOTS];
  u8 udp_q_src_is6[SOCK_DGRAM_Q_SLOTS];
  u8 udp_last_src_ip[16];
  u16 udp_last_src_port;
  u8 udp_last_src_is6;
  u8 udp_last_src_valid;
  u8 udp_q_head;
  u8 udp_q_tail;
  u8 udp_q_count;
};

struct vfs_handle {
  int used;
  int refcount;
  enum vfs_handle_kind kind;
  struct vfs_node *node;
  usize offset;
  void *private_data; /* Used for pipe, socket, etc. */
  const struct vfs_file_ops *ops;
  int flags;
  /* M109: a /proc/<pid>/ns/<kind> descriptor pins the namespace it named at
   * open(), the way Linux's nsfs does. Resolving it again at setns(2) time
   * would follow the task instead of the namespace, so a caller could never
   * hold a handle on the namespace it is about to leave. 0 = not an ns
   * handle; see procfs_ns_open_cb(). */
  u32 ns_pin;
};

/* Encoding of vfs_handle::ns_pin. */
#define VFS_NS_PIN_VALID 0x80000000u
#define VFS_NS_PIN_MAKE(kind, id)                                              \
  (VFS_NS_PIN_VALID | ((u32)(kind) << 24) | ((u32)(id) & 0xFFFFFFu))
#define VFS_NS_PIN_KIND(p) (int)(((p) >> 24) & 0x7Fu)
#define VFS_NS_PIN_ID(p) ((p) & 0xFFFFFFu)

/* Internal handle management for subsystems */
struct vfs_handle *alloc_raw_handle(enum vfs_handle_kind kind);
void vfs_handle_retain(struct vfs_handle *h);
void vfs_handle_release(struct vfs_handle *h);

extern const struct vfs_file_ops node_file_ops;
extern const struct vfs_file_ops pipe_read_ops;
extern const struct vfs_file_ops pipe_write_ops;
extern const struct vfs_file_ops socket_file_ops;

struct vfs_pipe;
struct vfs_socket_state;
struct task;
struct b1nix_aio_sqe;

/* Internal handle initialization helpers (called from subsystems) */
void vfs_pipe_init_handle(struct vfs_handle *h, struct vfs_pipe *pipe,
                          int is_write);
void vfs_socket_init_handle(struct vfs_handle *h, void *socket_state);
int vfs_submit_aio(struct task *owner, const struct b1nix_aio_sqe *sqe);

/* SLAB-style allocator for VFS structures (Phase 4) */
struct vfs_node *vfs_alloc_node(void);
void vfs_free_node(struct vfs_node *node);
struct vfs_inode *vfs_alloc_inode(void);
void vfs_free_inode(struct vfs_inode *inode);
struct vfs_handle *vfs_alloc_handle(void);
void vfs_free_handle(struct vfs_handle *handle);

#endif
