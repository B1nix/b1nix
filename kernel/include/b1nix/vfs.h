#ifndef B1NIX_VFS_H
#define B1NIX_VFS_H

#include <b1nix/dirent.h>
#include <b1nix/posix.h>
#include <b1nix/types.h>
#include <b1nix/uidgid.h>

#define VFS_MAX_PATH 256
#define MS_RDONLY 1

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
#define XATTR_VALUE_MAX 4096

struct vfs_inode {
  u64 ino;
  enum vfs_node_type type;
  u32 flags;
  volatile int rw_lock; /* >0: readers, -1: writer, 0: free */
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

  /* M56 file sealing (memfd). `seals` holds the active F_SEAL_* bitmask;
   * `seals_allowed` is set when the memfd was created with MFD_ALLOW_SEALING.
   * Both are 0 for ordinary files (which therefore behave as F_SEAL_SEAL-set:
   * F_ADD_SEALS is rejected). */
  u32 seals;
  u32 seals_allowed;

  /* Callbacks (Inode Operations) */
  isize (*read_cb)(struct vfs_node *node, u64 offset, char *buffer, usize size,
                   int flags);
  isize (*readdir_cb)(struct vfs_node *node, usize offset, struct dirent *buf,
                      usize max_entries);
  /* On-demand child materialisation for synthetic dirs (procfs/sysfs) whose
   * children exist lazily. The path resolver calls this when find_child misses,
   * so a DIRECT lookup (e.g. musl ttyname's readlink("/proc/self/fd/N"), which
   * never runs readdir first) can still resolve. NULL on all real filesystems,
   * so their lookup path is unchanged. Must materialise `name` as a physical
   * child of `dir` (idempotent) and return 0 on success, <0 if no such child. */
  int (*lookup_cb)(struct vfs_node *dir, const char *name);
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
  int (*statfs_cb)(struct vfs_node *node, struct b1nix_statfs *st);
  int (*fsync_cb)(struct vfs_node *node);
  int (*poll_cb)(struct vfs_node *node, struct b1nix_pollfd *pfd);
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
  /* Mapping-lifetime hooks. Called once per VMA, including fork copies and
   * VMA splits, with a matching close on munmap/exec/exit. */
  void (*mmap_open_cb)(struct vfs_node *node);
  void (*mmap_close_cb)(struct vfs_node *node);
  void (*mmap_range_open_cb)(struct vfs_node *node, u64 offset, usize length);
  void (*mmap_range_close_cb)(struct vfs_node *node, u64 offset, usize length);
};

/* Inode cache API */
void icache_init(void);
struct vfs_inode *icache_get(u32 fs_id, u64 ino);
void icache_insert(u32 fs_id, u64 ino, struct vfs_inode *inode);
void icache_invalidate(u32 fs_id, u64 ino);
void icache_invalidate_fs(u32 fs_id);

struct vfs_node {
  char name[64];
  struct vfs_inode *inode;
  int refcount; /* References to this NAME (e.g. current directory) */
  int deleted;

  struct vfs_node *parent;
  struct vfs_node *first_child;
  struct vfs_node *next_sibling;
};

struct vfs_fs {
  const char *name;
  struct vfs_node *(*mount)(const char *source, u64 flags, void *data);
  int (*umount)(struct vfs_node *root_node);
  struct vfs_fs *next;
};

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
isize vfs_readdir_children(struct vfs_node *dir, usize offset,
                           struct dirent *buf, usize max_entries);

/* Permission-aware operations */
int vfs_open(const char *path);
int vfs_open_flags(const char *path, int flags);
isize vfs_read(int handle, char *buffer, usize size);
isize vfs_write(int handle, const char *buffer, usize size);
/* Positioned I/O: read/write at `offset` without touching the fd's own offset
 * (thread-safe pread/pwrite; non-seekable handles return ESPIPE). */
isize vfs_pread(int handle, char *buffer, usize size, u64 offset);
isize vfs_pwrite(int handle, const char *buffer, usize size, u64 offset);
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
int vfs_fsync(int fd);
int vfs_mount(const char *source, const char *target, const char *fstype,
              u64 flags);
int vfs_umount(const char *target);
void vfs_register_fs(struct vfs_fs *fs);
void vfs_set_currently_mounting_root(struct vfs_node *root);
isize vfs_mounts(struct b1nix_mount_entry *out, usize max_entries);
int vfs_sync(void);
isize vfs_getdents(int handle, struct dirent *buf, usize max_entries);
int vfs_pipe(int pipefd[2]);

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
#define VFS_SCM_MAX_FDS 8
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
int vfs_socket_push_udp(u16 local_port_net, const void *data, usize len);

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
  isize (*write)(struct vfs_handle *h, const char *buf, usize len);
  int (*poll)(struct vfs_handle *h, struct b1nix_pollfd *pfd);
  isize (*lseek)(struct vfs_handle *h, isize offset, int whence);
  int (*close)(struct vfs_handle *h);
  void (*release)(struct vfs_handle *h);
  int (*getdents)(struct vfs_handle *h, struct dirent *buf, usize max_entries);
  int (*ioctl)(struct vfs_handle *h, u64 request, void *arg);
};

#define MAX_VFS_NODES 4096
#define MAX_VFS_PIPES 128
#define MAX_MOUNTS 32
#define PIPE_BUFFER_SIZE 4096
#define TTY_INPUT_SIZE 256

struct vfs_pipe {
  int used;
  char buffer[PIPE_BUFFER_SIZE];
  usize size;
  usize read_pos;
  usize write_pos;
  int readers;
  int writers;
  volatile int lock;
};

struct vfs_socket_state {
  int domain;
  int type;
  int protocol;
  union {
    struct b1nix_sockaddr_in in;
    struct b1nix_sockaddr_in6 in6;
    struct b1nix_sockaddr_un un;
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
  int tcp_nodelay;
  int ipv6_v6only;
  int so_error;
  int so_rcvbuf;
  int so_sndbuf;
  int backlog;
  int shut_rd;
  int shut_wr;
  void *tcp_conn;
  void *unix_data;
  char recv_buf[2048];
  usize recv_len;
  char udp_q_buf[8][2048];
  usize udp_q_len[8];
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
};

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
