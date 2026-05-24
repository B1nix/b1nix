#include <b1nix/arch.h>
#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/ext2.h>
#include <b1nix/fat32.h>
#include <b1nix/filelock.h>
#include <b1nix/initramfs.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/page_cache.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

#define VFS_NODE_OWNS_DATA 0x80000000u
#define MAX_FILE_SIZE (1024 * 1024 * 1024) /* 1 GB limit for now */

/* VFS time update masks */
#define VFS_ATIME 0x01
#define VFS_MTIME 0x02
#define VFS_CTIME 0x04
#define VFS_MAX_SYMLINK_DEPTH 16

static char poll_chan_obj;
void *vfs_poll_chan = &poll_chan_obj;

static volatile int node_pool_lock = 0;
static volatile int inode_pool_lock = 0;
static volatile int vfs_handle_lock = 0;
static volatile int vfs_mount_lock = 0;
static u32 next_fs_id = 1;

static u32 dcache_hash(struct vfs_node *parent, const char *name);
static struct vfs_node *dcache_lookup(struct vfs_node *parent,
                                      const char *name);
static void dcache_insert(struct vfs_node *parent, const char *name,
                          struct vfs_node *node);
static void dcache_invalidate(struct vfs_node *parent, const char *name);

void serial_init(void);
void serial_putc(char ch);
char serial_getc(void);
int serial_has_data(void);
void serial_write(const char *text);
static void copy_path(char *dst, usize dst_size, const char *src);
static int split_parent_path(const char *path, char *parent_path, char *name);
static int vfs_create_at_internal(const char *resolved_path, u32 mode);
static int vfs_mkdir_at_internal(const char *resolved_path, u32 mode);
static int vfs_unlink_at_internal(const char *resolved_path);

static struct vfs_fs *filesystems = NULL;

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

static struct vfs_mount_entry *vfs_get_mount_for_node(struct vfs_node *node) {
  if (!node)
    return 0;

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  struct vfs_node *curr = node;
  while (curr) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
      if (mounts[i].used && curr == mounts[i].root_node) {
        struct vfs_mount_entry *res = &mounts[i];
        __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
        return res;
      }
    }
    curr = curr->parent;
  }
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i].used && strcmp(mounts[i].target, "/") == 0) {
      struct vfs_mount_entry *res = &mounts[i];
      __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
      return res;
    }
  }
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
  return 0;
}

void vfs_register_fs(struct vfs_fs *fs) {
  fs->next = filesystems;
  filesystems = fs;
}

static struct vfs_fs *find_fs(const char *name) {
  struct vfs_fs *curr = filesystems;
  while (curr) {
    if (strcmp(curr->name, name) == 0)
      return curr;
    curr = curr->next;
  }
  return NULL;
}

#define DCACHE_SIZE 256
#define MAX_DCACHE_ENTRIES 512

struct dcache_entry {
  struct vfs_node *parent;
  char name[64];
  struct vfs_node *node;
  struct dcache_entry *next;
  struct dcache_entry *lru_next;
  struct dcache_entry *lru_prev;
};
static struct dcache_entry *dcache[DCACHE_SIZE] = {0};
static struct dcache_entry *dcache_lru_head = 0;
static struct dcache_entry *dcache_lru_tail = 0;
static int dcache_count = 0;
static volatile int dcache_lock = 0;

static void dcache_acquire(void) {
  while (__atomic_test_and_set(&dcache_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
}

static void dcache_release(void) {
  __atomic_clear(&dcache_lock, __ATOMIC_RELEASE);
}

/* Slab-like pool for dcache entries to avoid fragmentation and kmalloc overhead
 */
static struct dcache_entry dcache_pool[MAX_DCACHE_ENTRIES];
static struct dcache_entry *dcache_free_list = 0;

static void dcache_init_pool(void) {
  for (int i = 0; i < MAX_DCACHE_ENTRIES - 1; i++) {
    dcache_pool[i].next = &dcache_pool[i + 1];
  }
  dcache_pool[MAX_DCACHE_ENTRIES - 1].next = 0;
  dcache_free_list = &dcache_pool[0];
}

static struct dcache_entry *dcache_alloc(void) {
  if (!dcache_free_list)
    return 0;
  struct dcache_entry *e = dcache_free_list;
  dcache_free_list = e->next;
  memset(e, 0, sizeof(struct dcache_entry));
  return e;
}

static void dcache_free(struct dcache_entry *e) {
  e->next = dcache_free_list;
  dcache_free_list = e;
}

static u32 dcache_hash(struct vfs_node *parent, const char *name) {
  u32 h = 5381;
  h = ((h << 5) + h) + (u32)(usize)parent;
  while (*name)
    h = ((h << 5) + h) + (u32)(*name++);
  return h % DCACHE_SIZE;
}

static struct vfs_node *dcache_lookup(struct vfs_node *parent,
                                      const char *name) {
  dcache_acquire();
  u32 h = dcache_hash(parent, name);
  struct dcache_entry *e = dcache[h];
  while (e) {
    if (e->parent == parent && strcmp(e->name, name) == 0) {
      /* Move to LRU head */
      if (e != dcache_lru_head) {
        if (e == dcache_lru_tail)
          dcache_lru_tail = e->lru_prev;
        if (e->lru_prev)
          e->lru_prev->lru_next = e->lru_next;
        if (e->lru_next)
          e->lru_next->lru_prev = e->lru_prev;
        e->lru_next = dcache_lru_head;
        e->lru_prev = 0;
        if (dcache_lru_head)
          dcache_lru_head->lru_prev = e;
        dcache_lru_head = e;
        if (!dcache_lru_tail)
          dcache_lru_tail = e;
      }
      struct vfs_node *res = e->node;
      dcache_release();
      return res;
    }
    e = e->next;
  }
  dcache_release();
  return 0;
}

static void dcache_insert(struct vfs_node *parent, const char *name,
                          struct vfs_node *node) {
  dcache_acquire();
  if (dcache_count >= MAX_DCACHE_ENTRIES) {
    /* Evict LRU tail */
    struct dcache_entry *victim = dcache_lru_tail;
    if (victim) {
      /* Invalidate will handle locking if we call it carefully,
         but here we are already holding the lock.
         Let's manually remove the tail to avoid deadlock. */

      /* Remove from hash table */
      u32 vh = dcache_hash(victim->parent, victim->name);
      struct dcache_entry **prev_ptr = &dcache[vh];
      while (*prev_ptr && *prev_ptr != victim)
        prev_ptr = &(*prev_ptr)->next;
      if (*prev_ptr)
        *prev_ptr = victim->next;

      /* Remove from LRU */
      if (victim == dcache_lru_head)
        dcache_lru_head = victim->lru_next;
      if (victim == dcache_lru_tail)
        dcache_lru_tail = victim->lru_prev;
      if (victim->lru_prev)
        victim->lru_prev->lru_next = victim->lru_next;
      if (victim->lru_next)
        victim->lru_next->lru_prev = victim->lru_prev;

      dcache_free(victim);
      dcache_count--;
    }
  }

  u32 h = dcache_hash(parent, name);
  struct dcache_entry *e = dcache_alloc();
  if (!e) {
    dcache_release();
    return;
  }
  e->parent = parent;
  copy_path(e->name, 64, name);
  e->node = node;

  /* Insert into hash table */
  e->next = dcache[h];
  dcache[h] = e;

  /* Insert into LRU head */
  e->lru_next = dcache_lru_head;
  e->lru_prev = 0;
  if (dcache_lru_head)
    dcache_lru_head->lru_prev = e;
  dcache_lru_head = e;
  if (!dcache_lru_tail)
    dcache_lru_tail = e;

  dcache_count++;
  dcache_release();
}

static void dcache_invalidate(struct vfs_node *parent, const char *name) {
  dcache_acquire();
  u32 h = dcache_hash(parent, name);
  struct dcache_entry **prev_ptr = &dcache[h];
  struct dcache_entry *curr = *prev_ptr;
  while (curr) {
    if (curr->parent == parent && strcmp(curr->name, name) == 0) {
      /* Remove from hash table */
      *prev_ptr = curr->next;

      /* Remove from LRU list */
      if (curr == dcache_lru_head)
        dcache_lru_head = curr->lru_next;
      if (curr == dcache_lru_tail)
        dcache_lru_tail = curr->lru_prev;
      if (curr->lru_prev)
        curr->lru_prev->lru_next = curr->lru_next;
      if (curr->lru_next)
        curr->lru_next->lru_prev = curr->lru_prev;

      dcache_free(curr);
      dcache_count--;
      dcache_release();
      return;
    }
    prev_ptr = &curr->next;
    curr = *prev_ptr;
  }
  dcache_release();
}

static void vfs_inode_lock_read(struct vfs_inode *inode) {
  while (1) {
    int val = inode->rw_lock;
    if (val >= 0 &&
        __atomic_compare_exchange_n(&inode->rw_lock, &val, val + 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      break;
    }
    scheduler_block_on((void *)&inode->rw_lock);
  }
}

static void vfs_inode_unlock_read(struct vfs_inode *inode) {
  if (__atomic_add_fetch(&inode->rw_lock, -1, __ATOMIC_RELEASE) == 0) {
    scheduler_wake_all((void *)&inode->rw_lock);
  }
}

static void vfs_inode_lock_write(struct vfs_inode *inode) {
  while (1) {
    int val = 0;
    if (__atomic_compare_exchange_n(&inode->rw_lock, &val, -1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      break;
    }
    scheduler_block_on((void *)&inode->rw_lock);
  }
}

static void vfs_inode_unlock_write(struct vfs_inode *inode) {
  __atomic_store_n(&inode->rw_lock, 0, __ATOMIC_RELEASE);
  scheduler_wake_all((void *)&inode->rw_lock);
}

/* Compatibility wrappers */
static void vfs_inode_lock(struct vfs_inode *inode) {
  vfs_inode_lock_write(inode);
}
static void vfs_inode_unlock(struct vfs_inode *inode) {
  vfs_inode_unlock_write(inode);
}

static u16 bswap16(u16 v) { return (u16)((v << 8) | (v >> 8)); }

static const struct cred *get_current_cred(void) {
  return scheduler_get_current_cred();
}

extern u32 rtc_boot_time_seconds;
u32 vfs_get_unix_time(void) {
  /* 100Hz scheduler ticks converted to seconds + RTC boot offset */
  return rtc_boot_time_seconds + ((u32)scheduler_get_uptime_ticks() / 100);
}

static u16 scheduler_get_current_umask(void) {
  const struct cred *cred = get_current_cred();
  return cred ? cred->umask : 0022;
}

static void vfs_update_times(struct vfs_inode *inode, u32 mask) {
  if (!inode)
    return;
  u32 now = vfs_get_unix_time();
  if (mask & VFS_ATIME)
    inode->atime = now;
  if (mask & VFS_MTIME)
    inode->mtime = now;
  if (mask & VFS_CTIME)
    inode->ctime = now;
}

static u64 next_ino = 1;

static usize node_count = 0;
static struct vfs_node *root_node = 0;
static char tty_line[TTY_INPUT_SIZE];
static usize tty_line_pos;
static usize tty_line_len;

void virtio_blk_init(void);
#ifndef __aarch64__
extern char ps2_kbd_getc(void);
#endif

int vfs_mount(const char *source, const char *target, const char *fstype,
              u64 flags);

/* ── Permission Helpers ── */

extern struct cred *scheduler_get_current_cred(void);

int vfs_check_access(struct vfs_node *node, int requested_access) {
  if (!node)
    return -ENOENT;
  const struct cred *cred = get_current_cred();
  if (!cred)
    return -EACCES;
  if (vfs_get_node_perm(node, cred, (u32)requested_access)) {
    return 0;
  }
  return -EACCES;
}

int vfs_get_node_perm(const struct vfs_node *node, const struct cred *cred,
                      u32 mask) {
  if (!node || !node->inode || !cred)
    return 0;
  struct vfs_inode *inode = node->inode;
  if (cred->euid == ROOT_UID)
    return 1;
  if (cred_has_cap(cred, CAP_DAC_OVERRIDE))
    return 1;
  if (!(mask & 2) && cred_has_cap(cred, CAP_DAC_READ_SEARCH))
    return 1;

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
      case ACL_USER_OBJ:
        if (cred->euid == inode->uid) {
          matched_perms = inode->acls[i].perms;
          goto acl_check;
        }
        break;
      case ACL_USER:
        if (cred->euid == inode->acls[i].qualifier) {
          matched_perms = inode->acls[i].perms;
          goto acl_check;
        }
        break;
      case ACL_GROUP_OBJ:
        if (cred->egid == inode->gid) {
          matched_perms = inode->acls[i].perms;
          goto acl_check;
        }
        break;
      case ACL_GROUP:
        if (cred->egid == inode->acls[i].qualifier) {
          matched_perms = inode->acls[i].perms;
          goto acl_check;
        }
        for (int g = 0; g < cred->ngroups; g++) {
          if (cred->groups[g] == inode->acls[i].qualifier) {
            matched_perms = inode->acls[i].perms;
            goto acl_check;
          }
        }
        break;
      }
    }
    matched_perms = inode->mode & 7;

  acl_check:
    if (mask_found)
      matched_perms &= mask_perms;
    return (matched_perms & mask) == mask;
  }
  return cred_can_access(cred, inode->uid, inode->gid, inode->mode, mask);
}

/* ── Node/Inode allocation ── */

static struct vfs_inode *alloc_inode(void) {
  struct vfs_inode *inode = vfs_alloc_inode();
  if (inode) {
    inode->refcount = 0;
  }
  return inode;
}

struct vfs_inode *vfs_inode_get(struct vfs_inode *inode) {
  if (inode)
    __atomic_add_fetch(&inode->refcount, 1, __ATOMIC_RELAXED);
  return inode;
}

void vfs_inode_put(struct vfs_inode *inode) {
  if (!inode)
    return;
  if (__atomic_sub_fetch(&inode->refcount, 1, __ATOMIC_RELAXED) == 0 &&
      inode->nlink == 0) {
    if (inode->data && (inode->flags & VFS_NODE_OWNS_DATA)) {
      kfree(inode->data);
    }
    vfs_free_inode(inode);
  }
}

static struct vfs_node *alloc_node(void) {
  struct vfs_node *n = vfs_alloc_node();
  if (n) {
    n->refcount = 0;
    __atomic_add_fetch(&node_count, 1, __ATOMIC_RELAXED);
  }
  return n;
}

struct vfs_node *vfs_node_get(struct vfs_node *node) {
  if (node)
    __atomic_add_fetch(&node->refcount, 1, __ATOMIC_RELAXED);
  return node;
}

void vfs_node_put(struct vfs_node *node) {
  if (!node)
    return;
  if (__atomic_sub_fetch(&node->refcount, 1, __ATOMIC_RELAXED) == 0 &&
      node->deleted) {
    if (node->inode && node->inode->release_cb) {
      node->inode->release_cb(node);
    }
    vfs_inode_put(node->inode);
    vfs_free_node(node);
    __atomic_sub_fetch(&node_count, 1, __ATOMIC_RELAXED);
  }
}

static void split_path(const char *path, char *first_part, const char **rest) {
  if (!path) {
    if (first_part)
      first_part[0] = '\0';
    if (rest)
      *rest = 0;
    return;
  }
  while (*path == '/')
    path++;
  if (*path == '\0') {
    first_part[0] = '\0';
    *rest = 0;
    return;
  }
  usize i = 0;
  while (path[i] != '\0' && path[i] != '/') {
    first_part[i] = path[i];
    i++;
  }
  first_part[i] = '\0';
  *rest = path + i;
}

struct vfs_node *find_child(struct vfs_node *parent, const char *name) {
  if (!parent || !parent->inode || parent->inode->type != VFS_DIRECTORY)
    return 0;

  struct vfs_node *cached = dcache_lookup(parent, name);
  if (cached && !cached->deleted) {
    /* REFCOUNT RULE: All functions returning a VFS node MUST increment refcount.
     * Caller is responsible for calling vfs_node_put() when done. */
    return vfs_node_get(cached);
  }

  struct vfs_node *child = parent->first_child;
  while (child) {
    if (!child->deleted && strcmp(child->name, name) == 0) {
      dcache_insert(parent, name, child);
      /* REFCOUNT RULE: Return with refcount incremented for caller ownership */
      return vfs_node_get(child);
    }
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
  if (!path || !out)
    return;
  char combined[VFS_MAX_PATH];
  if (path[0] == '/') {
    strncpy(combined, path, VFS_MAX_PATH);
  } else {
    const char *cwd = scheduler_get_cwd();
    strncpy(combined, cwd, VFS_MAX_PATH);
    usize len = strlen(combined);
    if (len > 0 && combined[len - 1] != '/' && len < VFS_MAX_PATH - 1) {
      combined[len++] = '/';
      combined[len] = '\0';
    }
    strncat(combined, path, VFS_MAX_PATH - len - 1);
  }

  char *parts[64];
  int part_count = 0;
  char tmp[VFS_MAX_PATH];
  strncpy(tmp, combined, VFS_MAX_PATH);
  char *curr = tmp;
  while (*curr && part_count < 64) {
    while (*curr == '/')
      curr++;
    if (!*curr)
      break;
    char *start = curr;
    while (*curr && *curr != '/')
      curr++;
    if (*curr) {
      *curr = '\0';
      curr++;
    }
    /* Ignore current directory */
    if (strcmp(start, ".") == 0)
      continue;

    /* Handle parent directory */
    if (strcmp(start, "..") == 0) {
      if (part_count > 0)
        part_count--;
      continue;
    }

    parts[part_count++] = start;
  }
  out[0] = '/';
  out[1] = '\0';
  for (int i = 0; i < part_count; i++) {
    strcat(out, parts[i]);
    if (i < part_count - 1)
      strcat(out, "/");
  }

  /* POSIX: preserve trailing slash for directory resolution */
  usize path_len = strlen(path);
  if (path_len > 0 && path[path_len - 1] == '/' && part_count > 0) {
    usize out_len = strlen(out);
    if (out_len > 0 && out[out_len - 1] != '/' && out_len < VFS_MAX_PATH - 1) {
      out[out_len] = '/';
      out[out_len + 1] = '\0';
    }
  }

  /*
  console_write("VFS: resolve '");
  console_write(path);
  console_write("' -> '");
  console_write(out);
  console_write("'\n");
  */
}

/* POSIX: Iterative path resolution with symlink loop detection to prevent stack
 * overflow */
static struct vfs_node *
vfs_find_node_internal(const char *path, int follow_final, int symlink_depth) {
  if (!root_node || !path)
    return ERR_PTR(-ENOENT);

  if (symlink_depth > VFS_MAX_SYMLINK_DEPTH)
    return ERR_PTR(-ELOOP);

  char *curr_path = kmalloc(VFS_MAX_PATH);
  if (!curr_path)
    return ERR_PTR(-ENOMEM);
  strncpy(curr_path, path, VFS_MAX_PATH - 1);
  curr_path[VFS_MAX_PATH - 1] = '\0';

  char *parent_path = kmalloc(VFS_MAX_PATH);
  if (!parent_path) {
    kfree(curr_path);
    return ERR_PTR(-ENOMEM);
  }
  parent_path[0] = '/';
  parent_path[1] = '\0';

  vfs_node_get(root_node);
  struct vfs_node *current = root_node;
  vfs_inode_lock_read(current->inode);

  char part[64];
  const char *rest = curr_path;

restart_traversal:
  while (1) {
    while (*rest == '/')
      rest++;

    split_path(rest, part, &rest);

    if (part[0] == '\0') {
      int orig_len = strlen(path);
      if (orig_len > 0 && path[orig_len - 1] == '/') {
        if (current->inode->type != VFS_DIRECTORY) {
          vfs_inode_unlock_read(current->inode);
          vfs_node_put(current);
          kfree(curr_path);
          kfree(parent_path);
          return ERR_PTR(-ENOTDIR);
        }
      }
      kfree(curr_path);
      kfree(parent_path);
      vfs_inode_unlock_read(current->inode);
      return current;
    }

    if (strcmp(part, ".") == 0)
      continue;

    if (strcmp(part, "..") == 0) {
      while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
        scheduler_yield();
      for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].used && current == mounts[i].root_node) {
          struct vfs_node *mp = vfs_node_get(mounts[i].mount_point);
          __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
          vfs_inode_unlock_read(current->inode);
          vfs_node_put(current);
          current = mp;
          vfs_inode_lock_read(current->inode);
          continue;
        }
      }
      __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);

      struct vfs_node *parent = current->parent;
      if (parent) {
        vfs_node_get(parent);
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        current = parent;
        vfs_inode_lock_read(current->inode);
        pop_path_part(parent_path);
      }
      continue;
    }

    /* POSIX: Check directory traversal permission (execute bit) */
    if (vfs_check_access(current, X_OK) != 0) {
      vfs_inode_unlock_read(current->inode);
      vfs_node_put(current);
      kfree(curr_path);
      kfree(parent_path);
      return ERR_PTR(-EACCES);
    }

    if (current->inode->type != VFS_DIRECTORY) {
      vfs_inode_unlock_read(current->inode);
      vfs_node_put(current);
      kfree(curr_path);
      kfree(parent_path);
      return ERR_PTR(-ENOTDIR);
    }

    struct vfs_node *child = find_child(current, part);
    if (!child) {
      vfs_inode_unlock_read(current->inode);
      vfs_node_put(current);
      kfree(curr_path);
      kfree(parent_path);
      return ERR_PTR(-ENOENT);
    }

    /* find_child() already returns with refcount incremented */
    /* DOWNWARD MOUNT CROSSING */
    for (int i = 0; i < MAX_MOUNTS; i++) {
      if (mounts[i].used && child == mounts[i].mount_point) {
        struct vfs_node *root = vfs_node_get(mounts[i].root_node);
        vfs_node_put(child);
        child = root;
        break;
      }
    }

    vfs_inode_lock_read(child->inode);
    vfs_inode_unlock_read(current->inode);
    vfs_node_put(current);
    current = child;

    int is_final = (!rest || rest[0] == '\0');
    if (current->inode->type == VFS_SYMLINK && (follow_final || !is_final)) {
      if (++symlink_depth > VFS_MAX_SYMLINK_DEPTH) {
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        kfree(curr_path);
        kfree(parent_path);
        return ERR_PTR(-ELOOP);
      }

      char *target_path = kmalloc(VFS_MAX_PATH);
      if (!target_path) {
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        kfree(curr_path);
        kfree(parent_path);
        return ERR_PTR(-ENOMEM);
      }

      /* Read symlink target */
      isize target_len = 0;
      if (current->inode->read_cb) {
        target_len = current->inode->read_cb(current, 0, target_path,
                                             VFS_MAX_PATH - 1, 0);
      } else if (current->inode->data) {
        target_len = (isize)current->inode->size;
        if (target_len > VFS_MAX_PATH - 1)
          target_len = VFS_MAX_PATH - 1;
        memcpy(target_path, current->inode->data, (usize)target_len);
      } else {
        kfree(target_path);
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        kfree(curr_path);
        kfree(parent_path);
        return ERR_PTR(-EINVAL);
      }

      if (target_len < 0) {
        kfree(target_path);
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        kfree(curr_path);
        kfree(parent_path);
        return ERR_PTR((int)target_len);
      }
      target_path[target_len] = '\0';

      /* Path Injection: [target] + [rest] */
      char *new_path = kmalloc(VFS_MAX_PATH);
      if (!new_path) {
        kfree(target_path);
        vfs_inode_unlock_read(current->inode);
        vfs_node_put(current);
        kfree(curr_path);
        kfree(parent_path);
        return ERR_PTR(-ENOMEM);
      }

      if (target_path[0] == '/') {
        strncpy(new_path, target_path, VFS_MAX_PATH - 1);
      } else {
        strncpy(new_path, parent_path, VFS_MAX_PATH - 1);
        append_path_part(new_path, VFS_MAX_PATH, target_path);
      }

      if (!is_final) {
        append_path_part(new_path, VFS_MAX_PATH, rest);
      }
      new_path[VFS_MAX_PATH - 1] = '\0';

      kfree(target_path);
      kfree(curr_path);
      curr_path = new_path;
      rest = curr_path;

      /* Restart traversal from root because new_path is absolute */
      vfs_inode_unlock_read(current->inode);
      vfs_node_put(current);
      vfs_node_get(root_node);
      current = root_node;
      vfs_inode_lock_read(current->inode);
      parent_path[0] = '/';
      parent_path[1] = '\0';

      goto restart_traversal;
    }

    if (!is_final)
      append_path_part(parent_path, VFS_MAX_PATH, part);
  }

  /* Unreachable */
  vfs_inode_unlock_read(current->inode);
  vfs_node_put(current);
  kfree(curr_path);
  kfree(parent_path);
  return ERR_PTR(-EIO);
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
    if (!root_node)
      return ERR_PTR(-ENOMEM);
    root_node->inode = alloc_inode();
    if (!root_node->inode)
      return ERR_PTR(-ENOMEM);

    root_node->name[0] = '/';
    root_node->name[1] = '\0';
    root_node->inode->type = VFS_DIRECTORY;
    root_node->inode->mode = 0755;
    root_node->inode->atime = root_node->inode->mtime =
        root_node->inode->ctime = vfs_get_unix_time();
  }

  char part[64];
  const char *rest = path;
  struct vfs_node *current = root_node;

  while (1) {
    split_path(rest, part, &rest);
    if (part[0] == '\0')
      return current;

    struct vfs_node *child = find_child(current, part);
    int child_was_found = (child != NULL);
    if (child) {
      for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].used && child == mounts[i].mount_point) {
          vfs_node_put(child); /* Drop ref from find_child */
          child = vfs_node_get(mounts[i].root_node);
          break;
        }
      }
    }
    int is_leaf =
        (!rest || rest[0] == '\0' || (rest[0] == '/' && rest[1] == '\0'));

    if (is_leaf) {
      if (!child) {
        child = alloc_node();
        if (!child)
          return ERR_PTR(-ENOMEM);
        child->inode = alloc_inode();
        if (!child->inode) {
          vfs_free_node(child);
          __atomic_sub_fetch(&node_count, 1, __ATOMIC_RELAXED);
          return ERR_PTR(-ENOMEM);
        }

        copy_path(child->name, 64, part);
        child->inode->type = type;
        child->inode->data = data;
        child->inode->size = size;
        child->inode->flags = flags;
        child->inode->fs_id = current->inode->fs_id;
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

        child->inode->atime = child->inode->mtime = child->inode->ctime =
            vfs_get_unix_time();
      } else if (data != 0 || size != 0 || flags != 0 ||
                 type == VFS_DIRECTORY) {
        child->inode->type = type;
        child->inode->data = data;
        child->inode->size = size;
        child->inode->flags = flags;
        child->inode->mtime = child->inode->ctime = vfs_get_unix_time();
      } else {
        if (child_was_found) {
          vfs_node_put(child);
        }
        return child;
      }
      if (child_was_found) {
        vfs_node_put(child);
      }
      return child;
    } else {
      if (!child) {
        child = alloc_node();
        if (!child)
          return ERR_PTR(-ENOMEM);
        child->inode = alloc_inode();
        if (!child->inode) {
          vfs_free_node(child);
          __atomic_sub_fetch(&node_count, 1, __ATOMIC_RELAXED);
          return ERR_PTR(-ENOMEM);
        }

        copy_path(child->name, 64, part);
        child->inode->type = VFS_DIRECTORY;
        child->inode->fs_id = current->inode->fs_id;

        const struct cred *cred = get_current_cred();
        child->inode->uid = cred ? cred->euid : ROOT_UID;
        child->inode->gid = cred ? cred->egid : ROOT_GID;

        u16 umask = scheduler_get_current_umask();
        child->inode->mode = 0777 & ~umask;
        child->inode->atime = child->inode->mtime = child->inode->ctime =
            vfs_get_unix_time();

        child->parent = current;
        child->next_sibling = current->first_child;
        current->first_child = child;
      }
      current = child;
      if (child_was_found) {
        vfs_node_put(child);
      }
    }
  }
}

struct vfs_node *vfs_create_node(enum vfs_node_type type) {
  struct vfs_node *n = alloc_node();
  if (!n)
    return NULL;
  n->inode = alloc_inode();
  if (!n->inode) {
    n->refcount = 0;
    return NULL;
  }
  n->inode->type = type;
  n->inode->refcount = 1;
  n->inode->nlink = 1;
  n->refcount = 1;
  return n;
}

struct vfs_node *vfs_add_node(const char *path, enum vfs_node_type type,

                              void *data, usize size, u32 flags) {
  return add_node(path, type, data, size, flags);
}

static struct vfs_handle handles[MAX_VFS_HANDLES];

struct vfs_handle *get_handle_by_idx(int idx) {
  if (idx < 0 || idx >= MAX_VFS_HANDLES)
    return 0;
  return &handles[idx];
}

void vfs_acquire_handle_lock(void) {
  while (__atomic_test_and_set(&vfs_handle_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
}

void vfs_release_handle_lock(void) {
  __atomic_clear(&vfs_handle_lock, __ATOMIC_RELEASE);
}

int alloc_raw_handle(enum vfs_handle_kind kind) {
  vfs_acquire_handle_lock();
  for (usize i = 0; i < MAX_VFS_HANDLES; i++) {
    if (!handles[i].used) {
      memset(&handles[i], 0, sizeof(handles[i]));
      handles[i].used = 1;
      handles[i].refcount = 1;
      handles[i].kind = kind;
      vfs_release_handle_lock();
      return (int)i;
    }
  }
  vfs_release_handle_lock();
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

void release_handle(int handle) {
  if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES || !handles[handle].used)
    return;
  if (handles[handle].refcount > 1) {
    handles[handle].refcount--;
    return;
  }

  if (handles[handle].ops && handles[handle].ops->release) {
    handles[handle].ops->release(&handles[handle]);
  } else if (handles[handle].kind == VFS_HANDLE_NODE && handles[handle].node) {
    vfs_node_put(handles[handle].node);
  }

  handles[handle].used = 0;
  handles[handle].refcount = 0;
  handles[handle].kind = VFS_HANDLE_NONE;
  handles[handle].node = 0;
  handles[handle].offset = 0;
  handles[handle].private_data = 0;
  handles[handle].ops = 0;
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
      c = serial_getc();
    if (c == 0)
      scheduler_yield();
  }
  return c;
#endif
}

static isize tty_read(struct vfs_node *node, u64 offset, char *buffer,
                      usize size, int flags) {
  (void)node;
  (void)offset;
  /* Job control check for background reads: */
  if (current_task && console.fg_pgrp > 0) {
    if (current_task->process_group_id != console.fg_pgrp) {
      // Process is in the background trying to read from TTY
      scheduler_kill_process_group(current_task->process_group_id, SIGTTIN);
      return -EINTR; // Abort read, let scheduler block the task
    }
  }

  if ((console.termios.c_lflag & B1NIX_ICANON) == 0) {
    for (usize i = 0; i < size; i++)
      buffer[i] = tty_getc_blocking();
    return (isize)size;
  }

  while (tty_line_pos >= tty_line_len) {
    tty_line_pos = 0;
    tty_line_len = 0;

    while (tty_line_len < sizeof(tty_line) - 1) {
      char c = 0;
      if (flags & B1NIX_O_NONBLOCK) {
        c = ps2_kbd_getc();
        if (c == 0)
          c = serial_getc();
        if (c == 0) {
          if (tty_line_len > 0)
            break;
          return -EAGAIN;
        }
      } else {
        c = tty_getc_blocking();
      }
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
      if (c == 4)
        break;
      if (c == '\b' || c == 127) {
        if (tty_line_len > 0) {
          tty_line_len--;
          if (console.termios.c_lflag & B1NIX_ECHO)
            console_write("\b \b");
        }
        continue;
      }
      tty_line[tty_line_len++] = c;
      if (console.termios.c_lflag & B1NIX_ECHO)
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
                       usize size, int flags) {
  (void)flags;
  (void)node;
  (void)offset;
  if (!buffer)
    return -1;

  /* Job control check for background writes: */
  if (current_task && console.fg_pgrp > 0) {
    if (current_task->process_group_id != console.fg_pgrp) {
      if (console.termios.c_lflag & B1NIX_TOSTOP) {
        scheduler_kill_process_group(current_task->process_group_id, SIGTTOU);
        return -EINTR;
      }
    }
  }

  for (usize i = 0; i < size; i++) {
    if ((console.termios.c_oflag & B1NIX_OPOST) && buffer[i] == '\n')
      console_putc('\r');
    console_putc(buffer[i]);
  }
  return (isize)size;
}

static void tty_init_node(void) {
  memset(&console.termios, 0, sizeof(console.termios));
  console.termios.c_lflag = B1NIX_ICANON | B1NIX_ECHO | B1NIX_ISIG;
  console.termios.c_oflag = B1NIX_OPOST;
  console.fg_pgrp = 1; /* Boot group */
  console.session_id = 1; /* Boot session */
  struct vfs_node *tty = add_node("/dev/tty", VFS_DEVICE, 0, 0, 0);
  if (tty) {
    tty->inode->read_cb = tty_read;
    tty->inode->write_cb = tty_write;
    tty->inode->mode =
        VFS_IRUSR | VFS_IWUSR | VFS_IRGRP | VFS_IWGRP | VFS_IROTH | VFS_IWOTH;
  }
}

/* Forward declarations for internal VFS metadata operations (thread-unsafe
 * variants) */
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
  dcache_init_pool();
  node_count = 0;
  memset(handles, 0, sizeof(handles));
  memset(mounts, 0, sizeof(mounts));

  root_node = alloc_node();
  root_node->inode = alloc_inode();
  strcpy(root_node->name, "/");
  root_node->inode->type = VFS_DIRECTORY;
  root_node->inode->mode = 0755;
  root_node->inode->atime = root_node->inode->mtime = root_node->inode->ctime =
      vfs_get_unix_time();
  root_node->inode->fs_id = 1;
  next_fs_id = 2;

  add_node("/dev", VFS_DIRECTORY, 0, 0, 0);
  add_node("/home", VFS_DIRECTORY, 0, 0, 0);
  add_node("/tmp", VFS_DIRECTORY, 0, 0, 0);
  add_node("/var", VFS_DIRECTORY, 0, 0, 0);
  add_node("/mnt", VFS_DIRECTORY, 0, 0, 0);
  add_node("/proc", VFS_DIRECTORY, 0, 0, 0);
  add_node("/ext4", VFS_DIRECTORY, 0, 0, 0);
  add_node("/ext3", VFS_DIRECTORY, 0, 0, 0);

  add_node("/dev/console", VFS_DEVICE, 0, 0, 0);
  add_node("/dev/virtio-blk0", VFS_DEVICE, 0, 0, 0);
  vfs_create("/tmp/hello", 0644);
  vfs_mount("initramfs", "/", "initramfs", 0);
  tty_init_node();
  vfs_init_stdio();

#ifndef __aarch64__
  virtio_blk_init();
#endif

  for (usize i = 0; i < blk_count(); i++) {
    struct block_device *dev = blk_at(i);
    char dev_path[64];
    strcpy(dev_path, "/dev/");
    strcat(dev_path, dev->name);
    add_node(dev_path, VFS_DEVICE, 0, 0, 0);
  }

#ifndef __aarch64__
  struct block_device *blk = blk_get("virtio-blk0");
  if (blk)
    fat32_mount(blk, "/mnt");
#endif

  console_write(
      "vfs: full featured initialized (POSIX+, Refcounting, Mount Crossing)\n");
}

int vfs_open(const char *path) { return vfs_open_flags(path, B1NIX_O_RDONLY); }

int vfs_open_flags(const char *path, int flags) {
  int res = 0;
  char *resolved = kmalloc(VFS_MAX_PATH);
  if (!resolved)
    return -ENOMEM;
  vfs_resolve_path(path, resolved);

  struct vfs_node *node = vfs_find_node_internal(resolved, 1, 0);
  if (IS_ERR(node)) {
    if (PTR_ERR(node) == -ENOENT && (flags & B1NIX_O_CREAT)) {
      /* Use internal version to avoid redundant resolution/logging */
      int err = vfs_create_at_internal(resolved, 0666);
      if (err != 0) {
        res = err;
        goto out;
      }
      node = vfs_find_node_internal(resolved, 1, 0);
      if (IS_ERR(node)) {
        res = (int)PTR_ERR(node);
        goto out;
      }
    } else {
      res = (int)PTR_ERR(node);
      klog_warn("vfs: open failed");
      goto out;
    }
  } else {
    if ((flags & B1NIX_O_CREAT) && (flags & B1NIX_O_EXCL)) {
      res = -EEXIST;
      vfs_node_put(node);
      goto out;
    }
  }

  if ((flags & B1NIX_O_DIRECTORY) && node->inode->type != VFS_DIRECTORY) {
    res = -ENOTDIR;
    vfs_node_put(node);
    goto out;
  }
  /* POSIX: writing to a directory descriptor is not permitted */
  if (node->inode->type == VFS_DIRECTORY &&
      (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))) {
    res = -EISDIR;
    vfs_node_put(node);
    goto out;
  }

  int access_mask = 0;
  if (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR))
    access_mask |= W_OK;
  if ((flags & 3) == B1NIX_O_RDONLY || (flags & B1NIX_O_RDWR))
    access_mask |= R_OK;

  res = vfs_check_access(node, access_mask);
  if (res != 0) {
    vfs_node_put(node);
    goto out;
  }

  if ((flags & B1NIX_O_TRUNC) && node->inode->type == VFS_FILE) {
    /* O_TRUNC requires write permission regardless of open mode */
    res = vfs_check_access(node, W_OK);
    if (res != 0) {
      vfs_node_put(node);
      goto out;
    }
    vfs_inode_lock(node->inode);
    node->inode->size = 0;
    if (node->inode->data && !node->inode->write_cb && !node->inode->read_cb)
      ((char *)node->inode->data)[0] = '\0';
    if (node->inode->setattr_cb)
      node->inode->setattr_cb(node);
    vfs_inode_unlock(node->inode);
  }

  vfs_acquire_handle_lock();
  int h_idx = -1;
  for (int i = 0; i < MAX_VFS_HANDLES; i++) {
    if (!handles[i].used) {
      h_idx = i;
      break;
    }
  }
  if (h_idx < 0) {
    vfs_release_handle_lock();
    res = -ENFILE;
    goto out;
  }

  struct vfs_handle *h = &handles[h_idx];
  memset(h, 0, sizeof(*h));
  h->used = 1;
  h->refcount = 1;
  h->kind = VFS_HANDLE_NODE;
  h->node = node; /* Already has ref from find_node */
  extern const struct vfs_file_ops node_file_ops;
  h->ops = &node_file_ops;
  h->flags = flags;
  h->offset = (flags & B1NIX_O_APPEND) ? node->inode->size : 0;
  vfs_release_handle_lock();

  int fd = scheduler_fd_alloc(h_idx);
  if (fd < 0) {
    vfs_acquire_handle_lock();
    release_handle(h_idx);
    vfs_release_handle_lock();
    res = -EMFILE;
    goto out;
  }
  if (flags & B1NIX_O_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  res = fd;

out:
  if (resolved)
    kfree(resolved);
  if (res < 0) {
    if (node && !IS_ERR(node))
      vfs_node_put(node);
    console_write("vfs: open failed with ");
    console_write_dec(-res);
    console_write("\n");
  }
  return res;
}

static isize node_read(struct vfs_handle *h, char *buf, usize size) {
  if (!h->node)
    return -EBADF;
  struct vfs_node *node = vfs_node_get(h->node);
  u64 offset = h->offset;
  vfs_inode_lock(node->inode);
  vfs_update_times(node->inode, VFS_ATIME);
  isize res = 0;
  if (node->inode->type == VFS_FILE && node->inode->read_cb) {
    usize remaining = size;
    usize total_read = 0;
    u64 curr_offset = offset;

    while (remaining > 0) {
      if (curr_offset >= node->inode->size) break;

      u64 page_aligned = curr_offset & ~(PAGE_SIZE - 1);
      usize page_offset = curr_offset & (PAGE_SIZE - 1);
      usize chunk = PAGE_SIZE - page_offset;
      if (chunk > remaining) chunk = remaining;
      if (curr_offset + chunk > node->inode->size) {
        chunk = node->inode->size - curr_offset;
      }

      struct page_cache_entry *page = page_cache_get_page(node->inode, page_aligned);
      if (!page) {
        u64 frame = pmm_alloc_frame();
        if (!frame) {
          if (total_read == 0) res = -ENOMEM;
          break;
        }

        void *virt_addr = (void *)(usize)(frame + vmm_direct_map_base());
        memset(virt_addr, 0, PAGE_SIZE);
        isize read_res = node->inode->read_cb(node, page_aligned, virt_addr, PAGE_SIZE, 0);

        if (read_res < 0) {
          pmm_free_frame(frame);
          if (total_read == 0) res = read_res;
          break;
        }

        if (page_cache_add_page(node->inode, page_aligned, frame) < 0) {
          pmm_free_frame(frame);
          page = page_cache_get_page(node->inode, page_aligned);
          if (!page) {
            if (total_read == 0) res = -ENOMEM;
            break;
          }
        } else {
          page = page_cache_get_page(node->inode, page_aligned);
        }
      }

      void *virt_addr = (void *)(usize)(page->frame + vmm_direct_map_base());
      memcpy(buf + total_read, (char *)virt_addr + page_offset, chunk);
      page_cache_put_page(page);

      total_read += chunk;
      curr_offset += chunk;
      remaining -= chunk;
    }

    if (total_read > 0) {
      res = total_read;
      h->offset += total_read;
    }
  } else if (node->inode->read_cb) {
    res = node->inode->read_cb(node, offset, buf, size, h->flags);
    if (res > 0)
      h->offset += (usize)res;
  } else if (node->inode->type == VFS_FILE) {
    usize rem = node->inode->size > offset ? node->inode->size - offset : 0;
    usize to_r = size < rem ? size : rem;
    if (to_r > 0) {
      memcpy(buf, (const char *)node->inode->data + offset, to_r);
      h->offset += to_r;
    }
    res = (isize)to_r;
  }
  vfs_inode_unlock(node->inode);
  vfs_node_put(node);
  return res;
}

static isize node_write(struct vfs_handle *h, const char *buf, usize size) {
  if (!h->node)
    return -EBADF;
  struct vfs_node *node = vfs_node_get(h->node);
  if (h->flags & B1NIX_O_APPEND)
    h->offset = node->inode->size;
  u64 offset = h->offset;
  vfs_inode_lock(node->inode);
  isize res = 0;
  if (node->inode->type == VFS_FILE && node->inode->write_cb) {
    usize remaining = size;
    usize total_written = 0;
    u64 curr_offset = offset;

    while (remaining > 0) {
      u64 page_aligned = curr_offset & ~(PAGE_SIZE - 1);
      usize page_offset = curr_offset & (PAGE_SIZE - 1);
      usize chunk = PAGE_SIZE - page_offset;
      if (chunk > remaining) chunk = remaining;

      struct page_cache_entry *page = page_cache_get_page(node->inode, page_aligned);
      if (!page) {
        u64 frame = pmm_alloc_frame();
        if (!frame) {
          if (total_written == 0) res = -ENOMEM;
          break;
        }

        void *virt_addr = (void *)(usize)(frame + vmm_direct_map_base());
        memset(virt_addr, 0, PAGE_SIZE);
        
        if ((chunk < PAGE_SIZE) && (page_aligned < node->inode->size)) {
          if (node->inode->read_cb) {
             node->inode->read_cb(node, page_aligned, virt_addr, PAGE_SIZE, 0);
          }
        }

        if (page_cache_add_page(node->inode, page_aligned, frame) < 0) {
          pmm_free_frame(frame);
          page = page_cache_get_page(node->inode, page_aligned);
          if (!page) {
            if (total_written == 0) res = -ENOMEM;
            break;
          }
        } else {
          page = page_cache_get_page(node->inode, page_aligned);
        }
      }

      void *virt_addr = (void *)(usize)(page->frame + vmm_direct_map_base());
      memcpy((char *)virt_addr + page_offset, buf + total_written, chunk);
      page_cache_mark_dirty(page);
      page_cache_put_page(page);

      total_written += chunk;
      curr_offset += chunk;
      remaining -= chunk;
    }

    if (total_written > 0) {
      res = total_written;
      h->offset += total_written;
      if (h->offset > node->inode->size) {
        node->inode->size = h->offset;
      }
      vfs_update_times(node->inode, VFS_MTIME | VFS_CTIME);
    }
  } else if (node->inode->write_cb) {
    res = node->inode->write_cb(node, offset, buf, size, h->flags);
    if (res > 0) {
      vfs_update_times(node->inode, VFS_MTIME | VFS_CTIME);
      h->offset += (usize)res;
    }
  } else if (node->inode->type == VFS_FILE) {
    if (offset + size > MAX_FILE_SIZE) {
      vfs_inode_unlock(node->inode);
      vfs_node_put(node);
      return -EFBIG;
    }
    if (offset + size > node->inode->capacity) {
      usize new_cap =
          node->inode->capacity == 0 ? 1024 : node->inode->capacity * 2;
      while (new_cap < offset + size)
        new_cap *= 2;
      void *new_data = kzalloc(new_cap);
      if (!new_data) {
        vfs_inode_unlock(node->inode);
        vfs_node_put(node);
        return -ENOMEM;
      }
      if (node->inode->data) {
        memcpy(new_data, node->inode->data, node->inode->size);
        if (node->inode->flags & VFS_NODE_OWNS_DATA)
          kfree(node->inode->data);
      }
      node->inode->data = new_data;
      node->inode->capacity = new_cap;
      node->inode->flags |= VFS_NODE_OWNS_DATA;
    }
    memcpy((char *)node->inode->data + offset, buf, size);
    if (offset + size > node->inode->size)
      node->inode->size = (usize)(offset + size);
    vfs_update_times(node->inode, VFS_MTIME | VFS_CTIME);
    if (node->inode->setattr_cb)
      node->inode->setattr_cb(node);
    h->offset += size;
    res = (isize)size;
  }
  vfs_inode_unlock(node->inode);
  vfs_node_put(node);
  return res;
}

static int node_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  if (!h || !h->node)
    return -EBADF;
  if (h->node->inode->poll_cb)
    return h->node->inode->poll_cb(h->node, pfd);
  pfd->revents = 0;
  if (h->node->inode->type == VFS_FILE) {
    pfd->revents |= B1NIX_POLLIN | B1NIX_POLLOUT;
  }
  return 0;
}

const struct vfs_file_ops node_file_ops = {
    .read = node_read,
    .write = node_write,
    .poll = node_poll,
};

isize vfs_read(int fd, char *buf, usize size) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || !h->ops || !h->ops->read)
    return -EBADF;
  return h->ops->read(h, buf, size);
}

isize vfs_write(int fd, const char *buf, usize size) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || !h->ops || !h->ops->write)
    return -EBADF;

  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(h->node);
  if (mnt && (mnt->flags & MS_RDONLY))
    return -EROFS;

  return h->ops->write(h, buf, size);
}

int vfs_poll(int fd, struct b1nix_pollfd *pfd) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || !h->ops || !h->ops->poll) {
    pfd->revents = B1NIX_POLLNVAL;
    return -1;
  }
  return h->ops->poll(h, pfd);
}

void vfs_close(int fd) {
  vfs_acquire_handle_lock();
  int h_idx = scheduler_fd_get(fd);
  if (h_idx < 0) {
    vfs_release_handle_lock();
    return;
  }
  struct vfs_handle *h = &handles[h_idx];
  vfs_release_handle_lock();

  if (h->kind == VFS_HANDLE_NODE && h->node && h->node->inode) {
    int my_pid = current_task ? (int)current_task->id : 0;
    filelock_release_all_by_pid_inode(my_pid, h->node->inode);
  }

  if (h->ops && h->ops->close)
    h->ops->close(h);

  vfs_acquire_handle_lock();
  scheduler_fd_close(fd);
  release_handle(h_idx);
  vfs_release_handle_lock();
}

static int vfs_create_at_internal(const char *resolved_path, u32 mode) {
  int res = 0;
  char *p_path = kmalloc(VFS_MAX_PATH);
  char name[64];
  if (!p_path)
    return -ENOMEM;
  if (split_parent_path(resolved_path, p_path, name) < 0) {
    kfree(p_path);
    return -EINVAL;
  }

  struct vfs_node *parent = vfs_find_node_internal(p_path, 1, 0);
  if (IS_ERR(parent)) {
    kfree(p_path);
    return (int)PTR_ERR(parent);
  }
  kfree(p_path);

  struct vfs_node *node = 0;
  vfs_inode_lock(parent->inode);
  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(parent);
  if (mnt && (mnt->flags & MS_RDONLY)) {
    res = -EROFS;
    goto out_unlock;
  }
  struct vfs_node *existing_child = find_child(parent, name);
  if (existing_child) {
    vfs_node_put(existing_child); /* Drop ref from find_child */
    res = -EEXIST;
    goto out_unlock;
  }
  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) {
    res = -EACCES;
    goto out_unlock;
  }

  node = alloc_node();
  if (!node) {
    res = -ENOMEM;
    goto out_unlock;
  }
  node->inode = alloc_inode();
  if (!node->inode) {
    memset(node, 0, sizeof(*node));
    res = -ENOMEM;
    goto out_node_put;
  }

  node->inode->blk_dev = parent->inode->blk_dev;
  copy_path(node->name, 64, name);
  node->inode->type = VFS_FILE;
  node->parent = parent;

  u16 umask = scheduler_get_current_umask();
  node->inode->mode = mode & ~umask;
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  node->inode->atime = node->inode->mtime = node->inode->ctime =
      vfs_get_unix_time();

  node->next_sibling = parent->first_child;
  parent->first_child = node;

  if (parent->inode->create_cb) {
    int err = parent->inode->create_cb(parent, name, resolved_path,
                                       node->inode->mode);
    if (err < 0) {
      parent->first_child = node->next_sibling;
      res = err;
      goto out_node_put;
    }
    node->inode->read_cb = parent->inode->read_cb;
    node->inode->write_cb = parent->inode->write_cb;
    node->inode->create_cb = parent->inode->create_cb;
    node->inode->mkdir_cb = parent->inode->mkdir_cb;
    node->inode->unlink_cb = parent->inode->unlink_cb;
    node->inode->rmdir_cb = parent->inode->rmdir_cb;
    node->inode->rename_cb = parent->inode->rename_cb;
    node->inode->link_cb = parent->inode->link_cb;
    node->inode->poll_cb = parent->inode->poll_cb;
  }
  goto out_unlock;

out_node_put:
  if (node)
    vfs_node_put(node);
out_unlock:
  vfs_inode_unlock(parent->inode);
  vfs_node_put(parent);
  return res;
}

int vfs_create(const char *path, u32 mode) {
  char *resolved = kmalloc(VFS_MAX_PATH);
  if (!resolved)
    return -ENOMEM;
  vfs_resolve_path(path, resolved);
  int res = vfs_create_at_internal(resolved, mode);
  kfree(resolved);
  return res;
}

struct vfs_node *vfs_find_node_by_fd(int fd) {
  struct vfs_handle *h = get_handle(fd);
  if (!h || h->kind != VFS_HANDLE_NODE)
    return ERR_PTR(-EBADF);
  return h->node;
}

static int vfs_mkdir_at_internal(const char *resolved_path, u32 mode) {
  int res = 0;
  char *p_path = kmalloc(VFS_MAX_PATH);
  char name[64];
  if (!p_path)
    return -ENOMEM;
  if (split_parent_path(resolved_path, p_path, name) < 0) {
    kfree(p_path);
    return -EINVAL;
  }

  struct vfs_node *parent = vfs_find_node_internal(p_path, 1, 0);
  if (IS_ERR(parent)) {
    kfree(p_path);
    return (int)PTR_ERR(parent);
  }
  kfree(p_path);

  struct vfs_node *node = 0;
  vfs_inode_lock(parent->inode);
  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(parent);
  if (mnt && (mnt->flags & MS_RDONLY)) {
    res = -EROFS;
    goto out_unlock;
  }
  struct vfs_node *existing_child = find_child(parent, name);
  if (existing_child) {
    vfs_node_put(existing_child); /* Drop ref from find_child */
    res = -EEXIST;
    goto out_unlock;
  }
  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) {
    res = -EACCES;
    goto out_unlock;
  }

  node = alloc_node();
  if (!node) {
    res = -ENOMEM;
    goto out_unlock;
  }
  node->inode = alloc_inode();
  if (!node->inode) {
    memset(node, 0, sizeof(*node));
    res = -ENOMEM;
    goto out_node_put;
  }

  node->inode->blk_dev = parent->inode->blk_dev;
  copy_path(node->name, 64, name);
  node->inode->type = VFS_DIRECTORY;
  node->parent = parent;

  u16 umask = scheduler_get_current_umask();
  node->inode->mode = mode & ~umask;
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  node->inode->atime = node->inode->mtime = node->inode->ctime =
      vfs_get_unix_time();

  node->next_sibling = parent->first_child;
  parent->first_child = node;

  if (parent->inode->mkdir_cb) {
    int err = parent->inode->mkdir_cb(parent, name, node->inode->mode);
    if (err < 0) {
      parent->first_child = node->next_sibling;
      res = err;
      goto out_node_put;
    }
    node->inode->read_cb = parent->inode->read_cb;
    node->inode->write_cb = parent->inode->write_cb;
    node->inode->create_cb = parent->inode->create_cb;
    node->inode->mkdir_cb = parent->inode->mkdir_cb;
    node->inode->unlink_cb = parent->inode->unlink_cb;
    node->inode->rmdir_cb = parent->inode->rmdir_cb;
    node->inode->rename_cb = parent->inode->rename_cb;
    node->inode->link_cb = parent->inode->link_cb;
    node->inode->poll_cb = parent->inode->poll_cb;
  }
  goto out_unlock;

out_node_put:
  if (node)
    vfs_node_put(node);
out_unlock:
  vfs_inode_unlock(parent->inode);
  vfs_node_put(parent);
  return res;
}

int vfs_mkdir(const char *path, u32 mode) {
  char *resolved = kmalloc(VFS_MAX_PATH);
  if (!resolved)
    return -ENOMEM;
  vfs_resolve_path(path, resolved);
  int res = vfs_mkdir_at_internal(resolved, mode);
  kfree(resolved);
  return res;
}

isize vfs_list(const char *dir_path, const char **names, usize max_names) {
  struct vfs_node *dir = vfs_find_node(dir_path);
  if (IS_ERR(dir))
    return PTR_ERR(dir);
  if (dir->inode->type != VFS_DIRECTORY) {
    vfs_node_put(dir);
    return -ENOTDIR;
  }

  int res = vfs_check_access(dir, R_OK);
  if (res != 0) {
    vfs_node_put(dir);
    return 0;
  }

  vfs_inode_lock_read(dir->inode);
  usize count = 0;
  struct vfs_node *child = dir->first_child;
  while (child && count < max_names) {
    if (!child->deleted) {
      names[count++] = child->name;
    }
    child = child->next_sibling;
  }
  vfs_inode_unlock_read(dir->inode);
  vfs_node_put(dir);
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
  vfs_inode_lock_read(inode);
  memset(st, 0, sizeof(*st));
  st->st_ino = inode->ino;
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

  st->st_dev = inode->fs_id;
  if (inode->type == VFS_DEVICE) {
    st->st_rdev = (u64)inode->data;
  }
  vfs_inode_unlock_read(inode);
  return 0;
}

int vfs_stat(const char *path, struct b1nix_stat *st) {
  if (!path || !st)
    return -EINVAL;
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);
  int res = vfs_stat_node(node, st);
  vfs_node_put(node);
  return res;
}

int vfs_statfs(const char *path, struct b1nix_statfs *st) {
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);

  int res = 0;
  if (node->inode->statfs_cb) {
    res = node->inode->statfs_cb(node, st);
  } else {
    res = -ENOSYS;
  }
  vfs_node_put(node);
  return res;
}

int vfs_lstat(const char *path, struct b1nix_stat *st) {
  if (!path || !st)
    return -EINVAL;
  struct vfs_node *node = vfs_find_node_no_follow(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);
  int res = vfs_stat_node(node, st);
  vfs_node_put(node);
  return res;
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
  int res = 0;
  char r_path[VFS_MAX_PATH];
  vfs_resolve_path(path, r_path);
  char p_path[VFS_MAX_PATH], name[64];
  split_parent_path(r_path, p_path, name);
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return -EINVAL;

  struct vfs_node *parent = vfs_find_node(p_path);
  if (IS_ERR(parent))
    return (int)PTR_ERR(parent);

  vfs_inode_lock(parent->inode);
  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(parent);
  if (mnt && (mnt->flags & MS_RDONLY)) {
    res = -EROFS;
    goto out_unlock;
  }
  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) {
    res = -EACCES;
    goto out_unlock;
  }

  /* Защита точек монтирования */
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i].used && strcmp(mounts[i].target, r_path) == 0) {
      res = -EBUSY;
      goto out_unlock;
    }
  }

  struct vfs_node *prev = 0, *child = parent->first_child;
  while (child) {
    if (!child->deleted && strcmp(child->name, name) == 0) {
      if (is_rmdir) {
        if (child->inode->type != VFS_DIRECTORY) {
          res = -ENOTDIR;
          goto out_unlock;
        }
        if (child->first_child) {
          res = -ENOTEMPTY;
          goto out_unlock;
        }
      } else {
        if (child->inode->type == VFS_DIRECTORY) {
          res = -EISDIR;
          goto out_unlock;
        }
      }
      if (parent->inode->unlink_cb && !is_rmdir) {
        int err = parent->inode->unlink_cb(parent, name);
        if (err < 0) {
          res = err;
          goto out_unlock;
        }
      } else if (parent->inode->rmdir_cb && is_rmdir) {
        int err = parent->inode->rmdir_cb(parent, name);
        if (err < 0) {
          res = err;
          goto out_unlock;
        }
      }

      child->deleted = 1;
      child->inode->nlink--;
      if (prev)
        prev->next_sibling = child->next_sibling;
      else
        parent->first_child = child->next_sibling;
      vfs_node_put(child);
      dcache_invalidate(parent, name);
      res = 0;
      goto out_unlock;
    }
    prev = child;
    child = child->next_sibling;
  }
  res = -ENOENT;

out_unlock:
  vfs_inode_unlock(parent->inode);
  vfs_node_put(parent);
  return res;
}

static int vfs_unlink_at_internal(const char *resolved_path) {
  int res = 0;
  char *p_path = kmalloc(VFS_MAX_PATH);
  char name[64];
  if (!p_path)
    return -ENOMEM;
  if (split_parent_path(resolved_path, p_path, name) < 0) {
    kfree(p_path);
    return -EINVAL;
  }

  struct vfs_node *parent = vfs_find_node_internal(p_path, 1, 0);
  if (IS_ERR(parent)) {
    kfree(p_path);
    return (int)PTR_ERR(parent);
  }
  kfree(p_path);

  struct vfs_node *node = 0;
  vfs_inode_lock(parent->inode);
  struct vfs_mount_entry *mnt = vfs_get_mount_for_node(parent);
  if (mnt && (mnt->flags & MS_RDONLY)) {
    res = -EROFS;
    goto out_unlock;
  }
  node = find_child(parent, name);
  if (!node) {
    res = -ENOENT;
    goto out_unlock;
  }
  /* find_child() returns with refcount incremented, caller owns it */
  if (node->inode->type == VFS_DIRECTORY) {
    res = -EISDIR;
    goto out_unlock;
  }

  if (parent->inode->unlink_cb) {
    int err = parent->inode->unlink_cb(parent, name);
    if (err < 0) {
      res = err;
      goto out_unlock;
    }
  }

  /* Remove from parent's children list */
  struct vfs_node **prev = &parent->first_child;
  while (*prev) {
    if (*prev == node) {
      *prev = node->next_sibling;
      break;
    }
    prev = &(*prev)->next_sibling;
  }

  node->deleted = 1;
  node->inode->nlink--;
  dcache_invalidate(parent, name);

out_unlock:
  if (node)
    vfs_node_put(node);
  vfs_inode_unlock(parent->inode);
  vfs_node_put(parent);
  return res;
}

int vfs_unlink(const char *path) {
  char *resolved = kmalloc(VFS_MAX_PATH);
  if (!resolved)
    return -ENOMEM;
  vfs_resolve_path(path, resolved);
  int res = vfs_unlink_at_internal(resolved);
  kfree(resolved);
  return res;
}

int vfs_link(const char *target, const char *link_path) {
  int res = 0;
  struct vfs_node *target_node = vfs_find_node(target);
  struct vfs_node *parent = 0;
  if (IS_ERR(target_node)) {
    res = (int)PTR_ERR(target_node);
    goto out;
  }
  if (target_node->inode->type == VFS_DIRECTORY) {
    res = -EPERM;
    goto out;
  }

  struct vfs_node *existing = vfs_find_node_no_follow(link_path);
  if (!IS_ERR(existing)) {
    vfs_node_put(existing);
    res = -EEXIST;
    goto out;
  }

  char parent_path[VFS_MAX_PATH], name[64];
  if (split_parent_path(link_path, parent_path, name) < 0) {
    res = -EINVAL;
    goto out;
  }

  parent = vfs_find_node(parent_path);
  if (IS_ERR(parent)) {
    res = (int)PTR_ERR(parent);
    goto out;
  }
  if (parent->inode->type != VFS_DIRECTORY) {
    res = -ENOTDIR;
    goto out;
  }

  vfs_inode_lock(parent->inode);
  res = vfs_check_access(parent, W_OK);
  if (res != 0) {
    vfs_inode_unlock(parent->inode);
    goto out;
  }

  struct vfs_node *new_node = alloc_node();
  if (!new_node) {
    vfs_inode_unlock(parent->inode);
    res = -ENOMEM;
    goto out;
  }

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
      vfs_node_put(new_node);
    }
  }
  vfs_inode_unlock(parent->inode);

out:
  if (parent && !IS_ERR(parent))
    vfs_node_put(parent);
  if (target_node && !IS_ERR(target_node))
    vfs_node_put(target_node);
  return res;
}

int vfs_symlink(const char *target, const char *link_path) {
  int res = 0;
  if (!target || target[0] == '\0')
    return -EINVAL;

  char parent_path[VFS_MAX_PATH], name[64];
  if (split_parent_path(link_path, parent_path, name) < 0)
    return -EINVAL;

  struct vfs_node *parent = vfs_find_node(parent_path);
  if (IS_ERR(parent))
    return (int)PTR_ERR(parent);
  if (parent->inode->type != VFS_DIRECTORY) {
    vfs_node_put(parent);
    return -ENOTDIR;
  }

  struct vfs_node *node = 0;
  vfs_inode_lock(parent->inode);
  struct vfs_node *existing_child = find_child(parent, name);
  if (existing_child) {
    vfs_node_put(existing_child); /* Drop ref from find_child */
    res = -EEXIST;
    goto out_unlock;
  }

  const struct cred *cred = get_current_cred();
  if (cred && !vfs_get_node_perm(parent, cred, 2)) {
    res = -EACCES;
    goto out_unlock;
  }

  usize len = strlen(target);
  if (len >= VFS_MAX_PATH) {
    res = -ENAMETOOLONG;
    goto out_unlock;
  }
  char *target_copy = kmalloc(len + 1);
  if (!target_copy) {
    res = -ENOMEM;
    goto out_unlock;
  }
  memcpy(target_copy, target, len + 1);

  node = alloc_node();
  if (!node) {
    kfree(target_copy);
    res = -ENOMEM;
    goto out_unlock;
  }
  node->inode = alloc_inode();
  if (!node->inode) {
    kfree(target_copy);
    vfs_node_put(node);
    res = -ENOMEM;
    goto out_unlock;
  }

  copy_path(node->name, 64, name);
  node->inode->type = VFS_SYMLINK;
  node->inode->data = target_copy;
  node->inode->size = len;
  node->inode->flags = VFS_NODE_OWNS_DATA;
  node->inode->mode = 0777;
  node->inode->uid = cred ? cred->euid : ROOT_UID;
  node->inode->gid = cred ? cred->egid : ROOT_GID;
  node->inode->atime = node->inode->mtime = node->inode->ctime =
      vfs_get_unix_time();
  node->parent = parent;
  node->next_sibling = parent->first_child;
  parent->first_child = node;

  if (parent->inode->symlink_cb) {
    int err = parent->inode->symlink_cb(parent, name, target);
    if (err < 0) {
      parent->first_child = node->next_sibling;
      vfs_node_put(node);
      res = err;
      goto out_unlock;
    }
  }

out_unlock:
  vfs_inode_unlock(parent->inode);
  vfs_node_put(parent);
  return res;
}

isize vfs_readlink(const char *path, char *buffer, usize size) {
  isize res = 0;
  if (!path || !buffer || size == 0) {
    res = -EINVAL;
    goto out;
  }
  struct vfs_node *node = vfs_find_node_no_follow(path);
  if (IS_ERR(node)) {
    res = PTR_ERR(node);
    goto out;
  }

  vfs_inode_lock_read(node->inode);
  if (node->inode->type != VFS_SYMLINK || !node->inode->data) {
    vfs_inode_unlock_read(node->inode);
    vfs_node_put(node);
    res = -EINVAL;
    goto out;
  }

  usize len = node->inode->size;
  if (len > size)
    len = size;
  memcpy(buffer, node->inode->data, len);
  res = (isize)len;
  vfs_inode_unlock_read(node->inode);
  vfs_node_put(node);

out:
  return res;
}

static int vfs_rename_internal(const char *old_path, const char *new_path) {
  int res = 0;
  char old_res[VFS_MAX_PATH], new_res[VFS_MAX_PATH];
  vfs_resolve_path(old_path, old_res);
  vfs_resolve_path(new_path, new_res);
  if (strcmp(old_res, new_res) == 0)
    return 0;

  char old_p[VFS_MAX_PATH], old_n[64], new_p[VFS_MAX_PATH], new_n[64];
  split_parent_path(old_res, old_p, old_n);
  split_parent_path(new_res, new_p, new_n);

  struct vfs_node *old_parent = vfs_find_node(old_p);
  if (IS_ERR(old_parent))
    return (int)PTR_ERR(old_parent);
  struct vfs_node *new_parent = vfs_find_node(new_p);
  if (IS_ERR(new_parent)) {
    vfs_node_put(old_parent);
    return (int)PTR_ERR(new_parent);
  }

  struct vfs_node *node = 0;
  struct vfs_node *existing = 0;

  node = find_child(old_parent, old_n);
  if (!node) {
    res = -ENOENT;
    goto out_put_parents;
  }

  /* Рекурсивная защита */
  struct vfs_node *tmp = new_parent;
  while (tmp) {
    if (tmp == node) {
      res = -EINVAL;
      goto out_put_parents;
    }
    tmp = tmp->parent;
  }

  /* EXDEV check */
  if (old_parent->inode->fs_id != new_parent->inode->fs_id) {
    res = -EXDEV;
    goto out_put_parents;
  }

  res = vfs_check_access(old_parent, W_OK);
  if (res == 0)
    res = vfs_check_access(new_parent, W_OK);
  if (res != 0) {
    goto out_put_parents;
  }

  /* Lock parents in consistent order */
  struct vfs_node *p1 = old_parent, *p2 = new_parent;
  if (p1 > p2) {
    struct vfs_node *t = p1;
    p1 = p2;
    p2 = t;
  }
  vfs_inode_lock(p1->inode);
  if (p1 != p2)
    vfs_inode_lock(p2->inode);

  struct vfs_mount_entry *mnt_old = vfs_get_mount_for_node(old_parent);
  if (mnt_old && (mnt_old->flags & MS_RDONLY)) {
    res = -EROFS;
    goto out_unlock;
  }
  struct vfs_mount_entry *mnt_new = vfs_get_mount_for_node(new_parent);
  if (mnt_new && (mnt_new->flags & MS_RDONLY)) {
    res = -EROFS;
    goto out_unlock;
  }

  /* POSIX type-consistency checks */
  existing = find_child(new_parent, new_n);
  if (existing) {
    if (node->inode->type == VFS_DIRECTORY &&
        existing->inode->type != VFS_DIRECTORY) {
      res = -ENOTDIR;
      goto out_unlock;
    }
    if (node->inode->type != VFS_DIRECTORY &&
        existing->inode->type == VFS_DIRECTORY) {
      res = -EISDIR;
      goto out_unlock;
    }
    if (existing->inode->type == VFS_DIRECTORY && existing->first_child) {
      res = -ENOTEMPTY;
      goto out_unlock;
    }
    vfs_remove_node(new_res, 0);
  }

  /* Перенос узла */
  struct vfs_node *prev_c = 0, *c = old_parent->first_child;
  while (c) {
    if (c == node) {
      if (prev_c)
        prev_c->next_sibling = c->next_sibling;
      else
        old_parent->first_child = c->next_sibling;
      break;
    }
    prev_c = c;
    c = c->next_sibling;
  }

  if (old_parent->inode->rename_cb) {
    int err =
        old_parent->inode->rename_cb(old_parent, old_n, new_parent, new_n);
    if (err < 0) {
      node->next_sibling = old_parent->first_child;
      old_parent->first_child = node;
      res = err;
      goto out_unlock;
    }
  }

  copy_path(node->name, 64, new_n);
  node->parent = new_parent;
  node->next_sibling = new_parent->first_child;
  new_parent->first_child = node;
  dcache_invalidate(old_parent, old_n);
  dcache_invalidate(new_parent, new_n);

out_unlock:
  if (p1 != p2)
    vfs_inode_unlock(p2->inode);
  vfs_inode_unlock(p1->inode);
out_put_parents:
  if (existing)
    vfs_node_put(existing);
  if (node)
    vfs_node_put(node);
  vfs_node_put(new_parent);
  vfs_node_put(old_parent);
  return res;
}

int vfs_rename(const char *old_path, const char *new_path) {
  return vfs_rename_internal(old_path, new_path);
}

int vfs_rmdir(const char *path) {
  int res = vfs_remove_node(path, 1);
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
  if (h_idx < 0)
    return -EBADF;
  struct vfs_handle *h = &handles[h_idx];
  if (!h->used || h->kind != VFS_HANDLE_NODE)
    return -EBADF;
  struct vfs_node *node = h->node;

  page_cache_flush_inode(node->inode);

  if (node->inode->fsync_cb) {
    int err = node->inode->fsync_cb(node);
    if (err < 0)
      return err;
  }

  if (node->inode->blk_dev)
    blk_cache_flush(node->inode->blk_dev);
  return 0;
}

static struct vfs_mount_entry *currently_mounting = NULL;

void vfs_set_currently_mounting_root(struct vfs_node *root) {
  if (currently_mounting) {
    currently_mounting->root_node = root;
  }
}

int vfs_mount(const char *source, const char *target, const char *fstype,
              u64 flags) {
  if (!target || target[0] == '\0' || !fstype)
    return -EINVAL;

  struct vfs_node *target_node = vfs_find_node(target);
  if (IS_ERR(target_node)) {
    return (int)PTR_ERR(target_node);
  }
  if (IS_ERR(target_node))
    return (int)PTR_ERR(target_node);
  if (target_node->inode->type != VFS_DIRECTORY) {
    vfs_node_put(target_node);
    return -ENOTDIR;
  }

  struct vfs_fs *fs = find_fs(fstype);
  if (!fs) {
    vfs_node_put(target_node);
    return -ENODEV;
  }

  int midx = -1;
  for (usize i = 0; i < MAX_MOUNTS; i++) {
    if (!mounts[i].used) {
      midx = (int)i;
      break;
    }
  }

  if (midx == -1) {
    vfs_node_put(target_node);
    return -ENOMEM;
  }

  // Pre-register slot so mount crossing works during populate_vfs
  mounts[midx].used = 1;
  mounts[midx].mount_point = target_node;
  mounts[midx].root_node = NULL;

  currently_mounting = &mounts[midx];
  struct vfs_node *root_node = fs->mount(source, flags, (void *)target);
  currently_mounting = NULL;

  if (IS_ERR(root_node)) {
    mounts[midx].used = 0;
    vfs_node_put(target_node);
    return (int)PTR_ERR(root_node);
  }

  interrupts_disable();
  copy_path(mounts[midx].source, sizeof(mounts[midx].source),
            source ? source : "");
  copy_path(mounts[midx].target, sizeof(mounts[midx].target), target);
  copy_path(mounts[midx].fstype, sizeof(mounts[midx].fstype), fstype);
  mounts[midx].flags = flags;
  mounts[midx].root_node = root_node;

  root_node->inode->fs_id = next_fs_id++;
  interrupts_enable();

  return 0;
}

int vfs_umount(const char *target) {
  if (!target)
    return -EINVAL;

  while (__atomic_test_and_set(&vfs_mount_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  for (usize i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i].used && strcmp(mounts[i].target, target) == 0) {
      if (strcmp(target, "/") == 0) {
        __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
        return -EBUSY;
      }

      /* Basic busy check: if root_node has other refs than our mount entry */
      if (mounts[i].root_node->refcount > 1) {
        __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
        return -EBUSY;
      }

      mounts[i].used = 0;
      struct vfs_node *root = mounts[i].root_node;
      struct vfs_node *mp = mounts[i].mount_point;
      __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);

      if (root && root->inode && root->inode->blk_dev) {
        blk_cache_flush(root->inode->blk_dev);
        blk_cache_invalidate(root->inode->blk_dev);
      }

      vfs_node_put(root);
      vfs_node_put(mp);
      return 0;
    }
  }
  __atomic_clear(&vfs_mount_lock, __ATOMIC_RELEASE);
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
  if (h_idx < 0) {
    vfs_release_handle_lock();
    return -EBADF;
  }
  struct vfs_handle *h = &handles[h_idx];
  if (!h->used || h->kind != VFS_HANDLE_NODE) {
    vfs_release_handle_lock();
    return -EBADF;
  }
  struct vfs_node *dir = h->node;
  if (!dir || dir->inode->type != VFS_DIRECTORY || !buf) {
    vfs_release_handle_lock();
    return -EINVAL;
  }

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
    buf[count].is_dir = 1;
    buf[count].is_exec = 1;
    buf[count].size = 0;
    count++;
    offset++;
  }
  if (offset == 1 && count < max_entries) {
    copy_path(buf[count].name, 64, "..");
    buf[count].type = (u32)VFS_DIRECTORY;
    buf[count].is_dir = 1;
    buf[count].is_exec = 1;
    buf[count].size = 0;
    count++;
    offset++;
  }

  struct vfs_node *child = dir->first_child;
  usize skipped = 0;
  while (child && count < max_entries) {
    if (child->deleted) {
      child = child->next_sibling;
      continue;
    }
    if (skipped < offset - 2) {
      skipped++;
      child = child->next_sibling;
      continue;
    }
    copy_path(buf[count].name, 64, child->name);
    buf[count].type = (u32)child->inode->type;
    buf[count].is_dir = (child->inode->type == VFS_DIRECTORY);
    buf[count].is_exec = 0;
    buf[count].size = child->inode->size;
    count++;
    offset++;
    child = child->next_sibling;
  }

  vfs_acquire_handle_lock();
  h->offset = offset;
  vfs_release_handle_lock();
  res = (isize)count;

out:
  vfs_node_put(dir);
  vfs_acquire_handle_lock();
  release_handle(h_idx);
  vfs_release_handle_lock();
  return res;
}

int vfs_sync(void) {
  /* Flush in-memory filesystem structures to block cache first */
  ext2_sync_all_fs();
  fat32_sync_all_fs();

  /* Then flush the entire block cache to physical hardware */
  blk_sync_all();
  return 0;
}

int vfs_dup2(int oldfd, int newfd) {
  int old_handle = scheduler_fd_get(oldfd);
  if (old_handle < 0 || (usize)old_handle >= MAX_VFS_HANDLES ||
      !handles[old_handle].used)
    return -EBADF;
  if (newfd < 0 || (usize)newfd >= SCHED_MAX_FDS)
    return -EBADF;
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
    if (h->kind != VFS_HANDLE_NODE)
      return -EBADF;
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
    *(struct b1nix_termios *)arg = console.termios;
    return 0;
  }
  if (request == B1NIX_TCSETS) {
    console.termios = *(const struct b1nix_termios *)arg;
    return 0;
  }
  if (request == B1NIX_TIOCGPGRP) {
    usize *pgrp_ptr = (usize *)arg;
    if (!pgrp_ptr)
      return -EFAULT;
    *pgrp_ptr = console.fg_pgrp;
    return 0;
  }
  if (request == B1NIX_TIOCSPGRP) {
    usize *pgrp_ptr = (usize *)arg;
    if (!pgrp_ptr)
      return -EFAULT;
    // POSIX: process group must be in the same session, and calling process must be in the terminal's session
    if (current_task) {
      if (current_task->session_id != console.session_id) {
        return -EPERM;
      }
      if (!scheduler_is_pgrp_in_session(*pgrp_ptr, current_task->session_id)) {
        return -EPERM;
      }
    }
    console.fg_pgrp = *pgrp_ptr;
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

/* ── Permission Management Functions ── */

int vfs_chmod(const char *path, u16 mode) {
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);

  const struct cred *cred = get_current_cred();
  if (!cred)
    return -EACCES;

  if (cred->euid != ROOT_UID && cred->euid != node->inode->uid) {
    if (!cred_has_cap(cred, CAP_FOWNER))
      return -EPERM;
  }

  node->inode->mode = (node->inode->mode & ~0777) | (mode & 0777);
  vfs_update_times(node->inode, VFS_CTIME);
  if (node->inode->setattr_cb)
    return node->inode->setattr_cb(node);
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

  handle->node->inode->mode =
      (handle->node->inode->mode & ~0777) | (mode & 0777);
  vfs_update_times(handle->node->inode, VFS_CTIME);
  if (handle->node->inode->setattr_cb)
    return handle->node->inode->setattr_cb(handle->node);
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
  vfs_update_times(node->inode, VFS_CTIME);
  if (node->inode->setattr_cb)
    return node->inode->setattr_cb(node);
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
  int count = node->inode->acl_count < max_entries ? node->inode->acl_count
                                                   : max_entries;
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
  if (handle->node->inode->setattr_cb)
    return handle->node->inode->setattr_cb(handle->node);
  return 0;
}

int vfs_fstatfs(int fd, struct b1nix_statfs *st) {
  struct vfs_handle *handle = get_handle(fd);
  if (!handle || !handle->used)
    return -EBADF;
  if (handle->kind != VFS_HANDLE_NODE || !handle->node)
    return -EINVAL;

  if (handle->node->inode->statfs_cb)
    return handle->node->inode->statfs_cb(handle->node, st);

  return -ENOSYS;
}

int vfs_syncfs(int fd) {
  struct vfs_handle *handle = get_handle(fd);
  if (!handle || !handle->used)
    return -EBADF;
  return vfs_sync();
}