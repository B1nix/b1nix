#include <b1nix/filelock.h>
#include <b1nix/mm.h>
#include <b1nix/errno.h>
#include <b1nix/vfs.h>
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
  // If existing lock is read and we want read, no conflict
  if (existing->lock_type == F_RDLCK && lock_type == F_RDLCK) {
    return 0;
  }
  // All other combinations conflict (write wants any, or read wants write)
  return 1;
}

int filelock_set_lock(int fd, int cmd, struct flock *fl) {
  if (!filelock_initialized || fd < 0)
    return -EINVAL;

  struct vfs_node *node = vfs_find_node_by_fd(fd);
  if (!node)
    return -EBADF;

  if (fl->l_type == F_UNLCK) {
    // Find and remove matching lock
    for (int i = 0; i < MAX_FILE_LOCKS; i++) {
      if (file_locks[i].active && file_locks[i].pid == fl->l_pid &&
          lock_overlaps(file_locks[i].start, file_locks[i].len, fl->l_start,
                        fl->l_len)) {
        free_lock(&file_locks[i]);
      }
    }
    return 0;
  }

  // Check for conflicting locks
  int conflict_pid = 0;
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (file_locks[i].active &&
        lock_overlaps(file_locks[i].start, file_locks[i].len, fl->l_start,
                      fl->l_len)) {
      if (lock_conflicts(&file_locks[i], fl->l_type)) {
        conflict_pid = file_locks[i].pid;
        break;
      }
    }
  }

  if (conflict_pid) {
    if (cmd == F_SETLK) {
      // Non-blocking, return error
      return -EAGAIN;
    }
    // F_SETLKW would block - not implemented yet
    return -EAGAIN;
  }

  // Create new lock
  struct file_lock *lock = alloc_lock();
  if (!lock)
    return -ENOMEM;

  lock->pid = fl->l_pid;
  lock->lock_type = fl->l_type;
  lock->start = fl->l_start;
  lock->len = fl->l_len;

  return 0;
}

int filelock_unlock(int fd) {
  if (!filelock_initialized || fd < 0)
    return -EINVAL;

  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (file_locks[i].active) {
      free_lock(&file_locks[i]);
    }
  }

  return 0;
}

int filelock_check_lock(int fd, int lock_type, u64 start, u64 len,
                        int *conflict_pid) {
  if (!filelock_initialized)
    return 0;

  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (file_locks[i].active &&
        lock_overlaps(file_locks[i].start, file_locks[i].len, start, len)) {
      if (lock_conflicts(&file_locks[i], lock_type)) {
        if (conflict_pid)
          *conflict_pid = file_locks[i].pid;
        return 1; // Conflict
      }
    }
  }

  return 0; // No conflict
}

int filelock_flock(int fd, int operation) {
  if (!filelock_initialized || fd < 0)
    return -EINVAL;

  struct flock fl;
  memset(&fl, 0, sizeof(fl));
  fl.l_pid = 0; // Would need proper PID

  switch (operation & 0x0F) {
  case LOCK_SH:
    fl.l_type = F_RDLCK;
    fl.l_start = 0;
    fl.l_len = 0; // Whole file
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
