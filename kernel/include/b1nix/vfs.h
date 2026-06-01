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
int vfs_check_access(struct vfs_node *node, int requested_access);

enum vfs_node_type {
  VFS_FILE = 1,
  VFS_DEVICE = 2,
  VFS_DIRECTORY = 3,
  VFS_SYMLINK = 4,
  VFS_SOCKET = 5,
};

/* inode->flags: the VFS owns inode->data and must kfree() it on release. */
#define VFS_NODE_OWNS_DATA 0x80000000u

struct acl_entry {
  u16 tag;       /* ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_GROUP, ACL_MASK,
                    ACL_OTHER */
  u16 qualifier; /* UID or GID (for ACL_USER, ACL_GROUP) */
  u16 perms;     /* Permission bitmask */
};

#define ACL_MAX_ENTRIES 8

/* ACL tag types */
#define ACL_USER_OBJ 0x01
#define ACL_USER 0x02
#define ACL_GROUP_OBJ 0x04
#define ACL_GROUP 0x08
#define ACL_MASK 0x10
#define ACL_OTHER 0x20

struct vfs_node;

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

  /* Timestamps */
  u32 atime;
  u32 mtime;
  u32 ctime;

  struct block_device *blk_dev;
  u32 fs_id; /* Unique ID for this filesystem instance */

  /* Callbacks (Inode Operations) */
  isize (*read_cb)(struct vfs_node *node, u64 offset, char *buffer, usize size,
                   int flags);
  isize (*readdir_cb)(struct vfs_node *node, usize offset, struct dirent *buf,
                      usize max_entries);
  isize (*write_cb)(struct vfs_node *node, u64 offset, const char *buffer,
                    usize size, int flags);
  int (*create_cb)(struct vfs_node *dir, const char *name,
                   const char *full_path, u32 mode);
  int (*mkdir_cb)(struct vfs_node *dir, const char *name, u32 mode);
  int (*unlink_cb)(struct vfs_node *dir, const char *name);
  int (*rmdir_cb)(struct vfs_node *dir, const char *name);
  int (*rename_cb)(struct vfs_node *old_dir, const char *old_name,
                   struct vfs_node *new_dir, const char *new_name);
  int (*link_cb)(struct vfs_node *target, struct vfs_node *dir,
                 const char *name);
  int (*symlink_cb)(struct vfs_node *dir, const char *name, const char *target);
  void (*release_cb)(struct vfs_node *node);
  int (*setattr_cb)(struct vfs_node *node);
  int (*statfs_cb)(struct vfs_node *node, struct b1nix_statfs *st);
  int (*fsync_cb)(struct vfs_node *node);
  int (*poll_cb)(struct vfs_node *node, struct b1nix_pollfd *pfd);
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

u32 vfs_get_unix_time(void);
void vfs_init(void);
void vfs_resolve_path(const char *path, char *out);
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
int vfs_poll(int handle_idx, struct b1nix_pollfd *pfd);
void vfs_close(int handle);
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
int vfs_dup2(int oldfd, int newfd);
int vfs_fcntl(int fd, int cmd, u64 arg);
int vfs_ioctl(int fd, u64 request, void *arg);
void vfs_close_on_exec(void);
int vfs_socket(int domain, int type, int protocol);
int vfs_bind(int fd, const void *addr, usize addrlen);
int vfs_listen(int fd, int backlog);
int vfs_accept(int fd, void *addr, usize *addrlen);
int vfs_connect(int fd, const void *addr, usize addrlen);
isize vfs_socket_send(int fd, const void *buf, usize len, int flags);
isize vfs_socket_recv(int fd, void *buf, usize len, int flags);
int vfs_socket_push_udp(u16 local_port_net, const void *data, usize len);

extern void *vfs_poll_chan;

/* Permission management */
int vfs_chmod(const char *path, u16 mode);
int vfs_fchmod(int fd, u16 mode);
int vfs_utime(const char *path, u32 atime, u32 mtime);
int vfs_chown(const char *path, u16 uid, u16 gid);
int vfs_fchown(int fd, u16 uid, u16 gid);
int vfs_fstatfs(int fd, struct b1nix_statfs *st);
int vfs_syncfs(int fd);
int vfs_get_node_perm(const struct vfs_node *node, const struct cred *cred,
                      u32 mask);
int vfs_set_acl(struct vfs_node *node, const struct acl_entry *acl);
int vfs_get_acl(struct vfs_node *node, struct acl_entry *out_acl,
                int max_entries);

enum vfs_handle_kind {
  VFS_HANDLE_NONE = 0,
  VFS_HANDLE_NODE,
  VFS_HANDLE_PIPE_READ,
  VFS_HANDLE_PIPE_WRITE,
  VFS_HANDLE_SOCKET
};

struct vfs_handle;

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
#define MAX_VFS_PIPES 64
#define MAX_MOUNTS 16
#define PIPE_BUFFER_SIZE 512
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
    struct b1nix_sockaddr_un un;
  } local, peer;
  int bound;
  int connected;
  int listening;
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
