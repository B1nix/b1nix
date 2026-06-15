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
#define SHMMAX      0x2000000 /* Max segment size (32 MB) — large enough for
                               * full-screen graphics/framebuffer buffers
                               * (1280x800x4 = 4 MB) and several windows. Backing
                               * is allocated on demand, not pre-reserved. */
#define SHMMIN      1         /* Min segment size */
#define SHMMNI      32        /* Max number of shared memory segments system-wide */
#define SHMSEG      8         /* Max segments per process */
#define SHMLBA      PAGE_SIZE /* Segment low boundary address multiple */

/* Permission bits */
#define SHM_R       0400   /* Read permission */
#define SHM_W       0200   /* Write permission */

/* ── Data Structures ── */

struct ipc_perm {
    u16  uid;           /* Owner's user ID */
    u16  gid;           /* Owner's group ID */
    u16  cuid;          /* Creator's user ID */
    u16  cgid;          /* Creator's group ID */
    u16  mode;          /* Read/write permission */
    u16  seq;           /* Slot usage sequence number */
    u32  key;           /* IPC key */
};

struct shmid_ds {
    struct ipc_perm shm_perm;    /* Operation permissions */
    usize          shm_segsz;    /* Size of segment in bytes */
    u64            shm_atime;    /* Last attach time */
    u64            shm_dtime;    /* Last detach time */
    u64            shm_ctime;    /* Last change time */
    u16            shm_cpid;     /* PID of creator */
    u16            shm_lpid;     /* PID of last shmop */
    u16            shm_nattch;   /* Number of current attaches */
    u16            shm_npages;   /* Number of pages allocated */
};

struct shm_segment {
    int   used;
    u32   key;                /* IPC key */
    struct shmid_ds ds;
    u64   physical_pages[SHMMAX / PAGE_SIZE]; /* Array of physical page frames */
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
int  shmget(u32 key, usize size, int shmflg);
void *shmat(int shmid, const void *shmaddr, int shmflg);
int  shmdt(const void *shmaddr);
int  shmctl(int shmid, int cmd, struct shmid_ds *buf);

/* For per-process tracking need to know current task id */
struct shm_attach *shm_get_process_attaches(usize pid);

#endif /* B1NIX_SHM_H */
