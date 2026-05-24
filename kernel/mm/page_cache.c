#include <b1nix/page_cache.h>
#include <b1nix/vfs.h>
#include <b1nix/mm.h>
#include <b1nix/errno.h>
#include <string.h>

#define PC_HASH_SIZE 1024

static struct page_cache_entry *hash_table[PC_HASH_SIZE];
static struct page_cache_entry *lru_head;
static struct page_cache_entry *lru_tail;
static volatile int pc_lock = 0;

static void lock_pc(void) {
  while (__sync_lock_test_and_set(&pc_lock, 1)) {
    while (pc_lock) { __asm__ volatile("pause"); }
  }
}

static void unlock_pc(void) {
  __sync_lock_release(&pc_lock);
}

static u32 pc_hash(struct vfs_inode *inode, u64 offset) {
  u64 val = (u64)(usize)inode ^ (offset >> 12);
  val ^= val >> 16;
  return (u32)(val % PC_HASH_SIZE);
}

static void lru_remove(struct page_cache_entry *page) {
  if (page->lru_prev) page->lru_prev->lru_next = page->lru_next;
  else if (lru_head == page) lru_head = page->lru_next;

  if (page->lru_next) page->lru_next->lru_prev = page->lru_prev;
  else if (lru_tail == page) lru_tail = page->lru_prev;

  page->lru_next = page->lru_prev = 0;
}

static void lru_append(struct page_cache_entry *page) {
  page->lru_next = 0;
  page->lru_prev = lru_tail;
  if (lru_tail) lru_tail->lru_next = page;
  else lru_head = page;
  lru_tail = page;
}

void page_cache_init(void) {
  memset(hash_table, 0, sizeof(hash_table));
  lru_head = lru_tail = 0;
  pc_lock = 0;
}

struct page_cache_entry *page_cache_get_page(struct vfs_inode *inode, u64 offset) {
  u32 h = pc_hash(inode, offset);
  
  lock_pc();
  struct page_cache_entry *curr = hash_table[h];
  while (curr) {
    if (curr->inode == inode && curr->offset == offset) {
      curr->refcount++;
      // Move to end of LRU (most recently used)
      lru_remove(curr);
      lru_append(curr);
      unlock_pc();
      return curr;
    }
    curr = curr->hash_next;
  }
  unlock_pc();
  return 0;
}

int page_cache_add_page(struct vfs_inode *inode, u64 offset, u64 frame) {
  u32 h = pc_hash(inode, offset);
  
  struct page_cache_entry *new_entry = kmalloc(sizeof(struct page_cache_entry));
  if (!new_entry) return -ENOMEM;
  
  new_entry->inode = inode;
  new_entry->offset = offset;
  new_entry->frame = frame;
  new_entry->flags = PAGE_CACHE_UPTODATE;
  new_entry->refcount = 0; // Starts at 0, incremented by get_page if needed
  
  lock_pc();
  // Check if it was added concurrently
  struct page_cache_entry *curr = hash_table[h];
  while (curr) {
    if (curr->inode == inode && curr->offset == offset) {
      unlock_pc();
      kfree(new_entry);
      return -EEXIST;
    }
    curr = curr->hash_next;
  }
  
  new_entry->hash_next = hash_table[h];
  hash_table[h] = new_entry;
  
  lru_append(new_entry);
  unlock_pc();
  
  return 0;
}

void page_cache_mark_dirty(struct page_cache_entry *page) {
  lock_pc();
  page->flags |= PAGE_CACHE_DIRTY;
  unlock_pc();
}

static void writeback_page_locked(struct page_cache_entry *page) {
  if ((page->flags & PAGE_CACHE_DIRTY) && page->inode && page->inode->write_cb) {
    struct vfs_node dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.inode = page->inode;
    
    usize size = PAGE_SIZE;
    if (page->offset + PAGE_SIZE > page->inode->size) {
      if (page->offset >= page->inode->size) {
        size = 0;
      } else {
        size = page->inode->size - page->offset;
      }
    }
    
    if (size > 0) {
      unlock_pc();
      void *virt_addr = (void *)(usize)(page->frame + vmm_direct_map_base());
      page->inode->write_cb(&dummy, page->offset, virt_addr, size, 0);
      lock_pc();
    }
    
    page->flags &= ~PAGE_CACHE_DIRTY;
  }
}

int page_cache_flush_inode(struct vfs_inode *inode) {
  if (!inode) return -1;
  u64 curr_offset = 0;
  while (curr_offset < inode->size) {
    struct page_cache_entry *page = page_cache_get_page(inode, curr_offset);
    if (page) {
      if (page->flags & PAGE_CACHE_DIRTY) {
        lock_pc();
        writeback_page_locked(page);
        unlock_pc();
      }
      page_cache_put_page(page);
    }
    curr_offset += PAGE_SIZE;
  }
  return 0;
}

void page_cache_put_page(struct page_cache_entry *page) {
  lock_pc();
  if (page->refcount > 0) {
    page->refcount--;
  }
  unlock_pc();
}

void page_cache_evict(void) {
  lock_pc();
  
  struct page_cache_entry *curr = lru_head;
  int evicted = 0;
  
  while (curr && evicted < 16) { // Try to evict up to 16 pages
    struct page_cache_entry *next = curr->lru_next;
    
    if (curr->refcount == 0) {
      if (curr->flags & PAGE_CACHE_DIRTY) {
        writeback_page_locked(curr);
      }
      
      // Remove from hash table
      u32 h = pc_hash(curr->inode, curr->offset);
      struct page_cache_entry **prev = &hash_table[h];
      struct page_cache_entry *hcurr = *prev;
      while (hcurr) {
        if (hcurr == curr) {
          *prev = hcurr->hash_next;
          break;
        }
        prev = &hcurr->hash_next;
        hcurr = hcurr->hash_next;
      }
      
      lru_remove(curr);
      
      // Free frame and entry
      pmm_free_frame(curr->frame);
      kfree(curr);
      evicted++;
    }
    
    curr = next;
  }
  
  unlock_pc();
}
