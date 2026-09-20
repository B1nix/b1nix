/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_SHM_H
#define B1NIX_SHM_H

#include <b1nix/types.h>

/* ── POSIX Shared Memory Constants ── */

/* IPC flags */
#define IPC_CREAT  0x1000   /* Create key if key does not exist */
#define IPC_EXCL   0x2000   /* Fail if key exists */
#define IPC_NOWAIT 0x4000   /* Return error on wait */
#define IPC_RMID   0       /* Remove segment */
#define IPC_SET    1       /* Set owner, permissions */
#define IPC_STAT   2       /* Get shmid_ds */
#define IPC_INFO   3       /* Get system info */

/* shmget flags */
#define SHM_RDONLY  0x1000  /* Attach read-only */
#define SHM_RND     0x2000  /* Round attach address to SHMLBA */
#define SHM_REMAP   0x4000  /* Replace existing mapping */
#define SHM_EXEC    0x8000  /* Allow execution */

/* Limits */
#define SHMMAX      0x2000000 /* Default max segment size (32 MB) — large enough
                                * for full-screen graphics/framebuffer buffers
                                * (1280x800x4 = 4 MB) and several windows. Backing
                                * is allocated on demand, not pre-reserved. M77:
                                * the ENFORCED runtime cap is
                                * g_resource_caps.shmmax_bytes (tunable via
                                * /proc/sys/kernel/shmmax); SHMMAX remains the
                                * boot-time default and the value shm_init prints. */
#define SHMMIN      1         /* Min segment size */
#define SHMMNI      32        /* Max number of shared memory segments system-wide */
#define SHMSEG      8         /* Max segments per process */
#define SHMLBA      PAGE_SIZE /* Segment low boundary address multiple */

/* Permission bits */
#define SHM_R       0400   /* Read permission */
#define SHM_W       0200   /* Write permission */

/* ── Data Structures ── */

/* The ids in here are KERNEL ids (see <b1nix/user_namespace.h>); the syscall
 * layer translates them for the caller. */
struct ipc_perm {
    u32  uid;           /* Owner's user ID */
    u32  gid;           /* Owner's group ID */
    u32  cuid;          /* Creator's user ID */
    u32  cgid;          /* Creator's group ID */
    u16  mode;          /* Read/write permission, plus SHM_DEST */
    u16  seq;           /* Slot usage sequence number */
    u32  key;           /* IPC key */
};

/* shm mode bit: IPC_RMID was asked while attached; the segment goes away at
 * its last detach and can no longer be found by key. */
#define SHM_DEST    01000

struct shmid_ds {
    struct ipc_perm shm_perm;    /* Operation permissions */
    usize          shm_segsz;    /* Size of segment in bytes */
    u64            shm_atime;    /* Last attach time */
    u64            shm_dtime;    /* Last detach time */
    u64            shm_ctime;    /* Last change time */
    usize          shm_cpid;     /* kernel id of the creator */
    usize          shm_lpid;     /* kernel id of the last shmat/shmdt caller */
    u32            shm_nattch;   /* Number of current attaches */
    u32            shm_npages;   /* Number of pages allocated */
};

struct shm_segment {
    int   used;
    u32   key;                /* IPC key */
    u32   ns;                 /* IPC namespace */
    struct shmid_ds ds;
    /* Array of physical page frames backing the segment. Allocated (kmalloc)
     * on shmget to the segment's actual size — M77 made SHMMAX a runtime cap,
     * so the compile-time fixed array is gone. */
    u64   *physical_pages;
    int   page_count;
};

/* ── Process attach tracking ── */

#define SHM_MAX_ATTACH_PER_PROC 8

struct shm_attach {
    int   used;
    int   shmid;               /* Index into shm_segments */
    u64   virtual_addr;        /* Virtual address in process */
};

/* ── API ── */

void shm_init(void);
/* Each returns a negative errno on failure; shmat returns it cast to a
 * pointer. Ids name segments of the caller's IPC namespace only. */
int  shmget(u32 key, usize size, int shmflg);
void *shmat(int shmid, const void *shmaddr, int shmflg);
int  shmdt(const void *shmaddr);
/* IPC_STAT fills *buf (after a read-permission check), IPC_SET takes uid, gid
 * and mode from it (kernel ids), IPC_RMID ignores it. */
int  shmctl(int shmid, int cmd, struct shmid_ds *buf);
/* The segment at table index `idx` if it belongs to the caller's IPC
 * namespace, without a permission check — /proc/sysvipc/shm. */
int  shm_stat_index(int idx, struct shmid_ds *out);
/* Drop every segment of an IPC namespace that is going away. */
void shm_ns_destroy(u32 ns);

/* For per-process tracking need to know current task id */
struct shm_attach *shm_get_process_attaches(usize pid);

/* Called from user_address_space_cleanup() on address-space teardown
 * (voluntary exit, signal kill, execve): decrement shm_nattch for every
 * still-attached segment and free the per-process attach slot. Bookkeeping
 * only — the teardown unmaps the pages and frees the (refcounted) frames.
 * Without this shm_nattch never reaches 0, IPC_RMID stays blocked forever and
 * the attach table leaks. */
void shm_account_exit(usize pid);

/* fork: account the child's inherited VMM_SHARED attachments so shm_nattch
 * counts both processes and the child's exit cleans up its own slot. */
void shm_fork_inherit(usize parent_pid, usize child_pid);

#endif /* B1NIX_SHM_H */
