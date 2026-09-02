/* System V semaphores — semget(2), semop(2), semtimedop(2), semctl(2).
 *
 * A set is an array of counting semaphores operated on atomically: either the
 * whole sembuf array applies, or the caller blocks and nothing changes. That
 * all-or-nothing rule is the entire reason SysV semaphores still exist, so it
 * is what this implementation is built around — the operations are validated
 * against a snapshot first and only committed once every one of them can
 * proceed.
 *
 * SEM_UNDO is honoured: adjustments made under it are recorded per task and
 * rolled back when the task exits, which is what keeps a crashed process from
 * leaving a lock held forever.
 */

#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/sysv_ipc.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>
#include <string.h>

struct sem_slot {
  int used;
  u32 key;
  usize nsems;
  u16 vals[SEMMSL];
  usize pid[SEMMSL];  /* last process to operate on each semaphore (GETPID) */
  usize ncnt[SEMMSL]; /* waiters for value increase / for zero */
  usize zcnt[SEMMSL];
  struct ipc_perm perm;
  u64 otime, ctime;
};

/* One task's SEM_UNDO ledger: the net adjustment it must give back on exit. */
#define SEM_UNDO_MAX 64
struct sem_undo {
  int used;
  usize pid;
  int semid;
  u16 semnum;
  i32 adjust;
};

static struct sem_slot g_sets[SEMMNI];
static struct sem_undo g_undo[SEM_UNDO_MAX];
static u16 g_seq;
static spinlock_t g_sem_lock = SPINLOCK_INIT;

/* Wait channel: one per set, so a change to any semaphore in the set wakes
 * everyone sleeping on it and each re-evaluates its own operation array. */
static void *sem_chan(int semid) { return (void *)&g_sets[semid]; }

static int sem_id_valid(int semid) {
  return semid >= 0 && semid < SEMMNI && g_sets[semid].used;
}

static int sem_may_control(const struct sem_slot *s) {
  const struct cred *c = scheduler_get_current_cred();
  if (!c)
    return 1;
  return c->euid == 0 || c->euid == s->perm.uid || c->euid == s->perm.cuid;
}

int sysv_semget(u32 key, int nsems, int semflg) {
  if (nsems < 0 || nsems > SEMMSL)
    return -EINVAL;

  u64 flags;
  spin_lock_irqsave(&g_sem_lock, &flags);

  if (key != 0 /* IPC_PRIVATE */) {
    for (int i = 0; i < SEMMNI; i++) {
      if (!g_sets[i].used || g_sets[i].key != key)
        continue;
      if ((semflg & IPC_CREAT) && (semflg & IPC_EXCL)) {
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return -EEXIST;
      }
      if (nsems > 0 && (usize)nsems > g_sets[i].nsems) {
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return -EINVAL;
      }
      spin_unlock_irqrestore(&g_sem_lock, flags);
      return i;
    }
    if (!(semflg & IPC_CREAT)) {
      spin_unlock_irqrestore(&g_sem_lock, flags);
      return -ENOENT;
    }
  }

  if (nsems == 0) {
    spin_unlock_irqrestore(&g_sem_lock, flags);
    return -EINVAL; /* a new set needs at least one semaphore */
  }

  for (int i = 0; i < SEMMNI; i++) {
    if (g_sets[i].used)
      continue;
    memset(&g_sets[i], 0, sizeof(g_sets[i]));
    g_sets[i].used = 1;
    g_sets[i].key = key;
    g_sets[i].nsems = (usize)nsems;
    const struct cred *c = scheduler_get_current_cred();
    g_sets[i].perm.uid = g_sets[i].perm.cuid = c ? c->euid : 0;
    g_sets[i].perm.gid = g_sets[i].perm.cgid = c ? c->egid : 0;
    g_sets[i].perm.mode = (u16)(semflg & 0777);
    g_sets[i].perm.key = key;
    g_sets[i].perm.seq = g_seq++;
    g_sets[i].ctime = vfs_get_unix_time();
    spin_unlock_irqrestore(&g_sem_lock, flags);
    return i;
  }
  spin_unlock_irqrestore(&g_sem_lock, flags);
  return -ENOSPC;
}

/* Record (or fold into) this task's undo entry for one adjustment. */
static void undo_record(usize pid, int semid, u16 semnum, i32 adjust) {
  for (usize i = 0; i < SEM_UNDO_MAX; i++) {
    if (g_undo[i].used && g_undo[i].pid == pid && g_undo[i].semid == semid &&
        g_undo[i].semnum == semnum) {
      g_undo[i].adjust -= adjust; /* undo gives the opposite back */
      if (g_undo[i].adjust == 0)
        g_undo[i].used = 0;
      return;
    }
  }
  if (adjust == 0)
    return;
  for (usize i = 0; i < SEM_UNDO_MAX; i++) {
    if (g_undo[i].used)
      continue;
    g_undo[i].used = 1;
    g_undo[i].pid = pid;
    g_undo[i].semid = semid;
    g_undo[i].semnum = semnum;
    g_undo[i].adjust = -adjust;
    return;
  }
}

/* Try the whole operation array against the current values. Returns 0 if it
 * was applied, 1 if it must block, or -errno. Caller holds g_sem_lock. */
static int sem_try_ops(int semid, const struct sysv_sembuf *ops, usize nops,
                       usize pid) {
  struct sem_slot *s = &g_sets[semid];
  i32 next[SEMMSL];
  for (usize i = 0; i < s->nsems; i++)
    next[i] = (i32)s->vals[i];

  for (usize i = 0; i < nops; i++) {
    u16 n = ops[i].sem_num;
    if (n >= s->nsems)
      return -EFBIG;
    i32 op = ops[i].sem_op;
    if (op == 0) {
      if (next[n] != 0)
        return (ops[i].sem_flg & SYSV_IPC_NOWAIT) ? -EAGAIN : 1;
    } else if (op > 0) {
      if (next[n] + op > SEMVMX)
        return -ERANGE;
      next[n] += op;
    } else {
      if (next[n] + op < 0)
        return (ops[i].sem_flg & SYSV_IPC_NOWAIT) ? -EAGAIN : 1;
      next[n] += op;
    }
  }

  /* Every operation can proceed — commit them all. */
  for (usize i = 0; i < s->nsems; i++)
    s->vals[i] = (u16)next[i];
  for (usize i = 0; i < nops; i++) {
    s->pid[ops[i].sem_num] = pid;
    if (ops[i].sem_flg & SEM_UNDO)
      undo_record(pid, semid, ops[i].sem_num, ops[i].sem_op);
  }
  s->otime = vfs_get_unix_time();
  return 0;
}

int sysv_semop(int semid, const struct sysv_sembuf *ops, usize nops,
               i64 timeout_ms) {
  if (!ops || nops == 0 || nops > SEMMSL)
    return -EINVAL;
  usize pid = scheduler_get_pid();
  u64 deadline = 0;
  if (timeout_ms >= 0)
    deadline = scheduler_get_uptime_ticks() +
               (u64)timeout_ms * SCHED_TICKS_PER_SEC / 1000 + 1;

  while (1) {
    u64 flags;
    spin_lock_irqsave(&g_sem_lock, &flags);
    if (!sem_id_valid(semid)) {
      spin_unlock_irqrestore(&g_sem_lock, flags);
      return -EINVAL;
    }
    int rc = sem_try_ops(semid, ops, nops, pid);
    if (rc == 0) {
      spin_unlock_irqrestore(&g_sem_lock, flags);
      scheduler_wake_all(sem_chan(semid));
      return 0;
    }
    if (rc < 0) {
      spin_unlock_irqrestore(&g_sem_lock, flags);
      return rc;
    }

    /* Must block. Account the wait so GETNCNT/GETZCNT report the truth. */
    for (usize i = 0; i < nops; i++) {
      u16 n = ops[i].sem_num;
      if (n < g_sets[semid].nsems) {
        if (ops[i].sem_op == 0)
          g_sets[semid].zcnt[n]++;
        else if (ops[i].sem_op < 0)
          g_sets[semid].ncnt[n]++;
      }
    }
    spin_unlock_irqrestore(&g_sem_lock, flags);

    if (scheduler_signal_pending()) {
      rc = -ERESTARTSYS;
    } else if (timeout_ms >= 0) {
      scheduler_block_on_timeout(sem_chan(semid), deadline);
      rc = (scheduler_get_uptime_ticks() >= deadline) ? -EAGAIN : 0;
    } else {
      scheduler_block_on(sem_chan(semid));
      rc = 0;
    }

    spin_lock_irqsave(&g_sem_lock, &flags);
    if (sem_id_valid(semid)) {
      for (usize i = 0; i < nops; i++) {
        u16 n = ops[i].sem_num;
        if (n >= g_sets[semid].nsems)
          continue;
        if (ops[i].sem_op == 0 && g_sets[semid].zcnt[n])
          g_sets[semid].zcnt[n]--;
        else if (ops[i].sem_op < 0 && g_sets[semid].ncnt[n])
          g_sets[semid].ncnt[n]--;
      }
    } else {
      /* The set was removed while we slept — POSIX says EIDRM. */
      spin_unlock_irqrestore(&g_sem_lock, flags);
      return -EIDRM;
    }
    spin_unlock_irqrestore(&g_sem_lock, flags);

    if (rc < 0)
      return rc;
  }
}

int sysv_semctl_stat(int semid, struct sysv_semid_info *out) {
  u64 flags;
  spin_lock_irqsave(&g_sem_lock, &flags);
  if (!sem_id_valid(semid)) {
    spin_unlock_irqrestore(&g_sem_lock, flags);
    return -EINVAL;
  }
  out->sem_perm = g_sets[semid].perm;
  out->sem_otime = g_sets[semid].otime;
  out->sem_ctime = g_sets[semid].ctime;
  out->sem_nsems = g_sets[semid].nsems;
  spin_unlock_irqrestore(&g_sem_lock, flags);
  return 0;
}

int sysv_semctl_set(int semid, u16 uid, u16 gid, u16 mode) {
  u64 flags;
  spin_lock_irqsave(&g_sem_lock, &flags);
  if (!sem_id_valid(semid)) {
    spin_unlock_irqrestore(&g_sem_lock, flags);
    return -EINVAL;
  }
  if (!sem_may_control(&g_sets[semid])) {
    spin_unlock_irqrestore(&g_sem_lock, flags);
    return -EPERM;
  }
  g_sets[semid].perm.uid = uid;
  g_sets[semid].perm.gid = gid;
  g_sets[semid].perm.mode = mode & 0777;
  g_sets[semid].ctime = vfs_get_unix_time();
  spin_unlock_irqrestore(&g_sem_lock, flags);
  return 0;
}

int sysv_semctl_rmid(int semid) {
  u64 flags;
  spin_lock_irqsave(&g_sem_lock, &flags);
  if (!sem_id_valid(semid)) {
    spin_unlock_irqrestore(&g_sem_lock, flags);
    return -EINVAL;
  }
  if (!sem_may_control(&g_sets[semid])) {
    spin_unlock_irqrestore(&g_sem_lock, flags);
    return -EPERM;
  }
  g_sets[semid].used = 0;
  for (usize i = 0; i < SEM_UNDO_MAX; i++)
    if (g_undo[i].used && g_undo[i].semid == semid)
      g_undo[i].used = 0;
  spin_unlock_irqrestore(&g_sem_lock, flags);
  /* Sleepers wake, see the set is gone, and return EIDRM. */
  scheduler_wake_all(sem_chan(semid));
  return 0;
}

int sysv_semctl_getval(int semid, int semnum) {
  u64 flags;
  spin_lock_irqsave(&g_sem_lock, &flags);
  int rc = -EINVAL;
  if (sem_id_valid(semid) && semnum >= 0 &&
      (usize)semnum < g_sets[semid].nsems)
    rc = (int)g_sets[semid].vals[semnum];
  spin_unlock_irqrestore(&g_sem_lock, flags);
  return rc;
}

int sysv_semctl_setval(int semid, int semnum, int val) {
  if (val < 0 || val > SEMVMX)
    return -ERANGE;
  u64 flags;
  spin_lock_irqsave(&g_sem_lock, &flags);
  if (!sem_id_valid(semid) || semnum < 0 ||
      (usize)semnum >= g_sets[semid].nsems) {
    spin_unlock_irqrestore(&g_sem_lock, flags);
    return -EINVAL;
  }
  g_sets[semid].vals[semnum] = (u16)val;
  g_sets[semid].ctime = vfs_get_unix_time();
  /* SETVAL clears any undo entry for that semaphore, as SysV requires. */
  for (usize i = 0; i < SEM_UNDO_MAX; i++)
    if (g_undo[i].used && g_undo[i].semid == semid &&
        g_undo[i].semnum == (u16)semnum)
      g_undo[i].used = 0;
  spin_unlock_irqrestore(&g_sem_lock, flags);
  scheduler_wake_all(sem_chan(semid));
  return 0;
}

int sysv_semctl_getall(int semid, u16 *out, usize count) {
  u64 flags;
  spin_lock_irqsave(&g_sem_lock, &flags);
  if (!sem_id_valid(semid) || count < g_sets[semid].nsems) {
    spin_unlock_irqrestore(&g_sem_lock, flags);
    return -EINVAL;
  }
  for (usize i = 0; i < g_sets[semid].nsems; i++)
    out[i] = g_sets[semid].vals[i];
  int n = (int)g_sets[semid].nsems;
  spin_unlock_irqrestore(&g_sem_lock, flags);
  return n;
}

int sysv_semctl_setall(int semid, const u16 *vals, usize count) {
  u64 flags;
  spin_lock_irqsave(&g_sem_lock, &flags);
  if (!sem_id_valid(semid) || count < g_sets[semid].nsems) {
    spin_unlock_irqrestore(&g_sem_lock, flags);
    return -EINVAL;
  }
  for (usize i = 0; i < g_sets[semid].nsems; i++) {
    if (vals[i] > SEMVMX) {
      spin_unlock_irqrestore(&g_sem_lock, flags);
      return -ERANGE;
    }
  }
  for (usize i = 0; i < g_sets[semid].nsems; i++)
    g_sets[semid].vals[i] = vals[i];
  g_sets[semid].ctime = vfs_get_unix_time();
  for (usize i = 0; i < SEM_UNDO_MAX; i++)
    if (g_undo[i].used && g_undo[i].semid == semid)
      g_undo[i].used = 0;
  spin_unlock_irqrestore(&g_sem_lock, flags);
  scheduler_wake_all(sem_chan(semid));
  return 0;
}

int sysv_semctl_getcnt(int semid, int semnum, int want_zero) {
  u64 flags;
  spin_lock_irqsave(&g_sem_lock, &flags);
  int rc = -EINVAL;
  if (sem_id_valid(semid) && semnum >= 0 &&
      (usize)semnum < g_sets[semid].nsems)
    rc = (int)(want_zero ? g_sets[semid].zcnt[semnum]
                         : g_sets[semid].ncnt[semnum]);
  spin_unlock_irqrestore(&g_sem_lock, flags);
  return rc;
}

int sysv_semctl_getpid(int semid, int semnum) {
  u64 flags;
  spin_lock_irqsave(&g_sem_lock, &flags);
  int rc = -EINVAL;
  if (sem_id_valid(semid) && semnum >= 0 &&
      (usize)semnum < g_sets[semid].nsems)
    rc = (int)g_sets[semid].pid[semnum];
  spin_unlock_irqrestore(&g_sem_lock, flags);
  return rc;
}

void sysv_sem_task_cleanup(usize pid) {
  int woke = -1;
  u64 flags;
  spin_lock_irqsave(&g_sem_lock, &flags);
  for (usize i = 0; i < SEM_UNDO_MAX; i++) {
    if (!g_undo[i].used || g_undo[i].pid != pid)
      continue;
    int semid = g_undo[i].semid;
    u16 n = g_undo[i].semnum;
    if (sem_id_valid(semid) && n < g_sets[semid].nsems) {
      i32 v = (i32)g_sets[semid].vals[n] + g_undo[i].adjust;
      if (v < 0)
        v = 0;
      if (v > SEMVMX)
        v = SEMVMX;
      g_sets[semid].vals[n] = (u16)v;
      woke = semid;
    }
    g_undo[i].used = 0;
  }
  spin_unlock_irqrestore(&g_sem_lock, flags);
  if (woke >= 0)
    scheduler_wake_all(sem_chan(woke));
}
