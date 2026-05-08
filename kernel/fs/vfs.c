#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/fat32.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/sched.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>
#include <b1nix/filelock.h>
#include <stdio.h>
#include <string.h>

#define MAX_VFS_NODES 4096
#define MAX_VFS_HANDLES 512
#define MAX_VFS_PIPES 64
#define MAX_MOUNTS 16
#define PIPE_BUFFER_SIZE 512
#define TTY_INPUT_SIZE 256
#define VFS_NODE_OWNS_DATA 0x80000000u

static volatile int vfs_lock_val = 0;
static volatile usize vfs_lock_owner = 0;
static volatile int vfs_lock_count = 0;
static volatile int handle_lock = 0;
static u32 next_fs_id = 1;

static void vfs_acquire_handle_lock(void) {
  while (__atomic_test_and_set(&handle_lock, __ATOMIC_ACQUIRE)) scheduler_yield();
}

static void vfs_release_handle_lock(void) {
  __atomic_clear(&handle_lock, __ATOMIC_RELEASE);
}

static void vfs_acquire_lock(void) {
  usize tid = scheduler_get_pid();
  if (vfs_lock_owner == tid && vfs_lock_val) {
    vfs_lock_count++;
    return;
  }
  while (__atomic_test_and_set(&vfs_lock_val, __ATOMIC_ACQUIRE)) {
    scheduler_yield();
  }
  vfs_lock_owner = tid;
  vfs_lock_count = 1;
}

static void vfs_release_lock(void) {
  if (--vfs_lock_count == 0) {
    vfs_lock_owner = 0;
    __atomic_clear(&vfs_lock_val, __ATOMIC_RELEASE);
  }
}

static void vfs_inode_lock(struct vfs_inode *inode) {
  while (__atomic_test_and_set(&inode->lock, __ATOMIC_ACQUIRE)) {
    scheduler_yield();
  }
}

static void vfs_inode_unlock(struct vfs_inode *inode) {
  __atomic_clear(&inode->lock, __ATOMIC_RELEASE);
}

static u16 bswap16(u16 v) { return (u16)((v << 8) | (v >> 8)); }

static const struct cred *get_current_cred(void) {
  return scheduler_get_current_cred();
}

u32 vfs_get_unix_time(void) {
  return (u32)scheduler_get_uptime_ticks();
}

static u16 scheduler_get_current_umask(void) {
  const struct cred *cred = get_current_cred();
  return cred ? cred->umask : 0022;
}

enum vfs_handle_kind {
  VFS_HANDLE_NONE = 0,
  VFS_HANDLE_NODE,
  VFS_HANDLE_PIPE_READ,
  VFS_HANDLE_PIPE_WRITE,
  VFS_HANDLE_SOCKET,
};

struct vfs_pipe {
  int used;
  volatile int lock;
  char buffer[PIPE_BUFFER_SIZE];
  usize read_pos;
  usize write_pos;
  usize size;
  int readers;
  int writers;
};

struct vfs_socket_state {
  int domain;
  int type;
  int protocol;
  int bound;
  int connected;
  struct b1nix_sockaddr_in local;
  struct b1nix_sockaddr_in peer;
  u8 recv_buf[2048];
  usize recv_len;
  void *tcp_conn;
};

struct vfs_handle {
  int used;
  int refcount;
  enum vfs_handle_kind kind;
  struct vfs_node *node;
  usize offset;
  struct vfs_pipe *pipe;
  struct vfs_socket_state socket;
  int flags;
};

static struct vfs_node nodes[MAX_VFS_NODES];
static struct vfs_inode inodes[MAX_VFS_NODES];
static struct vfs_handle handles[MAX_VFS_HANDLES];
static struct vfs_pipe pipes[MAX_VFS_PIPES];
/* Dedicated UDP binding table for O(1) lookups */
struct udp_binding {
  u16 port;
  int handle_idx;
};
static struct udp_binding udp_bindings[MAX_VFS_HANDLES];

struct vfs_mount_entry {
  int used;
  char source[VFS_MAX_PATH];
  char target[VFS_MAX_PATH];
  char fstype[16];
  u64 flags;
  struct vfs_node *root_node;
  struct vfs_node *mount_point;
};
static struct vfs_mount_entry mounts[MAX_MOUNTS];
static usize node_count = 0;
static struct vfs_node *root_node = 0;
static struct b1nix_termios tty_termios;
static char tty_line[TTY_INPUT_SIZE];
static usize tty_line_pos;
static usize tty_line_len;

void virtio_blk_init(void);
#ifndef __aarch64__
extern char ps2_kbd_getc(void);
#endif

int vfs_mount(const char *source, const char *target, const char *fstype,
              u64 flags);
static void copy_path(char *dst, usize dst_size, const char *src);
static int split_parent_path(const char *path, char *parent_path, char *name);


/* ── Permission Helpers ── */

extern struct cred *scheduler_get_current_cred(void);

int vfs_get_node_perm(const struct vfs_node *node, const struct cred *cred, u32 mask) {
  if (!node || !node->inode || !cred) return 0;
  struct vfs_inode *inode = node->inode;
  if (cred->euid == ROOT_UID) return 1;
  if (cred_has_cap(cred, CAP_DAC_OVERRIDE)) return 1;
  if (!(mask & 2) && cred_has_cap(cred, CAP_DAC_READ_SEARCH)) return 1;

  if (inode->acl_count > 0) {
    u16 matched_perms = 0;
    int mask_found = 0;
    u16 mask_perms = 0;
    for (int i = 0; i < inode->acl_count; i++) {
      if (inode->acls[i].tag == ACL_MASK) {
        mask_found = 1;
        mask_perms = inode->acls[i].perms;
      }
    }
    for (int i = 0; i < inode->acl_count; i++) {
      switch (inode->acls[i].tag) {
        case ACL_USER_OBJ: if (cred->euid == inode->uid) { matched_perms = inode->acls[i].perms; goto acl_check; } break;
        case ACL_USER: if (cred->euid == inode->acls[i].qualifier) { matched_perms = inode->acls[i].perms; goto acl_check; } break;
        case ACL_GROUP_OBJ: if (cred->egid == inode->gid) { matched_perms = inode->acls[i].perms; goto acl_check; } break;
        case ACL_GROUP: 
          if (cred->egid == inode->acls[i].qualifier) { matched_perms = inode->acls[i].perms; goto acl_check; }
          for (int g = 0; g < cred->ngroups; g++) { if (cred->groups[g] == inode->acls[i].qualifier) { matched_perms = inode->acls[i].perms; goto acl_check; } }
          break;
      }
    }
    matched_perms = inode->mode & 7;

acl_check:
    if (mask_found) matched_perms &= mask_perms;
    return (matched_perms & mask) == mask;
  }
  return cred_can_access(cred, inode->uid, inode->gid, inode->mode, mask);
}

/* ── Node/Inode allocation ── */

static struct vfs_inode *alloc_inode(void) {
  for (int i = 0; i < MAX_VFS_NODES; i++) {
    if (inodes[i].nlink == 0 && inodes[i].refcount == 0) {
      memset(&inodes[i], 0, sizeof(struct vfs_inode));
      inodes[i].nlink = 1;
      return &inodes[i];
    }
  }
  return 0;
}

void vfs_inode_get(struct vfs_inode *inode) {
  if (inode) inode->refcount++;
}

void vfs_inode_put(struct vfs_inode *inode) {
  if (!inode) return;
  inode->refcount--;
  if (inode->refcount == 0 && inode->nlink == 0) {
    if (inode->data && (inode->flags & VFS_NODE_OWNS_DATA)) {
      kfree(inode->data);
    }
    memset(inode, 0, sizeof(struct vfs_inode));
  }
}

static struct vfs_node *alloc_node(void) {
  for (int i = 0; i < MAX_VFS_NODES; i++) {
    if (!nodes[i].inode && nodes[i].refcount == 0) {
      memset(&nodes[i], 0, sizeof(struct vfs_node));
      nodes[i].refcount = 1;
      return &nodes[i];
    }
  }
  if (node_count < MAX_VFS_NODES) return &nodes[node_count++];
  return 0;
}

void vfs_node_get(struct vfs_node *node) {
  if (node) node->refcount++;
}

void vfs_node_put(struct vfs_node *node) {
  if (!node) return;
  node->refcount--;
  if (node->refcount == 0 && node->deleted) {
    vfs_inode_put(node->inode);
    memset(node, 0, sizeof(struct vfs_node));
  }
}

static void split_path(const char *path, char *first_part, const char **rest) {
  while (*path == '/') path++;
  if (*path == '\0') { first_part[0] = '\0'; *rest = 0; return; }
  usize i = 0;
  while (path[i] != '\0' && path[i] != '/') { first_part[i] = path[i]; i++; }
  first_part[i] = '\0';
  *rest = path + i;
}

static struct vfs_node *find_child(struct vfs_node *parent, const char *name) {
  if (!parent || !parent->inode || parent->inode->type != VFS_DIRECTORY) return 0;
  struct vfs_node *child = parent->first_child;
  while (child) {
    if (!child->deleted && strcmp(child->name, name) == 0) return child;
    child = child->next_sibling;
  }
  return 0;
}

static void append_path_part(char *dst, usize dst_size, const char *part) {
  usize len = strlen(dst);
  if (len == 0 && dst_size > 1) {
    dst[0] = '/';
    dst[1] = '\0';
    len = 1;
  }
  if (len > 1 && len < dst_size - 1) {
    dst[len++] = '/';
    dst[len] = '\0';
  }
  usize i = 0;
  while (part[i] && len < dst_size - 1)
    dst[len++] = part[i++];
  dst[len] = '\0';
}

static void pop_path_part(char *path) {
  usize len = strlen(path);
  if (len <= 1) {
    if (len == 0) {
      path[0] = '/';
      path[1] = '\0';
    }
    return;
  }
  if (path[len - 1] == '/')
    len--;
  while (len > 1 && path[len - 1] != '/')
    len--;
  path[len] = '\0';
}

static void compose_symlink_path(const char *parent_path, const char *target,
                                 const char *rest, char *out, usize out_size) {
  out[0] = '\0';
  if (!target || target[0] == '\0')
    return;

  if (target[0] == '/') {
    copy_path(out, out_size, target);
  } else {
    copy_path(out, out_size, parent_path && parent_path[0] ? parent_path : "/");
    append_path_part(out, out_size, target);
  }

  if (rest && rest[0]) {
    while (*rest == '/')
      rest++;
    if (*rest)
      append_path_part(out, out_size, rest);
  }
}

void vfs_resolve_path(const char *path, char *out) {
  if (!path || !out) return;
  char combined[VFS_MAX_PATH];
  if (path[0] == '/') { strncpy(combined, path, VFS_MAX_PATH); }
  else {
    const char *cwd = scheduler_get_cwd();
    strncpy(combined, cwd, VFS_MAX_PATH);
    usize len = strlen(combined);
    if (len > 0 && combined[len-1] != '/' && len < VFS_MAX_PATH-1) { combined[len++] = '/'; combined[len] = '\0'; }
    strncat(combined, path, VFS_MAX_PATH - len - 1);
  }

  char *parts[64]; int part_count = 0;
  char tmp[VFS_MAX_PATH]; strncpy(tmp, combined, VFS_MAX_PATH);
  char *curr = tmp;
  while (*curr && part_count < 64) {
    while (*curr == '/') curr++;
    if (!*curr) break;
    char *start = curr;
    while (*curr && *curr != '/') curr++;
    if (*curr) { *curr = '\0'; curr++; }
    if (strcmp(start, ".") == 0) continue;
    if (strcmp(start, "..") == 0) { if (part_count > 0) part_count--; continue; }
    parts[part_count++] = start;
  }
  out[0] = '/'; out[1] = '\0';
  for (int i = 0; i < part_count; i++) {
    strcat(out, parts[i]);
    if (i < part_count - 1) strcat(out, "/");
  }
}

/* POSIX: Iterative path resolution with symlink loop detection to prevent stack overflow */
static struct vfs_node *vfs_find_node_internal(const char *path, int follow_final, int depth_unused) {
  (void)depth_unused;
  if (!root_node || !path) return ERR_PTR(-ENOENT);
  
  char *curr_path = kmalloc(VFS_MAX_PATH);
  if (!curr_path) return ERR_PTR(-ENOMEM);
  strncpy(curr_path, path, VFS_MAX_PATH - 1);
  curr_path[VFS_MAX_PATH - 1] = '\0';

  struct vfs_node *current = root_node;
  int symlink_count = 0;

  while (symlink_count < 16) {
    char part[64];
    const char *rest = curr_path;
    char parent_path[VFS_MAX_PATH];
    parent_path[0] = '/'; parent_path[1] = '\0';

    if (curr_path[0] == '/') {
      current = root_node;
      while (*rest == '/') rest++;
    }

    while (1) {
      split_path(rest, part, &rest);
      if (part[0] == '\0') {
        kfree(curr_path);
        return current;
      }

      if (current->inode->type != VFS_DIRECTORY) { kfree(curr_path); return ERR_PTR(-ENOTDIR); }
      if (strcmp(part, ".") == 0) continue;

      const struct cred *cred = get_current_cred();
      if (cred && !vfs_get_node_perm(current, cred, 1)) { kfree(curr_path); return ERR_PTR(-EACCES); }

      if (strcmp(part, "..") == 0) {
        /* MOUNT CROSSING */
        for (int i = 0; i < MAX_MOUNTS; i++) {
          if (mounts[i].used && current == mounts[i].root_node) {
            struct vfs_node *mnt_point = mounts[i].mount_point;
            if (mnt_point && mnt_point->parent) current = mnt_point->parent;
            pop_path_part(parent_path);
            goto next_tok;
          }
        }
        if (current->parent) current = current->parent;
        pop_path_part(parent_path);
        next_tok: continue;
      }

      struct vfs_node *child = find_child(current, part);
      if (!child) { kfree(curr_path); return ERR_PTR(-ENOENT); }

      int is_final = (!rest || rest[0] == '\0');
      if (child->inode->type == VFS_SYMLINK && (follow_final || !is_final)) {
        symlink_count++;
        char *next_path = kmalloc(VFS_MAX_PATH);
        if (!next_path) { kfree(curr_path); return ERR_PTR(-ENOMEM); }
        compose_symlink_path(parent_path, (const char *)child->inode->data, rest, next_path, VFS_MAX_PATH);
        kfree(curr_path);
        curr_path = next_path;
        goto restart_lookup;
      }
      current = child;
      if (!is_final) append_path_part(parent_path, sizeof(parent_path), part);
    }
    restart_lookup:;
  }

  kfree(curr_path);
  return ERR_PTR(-ELOOP);
}

struct vfs_node *vfs_find_node(const char *path) {
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(path, resolved);
  return vfs_find_node_internal(resolved, 1, 0);
}

static struct vfs_node *vfs_find_node_no_follow(const char *path) {
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(path, resolved);
  return vfs_find_node_internal(resolved, 0, 0);
}

static struct vfs_node *add_node(const char *path, enum vfs_node_type type,
                                 void *data, usize size, u32 flags) {
  if (!root_node) {
    root_node = alloc_node();
    if (!root_node) return ERR_PTR(-ENOMEM);
    root_node->inode = alloc_inode();
    if (!root_node->inode) return ERR_PTR(-ENOMEM);
    
    root_node->name[0] = '/';
    root_node->name[1] = '\0';
    root_node->inode->type = VFS_DIRECTORY;
    root_node->inode->mode = 0755;
    root_node->inode->atime = root_node->inode->mtime = root_node->inode->ctime = vfs_get_unix_time();
  }

  char part[64];
  const char *rest = path;
  struct vfs_node *current = root_node;

  while (1) {
    split_path(rest, part, &rest);
    if (part[0] == '\0')
      return current;

    struct vfs_node *child = find_child(current, part);
    int is_leaf =
        (!rest || rest[0] == '\0' || (rest[0] == '/' && rest[1] == '\0'));

    if (is_leaf) {
      if (!child) {
        child = alloc_node();
        if (!child) return ERR_PTR(-ENOMEM);
        child->inode = alloc_inode();
        if (!child->inode) return ERR_PTR(-ENOMEM);

        copy_path(child->name, 64, part);
        child->inode->type = type;
        child->inode->data = data;
        child->inode->size = size;
        child->inode->flags = flags;
        child->parent = current;
        child->next_sibling = current->first_child;
        current->first_child = child;

        const struct cred *cred = get_current_cred();
        child->inode->uid = cred ? cred->euid : ROOT_UID;
        child->inode->gid = cred ? cred->egid : ROOT_GID;
        
        u16 umask = scheduler_get_current_umask();
        if (type == VFS_DIRECTORY)
          child->inode->mode = 0777 & ~umask;
        else
          child->inode->mode = 0666 & ~umask;
          
        if (flags & INITRAMFS_EXECUTABLE)
          child->inode->mode |= VFS_IXUSR | VFS_IXGRP | VFS_IXOTH;

        child->inode->atime = child->inode->mtime = child->inode->ctime = vfs_get_unix_time();
      } else if (data != 0 || size != 0 || flags != 0 || type == VFS_DIRECTORY) {
        child->inode->type = type;
        child->inode->data = data;
        child->inode->size = size;
        child->inode->flags = flags;
        child->inode->mtime = child->inode->ctime = vfs_get_unix_time();
      } else {
        return ERR_PTR(-EEXIST);
      }
      return child;
    } else {
      if (!child) {
        child = alloc_node();
        if (!child) return ERR_PTR(-ENOMEM);
        child->inode = alloc_inode();
        if (!child->inode) return ERR_PTR(-ENOMEM);

        copy_path(child->name, 64, part);
        child->inode->type = VFS_DIRECTORY;
        
        const struct cred *cred = get_current_cred();
        child->inode->uid = cred ? cred->euid : ROOT_UID;
        child->inode->gid = cred ? cred->egid : ROOT_GID;

        u16 umask = scheduler_get_current_umask();
        child->inode->mode = 0777 & ~umask;
        child->inode->atime = child->inode->mtime = child->inode->ctime = vfs_get_unix_time();

        child->parent = current;
        child->next_sibling = current->first_child;
        current->first_child = child;
      }
      current = child;
    }
  }
}

struct vfs_node *vfs_add_node(const char *path, enum vfs_node_type type,
                              void *data, usize size, u32 flags) {
  return add_node(path, type, data, size, flags);
}



static int alloc_raw_handle(enum vfs_handle_kind kind) {
  for (usize i = 0; i < MAX_VFS_HANDLES; i++) {
    if (!handles[i].used) {
      memset(&handles[i], 0, sizeof(handles[i]));
      handles[i].used = 1;
      handles[i].refcount = 1;
      handles[i].kind = kind;
      return (int)i;
    }
  }
  return -ENFILE;
}

void vfs_handle_retain(int handle) {
  if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES || !handles[handle].used)
    return;
  handles[handle].refcount++;
}

static struct vfs_handle *get_handle(int fd) {
  int handle = scheduler_fd_get(fd);
  if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES || !handles[handle].used)
    return 0;
  return &handles[handle];
}

static void copy_path(char *dst, usize dst_size, const char *src) {
  if (!dst || dst_size == 0)
    return;
  if (!src)
    src = "";
  usize len = strlen(src);
  if (len >= dst_size)
    len = dst_size - 1;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static void release_handle(int handle) {
  if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES || !handles[handle].used) return;
  if (handles[handle].refcount > 1) { handles[handle].refcount--; return; }

  if (handles[handle].kind == VFS_HANDLE_PIPE_READ && handles[handle].pipe) {
    handles[handle].pipe->readers--;
    if (handles[handle].pipe->readers <= 0 && handles[handle].pipe->writers <= 0) handles[handle].pipe->used = 0;
  } else if (handles[handle].kind == VFS_HANDLE_PIPE_WRITE && handles[handle].pipe) {
    handles[handle].pipe->writers--;
    if (handles[handle].pipe->readers <= 0 && handles[handle].pipe->writers <= 0) handles[handle].pipe->used = 0;
  } else if (handles[handle].kind == VFS_HANDLE_NODE) {
    vfs_node_put(handles[handle].node);
  }
  handles[handle].used = 0;
  handles[handle].refcount = 0;
  handles[handle].kind = VFS_HANDLE_NONE;
  handles[handle].node = 0;
  handles[handle].offset = 0;
  handles[handle].pipe = 0;
  handles[handle].flags = 0;
}

void vfs_handle_release(int handle) { release_handle(handle); }

static char tty_getc_blocking(void) {
#ifdef __aarch64__
  return 0;
#else
  char c = 0;
  while (c == 0) {
    c = ps2_kbd_getc();
    if (c == 0)
      scheduler_yield();
  }
  return c;
#endif
}

static isize tty_read(struct vfs_node *node, u64 offset, char *buffer,
                      usize size) {
  (void)node;
  (void)offset;
  if (!buffer || size == 0)
    return 0;

  if ((tty_termios.c_lflag & B1NIX_ICANON) == 0) {
    for (usize i = 0; i < size; i++)
      buffer[i] = tty_getc_blocking();
    return (isize)size;
  }

  while (tty_line_pos >= tty_line_len) {
    tty_line_pos = 0;
    tty_line_len = 0;

    while (tty_line_len < sizeof(tty_line) - 1) {
      char c = tty_getc_blocking();
      if (c == 0)
        return 0;
      if (c == 27) {
        char next = tty_getc_blocking();
        if (next == '[') {
          (void)tty_getc_blocking();
          continue;
        }
        continue;
      }
      if ((tty_termios.c_lflag & B1NIX_ISIG) && c == 3) {
        console_write("^C\n");
        tty_line_len = 0;
        tty_line[tty_line_len++] = '\n';
        break;
      }
      if ((tty_termios.c_lflag & B1NIX_ISIG) && c == 26) {
        console_write("^Z\n");
        tty_line_len = 0;
        tty_line[tty_line_len++] = '\n';
        break;
      }
      if (c == 4)
        break;
      if (c == '\b' || c == 127) {
        if (tty_line_len > 0) {
          tty_line_len--;
          if (tty_termios.c_lflag & B1NIX_ECHO)
            console_write("\b \b");
        }
        continue;
      }
      tty_line[tty_line_len++] = c;
      if (tty_termios.c_lflag & B1NIX_ECHO)
        console_putc(c);
      if (c == '\n')
        break;
    }
  }

  usize copied = 0;
  while (copied < size && tty_line_pos < tty_line_len)
    buffer[copied++] = tty_line[tty_line_pos++];
  return (isize)copied;
}

static isize tty_write(struct vfs_node *node, u64 offset, const char *buffer,
                       usize size) {
  (void)node;
  (void)offset;
  if (!buffer)
    return -1;
  for (usize i = 0; i < size; i++) {
    if ((tty_termios.c_oflag & B1NIX_OPOST) && buffer[i] == '\n')
      console_putc('\r');
    console_putc(buffer[i]);
  }
  return (isize)size;
}

static void tty_init_node(void) {
  memset(&tty_termios, 0, sizeof(tty_termios));
  tty_termios.c_lflag = B1NIX_ICANON | B1NIX_ECHO | B1NIX_ISIG;
  tty_termios.c_oflag = B1NIX_OPOST;
  struct vfs_node *tty = add_node("/dev/tty", VFS_DEVICE, 0, 0, 0);
  if (tty) {
    tty->inode->read_cb = tty_read;
    tty->inode->write_cb = tty_write;
    tty->inode->mode =
        VFS_IRUSR | VFS_IWUSR | VFS_IRGRP | VFS_IWGRP | VFS_IROTH | VFS_IWOTH;
  }
}

/* Forward declarations for internal VFS metadata operations (thread-unsafe variants) */
static int vfs_create_internal(const char *path, const char *data);
static int vfs_mkdir_internal(const char *path);
static int vfs_rename_internal(const char *old_path, const char *new_path);

static void vfs_init_stdio(void) {
  scheduler_fd_table_init_current();
  int tty = vfs_open_flags("/dev/tty", B1NIX_O_RDWR);
  if (tty < 0)
    return;
  vfs_dup2(tty, 0);
  vfs_dup2(tty, 1);
  vfs_dup2(tty, 2);
}

void vfs_init(void) {
  node_count = 0;
  memset(handles, 0, sizeof(handles));
  memset(pipes, 0, sizeof(pipes));
  memset(mounts, 0, sizeof(mounts));
  memset(nodes, 0, sizeof(nodes));

  root_node = alloc_node();
  root_node->inode = alloc_inode();
  strcpy(root_node->name, "/");
  root_node->inode->type = VFS_DIRECTORY;
  root_node->inode->mode = 0755;
  root_node->inode->atime = root_node->inode->mtime = root_node->inode->ctime = vfs_get_unix_time();

  add_node("/dev", VFS_DIRECTORY, 0, 0, 0);
  add_node("/home", VFS_DIRECTORY, 0, 0, 0);
  add_node("/tmp", VFS_DIRECTORY, 0, 0, 0);
  add_node("/var", VFS_DIRECTORY, 0, 0, 0);
  add_node("/mnt", VFS_DIRECTORY, 0, 0, 0);
  add_node("/proc", VFS_DIRECTORY, 0, 0, 0);

  for (usize i = 0; i < initramfs_count(); i++) {
    const struct initramfs_file *file = initramfs_get(i);
    vfs_add_node(file->path, VFS_FILE, (void *)file->data, file->size, file->flags);
  }
  
  add_node("/dev/console", VFS_DEVICE, 0, 0, 0);
  add_node("/dev/virtio-blk0", VFS_DEVICE, 0, 0, 0);
  vfs_create("/tmp/hello", "tmpfs says hello\n");
  vfs_mount("initramfs", "/", "initramfs", 0);
  tty_init_node();
  vfs_init_stdio();

#ifndef __aarch64__
  virtio_blk_init();
  struct block_device *blk = blk_get("virtio-blk0");
  if (blk) fat32_mount(blk, "/mnt");
#endif

  console_write("vfs: full featured initialized (POSIX+, Refcounting, Mount Crossing)\n");
}

int vfs_open(const char *path) { return vfs_open_flags(path, B1NIX_O_RDONLY); }

int vfs_open_flags(const char *path, int flags) {
  vfs_acquire_lock();
  int res = 0;
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(path, resolved);
  struct vfs_node *node = vfs_find_node(resolved);
  if (IS_ERR(node)) {
    if (PTR_ERR(node) == -ENOENT && (flags & B1NIX_O_CREAT)) {
      int err = vfs_create_internal(resolved, "");
      if (err != 0) { res = err; goto out; }
      node = vfs_find_node(resolved);
      if (IS_ERR(node)) { res = (int)PTR_ERR(node); goto out; }
    } else {
      res = (int)PTR_ERR(node);
      goto out;
    }
  } else {
    if ((flags & B1NIX_O_CREAT) && (flags & B1NIX_O_EXCL)) {
      res = -EEXIST;
      goto out;
    }
  }

  if ((flags & B1NIX_O_DIRECTORY) && node->inode->type != VFS_DIRECTORY) { res = -ENOTDIR; goto out; }
  /* POSIX: writing to a directory descriptor is not permitted */
  if (node->inode->type == VFS_DIRECTORY && (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))) { res = -EISDIR; goto out; }

  const struct cred *cred = get_current_cred();
  u32 access_mask = 0;
  if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR)) access_mask |= 2;
  if ((flags & 3) == B1NIX_O_RDONLY || (flags & B1NIX_O_RDWR)) access_mask |= 4;
  if (cred && !vfs_get_node_perm(node, cred, access_mask)) { res = -EACCES; goto out; }

  if ((flags & B1NIX_O_TRUNC) && node->inode->type == VFS_FILE) {
    /* O_TRUNC requires write permission regardless of open mode */
    const struct cred *tc = get_current_cred();
    if (tc && !vfs_get_node_perm(node, tc, 2)) { res = -EACCES; goto out; }
    node->inode->size = 0; if (node->inode->data) ((char*)node->inode->data)[0] = '\0';
  }

  vfs_acquire_handle_lock();
  int h_idx = -1;
  for (int i = 0; i < MAX_VFS_HANDLES; i++) { if (!handles[i].used) { h_idx = i; break; } }
  if (h_idx < 0) { vfs_release_handle_lock(); res = -ENFILE; goto out; }

  struct vfs_handle *h = &handles[h_idx];
  memset(h, 0, sizeof(*h));
  h->used = 1; h->refcount = 1; h->kind = VFS_HANDLE_NODE; h->node = node;
  h->flags = flags; h->offset = (flags & B1NIX_O_APPEND) ? node->inode->size : 0;
  vfs_node_get(node);
  vfs_release_handle_lock();

  int fd = scheduler_fd_alloc(h_idx);
  if (fd < 0) { 
    vfs_acquire_handle_lock();
    release_handle(h_idx); 
    vfs_release_handle_lock();
    res = -EMFILE; goto out; 
  }
  if (flags & B1NIX_O_CLOEXEC) scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  res = fd;

out:
  vfs_release_lock();
  return res;
}

isize vfs_read(int fd, char *buf, usize size) {
  isize res = 0;
  vfs_acquire_handle_lock();
  int h_idx = scheduler_fd_get(fd);
  if (h_idx < 0) { vfs_release_handle_lock(); return -EBADF; }
  struct vfs_handle *h = &handles[h_idx];
  if (!h->used || (h->flags & 3) == B1NIX_O_WRONLY) { vfs_release_handle_lock(); return -EBADF; }

  /* Increment refcounts to protect handle and node during unlocked I/O */
  h->refcount++;
  struct vfs_node *node = h->node;
  if (node) vfs_node_get(node);
  enum vfs_handle_kind kind = h->kind;
  struct vfs_pipe *pipe = h->pipe;
  usize offset = h->offset;
  vfs_release_handle_lock();

  if (kind == VFS_HANDLE_PIPE_READ) {
    if (!pipe || !pipe->used) { res = -EIO; goto out; }
    
    /* Acquire pipe-specific lock */
    while (__atomic_test_and_set(&pipe->lock, __ATOMIC_ACQUIRE)) scheduler_yield();

    usize to_read = size < pipe->size ? size : pipe->size;
    for (usize i = 0; i < to_read; i++) {
      buf[i] = pipe->buffer[pipe->read_pos];
      pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUFFER_SIZE;
    }
    pipe->size -= to_read;
    res = (isize)to_read;

    /* Release pipe-specific lock */
    __atomic_clear(&pipe->lock, __ATOMIC_RELEASE);
    goto out;
  }

  if (kind == VFS_HANDLE_SOCKET) {
    res = vfs_socket_recv(fd, buf, size, 0);
    goto out;
  }

  if (kind != VFS_HANDLE_NODE || !node) { res = -EBADF; goto out; }

  node->inode->atime = vfs_get_unix_time();

  if (node->inode->read_cb) {
    res = node->inode->read_cb(node, offset, buf, size);
    if (res > 0) __atomic_add_fetch(&h->offset, (usize)res, __ATOMIC_RELAXED);
    goto out;
  }
  
  if (node->inode->type == VFS_DEVICE || node->inode->type == VFS_DIRECTORY) { res = 0; goto out; }

  vfs_inode_lock(node->inode);
  usize rem = node->inode->size > offset ? node->inode->size - offset : 0;
  usize to_r = size < rem ? size : rem;
  if (to_r > 0) {
    memcpy(buf, (const char*)node->inode->data + offset, to_r);
  }
  vfs_inode_unlock(node->inode);

  if (to_r > 0) __atomic_add_fetch(&h->offset, to_r, __ATOMIC_RELAXED);
  res = (isize)to_r;

out:
  vfs_acquire_lock();
  if (node) vfs_node_put(node);
  vfs_release_lock();

  vfs_acquire_handle_lock();
  release_handle(h_idx);
  vfs_release_handle_lock();
  return res;
}

isize vfs_write(int fd, const char *buf, usize size) {
  isize res = 0;
  vfs_acquire_handle_lock();
  int h_idx = scheduler_fd_get(fd);
  if (h_idx < 0) { vfs_release_handle_lock(); return -EBADF; }
  struct vfs_handle *h = &handles[h_idx];
  if (!h->used || (h->flags & 3) == B1NIX_O_RDONLY) { vfs_release_handle_lock(); return -EBADF; }
  
  if (h->kind == VFS_HANDLE_NODE && h->node && (h->flags & B1NIX_O_APPEND)) h->offset = h->node->inode->size;

  /* Protect handle and node during potential unlocked I/O */
  h->refcount++;
  struct vfs_node *node = h->node;
  if (node) vfs_node_get(node);
  enum vfs_handle_kind kind = h->kind;
  struct vfs_pipe *pipe = h->pipe;
  usize offset = h->offset;
  vfs_release_handle_lock();

  if (kind == VFS_HANDLE_PIPE_WRITE) {
    if (!pipe || !pipe->used) { res = -EIO; goto out; }
    
    while (__atomic_test_and_set(&pipe->lock, __ATOMIC_ACQUIRE)) scheduler_yield();
    usize written = 0;
    while (written < size && pipe->size < PIPE_BUFFER_SIZE) {
      pipe->buffer[pipe->write_pos] = buf[written++];
      pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUFFER_SIZE;
      pipe->size++;
    }
    res = (isize)written;
    __atomic_clear(&pipe->lock, __ATOMIC_RELEASE);
    goto out;
  }

  if (kind == VFS_HANDLE_SOCKET) {
    res = vfs_socket_send(fd, buf, size, 0);
    goto out;
  }

  if (kind != VFS_HANDLE_NODE || !node) { res = -EBADF; goto out; }

  if (node->inode->write_cb) {
    res = node->inode->write_cb(node, offset, buf, size);
    if (res > 0) __atomic_add_fetch(&h->offset, (usize)res, __ATOMIC_RELAXED);
    goto out;
  }

  if (node->inode->type == VFS_DEVICE && strcmp(node->name, "console") == 0) {
    for (usize i = 0; i < size; i++) console_putc(buf[i]);
    res = (isize)size;
    goto out;
  }

  if (node->inode->type == VFS_FILE) {
    vfs_inode_lock(node->inode);
    usize need = offset + size;
    if (need > node->inode->capacity) {
      usize new_cap = node->inode->capacity == 0 ? 64 : node->inode->capacity;
      while (new_cap < need) new_cap *= 2;
      char *new_d = kmalloc(new_cap);
      if (!new_d) { vfs_inode_unlock(node->inode); res = -ENOMEM; goto out; }
      if (node->inode->data) memcpy(new_d, node->inode->data, node->inode->size);
      if (new_cap > node->inode->size) memset(new_d + node->inode->size, 0, new_cap - node->inode->size);
      if (node->inode->data && (node->inode->flags & VFS_NODE_OWNS_DATA)) kfree(node->inode->data);
      node->inode->data = new_d;
      node->inode->capacity = new_cap;
      node->inode->flags |= VFS_NODE_OWNS_DATA;
    }
    memcpy((char*)node->inode->data + offset, buf, size);
    if (need > node->inode->size) node->inode->size = need;
    vfs_inode_unlock(node->inode);

    __atomic_add_fetch(&h->offset, size, __ATOMIC_RELAXED);
    res = (isize)size;
    goto out;
  }
  res = -EINVAL;

out:
  /* 1. Работаем с узлом (влияет на глобальное дерево VFS) */
  vfs_acquire_lock();
  if (node) vfs_node_put(node);
  vfs_release_lock();

  /* 2. Работаем с таблицей дескрипторов */
  vfs_acquire_handle_lock();
  release_handle(h_idx);
  vfs_release_handle_lock();

  return res;
}

void vfs_close(int fd) {
  vfs_acquire_handle_lock();
  int h_idx = scheduler_fd_get(fd);
  if (h_idx < 0) { vfs_release_handle_lock(); return; }
  struct vfs_handle *h = &handles[h_idx];
  if (h->kind == VFS_HANDLE_SOCKET && h->socket.type == B1NIX_SOCK_STREAM && h->socket.tcp_conn) {
    tcp_close((struct tcp_conn *)h->socket.tcp_conn);
    h->socket.tcp_conn = 0;
  }
  scheduler_fd_close(fd);
  release_handle(h_idx);
  vfs_release_handle_lock();
}

static int vfs_create_internal(const char *path, const char *data) {
  char res[VFS_MAX_PATH]; vfs_resolve_path(path, res);
  char p_path[VFS_MAX_PATH], name[64]; split_parent_path(res, p_path, name);
  struct vfs_node *parent = vfs_find_node(p_path);
  if (IS_ERR(parent)) return (int)PTR_ERR(parent);
  if (find_child(parent, name)) return -EEXIST;
  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) return -EACCES;

  struct vfs_node *node = alloc_node(); if (!node) return -ENOMEM;
  node->inode = alloc_inode(); if (!node->inode) { memset(node, 0, sizeof(*node)); return -ENOMEM; }

  node->inode->blk_dev = parent->inode->blk_dev;
  copy_path(node->name, 64, name);
  node->inode->type = VFS_FILE; node->parent = parent;
  
  u16 umask = scheduler_get_current_umask();
  node->inode->mode = 0666 & ~umask;
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  node->inode->atime = node->inode->mtime = node->inode->ctime = vfs_get_unix_time();

  node->inode->size = data ? strlen(data) : 0;
  if (node->inode->size > 0) {
    node->inode->data = kmalloc(node->inode->size + 1); if (!node->inode->data) { vfs_node_put(node); return -ENOMEM; }
    memcpy(node->inode->data, data, node->inode->size + 1); node->inode->flags |= VFS_NODE_OWNS_DATA;
  }
  node->next_sibling = parent->first_child; parent->first_child = node;

  if (parent->inode->create_cb) {
    int err = parent->inode->create_cb(parent, name, res, node->inode->mode);
    if (err < 0) {
      parent->first_child = node->next_sibling;
      vfs_node_put(node);
      return err;
    }
    node->inode->read_cb = parent->inode->read_cb;
    node->inode->write_cb = parent->inode->write_cb;
    node->inode->create_cb = parent->inode->create_cb;
    node->inode->mkdir_cb = parent->inode->mkdir_cb;
    node->inode->unlink_cb = parent->inode->unlink_cb;
    node->inode->rmdir_cb = parent->inode->rmdir_cb;
    node->inode->rename_cb = parent->inode->rename_cb;
    node->inode->link_cb = parent->inode->link_cb;
  }
  return 0;
}

int vfs_create(const char *path, const char *data) {
  vfs_acquire_lock();
  int res = vfs_create_internal(path, data);
  vfs_release_lock();
  return res;
}

struct vfs_node *vfs_find_node_by_fd(int fd) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || h->kind != VFS_HANDLE_NODE)
    return ERR_PTR(-EBADF);
  return h->node;
}

static int vfs_mkdir_internal(const char *path) {
  char res[VFS_MAX_PATH]; vfs_resolve_path(path, res);
  char p_path[VFS_MAX_PATH], name[64]; split_parent_path(res, p_path, name);
  struct vfs_node *parent = vfs_find_node(p_path);
  if (IS_ERR(parent)) return (int)PTR_ERR(parent);
  if (find_child(parent, name)) return -EEXIST;
  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) return -EACCES;

  struct vfs_node *node = alloc_node(); if (!node) return -ENOMEM;
  node->inode = alloc_inode(); if (!node->inode) { memset(node, 0, sizeof(*node)); return -ENOMEM; }

  node->inode->blk_dev = parent->inode->blk_dev;
  copy_path(node->name, 64, name);
  node->inode->type = VFS_DIRECTORY; node->parent = parent;

  u16 umask = scheduler_get_current_umask();
  node->inode->mode = 0777 & ~umask;
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  node->inode->atime = node->inode->mtime = node->inode->ctime = vfs_get_unix_time();

  node->next_sibling = parent->first_child; parent->first_child = node;

  if (parent->inode->mkdir_cb) {
    int err = parent->inode->mkdir_cb(parent, name, node->inode->mode);
    if (err < 0) {
      parent->first_child = node->next_sibling;
      vfs_node_put(node);
      return err;
    }
    node->inode->read_cb = parent->inode->read_cb;
    node->inode->write_cb = parent->inode->write_cb;
    node->inode->create_cb = parent->inode->create_cb;
    node->inode->mkdir_cb = parent->inode->mkdir_cb;
    node->inode->unlink_cb = parent->inode->unlink_cb;
    node->inode->rmdir_cb = parent->inode->rmdir_cb;
    node->inode->rename_cb = parent->inode->rename_cb;
    node->inode->link_cb = parent->inode->link_cb;
  }
  return 0;
}

int vfs_mkdir(const char *path) {
  vfs_acquire_lock();
  int res = vfs_mkdir_internal(path);
  vfs_release_lock();
  return res;
}

isize vfs_list(const char *dir_path, const char **names, usize max_names) {
  struct vfs_node *dir = vfs_find_node(dir_path);
  if (IS_ERR(dir))
    return PTR_ERR(dir);
  if (dir->inode->type != VFS_DIRECTORY)
    return -ENOTDIR;

  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(dir, cred, 4))
    return 0;

  /* Вызов list_cb удален, так как драйверы теперь используют readdir_cb и getdents */

  usize count = 0;
  struct vfs_node *child = dir->first_child;
  while (child && count < max_names) {
    if (!child->deleted) {
      names[count++] = child->name;
    }
    child = child->next_sibling;
  }

  return (isize)count;
}

static u32 vfs_node_type_mode(const struct vfs_node *node) {
  if (!node || !node->inode)
    return B1NIX_S_IFREG;
  if (node->inode->type == VFS_DIRECTORY)
    return B1NIX_S_IFDIR;
  if (node->inode->type == VFS_DEVICE)
    return B1NIX_S_IFCHR;
  if (node->inode->type == VFS_SYMLINK)
    return B1NIX_S_IFLNK;
  return B1NIX_S_IFREG;
}
static int vfs_stat_node(struct vfs_node *node, struct b1nix_stat *st) {
  if (!node || !node->inode)
    return -ENOENT;
  if (!st)
    return -EINVAL;

  struct vfs_inode *inode = node->inode;
  memset(st, 0, sizeof(*st));
  st->st_ino = (u64)(inode - inodes + 1);
  st->st_uid = inode->uid;
  st->st_gid = inode->gid;
  st->st_size = inode->size;
  st->st_blksize = 512;
  st->st_blocks = (inode->size + 511) / 512;
  st->st_nlink = (u32)inode->nlink;
  st->st_mode = vfs_node_type_mode(node) | (inode->mode & 0777);
  
  st->st_atime = inode->atime;
  st->st_mtime = inode->mtime;
  st->st_ctime = inode->ctime;
  return 0;
}

int vfs_stat(const char *path, struct b1nix_stat *st) {
  if (!path || !st)
    return -EINVAL;
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);
  return vfs_stat_node(node, st);
}

int vfs_statfs(const char *path, struct b1nix_statfs *st) {
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node)) return (int)PTR_ERR(node);
  
  if (node->inode->statfs_cb) return node->inode->statfs_cb(node, st);

  memset(st, 0, sizeof(*st));
  st->f_type = 0x1337;
  st->f_bsize = 4096;
  st->f_blocks = 1024 * 1024;
  st->f_bfree = 512 * 1024;
  st->f_bavail = 512 * 1024;
  st->f_files = 10000;
  st->f_ffree = 9000;
  st->f_namelen = 255;
  return 0;
}

int vfs_lstat(const char *path, struct b1nix_stat *st) {
  if (!path || !st)
    return -EINVAL;
  struct vfs_node *node = vfs_find_node_no_follow(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);
  return vfs_stat_node(node, st);
}

isize vfs_lseek(int handle, isize offset, int whence) {
  struct vfs_handle *h = get_handle(handle);
  if (!h || h->kind != VFS_HANDLE_NODE)
    return -EBADF;
  isize base = 0;
  if (whence == B1NIX_SEEK_SET)
    base = 0;
  else if (whence == B1NIX_SEEK_CUR)
    base = (isize)h->offset;
  else if (whence == B1NIX_SEEK_END)
    base = (h->node && h->node->inode) ? (isize)h->node->inode->size : 0;
  else
    return -EINVAL;

  isize next = base + offset;
  if (next < 0)
    return -EINVAL;
  h->offset = (usize)next;
  return next;
}

static int split_parent_path(const char *path, char *parent_path, char *name) {
  if (!path || path[0] == '\0')
    return -1;
  usize len = strlen(path);
  if (len == 0 || len >= 256)
    return -1;
  isize last_slash = -1;
  for (isize i = (isize)len - 1; i >= 0; i--) {
    if (path[i] == '/') {
      last_slash = i;
      break;
    }
  }

  if (last_slash < 0) {
    parent_path[0] = '/';
    parent_path[1] = '\0';
    memcpy(name, path, len + 1);
    return 0;
  }

  if ((usize)last_slash == len - 1)
    return -1;
  if (last_slash == 0) {
    parent_path[0] = '/';
    parent_path[1] = '\0';
  } else {
    memcpy(parent_path, path, (usize)last_slash);
    parent_path[last_slash] = '\0';
  }
  memcpy(name, path + last_slash + 1, len - (usize)last_slash);
  return 0;
}



static int vfs_remove_node(const char *path, int is_rmdir) {
  char res[VFS_MAX_PATH]; vfs_resolve_path(path, res);
  char p_path[VFS_MAX_PATH], name[64]; split_parent_path(res, p_path, name);
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -EINVAL;

  struct vfs_node *parent = vfs_find_node(p_path);
  if (IS_ERR(parent)) return (int)PTR_ERR(parent);
  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) return -EACCES;

  /* Защита точек монтирования */
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i].used && strcmp(mounts[i].target, res) == 0) return -EBUSY;
  }

  struct vfs_node *prev = 0, *child = parent->first_child;
  while (child) {
    if (!child->deleted && strcmp(child->name, name) == 0) {
      if (is_rmdir) {
        if (child->inode->type != VFS_DIRECTORY) return -ENOTDIR;
        if (child->first_child) return -ENOTEMPTY;
      } else {
        if (child->inode->type == VFS_DIRECTORY) return -EISDIR;
      }
      if (parent->inode->unlink_cb && !is_rmdir) {
        int err = parent->inode->unlink_cb(parent, name);
        if (err < 0) return err;
      } else if (parent->inode->rmdir_cb && is_rmdir) {
        int err = parent->inode->rmdir_cb(parent, name);
        if (err < 0) return err;
      }

      child->deleted = 1;
      child->inode->nlink--;
      if (prev) prev->next_sibling = child->next_sibling; else parent->first_child = child->next_sibling;
      vfs_node_put(child);
      return 0;
    }
    prev = child; child = child->next_sibling;
  }
  return -ENOENT;
}

int vfs_unlink(const char *path) {
  vfs_acquire_lock();
  int res = vfs_remove_node(path, 0);
  vfs_release_lock();
  return res;
}

int vfs_link(const char *target, const char *link_path) {
  vfs_acquire_lock();
  int res = 0;
  struct vfs_node *target_node = vfs_find_node(target);
  if (IS_ERR(target_node)) { res = (int)PTR_ERR(target_node); goto out; }
  if (target_node->inode->type == VFS_DIRECTORY) { res = -EPERM; goto out; }

  struct vfs_node *existing = vfs_find_node_no_follow(link_path);
  if (!IS_ERR(existing)) { res = -EEXIST; goto out; }

  char parent_path[VFS_MAX_PATH], name[64];
  if (split_parent_path(link_path, parent_path, name) < 0) { res = -EINVAL; goto out; }

  struct vfs_node *parent = vfs_find_node(parent_path);
  if (IS_ERR(parent)) { res = (int)PTR_ERR(parent); goto out; }
  if (parent->inode->type != VFS_DIRECTORY) { res = -ENOTDIR; goto out; }

  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) { res = -EACCES; goto out; }

  struct vfs_node *new_node = alloc_node();
  if (!new_node) { res = -ENOMEM; goto out; }
  
  copy_path(new_node->name, 64, name);
  new_node->inode = target_node->inode;
  new_node->inode->nlink++;
  new_node->parent = parent;
  new_node->next_sibling = parent->first_child;
  parent->first_child = new_node;

  if (parent->inode->link_cb) {
    res = parent->inode->link_cb(target_node, parent, name);
    if (res < 0) {
      parent->first_child = new_node->next_sibling;
      new_node->inode->nlink--;
      new_node->inode = 0;
      memset(new_node, 0, sizeof(*new_node));
    }
  }

out:
  vfs_release_lock();
  return res;
}

int vfs_symlink(const char *target, const char *link_path) {
  vfs_acquire_lock();
  int res = 0;
  if (!target || target[0] == '\0') { res = -EINVAL; goto out; }
  struct vfs_node *existing = vfs_find_node_no_follow(link_path);
  if (!IS_ERR(existing)) { res = -EEXIST; goto out; }

  char parent_path[VFS_MAX_PATH], name[64];
  if (split_parent_path(link_path, parent_path, name) < 0) { res = -EINVAL; goto out; }

  struct vfs_node *parent = vfs_find_node(parent_path);
  if (IS_ERR(parent)) { res = (int)PTR_ERR(parent); goto out; }
  if (parent->inode->type != VFS_DIRECTORY) { res = -ENOTDIR; goto out; }

  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) { res = -EACCES; goto out; }

  usize len = strlen(target);
  if (len >= VFS_MAX_PATH) { res = -ENAMETOOLONG; goto out; }
  char *target_copy = kmalloc(len + 1);
  if (!target_copy) { res = -ENOMEM; goto out; }
  memcpy(target_copy, target, len + 1);

  struct vfs_node *node = alloc_node();
  if (!node) { kfree(target_copy); res = -ENOMEM; goto out; }
  node->inode = alloc_inode();
  if (!node->inode) { kfree(target_copy); memset(node, 0, sizeof(*node)); res = -ENOMEM; goto out; }

  copy_path(node->name, 64, name);
  node->inode->type = VFS_SYMLINK;
  node->inode->data = target_copy;
  node->inode->size = len;
  node->inode->flags = VFS_NODE_OWNS_DATA;
  node->inode->mode = 0777;
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  node->inode->atime = node->inode->mtime = node->inode->ctime = vfs_get_unix_time();
  node->parent = parent;
  node->next_sibling = parent->first_child;
  parent->first_child = node;

  if (parent->inode->symlink_cb) {
    int err = parent->inode->symlink_cb(parent, name, target);
    if (err < 0) { vfs_node_put(node); res = err; goto out; }
  }

out:
  vfs_release_lock();
  return res;
}

isize vfs_readlink(const char *path, char *buffer, usize size) {
  vfs_acquire_lock();
  isize res = 0;
  if (!path || !buffer || size == 0) { res = -EINVAL; goto out; }
  struct vfs_node *node = vfs_find_node_no_follow(path);
  if (IS_ERR(node)) { res = PTR_ERR(node); goto out; }
  if (node->inode->type != VFS_SYMLINK || !node->inode->data) { res = -EINVAL; goto out; }

  usize len = node->inode->size;
  if (len > size) len = size;
  memcpy(buffer, node->inode->data, len);
  res = (isize)len;

out:
  vfs_release_lock();
  return res;
}

static int vfs_rename_internal(const char *old_path, const char *new_path) {
  char old_res[VFS_MAX_PATH], new_res[VFS_MAX_PATH];
  vfs_resolve_path(old_path, old_res); vfs_resolve_path(new_path, new_res);
  if (strcmp(old_res, new_res) == 0) return 0;

  char old_p[VFS_MAX_PATH], old_n[64], new_p[VFS_MAX_PATH], new_n[64];
  split_parent_path(old_res, old_p, old_n); split_parent_path(new_res, new_p, new_n);

  struct vfs_node *old_parent = vfs_find_node(old_p);
  struct vfs_node *new_parent = vfs_find_node(new_p);
  if (IS_ERR(old_parent) || IS_ERR(new_parent)) return -ENOENT;

  struct vfs_node *node = find_child(old_parent, old_n);
  if (!node) return -ENOENT;

  /* Рекурсивная защита */
  struct vfs_node *tmp = new_parent;
  while (tmp) { if (tmp == node) return -EINVAL; tmp = tmp->parent; }

  /* EXDEV check — must be before any tree mutation to avoid orphaned node */
  if (old_parent->inode->fs_id != new_parent->inode->fs_id) return -EXDEV;

  const struct cred *cred = get_current_cred();
  if (cred && (!vfs_get_node_perm(old_parent, cred, 2) || !vfs_get_node_perm(new_parent, cred, 2))) return -EACCES;

  /* Атомарная замена in-memory */
  struct vfs_node *existing = find_child(new_parent, new_n);
  if (existing) {
    if (existing->inode->type == VFS_DIRECTORY && existing->first_child) return -ENOTEMPTY;
    vfs_remove_node(new_res, 0);
  }

  /* Перенос узла */
  struct vfs_node *prev = 0, *c = old_parent->first_child;
  while (c) {
    if (c == node) {
      if (prev) prev->next_sibling = c->next_sibling; else old_parent->first_child = c->next_sibling;
      break;
    }
    prev = c; c = c->next_sibling;
  }

  if (old_parent->inode->rename_cb) {
    int err = old_parent->inode->rename_cb(old_parent, old_n, new_parent, new_n);
    if (err < 0) return err;
  }

  copy_path(node->name, 64, new_n);
  node->parent = new_parent; node->next_sibling = new_parent->first_child; new_parent->first_child = node;
  return 0;
}

int vfs_rename(const char *old_path, const char *new_path) {
  vfs_acquire_lock();
  int res = vfs_rename_internal(old_path, new_path);
  vfs_release_lock();
  return res;
}

int vfs_rmdir(const char *path) {
  vfs_acquire_lock();
  int res = vfs_remove_node(path, 1);
  vfs_release_lock();
  return res;
}

int vfs_fstat(int fd, struct b1nix_stat *st) {
  struct vfs_node *node = vfs_find_node_by_fd(fd);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);
  return vfs_stat_node(node, st);
}

int vfs_fsync(int fd) {
  int h_idx = scheduler_fd_get(fd);
  if (h_idx < 0) return -EBADF;
  struct vfs_handle *h = &handles[h_idx];
  if (!h->used || h->kind != VFS_HANDLE_NODE) return -EBADF;
  struct vfs_node *node = h->node;

  if (node->inode->fsync_cb) {
    int err = node->inode->fsync_cb(node);
    if (err < 0) return err;
  }

  if (node->inode->blk_dev) blk_cache_flush(node->inode->blk_dev);
  return 0;
}

int vfs_mount(const char *source, const char *target, const char *fstype,
              u64 flags) {
  if (!target || target[0] == '\0')
    return -EINVAL;
  struct vfs_node *target_node = vfs_find_node(target);
  if (IS_ERR(target_node)) {
    int err = vfs_mkdir(target);
    if (err != 0)
      return err;
    target_node = vfs_find_node(target);
  }
  if (IS_ERR(target_node))
    return (int)PTR_ERR(target_node);
  if (target_node->inode->type != VFS_DIRECTORY)
    return -ENOTDIR;

  for (usize i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i].used && strcmp(mounts[i].target, target) == 0) {
      copy_path(mounts[i].source, sizeof(mounts[i].source), source);
      copy_path(mounts[i].fstype, sizeof(mounts[i].fstype), fstype);
      mounts[i].flags = flags;
      mounts[i].root_node = target_node;
      return 0;
    }
  }
  for (usize i = 0; i < MAX_MOUNTS; i++) {
    if (!mounts[i].used) {
      mounts[i].used = 1;
      copy_path(mounts[i].source, sizeof(mounts[i].source), source);
      copy_path(mounts[i].target, sizeof(mounts[i].target), target);
      copy_path(mounts[i].fstype, sizeof(mounts[i].fstype), fstype);
      mounts[i].flags = flags;
      mounts[i].root_node = target_node;
      target_node->inode->fs_id = next_fs_id++;
      return 0;
    }
  }
  return -ENOMEM;
}

int vfs_umount(const char *target) {
  if (!target)
    return -EINVAL;
  for (usize i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i].used && strcmp(mounts[i].target, target) == 0) {
      if (strcmp(target, "/") == 0)
        return -EBUSY;
      mounts[i].used = 0;
      return 0;
    }
  }
  return -EINVAL;
}

isize vfs_mounts(struct b1nix_mount_entry *out, usize max_entries) {
  if (!out && max_entries > 0)
    return -EFAULT;

  usize count = 0;
  for (usize i = 0; i < MAX_MOUNTS; i++) {
    if (!mounts[i].used)
      continue;
    if (count < max_entries) {
      copy_path(out[count].source, sizeof(out[count].source), mounts[i].source);
      copy_path(out[count].target, sizeof(out[count].target), mounts[i].target);
      copy_path(out[count].fstype, sizeof(out[count].fstype), mounts[i].fstype);
      out[count].flags = mounts[i].flags;
    }
    count++;
  }
  return (isize)count;
}

isize vfs_getdents(int fd, struct dirent *buf, usize max_entries) {
  isize res = 0;
  vfs_acquire_handle_lock();
  int h_idx = scheduler_fd_get(fd);
  if (h_idx < 0) { vfs_release_handle_lock(); return -EBADF; }
  struct vfs_handle *h = &handles[h_idx];
  if (!h->used || h->kind != VFS_HANDLE_NODE) { vfs_release_handle_lock(); return -EBADF; }
  struct vfs_node *dir = h->node;
  if (!dir || dir->inode->type != VFS_DIRECTORY || !buf) { vfs_release_handle_lock(); return -EINVAL; }

  h->refcount++;
  vfs_node_get(dir);
  usize offset = h->offset;
  vfs_release_handle_lock();

  if (dir->inode->readdir_cb) {
    res = dir->inode->readdir_cb(dir, offset, buf, max_entries);
    if (res > 0) {
      vfs_acquire_handle_lock();
      h->offset += (usize)res;
      vfs_release_handle_lock();
    }
    goto out;
  }

  /* Fallback to in-memory Ramfs-style readdir */
  usize count = 0;
  if (offset == 0 && count < max_entries) {
    copy_path(buf[count].name, 64, ".");
    buf[count].type = (u32)VFS_DIRECTORY;
    buf[count].is_dir = 1; buf[count].is_exec = 1; buf[count].size = 0;
    count++; offset++;
  }
  if (offset == 1 && count < max_entries) {
    copy_path(buf[count].name, 64, "..");
    buf[count].type = (u32)VFS_DIRECTORY;
    buf[count].is_dir = 1; buf[count].is_exec = 1; buf[count].size = 0;
    count++; offset++;
  }

  vfs_acquire_lock();
  struct vfs_node *child = dir->first_child;
  usize skipped = 0;
  while (child && count < max_entries) {
    if (child->deleted) { child = child->next_sibling; continue; }
    if (skipped < offset - 2) { skipped++; child = child->next_sibling; continue; }
    copy_path(buf[count].name, 64, child->name);
    buf[count].type = (u32)child->inode->type;
    buf[count].is_dir = (child->inode->type == VFS_DIRECTORY);
    buf[count].is_exec = 0; buf[count].size = child->inode->size;
    count++; offset++;
    child = child->next_sibling;
  }
  vfs_release_lock();

  vfs_acquire_handle_lock();
  h->offset = offset;
  vfs_release_handle_lock();
  res = (isize)count;

out:
  vfs_acquire_lock();
  vfs_node_put(dir);
  vfs_release_lock();
  vfs_acquire_handle_lock();
  release_handle(h_idx);
  vfs_release_handle_lock();
  return res;
}

int vfs_sync(void) {
  blk_cache_flush(0);
  return 0;
}

int vfs_pipe(int pipefd[2]) {
  if (!pipefd)
    return -EINVAL;
  struct vfs_pipe *pipe = 0;
  for (usize i = 0; i < MAX_VFS_PIPES; i++) {
    if (!pipes[i].used) {
      pipe = &pipes[i];
      break;
    }
  }
  if (!pipe)
    return -ENFILE;

  int rfd = alloc_raw_handle(VFS_HANDLE_PIPE_READ);
  if (rfd < 0)
    return -EMFILE;
  int wfd = alloc_raw_handle(VFS_HANDLE_PIPE_WRITE);
  if (wfd < 0) {
    release_handle(rfd);
    return -EMFILE;
  }

  memset(pipe, 0, sizeof(*pipe));
  pipe->used = 1;
  pipe->readers = 1;
  pipe->writers = 1;
  handles[rfd].pipe = pipe;
  handles[wfd].pipe = pipe;
  pipefd[0] = scheduler_fd_alloc(rfd);
  pipefd[1] = scheduler_fd_alloc(wfd);
  if (pipefd[0] < 0 || pipefd[1] < 0) {
    if (pipefd[0] >= 0)
      scheduler_fd_close(pipefd[0]);
    if (pipefd[1] >= 0)
      scheduler_fd_close(pipefd[1]);
    release_handle(rfd);
    release_handle(wfd);
    return -EMFILE;
  }
  return 0;
}

int vfs_dup2(int oldfd, int newfd) {
  int old_handle = scheduler_fd_get(oldfd);
  if (old_handle < 0 || (usize)old_handle >= MAX_VFS_HANDLES ||
      !handles[old_handle].used)
    return -1;
  if (newfd < 0 || (usize)newfd >= SCHED_MAX_FDS)
    return -1;
  if (oldfd == newfd)
    return newfd;

  if (scheduler_fd_get(newfd) >= 0)
    vfs_close(newfd);
  scheduler_fd_set(newfd, old_handle);
  vfs_handle_retain(old_handle);
  return newfd;
}

int vfs_fcntl(int fd, int cmd, u64 arg) {
  struct vfs_handle *h = get_handle(fd);
  if (!h)
    return -1;
  switch (cmd) {
  case B1NIX_F_GETFD:
    return scheduler_fd_flags_get(fd);
  case B1NIX_F_SETFD:
    return scheduler_fd_flags_set(fd, (int)arg);
  case B1NIX_F_GETFL:
    return h->flags;
  case B1NIX_F_SETFL:
    h->flags = (int)arg;
    return 0;
  case B1NIX_F_GETLK:
  case B1NIX_F_SETLK:
  case B1NIX_F_SETLKW:
    if (h->kind != VFS_HANDLE_NODE) return -EBADF;
    return filelock_set_lock(fd, cmd, (struct flock *)(usize)arg);
  default:
    return -1;
  }
}

int vfs_ioctl(int fd, u64 request, void *arg) {
  struct vfs_node *node = vfs_find_node_by_fd(fd);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);
  if (node->inode->type != VFS_DEVICE || !arg)
    return -EINVAL;
  if (strcmp(node->name, "tty") != 0 && strcmp(node->name, "console") != 0)
    return -ENOTTY;

  if (request == B1NIX_TCGETS) {
    *(struct b1nix_termios *)arg = tty_termios;
    return 0;
  }
  if (request == B1NIX_TCSETS) {
    tty_termios = *(const struct b1nix_termios *)arg;
    return 0;
  }
  return -1;
}

void vfs_close_on_exec(void) {
  for (int fd = 0; fd < SCHED_MAX_FDS; fd++) {
    int flags = scheduler_fd_flags_get(fd);
    if (flags >= 0 && (flags & B1NIX_FD_CLOEXEC) != 0) {
      vfs_close(fd);
    }
  }
}

int vfs_socket(int domain, int type, int protocol) {
  if (domain != B1NIX_AF_INET)
    return -1;
  if (type != B1NIX_SOCK_DGRAM && type != B1NIX_SOCK_STREAM)
    return -1;
  int handle = alloc_raw_handle(VFS_HANDLE_SOCKET);
  if (handle < 0)
    return handle;
  handles[handle].socket.domain = domain;
  handles[handle].socket.type = type;
  handles[handle].socket.protocol = protocol;
  int fd = scheduler_fd_alloc(handle);
  if (fd < 0) {
    release_handle(handle);
    return -1;
  }
  return fd;
}

int vfs_bind(int fd, const void *addr, usize addrlen) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || h->kind != VFS_HANDLE_SOCKET) return -1;
  if (!addr || addrlen < sizeof(struct b1nix_sockaddr_in)) return -1;
  h->socket.local = *(const struct b1nix_sockaddr_in *)addr;
  h->socket.bound = 1;
  
  if (h->socket.type == B1NIX_SOCK_DGRAM) {
    u16 port = bswap16(h->socket.local.sin_port);
    for (int i = 0; i < MAX_VFS_HANDLES; i++) {
      if (udp_bindings[i].port == 0) {
        udp_bindings[i].port = port;
        int h_idx = scheduler_fd_get(fd);
        udp_bindings[i].handle_idx = h_idx;
        break;
      }
    }
  }
  return 0;
}

int vfs_connect(int fd, const void *addr, usize addrlen) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || h->kind != VFS_HANDLE_SOCKET)
    return -1;
  if (!addr || addrlen < sizeof(struct b1nix_sockaddr_in))
    return -1;
  h->socket.peer = *(const struct b1nix_sockaddr_in *)addr;
  h->socket.connected = 1;

  if (h->socket.type == B1NIX_SOCK_STREAM) {
    struct ipv4_addr dst;
    u32 raw_addr = h->socket.peer.sin_addr;
    dst.bytes[0] = (u8)(raw_addr & 0xFF);
    dst.bytes[1] = (u8)((raw_addr >> 8) & 0xFF);
    dst.bytes[2] = (u8)((raw_addr >> 16) & 0xFF);
    dst.bytes[3] = (u8)((raw_addr >> 24) & 0xFF);

    u16 port = bswap16(h->socket.peer.sin_port);
    struct tcp_conn *conn = tcp_connect(dst, port);
    if (!conn) {
      h->socket.connected = 0;
      return -ECONNREFUSED;
    }
    h->socket.tcp_conn = conn;
  }
  return 0;
}

isize vfs_socket_send(int fd, const void *buf, usize len, int flags) {
  (void)flags;
  struct vfs_handle *h = get_handle(fd);
  if (!h || h->kind != VFS_HANDLE_SOCKET || !buf)
    return -EBADF;
  if (!h->socket.connected && !h->socket.bound)
    return -ENOTCONN;

  if (h->socket.type == B1NIX_SOCK_DGRAM) {
    struct ipv4_addr dst;
    u32 raw_addr = h->socket.peer.sin_addr;
    dst.bytes[0] = (u8)(raw_addr & 0xFF);
    dst.bytes[1] = (u8)((raw_addr >> 8) & 0xFF);
    dst.bytes[2] = (u8)((raw_addr >> 16) & 0xFF);
    dst.bytes[3] = (u8)((raw_addr >> 24) & 0xFF);

    u16 src_port = bswap16(h->socket.local.sin_port);
    u16 dst_port = bswap16(h->socket.peer.sin_port);

    udp_send(dst, src_port, dst_port, buf, len);
    return (isize)len;
  } else if (h->socket.type == B1NIX_SOCK_STREAM) {
    if (!h->socket.tcp_conn)
      return -ENOTCONN;
    return (isize)tcp_send((struct tcp_conn *)h->socket.tcp_conn, buf, len);
  }
  return -EOPNOTSUPP;
}

isize vfs_socket_recv(int fd, void *buf, usize len, int flags) {
  (void)flags;
  struct vfs_handle *h = get_handle(fd);
  if (!h || h->kind != VFS_HANDLE_SOCKET || !buf)
    return -1;

  if (h->socket.type == B1NIX_SOCK_DGRAM) {
    if (h->socket.recv_len == 0)
      net_poll();
    if (h->socket.recv_len == 0)
      return 0;

    usize copy = (len < h->socket.recv_len) ? len : h->socket.recv_len;
    memcpy(buf, h->socket.recv_buf, copy);
    h->socket.recv_len = 0;
    return (isize)copy;
  } else if (h->socket.type == B1NIX_SOCK_STREAM) {
    if (!h->socket.tcp_conn)
      return -ENOTCONN;
    return (isize)tcp_recv((struct tcp_conn *)h->socket.tcp_conn, buf, len);
  }

  if (len > 0)
    memset(buf, 0, len);
  return 0;
}

/* Dedicated UDP binding table for O(1) lookups moved to top of file */

void vfs_socket_push_udp(u16 local_port, const void *data, usize len) {
  for (int i = 0; i < MAX_VFS_HANDLES; i++) {
    if (udp_bindings[i].port == local_port) {
      int h_idx = udp_bindings[i].handle_idx;
      struct vfs_handle *h = &handles[h_idx];
      if (!h->used || h->kind != VFS_HANDLE_SOCKET) {
        udp_bindings[i].port = 0; // Stale binding
        continue;
      }
      usize copy = (len > sizeof(h->socket.recv_buf)) ? sizeof(h->socket.recv_buf) : len;
      memcpy(h->socket.recv_buf, data, copy);
      h->socket.recv_len = copy;
      return;
    }
  }
}

/* ── Permission Management Functions ── */

int vfs_chmod(const char *path, u16 mode) {
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node)) return (int)PTR_ERR(node);

  const struct cred *cred = get_current_cred();
  if (!cred) return -EACCES;

  if (cred->euid != ROOT_UID && cred->euid != node->inode->uid) {
    if (!cred_has_cap(cred, CAP_FOWNER)) return -EPERM;
  }

  node->inode->mode = (node->inode->mode & ~0777) | (mode & 0777);
  node->inode->ctime = vfs_get_unix_time();
  if (node->inode->setattr_cb) return node->inode->setattr_cb(node);
  return 0;
}

int vfs_fchmod(int fd, u16 mode) {
  struct vfs_handle *handle = get_handle(fd);
  if (!handle || !handle->used)
    return -EBADF;
  if (handle->kind != VFS_HANDLE_NODE || !handle->node)
    return -EINVAL;

  const struct cred *cred = get_current_cred();
  if (!cred)
    return -EACCES;

  if (cred->euid != ROOT_UID && cred->euid != handle->node->inode->uid) {
    if (!cred_has_cap(cred, CAP_FOWNER))
      return -EPERM;
  }

  handle->node->inode->mode = (handle->node->inode->mode & ~0777) | (mode & 0777);
  handle->node->inode->ctime = vfs_get_unix_time();
  if (handle->node->inode->setattr_cb) return handle->node->inode->setattr_cb(handle->node);
  return 0;
}

int vfs_chown(const char *path, u16 uid, u16 gid) {
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);

  const struct cred *cred = get_current_cred();
  if (!cred)
    return -EACCES;

  /* Only root can change owner */
  if (cred->euid != ROOT_UID && !cred_has_cap(cred, CAP_CHOWN))
    return -EPERM;

  if (uid != (u16)-1)
    node->inode->uid = uid;
  if (gid != (u16)-1)
    node->inode->gid = gid;
  node->inode->ctime = vfs_get_unix_time();
  if (node->inode->setattr_cb) return node->inode->setattr_cb(node);
  return 0;
}

int vfs_set_acl(struct vfs_node *node, const struct acl_entry *acl) {
  if (!node || !acl)
    return -1;

  const struct cred *cred = get_current_cred();
  if (!cred)
    return -1;

  if (cred->euid != ROOT_UID && cred->euid != node->inode->uid) {
    if (!cred_has_cap(cred, CAP_FOWNER))
      return -1;
  }

  if (node->inode->acl_count >= ACL_MAX_ENTRIES)
    return -1;
  node->inode->acls[node->inode->acl_count++] = *acl;
  return 0;
}

int vfs_get_acl(struct vfs_node *node, struct acl_entry *out_acl,
                int max_entries) {
  if (!node || !out_acl)
    return -1;
  int count = node->inode->acl_count < max_entries ? node->inode->acl_count : max_entries;
  for (int i = 0; i < count; i++)
    out_acl[i] = node->inode->acls[i];
  return count;
}

int vfs_fchown(int fd, u16 uid, u16 gid) {
  struct vfs_handle *handle = get_handle(fd);
  if (!handle || !handle->used)
    return -EBADF;
  if (handle->kind != VFS_HANDLE_NODE || !handle->node)
    return -EINVAL;

  const struct cred *cred = get_current_cred();
  if (!cred)
    return -EACCES;

  if (cred->euid != ROOT_UID && !cred_has_cap(cred, CAP_CHOWN))
    return -EPERM;

  if (uid != (u16)-1)
    handle->node->inode->uid = uid;
  if (gid != (u16)-1)
    handle->node->inode->gid = gid;
  handle->node->inode->ctime = vfs_get_unix_time();
  if (handle->node->inode->setattr_cb) return handle->node->inode->setattr_cb(handle->node);
  return 0;
}

int vfs_fstatfs(int fd, struct b1nix_statfs *st) {
  struct vfs_handle *handle = get_handle(fd);
  if (!handle || !handle->used)
    return -EBADF;
  if (handle->kind != VFS_HANDLE_NODE || !handle->node)
    return -EINVAL;

  if (handle->node->inode->statfs_cb) return handle->node->inode->statfs_cb(handle->node, st);

  /* Stub for now, fill with basic info */
  memset(st, 0, sizeof(*st));
  st->f_type = 0x1337;
  st->f_bsize = 4096;
  st->f_blocks = 1024 * 1024;
  st->f_bfree = 512 * 1024;
  st->f_bavail = 512 * 1024;
  st->f_files = 10000;
  st->f_ffree = 9000;
  st->f_namelen = 255;
  return 0;
}

int vfs_syncfs(int fd) {
  struct vfs_handle *handle = get_handle(fd);
  if (!handle || !handle->used)
    return -EBADF;
  return vfs_sync();
}
