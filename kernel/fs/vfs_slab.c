#include <b1nix/vfs.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <stdlib.h>
#include <string.h>

/* Object pool for VFS structures — non-intrusive free list.
 * Uses a separate pointer array so use-after-free of a pooled object
 * cannot corrupt the pool itself (unlike an intrusive linked-list pool
 * that writes into the freed object's memory). */
#define POOL_FREE_MAX 64

struct vfs_pool {
    void *free_list[POOL_FREE_MAX];
    int free_count;
    usize obj_size;
    spinlock_t lock;
};

static struct vfs_pool node_pool = { .obj_size = sizeof(struct vfs_node), .lock = SPINLOCK_INIT };
static struct vfs_pool inode_pool = { .obj_size = sizeof(struct vfs_inode), .lock = SPINLOCK_INIT };
static struct vfs_pool handle_pool = { .obj_size = sizeof(struct vfs_handle), .lock = SPINLOCK_INIT };

static void *pool_alloc(struct vfs_pool *pool) {
    u64 flags;
    spin_lock_irqsave(&pool->lock, &flags);
    if (pool->free_count > 0) {
        void *obj = pool->free_list[--pool->free_count];
        pool->free_list[pool->free_count] = NULL;
        spin_unlock_irqrestore(&pool->lock, flags);
        memset(obj, 0, pool->obj_size);
        return obj;
    }
    spin_unlock_irqrestore(&pool->lock, flags);
    return kzalloc(pool->obj_size);
}

static void pool_free(struct vfs_pool *pool, void *obj) {
    if (!obj) return;
    u64 flags;
    spin_lock_irqsave(&pool->lock, &flags);
    if (pool->free_count < POOL_FREE_MAX) {
        pool->free_list[pool->free_count++] = obj;
        spin_unlock_irqrestore(&pool->lock, flags);
    } else {
        spin_unlock_irqrestore(&pool->lock, flags);
        kfree(obj);
    }
}

struct vfs_node *vfs_alloc_node(void) {
    return pool_alloc(&node_pool);
}

void vfs_free_node(struct vfs_node *node) {
    pool_free(&node_pool, node);
}

static u64 next_ino = 1;

struct vfs_inode *vfs_alloc_inode(void) {
    struct vfs_inode *inode = pool_alloc(&inode_pool);
    if (inode) inode->ino = __atomic_fetch_add(&next_ino, 1, __ATOMIC_RELAXED);
    return inode;
}

void vfs_free_inode(struct vfs_inode *inode) {
    pool_free(&inode_pool, inode);
}

struct vfs_handle *vfs_alloc_handle(void) {
    return pool_alloc(&handle_pool);
}

void vfs_free_handle(struct vfs_handle *handle) {
    pool_free(&handle_pool, handle);
}
