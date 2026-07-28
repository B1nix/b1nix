/* System V message queues — msgget(2), msgsnd(2), msgrcv(2), msgctl(2).
 *
 * A queue is an ordered list of typed messages. Unlike a pipe, a reader picks
 * which message it wants by type: 0 takes the head, a positive type takes the
 * first message of exactly that type, and a negative type takes the lowest
 * type that is <= |msgtyp|. That type-selective receive is the reason programs
 * still reach for these, so it is implemented exactly, including the ordering
 * rule that ties are broken by arrival order.
 */

#include <b1nix/arch.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/sysv_ipc.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>
#include <string.h>

struct msg_entry {
  int used;
  i64 mtype;
  usize size;
  u64 seq; /* arrival order */
  char *text;
};

struct msg_queue {
  int used;
  u32 key;
  struct ipc_perm perm;
  struct msg_entry msgs[MSGTQL];
  u64 next_seq;
  u64 qbytes;   /* capacity in bytes */
  u64 cbytes;   /* bytes currently queued */
  u64 stime, rtime, ctime;
};

static struct msg_queue g_queues[MSGMNI];
static u16 g_seq;
static spinlock_t g_msg_lock = SPINLOCK_INIT;

static void *msg_chan(int id) { return (void *)&g_queues[id]; }

static int msg_id_valid(int id) {
  return id >= 0 && id < MSGMNI && g_queues[id].used;
}

static int msg_may_control(const struct msg_queue *q) {
  const struct cred *c = scheduler_get_current_cred();
  if (!c)
    return 1;
  return c->euid == 0 || c->euid == q->perm.uid || c->euid == q->perm.cuid;
}

int sysv_msgget(u32 key, int msgflg) {
  u64 flags;
  spin_lock_irqsave(&g_msg_lock, &flags);

  if (key != 0 /* IPC_PRIVATE */) {
    for (int i = 0; i < MSGMNI; i++) {
      if (!g_queues[i].used || g_queues[i].key != key)
        continue;
      if ((msgflg & IPC_CREAT) && (msgflg & IPC_EXCL)) {
        spin_unlock_irqrestore(&g_msg_lock, flags);
        return -EEXIST;
      }
      spin_unlock_irqrestore(&g_msg_lock, flags);
      return i;
    }
    if (!(msgflg & IPC_CREAT)) {
      spin_unlock_irqrestore(&g_msg_lock, flags);
      return -ENOENT;
    }
  }

  for (int i = 0; i < MSGMNI; i++) {
    if (g_queues[i].used)
      continue;
    memset(&g_queues[i], 0, sizeof(g_queues[i]));
    g_queues[i].used = 1;
    g_queues[i].key = key;
    g_queues[i].qbytes = MSGMAX * MSGTQL;
    const struct cred *c = scheduler_get_current_cred();
    g_queues[i].perm.uid = g_queues[i].perm.cuid = c ? c->euid : 0;
    g_queues[i].perm.gid = g_queues[i].perm.cgid = c ? c->egid : 0;
    g_queues[i].perm.mode = (u16)(msgflg & 0777);
    g_queues[i].perm.key = key;
    g_queues[i].perm.seq = g_seq++;
    g_queues[i].ctime = vfs_get_unix_time();
    spin_unlock_irqrestore(&g_msg_lock, flags);
    return i;
  }
  spin_unlock_irqrestore(&g_msg_lock, flags);
  return -ENOSPC;
}

isize sysv_msgsnd(int msqid, i64 mtype, const void *text, usize size,
                  int msgflg) {
  if (mtype < 1)
    return -EINVAL;
  if (size > MSGMAX)
    return -EINVAL;

  /* Allocate outside the lock — kmalloc may sleep on heap growth. */
  char *copy = size ? kmalloc(size) : kmalloc(1);
  if (!copy)
    return -ENOMEM;
  if (size)
    memcpy(copy, text, size);

  while (1) {
    u64 flags;
    spin_lock_irqsave(&g_msg_lock, &flags);
    if (!msg_id_valid(msqid)) {
      spin_unlock_irqrestore(&g_msg_lock, flags);
      kfree(copy);
      return -EINVAL;
    }
    struct msg_queue *q = &g_queues[msqid];
    int slot = -1;
    for (int i = 0; i < MSGTQL; i++)
      if (!q->msgs[i].used) {
        slot = i;
        break;
      }
    if (slot >= 0 && q->cbytes + size <= q->qbytes) {
      q->msgs[slot].used = 1;
      q->msgs[slot].mtype = mtype;
      q->msgs[slot].size = size;
      q->msgs[slot].seq = q->next_seq++;
      q->msgs[slot].text = copy;
      q->cbytes += size;
      q->stime = vfs_get_unix_time();
      spin_unlock_irqrestore(&g_msg_lock, flags);
      scheduler_wake_all(msg_chan(msqid));
      return (isize)size;
    }
    spin_unlock_irqrestore(&g_msg_lock, flags);

    if (msgflg & SYSV_IPC_NOWAIT) {
      kfree(copy);
      return -EAGAIN;
    }
    if (scheduler_signal_pending()) {
      kfree(copy);
      return -ERESTARTSYS;
    }
    scheduler_block_on(msg_chan(msqid));
  }
}

isize sysv_msgrcv(int msqid, i64 msgtyp, void *text, usize size, int msgflg,
                  i64 *out_type) {
  while (1) {
    u64 flags;
    spin_lock_irqsave(&g_msg_lock, &flags);
    if (!msg_id_valid(msqid)) {
      spin_unlock_irqrestore(&g_msg_lock, flags);
      return -EINVAL;
    }
    struct msg_queue *q = &g_queues[msqid];

    int best = -1;
    for (int i = 0; i < MSGTQL; i++) {
      if (!q->msgs[i].used)
        continue;
      i64 t = q->msgs[i].mtype;
      int match;
      if (msgtyp == 0)
        match = 1;
      else if (msgtyp > 0)
        match = (t == msgtyp);
      else
        match = (t <= -msgtyp);
      if (!match)
        continue;
      if (best < 0) {
        best = i;
        continue;
      }
      /* msgtyp < 0 selects the LOWEST type; ties (and the other modes) go by
       * arrival order. */
      if (msgtyp < 0 && q->msgs[i].mtype != q->msgs[best].mtype)
        best = (q->msgs[i].mtype < q->msgs[best].mtype) ? i : best;
      else if (q->msgs[i].seq < q->msgs[best].seq)
        best = i;
    }

    if (best >= 0) {
      struct msg_entry *m = &q->msgs[best];
      usize n = m->size;
      if (n > size) {
        if (!(msgflg & SYSV_MSG_NOERROR)) {
          spin_unlock_irqrestore(&g_msg_lock, flags);
          return -E2BIG;
        }
        n = size; /* SYSV_MSG_NOERROR: truncate, and the rest is discarded */
      }
      if (n)
        memcpy(text, m->text, n);
      if (out_type)
        *out_type = m->mtype;
      q->cbytes -= m->size;
      char *freed = m->text;
      m->used = 0;
      m->text = 0;
      q->rtime = vfs_get_unix_time();
      spin_unlock_irqrestore(&g_msg_lock, flags);
      kfree(freed);
      scheduler_wake_all(msg_chan(msqid)); /* a blocked sender may fit now */
      return (isize)n;
    }
    spin_unlock_irqrestore(&g_msg_lock, flags);

    if (msgflg & SYSV_IPC_NOWAIT)
      return -ENOMSG;
    if (scheduler_signal_pending())
      return -ERESTARTSYS;
    scheduler_block_on(msg_chan(msqid));
  }
}

int sysv_msgctl_stat(int msqid, struct sysv_msqid_info *out) {
  u64 flags;
  spin_lock_irqsave(&g_msg_lock, &flags);
  if (!msg_id_valid(msqid)) {
    spin_unlock_irqrestore(&g_msg_lock, flags);
    return -EINVAL;
  }
  struct msg_queue *q = &g_queues[msqid];
  usize qnum = 0;
  for (int i = 0; i < MSGTQL; i++)
    if (q->msgs[i].used)
      qnum++;
  out->msg_perm = q->perm;
  out->msg_stime = q->stime;
  out->msg_rtime = q->rtime;
  out->msg_ctime = q->ctime;
  out->msg_qnum = qnum;
  out->msg_qbytes = q->qbytes;
  spin_unlock_irqrestore(&g_msg_lock, flags);
  return 0;
}

int sysv_msgctl_set(int msqid, u16 uid, u16 gid, u16 mode, u64 qbytes) {
  u64 flags;
  spin_lock_irqsave(&g_msg_lock, &flags);
  if (!msg_id_valid(msqid)) {
    spin_unlock_irqrestore(&g_msg_lock, flags);
    return -EINVAL;
  }
  if (!msg_may_control(&g_queues[msqid])) {
    spin_unlock_irqrestore(&g_msg_lock, flags);
    return -EPERM;
  }
  struct cred *c = scheduler_get_current_cred();
  /* Only root may RAISE the capacity, exactly as Linux gates msg_qbytes. */
  if (qbytes > g_queues[msqid].qbytes && !(c && c->euid == 0)) {
    spin_unlock_irqrestore(&g_msg_lock, flags);
    return -EPERM;
  }
  g_queues[msqid].perm.uid = uid;
  g_queues[msqid].perm.gid = gid;
  g_queues[msqid].perm.mode = mode & 0777;
  if (qbytes)
    g_queues[msqid].qbytes = qbytes;
  g_queues[msqid].ctime = vfs_get_unix_time();
  spin_unlock_irqrestore(&g_msg_lock, flags);
  return 0;
}

int sysv_msgctl_rmid(int msqid) {
  u64 flags;
  spin_lock_irqsave(&g_msg_lock, &flags);
  if (!msg_id_valid(msqid)) {
    spin_unlock_irqrestore(&g_msg_lock, flags);
    return -EINVAL;
  }
  if (!msg_may_control(&g_queues[msqid])) {
    spin_unlock_irqrestore(&g_msg_lock, flags);
    return -EPERM;
  }
  char *to_free[MSGTQL];
  usize nfree = 0;
  for (int i = 0; i < MSGTQL; i++) {
    if (g_queues[msqid].msgs[i].used && g_queues[msqid].msgs[i].text)
      to_free[nfree++] = g_queues[msqid].msgs[i].text;
    g_queues[msqid].msgs[i].used = 0;
    g_queues[msqid].msgs[i].text = 0;
  }
  g_queues[msqid].used = 0;
  spin_unlock_irqrestore(&g_msg_lock, flags);
  for (usize i = 0; i < nfree; i++)
    kfree(to_free[i]);
  scheduler_wake_all(msg_chan(msqid));
  return 0;
}

void sysv_ipc_init(void) {
  memset(g_queues, 0, sizeof(g_queues));
}
