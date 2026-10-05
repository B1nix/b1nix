/* SPDX-License-Identifier: GPL-2.0-only */
#include <b1nix/vfs.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/klog.h>
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

/* A pooled object is filled with this while it waits for reuse, and checked
 * when it is handed out again: a byte that changed is a write through a
 * pointer somebody kept after freeing it. */
#define POOL_JUNK 0x6b

static void pool_check_junk(struct vfs_pool *pool, void *obj) {
    const u8 *b = obj;

    for (usize i = 0; i < pool->obj_size; i++)
        KASSERT(b[i] == POOL_JUNK,
                "vfs pool object %p (size %lu) written at +%lu after it was "
                "freed: 0x%02x", obj, (unsigned long)pool->obj_size,
                (unsigned long)i, b[i]);
}

static void *pool_alloc(struct vfs_pool *pool) {
    u64 flags;
    spin_lock_irqsave(&pool->lock, &flags);
    if (pool->free_count > 0) {
        void *obj = pool->free_list[--pool->free_count];
        pool->free_list[pool->free_count] = NULL;
        spin_unlock_irqrestore(&pool->lock, flags);
        pool_check_junk(pool, obj);
        memset(obj, 0, pool->obj_size);
        return obj;
    }
    spin_unlock_irqrestore(&pool->lock, flags);
    return kzalloc(pool->obj_size);
}

static void pool_free(struct vfs_pool *pool, void *obj) {
    if (!obj) return;
    u64 flags;
    /* Junk before it is published: the object is nobody's from here on. */
    memset(obj, POOL_JUNK, pool->obj_size);
    spin_lock_irqsave(&pool->lock, &flags);
    /* The same object twice in the pool would be handed to two owners. */
    for (int i = 0; i < pool->free_count; i++)
        KASSERT(pool->free_list[i] != obj, "vfs pool object %p freed twice",
                obj);
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
/* Every freshly allocated inode gets a distinct generation, so a file handle
 * stored for one file cannot match a later file that ends up on the same inode
 * number (filesystems with an on-disk generation overwrite this with theirs). */
static u32 next_generation = 1;

struct vfs_inode *vfs_alloc_inode(void) {
    struct vfs_inode *inode = pool_alloc(&inode_pool);
    if (inode) {
        inode->ino = __atomic_fetch_add(&next_ino, 1, __ATOMIC_RELAXED);
        inode->generation =
            __atomic_fetch_add(&next_generation, 1, __ATOMIC_RELAXED);
    }
    return inode;
}

/* Give an inode a new identity because the file that lived on it is gone and
 * something else is taking its place (a create that reuses a node). */
void vfs_inode_new_generation(struct vfs_inode *inode) {
    if (inode)
        inode->generation =
            __atomic_fetch_add(&next_generation, 1, __ATOMIC_RELAXED);
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
