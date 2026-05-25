#include <b1nix/filelock.h>
#include <b1nix/mm.h>
#include <b1nix/errno.h>
#include <b1nix/vfs.h>
#include <b1nix/sched.h>
#include <string.h>

#define MAX_FILE_LOCKS 64

static struct file_lock file_locks[MAX_FILE_LOCKS];
static int filelock_initialized = 0;

void filelock_init(void) {
  memset(file_locks, 0, sizeof(file_locks));
  filelock_initialized = 1;
}

static struct file_lock *alloc_lock(void) {
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (!file_locks[i].active) {
      memset(&file_locks[i], 0, sizeof(struct file_lock));
      file_locks[i].active = 1;
      return &file_locks[i];
    }
  }
  return 0;
}

static void free_lock(struct file_lock *lock) {
  if (lock) {
    lock->active = 0;
  }
}

static int lock_overlaps(u64 start1, u64 len1, u64 start2, u64 len2) {
  u64 end1 = len1 ? start1 + len1 - 1 : (u64)-1;
  u64 end2 = len2 ? start2 + len2 - 1 : (u64)-1;

  return start1 <= end2 && start2 <= end1;
}

static int lock_conflicts(struct file_lock *existing, int lock_type) {
  if (existing->lock_type == F_RDLCK && lock_type == F_RDLCK) {
    return 0;
  }
  return 1;
}

static int can_merge(u64 start1, u64 len1, u64 start2, u64 len2) {
  u64 end1 = len1 ? start1 + len1 - 1 : (u64)-1;
  u64 end2 = len2 ? start2 + len2 - 1 : (u64)-1;

  if (end1 == (u64)-1) {
    if (start2 < start1) {
      return end2 == (u64)-1 || end2 + 1 >= start1;
    }
    return 1;
  }
  if (end2 == (u64)-1) {
    if (start1 < start2) {
      return end1 + 1 >= start2;
    }
    return 1;
  }
  return (start1 <= end2 + 1) && (start2 <= end1 + 1);
}

static void merge_adjacent_locks(struct vfs_inode *inode, int pid) {
  int merged;
  do {
    merged = 0;
    for (int i = 0; i < MAX_FILE_LOCKS; i++) {
      if (!file_locks[i].active || file_locks[i].inode != inode || file_locks[i].pid != pid)
        continue;
      for (int j = i + 1; j < MAX_FILE_LOCKS; j++) {
        if (!file_locks[j].active || file_locks[j].inode != inode || file_locks[j].pid != pid)
          continue;
        if (file_locks[i].lock_type != file_locks[j].lock_type)
          continue;

        if (can_merge(file_locks[i].start, file_locks[i].len, file_locks[j].start, file_locks[j].len)) {
          u64 start1 = file_locks[i].start;
          u64 end1 = file_locks[i].len ? start1 + file_locks[i].len - 1 : (u64)-1;
          u64 start2 = file_locks[j].start;
          u64 end2 = file_locks[j].len ? start2 + file_locks[j].len - 1 : (u64)-1;

          u64 new_start = start1 < start2 ? start1 : start2;
          u64 new_end = (end1 == (u64)-1 || end2 == (u64)-1) ? (u64)-1 : (end1 > end2 ? end1 : end2);

          file_locks[i].start = new_start;
          file_locks[i].len = (new_end == (u64)-1) ? 0 : (new_end - new_start + 1);
          free_lock(&file_locks[j]);
          merged = 1;
          break;
        }
      }
      if (merged) break;
    }
  } while (merged);
}

int filelock_set_lock(int fd, int cmd, struct flock *fl) {
  if (!filelock_initialized || fd < 0)
    return -EINVAL;

  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h)
    return -EBADF;

  struct vfs_inode *inode = h->node->inode;
  int my_pid = current_task ? (int)current_task->id : 0;

  u64 start = fl->l_start;
  if (fl->l_whence == B1NIX_SEEK_CUR) {
    start = h->offset + fl->l_start;
  } else if (fl->l_whence == B1NIX_SEEK_END) {
    start = inode->size + fl->l_start;
  }
  u64 end = fl->l_len ? start + fl->l_len - 1 : (u64)-1;

  if (cmd == F_GETLK) {
    struct file_lock *conflicting = NULL;
    for (int i = 0; i < MAX_FILE_LOCKS; i++) {
      if (file_locks[i].active && file_locks[i].inode == inode && file_locks[i].pid != my_pid) {
        if (lock_overlaps(file_locks[i].start, file_locks[i].len, start, fl->l_len)) {
          if (lock_conflicts(&file_locks[i], fl->l_type)) {
            conflicting = &file_locks[i];
            break;
          }
        }
      }
    }
    if (conflicting) {
      fl->l_type = conflicting->lock_type;
      fl->l_start = conflicting->start;
      fl->l_len = conflicting->len;
      fl->l_pid = conflicting->pid;
      fl->l_whence = B1NIX_SEEK_SET;
    } else {
      fl->l_type = F_UNLCK;
    }
    return 0;
  }

  if (fl->l_type != F_UNLCK) {
    while (1) {
      int conflict = 0;
      for (int i = 0; i < MAX_FILE_LOCKS; i++) {
        if (file_locks[i].active && file_locks[i].inode == inode && file_locks[i].pid != my_pid) {
          if (lock_overlaps(file_locks[i].start, file_locks[i].len, start, fl->l_len)) {
            if (lock_conflicts(&file_locks[i], fl->l_type)) {
              conflict = 1;
              break;
            }
          }
        }
      }
      if (!conflict) {
        break;
      }
      if (cmd == F_SETLK) {
        return -EAGAIN;
      }
      scheduler_block_on(inode);
    }
  }

  // Pre-check for split slot capacity
  int free_slots = 0;
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (!file_locks[i].active) free_slots++;
  }
  int needs_split = 0;
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (file_locks[i].active && file_locks[i].inode == inode && file_locks[i].pid == my_pid) {
      u64 L_end = file_locks[i].len ? file_locks[i].start + file_locks[i].len - 1 : (u64)-1;
      if (file_locks[i].start < start && L_end > end) {
        needs_split = 1;
        break;
      }
    }
  }
  if (fl->l_type == F_UNLCK) {
    if (needs_split && free_slots < 1) return -ENOMEM;
  } else {
    if (needs_split && free_slots < 2) return -ENOMEM;
    if (!needs_split && free_slots < 1) return -ENOMEM;
  }

  // Apply split/shrink/delete for our own locks in this range
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (file_locks[i].active && file_locks[i].inode == inode && file_locks[i].pid == my_pid) {
      if (lock_overlaps(file_locks[i].start, file_locks[i].len, start, fl->l_len)) {
        u64 L_start = file_locks[i].start;
        u64 L_end = file_locks[i].len ? L_start + file_locks[i].len - 1 : (u64)-1;

        if (L_start < start && L_end > end) {
          struct file_lock *L2 = alloc_lock();
          L2->inode = inode;
          L2->pid = my_pid;
          L2->lock_type = file_locks[i].lock_type;
          L2->start = end + 1;
          L2->len = (L_end == (u64)-1) ? 0 : (L_end - L2->start + 1);

          file_locks[i].len = start - L_start;
        } else if (L_start < start && L_end <= end) {
          file_locks[i].len = start - L_start;
        } else if (L_start >= start && L_end > end) {
          file_locks[i].start = end + 1;
          file_locks[i].len = (L_end == (u64)-1) ? 0 : (L_end - file_locks[i].start + 1);
        } else {
          free_lock(&file_locks[i]);
        }
      }
    }
  }

  if (fl->l_type != F_UNLCK) {
    struct file_lock *lock = alloc_lock();
    lock->inode = inode;
    lock->pid = my_pid;
    lock->lock_type = fl->l_type;
    lock->start = start;
    lock->len = fl->l_len;

    merge_adjacent_locks(inode, my_pid);
  }

  scheduler_wake_all(inode);
  return 0;
}

int filelock_unlock(int fd) {
  if (!filelock_initialized || fd < 0)
    return -EINVAL;

  struct vfs_node *node = vfs_find_node_by_fd(fd);
  if (!node)
    return -EBADF;

  int my_pid = current_task ? (int)current_task->id : 0;
  filelock_release_all_by_pid_inode(my_pid, node->inode);
  return 0;
}

int filelock_check_lock(int fd, int lock_type, u64 start, u64 len,
                        int *conflict_pid) {
  if (!filelock_initialized || fd < 0)
    return 0;

  struct vfs_node *node = vfs_find_node_by_fd(fd);
  if (!node)
    return 0;

  struct vfs_inode *inode = node->inode;
  int my_pid = current_task ? (int)current_task->id : 0;

  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (file_locks[i].active && file_locks[i].inode == inode && file_locks[i].pid != my_pid) {
      if (lock_overlaps(file_locks[i].start, file_locks[i].len, start, len)) {
        if (lock_conflicts(&file_locks[i], lock_type)) {
          if (conflict_pid)
            *conflict_pid = file_locks[i].pid;
          return 1;
        }
      }
    }
  }

  return 0;
}

int filelock_flock(int fd, int operation) {
  if (!filelock_initialized || fd < 0)
    return -EINVAL;

  int my_pid = current_task ? (int)current_task->id : 0;

  struct flock fl;
  memset(&fl, 0, sizeof(fl));
  fl.l_pid = my_pid;

  switch (operation & 0x0F) {
  case LOCK_SH:
    fl.l_type = F_RDLCK;
    fl.l_start = 0;
    fl.l_len = 0;
    break;
  case LOCK_EX:
    fl.l_type = F_WRLCK;
    fl.l_start = 0;
    fl.l_len = 0;
    break;
  case LOCK_UN:
    fl.l_type = F_UNLCK;
    fl.l_start = 0;
    fl.l_len = 0;
    break;
  default:
    return -EINVAL;
  }

  int cmd = (operation & LOCK_NB) ? F_SETLK : F_SETLKW;
  return filelock_set_lock(fd, cmd, &fl);
}

void filelock_release_all_by_pid_inode(int pid, struct vfs_inode *inode) {
  if (!filelock_initialized || !inode)
    return;

  int woke_any = 0;
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (file_locks[i].active && file_locks[i].inode == inode && file_locks[i].pid == pid) {
      free_lock(&file_locks[i]);
      woke_any = 1;
    }
  }
  if (woke_any) {
    scheduler_wake_all(inode);
  }
}
