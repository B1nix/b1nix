#ifndef B1NIX_LANDLOCK_H
#define B1NIX_LANDLOCK_H
#include <b1nix/types.h>

/* Landlock filesystem access rights (uapi/linux/landlock.h). */
#define LL_EXECUTE     (1ULL << 0)
#define LL_WRITE_FILE  (1ULL << 1)
#define LL_READ_FILE   (1ULL << 2)
#define LL_READ_DIR    (1ULL << 3)
#define LL_REMOVE_DIR  (1ULL << 4)
#define LL_REMOVE_FILE (1ULL << 5)
#define LL_MAKE_CHAR   (1ULL << 6)
#define LL_MAKE_DIR    (1ULL << 7)
#define LL_MAKE_REG    (1ULL << 8)
#define LL_MAKE_SOCK   (1ULL << 9)
#define LL_MAKE_FIFO   (1ULL << 10)
#define LL_MAKE_BLOCK  (1ULL << 11)
#define LL_MAKE_SYM    (1ULL << 12)
#define LL_REFER       (1ULL << 13)
#define LL_TRUNCATE    (1ULL << 14)

int landlock_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 *ret);
void landlock_fork(usize parent_row, usize child_row);
void landlock_task_reset(usize row);
/* Checks for the current task; 0 when allowed or unrestricted. */
int landlock_check_path(const char *path, u64 access);
int landlock_check_parent(const char *path, u64 access);
int landlock_check_open(const char *path, int flags);
int landlock_check_move(const char *old_path, const char *new_path, int is_dir,
                        int is_link);
#endif
