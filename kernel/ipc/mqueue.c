#include <string.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/mqueue.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>

static struct mqueue queues[MQ_MAX_QUEUES];
/* One global lock for the whole table: protects the used/refcount slots, the
 * name namespace, and each queue's circular-buffer indices. The table is tiny
 * (16 queues) so a single lock is adequate and avoids per-queue lock ordering. */
static spinlock_t mq_lock = SPINLOCK_INIT;

void mqueue_init(void)
{
    memset(queues, 0, sizeof(queues));
    console_write("mqueue: initialized\n");
}

/* Caller must hold mq_lock. */
static struct mqueue *mq_find_locked(const char *name)
{
    for (int i = 0; i < MQ_MAX_QUEUES; i++) {
        if (queues[i].used && strcmp(queues[i].name, name) == 0)
            return &queues[i];
    }
    return 0;
}

/* Resolve a userspace handle to a queue slot. Returns NULL for an
 * out-of-range or stale (closed) index. Caller must hold mq_lock. */
static struct mqueue *mq_resolve_locked(int mqd)
{
    if (mqd < 0 || mqd >= MQ_MAX_QUEUES)
        return 0;
    if (!queues[mqd].used)
        return 0;
    return &queues[mqd];
}

int mqueue_create(const char *name)
{
    u64 flags;
    spin_lock_irqsave(&mq_lock, &flags);

    struct mqueue *existing = mq_find_locked(name);
    if (existing) {
        existing->refcount++;
        int mqd = (int)(existing - queues);
        spin_unlock_irqrestore(&mq_lock, flags);
        return mqd;
    }

    for (int i = 0; i < MQ_MAX_QUEUES; i++) {
        if (!queues[i].used) {
            memset(&queues[i], 0, sizeof(struct mqueue));
            usize len = strlen(name);
            if (len > 63) len = 63;
            memcpy(queues[i].name, name, len);
            queues[i].name[len] = '\0';
            queues[i].used = 1;
            queues[i].refcount = 1;
            spin_unlock_irqrestore(&mq_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&mq_lock, flags);
    return -ENOSPC;
}

int mqueue_send(int mqd, const void *data, u32 len)
{
    if (len > MQ_MAX_MSG_SIZE) return -EMSGSIZE;

    u64 flags;
    spin_lock_irqsave(&mq_lock, &flags);
    struct mqueue *mq = mq_resolve_locked(mqd);
    if (!mq) {
        spin_unlock_irqrestore(&mq_lock, flags);
        return -EBADF;
    }

    /* If the queue is full, drop the lock and yield, then recheck the slot is
     * still valid (it could be unlinked while we slept). */
    while (mq->count >= MQ_MAX_MSGS) {
        mq->writers_waiting++;
        spin_unlock_irqrestore(&mq_lock, flags);
        scheduler_yield();
        spin_lock_irqsave(&mq_lock, &flags);
        mq = mq_resolve_locked(mqd);
        if (!mq) {
            spin_unlock_irqrestore(&mq_lock, flags);
            return -EBADF;
        }
        mq->writers_waiting--;
    }

    struct mq_msg *msg = &mq->msgs[mq->tail];
    msg->len = len;
    memcpy(msg->data, data, len);
    mq->tail = (mq->tail + 1) % MQ_MAX_MSGS;
    mq->count++;
    spin_unlock_irqrestore(&mq_lock, flags);
    return 0;
}

int mqueue_receive(int mqd, void *data, u32 *len)
{
    if (!data || !len) return -EINVAL;

    u64 flags;
    spin_lock_irqsave(&mq_lock, &flags);
    struct mqueue *mq = mq_resolve_locked(mqd);
    if (!mq) {
        spin_unlock_irqrestore(&mq_lock, flags);
        return -EBADF;
    }

    while (mq->count == 0) {
        mq->readers_waiting++;
        spin_unlock_irqrestore(&mq_lock, flags);
        scheduler_yield();
        spin_lock_irqsave(&mq_lock, &flags);
        mq = mq_resolve_locked(mqd);
        if (!mq) {
            spin_unlock_irqrestore(&mq_lock, flags);
            return -EBADF;
        }
        mq->readers_waiting--;
    }

    struct mq_msg *msg = &mq->msgs[mq->head];
    *len = msg->len;
    memcpy(data, msg->data, msg->len);
    mq->head = (mq->head + 1) % MQ_MAX_MSGS;
    mq->count--;
    spin_unlock_irqrestore(&mq_lock, flags);
    return 0;
}

void mqueue_close(int mqd)
{
    u64 flags;
    spin_lock_irqsave(&mq_lock, &flags);
    struct mqueue *mq = mq_resolve_locked(mqd);
    if (mq && mq->refcount > 0)
        mq->refcount--;
    spin_unlock_irqrestore(&mq_lock, flags);
}

int mqueue_unlink(const char *name)
{
    u64 flags;
    spin_lock_irqsave(&mq_lock, &flags);
    struct mqueue *mq = mq_find_locked(name);
    if (!mq) {
        spin_unlock_irqrestore(&mq_lock, flags);
        return -ENOENT;
    }

    /* Only free the slot once the last reference is gone. */
    if (mq->refcount <= 1) {
        mq->used = 0;
        mq->refcount = 0;
        memset(mq->name, 0, sizeof(mq->name));
        spin_unlock_irqrestore(&mq_lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&mq_lock, flags);
    return -EBUSY;
}
