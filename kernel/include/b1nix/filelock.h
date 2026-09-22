/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_FILELOCK_H
#define B1NIX_FILELOCK_H

#include <b1nix/types.h>

// Lock types
#define F_RDLCK 0 // Shared/read lock
#define F_WRLCK 1 // Exclusive/write lock
#define F_UNLCK 2 // Unlock

// Lock commands
#define F_GETLK  5  // Get lock
#define F_SETLK  6  // Set lock (non-blocking)
#define F_SETLKW 7  // Set lock (blocking)

/* Open-file-description locks. Same records and the same conflicts as the
 * POSIX ones, owned by the open file rather than by the process: they survive
 * a fork, they are not dropped when some unrelated descriptor on the same file
 * is closed, and two threads of one process can contend for them.
 *
 * systemd uses them for anything it must not lose to a stray close --
 * systemd-random-seed takes one on the seed file in the ESP, and with the call
 * refused the unit waits and the boot stops there. */
#define F_OFD_GETLK  36
#define F_OFD_SETLK  37
#define F_OFD_SETLKW 38

struct flock {
    short l_type;   // F_RDLCK, F_WRLCK, F_UNLCK
    short l_whence; // SEEK_SET, SEEK_CUR, SEEK_END
    u64   l_start;
    u64   l_len;    // 0 = to EOF
    int   l_pid;    // Process holding the lock
    int   l_sysid;
};

// Flock operations
#define LOCK_SH 1  // Shared lock
#define LOCK_EX 2  // Exclusive lock
#define LOCK_NB 4  // Non-blocking
#define LOCK_UN 8  // Unlock

struct vfs_inode;

// File lock structure
struct file_lock {
    struct file_lock *next;
    /* What the lock applies to: the inode for a file, and the open file
     * description itself for a descriptor that has no inode -- a socket, a
     * pipe, an eventfd. Compared, never dereferenced. */
    void *inode;
    int pid;              // Process (thread group) owning a POSIX lock
    /* The open file description owning an OFD lock, and what tells the two
     * kinds apart: null for a POSIX lock. Never dereferenced -- it is an
     * identity, and the handle it names may be freed while this record is
     * being cleaned up. */
    void *ofd;
    int lock_type;        // F_RDLCK or F_WRLCK
    u64 start;            // Start offset
    u64 len;              // Length (0 = whole file)
    int active;
};

void filelock_init(void);
int filelock_set_lock(int fd, int cmd, struct flock *fl);
int filelock_unlock(int fd);
int filelock_check_lock(int fd, int lock_type, u64 start, u64 len, int *conflict_pid);
int filelock_flock(int fd, int operation);
int filelock_set_lock_ofd(int fd, int cmd, struct flock *fl);
/* Drop this process's POSIX locks on one scope: an inode, or -- for a
 * descriptor that has none -- the open file description itself. */
void filelock_release_all_by_pid_inode(int pid, void *scope);
/* Every OFD lock held by one open file description, released when it closes. */
void filelock_release_all_by_ofd(void *ofd);

#endif
