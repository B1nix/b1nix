#ifndef B1NIX_FILELOCK_H
#define B1NIX_FILELOCK_H

#include <b1nix/types.h>

// Lock types
#define F_RDLCK 0 // Shared/read lock
#define F_WRLCK 1 // Exclusive/write lock
#define F_UNLCK 2 // Unlock

// Lock commands
#define F_SETLK  6  // Set lock (non-blocking)
#define F_SETLKW 7  // Set lock (blocking)

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

// File lock structure
struct file_lock {
    struct file_lock *next;
    int pid;              // Process ID owning this lock
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

#endif
