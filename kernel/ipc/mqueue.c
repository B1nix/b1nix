#include <string.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/mqueue.h>
#include <b1nix/sched.h>

static struct mqueue queues[MQ_MAX_QUEUES];

void mqueue_init(void)
{
    memset(queues, 0, sizeof(queues));
    console_write("mqueue: initialized\n");
}

struct mqueue *mqueue_find(const char *name)
{
    for (int i = 0; i < MQ_MAX_QUEUES; i++) {
        if (queues[i].used && strcmp(queues[i].name, name) == 0) {
            return &queues[i];
        }
    }
    return 0;
}

struct mqueue *mqueue_create(const char *name)
{
    struct mqueue *existing = mqueue_find(name);
    if (existing) {
        existing->refcount++;
        return existing;
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
            queues[i].head = 0;
            queues[i].tail = 0;
            queues[i].count = 0;
            return &queues[i];
        }
    }
    return 0;
}

int mqueue_send(struct mqueue *mq, const void *data, u32 len)
{
    if (!mq || len > MQ_MAX_MSG_SIZE) return -1;

    /* If queue is full, block (yield) */
    while (mq->count >= MQ_MAX_MSGS) {
        mq->writers_waiting++;
        /* Wake a reader if any are waiting */
        if (mq->readers_waiting > 0) {
            /* The reader will be woken by scheduler — we just yield */
        }
        scheduler_yield();
        mq->writers_waiting--;
    }

    /* Copy message into circular buffer */
    struct mq_msg *msg = &mq->msgs[mq->tail];
    msg->len = len;
    memcpy(msg->data, data, len);
    mq->tail = (mq->tail + 1) % MQ_MAX_MSGS;
    mq->count++;

    return 0;
}

int mqueue_receive(struct mqueue *mq, void *data, u32 *len)
{
    if (!mq || !data || !len) return -1;

    /* If queue is empty, block */
    while (mq->count == 0) {
        mq->readers_waiting++;
        scheduler_yield();
        mq->readers_waiting--;
    }

    /* Read message from circular buffer */
    struct mq_msg *msg = &mq->msgs[mq->head];
    *len = msg->len;
    memcpy(data, msg->data, msg->len);
    mq->head = (mq->head + 1) % MQ_MAX_MSGS;
    mq->count--;

    return 0;
}

void mqueue_close(struct mqueue *mq)
{
    if (!mq) return;
    if (mq->refcount > 0) mq->refcount--;
}

int mqueue_unlink(const char *name)
{
    struct mqueue *mq = mqueue_find(name);
    if (!mq) return -1;

    /* Only unlink if no more references */
    if (mq->refcount <= 1) {
        mq->used = 0;
        mq->refcount = 0;
        memset(mq->name, 0, sizeof(mq->name));
        return 0;
    }
    return -1;
}
