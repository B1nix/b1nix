#ifndef B1NIX_MQUEUE_H
#define B1NIX_MQUEUE_H

#include <b1nix/types.h>

/* Maximum message size */
#define MQ_MAX_MSG_SIZE  256
/* Maximum number of messages per queue */
#define MQ_MAX_MSGS      64
/* Maximum total queues */
#define MQ_MAX_QUEUES    16

struct mq_msg {
    u32   len;
    u8    data[MQ_MAX_MSG_SIZE];
};

struct mqueue {
    char   name[64];
    int    used;
    int    refcount;

    /* Circular buffer */
    struct mq_msg msgs[MQ_MAX_MSGS];
    u32    head;  /* read index */
    u32    tail;  /* write index */
    u32    count; /* number of messages */

    /* Blocking support */
    usize  readers_waiting;
    usize  writers_waiting;
};

/* ── Kernel API ── */
void       mqueue_init(void);
struct mqueue *mqueue_find(const char *name);
struct mqueue *mqueue_create(const char *name);
int        mqueue_send(struct mqueue *mq, const void *data, u32 len);
int        mqueue_receive(struct mqueue *mq, void *data, u32 *len);
void       mqueue_close(struct mqueue *mq);
int        mqueue_unlink(const char *name);

#endif /* B1NIX_MQUEUE_H */
