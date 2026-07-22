#ifndef B1NIX_PAGE_CACHE_H
#define B1NIX_PAGE_CACHE_H

#include <b1nix/types.h>

struct vfs_inode;

#define PAGE_CACHE_UPTODATE 1
#define PAGE_CACHE_DIRTY    2
/* Variant E — the entry is on the ACTIVE LRU list (referenced more than once /
 * refaulted). Active pages are the protected working set: eviction drains the
 * inactive list first and only demotes active pages when inactive is empty. */
#define PAGE_CACHE_ACTIVE   4
/* Entry's inode was destroyed while a reader still held a reference. The entry
 * is already unlinked from the hash and LRU; the last page_cache_put_page frees
 * its frame and defers the entry itself. */
#define PAGE_CACHE_ORPHAN   8

struct page_cache_entry {
  struct vfs_inode *inode;
  u64 key_ino; // Cache key: vfs inode->ino (unique, never recycled — unlike the
               // inode POINTER, which the slab pool reuses; keying by pointer
               // let a recycled inode inherit a previous file's cached pages)
  u64 offset; // Page-aligned offset
  u64 frame;  // Physical frame
  u32 flags;
  int refcount;
  
  struct page_cache_entry *hash_next;
  struct page_cache_entry *lru_next;
  struct page_cache_entry *lru_prev;
};

void page_cache_init(void);

// Returns a referenced page cache entry. The caller must unref it when done.
struct page_cache_entry *page_cache_get_page(struct vfs_inode *inode, u64 offset);

// Adds a new page cache entry. Takes ownership of the frame if successful.
// Returns 0 on success, < 0 on error.
int page_cache_add_page(struct vfs_inode *inode, u64 offset, u64 frame);

// Marks the page as dirty, meaning it needs to be written to disk.
void page_cache_mark_dirty(struct page_cache_entry *page);

// Write back all dirty pages for the given inode.
int page_cache_flush_inode(struct vfs_inode *inode);

// Drop cached pages for an inode that is being destroyed.
void page_cache_invalidate_inode(struct vfs_inode *inode);

// Truncate-time invalidation: drop pages at/after new_size and zero the tail
// of the partial page so a later re-grow reads zeros, not stale contents.
void page_cache_truncate_inode(struct vfs_inode *inode, u64 new_size);

// Decrease refcount. If 0, page is eligible for eviction (stays in cache).
void page_cache_put_page(struct page_cache_entry *page);

// Evicts up to target_pages unused pages, writing dirty ones back first.
usize page_cache_evict(usize target_pages);

// Evicts up to target_pages CLEAN unused pages only — never writes back. Cheap
// reclaim for hot paths (the proactive cap) that must not drive AHCI writeback.
usize page_cache_evict_clean(usize target_pages);

#endif
