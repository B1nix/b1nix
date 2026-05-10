#include <b1nix/vfs.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <stdlib.h>
#include <string.h>

/* Simple object pool for VFS structures to avoid fragmentation */
struct vfs_pool {
    void *free_list;
    usize obj_size;
    volatile int lock;
};

static struct vfs_pool node_pool = { .obj_size = sizeof(struct vfs_node) };
static struct vfs_pool inode_pool = { .obj_size = sizeof(struct vfs_inode) };
static struct vfs_pool handle_pool = { .obj_size = sizeof(struct vfs_handle) };

static void *pool_alloc(struct vfs_pool *pool) {
    while (__atomic_test_and_set(&pool->lock, __ATOMIC_ACQUIRE)) scheduler_yield();
    if (pool->free_list) {
        void *obj = pool->free_list;
        pool->free_list = *(void **)obj;
        __atomic_clear(&pool->lock, __ATOMIC_RELEASE);
        memset(obj, 0, pool->obj_size);
        return obj;
    }
    __atomic_clear(&pool->lock, __ATOMIC_RELEASE);
    
    /* Fallback to heap if pool is empty */
    return kzalloc(pool->obj_size);
}

static void pool_free(struct vfs_pool *pool, void *obj) {
    if (!obj) return;
    while (__atomic_test_and_set(&pool->lock, __ATOMIC_ACQUIRE)) scheduler_yield();
    *(void **)obj = pool->free_list;
    pool->free_list = obj;
    __atomic_clear(&pool->lock, __ATOMIC_RELEASE);
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
