#ifndef B1NIX_SYSV_IPC_H
#define B1NIX_SYSV_IPC_H

#include <b1nix/posix.h>
#include <b1nix/shm.h> /* struct ipc_perm, IPC_CREAT/IPC_EXCL/IPC_RMID/… */
#include <b1nix/types.h>

/* ── System V semaphores (semget/semop/semctl/semtimedop) ──
 * The structures below use the Linux x86_64 layouts, because the only callers
 * are Linux-personality tasks (b1nix's own libc has POSIX semaphores and
 * futexes). Values are the ones <sys/sem.h> defines. */

#define SEMMNI 32  /* semaphore sets, system wide */
#define SEMMSL 32  /* semaphores per set */
#define SEMVMX 32767 /* maximum semaphore value */

#define SEM_UNDO 0x1000

/* semctl commands (the SysV-specific ones; IPC_RMID/SET/STAT come from shm.h) */
#define GETPID  11
#define GETVAL  12
#define GETALL  13
#define GETNCNT 14
#define GETZCNT 15
#define SETVAL  16
#define SETALL  17

struct sysv_sembuf {
  u16 sem_num;
  i16 sem_op;
  i16 sem_flg;
};

/* Kernel-side view of one set's metadata, translated to/from the caller's
 * struct semid_ds by the syscall layer. */
struct sysv_semid_info {
  struct ipc_perm sem_perm;
  u64 sem_otime;
  u64 sem_ctime;
  u64 sem_nsems;
};

int sysv_semget(u32 key, int nsems, int semflg);
/* ops points at kernel memory. timeout_ms < 0 blocks forever. */
int sysv_semop(int semid, const struct sysv_sembuf *ops, usize nops,
               i64 timeout_ms);
int sysv_semctl_stat(int semid, struct sysv_semid_info *out);
int sysv_semctl_set(int semid, u16 uid, u16 gid, u16 mode);
int sysv_semctl_rmid(int semid);
int sysv_semctl_getval(int semid, int semnum);
int sysv_semctl_setval(int semid, int semnum, int val);
int sysv_semctl_getall(int semid, u16 *out, usize count);
int sysv_semctl_setall(int semid, const u16 *vals, usize count);
int sysv_semctl_getcnt(int semid, int semnum, int want_zero);
int sysv_semctl_getpid(int semid, int semnum);
/* Apply a task's SEM_UNDO adjustments and drop its bookkeeping (exit path). */
void sysv_sem_task_cleanup(usize pid);

/* ── System V message queues (msgget/msgsnd/msgrcv/msgctl) ── */

#define MSGMNI 16   /* queues, system wide */
#define MSGMAX 4096 /* bytes per message */
#define MSGTQL 32   /* messages per queue */

/* Flag bits as the System V ABI (and Linux) define them. b1nix's own shm.h
 * uses different values for its native shmget flags, hence the SYSV_ prefix. */
#define SYSV_MSG_NOERROR 010000
#define SYSV_IPC_NOWAIT 04000

struct sysv_msqid_info {
  struct ipc_perm msg_perm;
  u64 msg_stime;
  u64 msg_rtime;
  u64 msg_ctime;
  u64 msg_qnum;
  u64 msg_qbytes;
};

int sysv_msgget(u32 key, int msgflg);
/* text points at kernel memory. */
isize sysv_msgsnd(int msqid, i64 mtype, const void *text, usize size,
                  int msgflg);
isize sysv_msgrcv(int msqid, i64 msgtyp, void *text, usize size, int msgflg,
                  i64 *out_type);
int sysv_msgctl_stat(int msqid, struct sysv_msqid_info *out);
int sysv_msgctl_set(int msqid, u16 uid, u16 gid, u16 mode, u64 qbytes);
int sysv_msgctl_rmid(int msqid);

void sysv_ipc_init(void);

#endif
