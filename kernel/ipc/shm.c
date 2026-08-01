#include <string.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/resource_caps.h>
#include <b1nix/sched.h>
#include <b1nix/shm.h>
#include <b1nix/spinlock.h>
#include <b1nix/uidgid.h>
#include <b1nix/panic.h>

/* Global lock for the shm_segments[] and proc_attaches[] tables. Held only
 * around bookkeeping (segment/slot allocation, shm_nattch, the attach table);
 * the per-process page-table work (vm_find_free_area / vmm_map_page /
 * vmm_unmap_page + TLB shootdown) and serial logging are done OUTSIDE the lock
 * so a shootdown round-trip or a slow UART never runs with the lock held.
 * A segment reserved with shm_nattch>0 cannot be IPC_RMID'd, which is what
 * lets shmat map its pages safely after dropping the lock. */
static spinlock_t shm_lock = SPINLOCK_INIT;

/* POSIX: only the segment's owner/creator or the superuser may IPC_RMID or
 * IPC_SET a segment. Returns 1 if the caller is permitted. */
static int shm_caller_may_control(const struct shm_segment *seg) {
    const struct cred *c = scheduler_get_current_cred();
    if (!c)
        return 1; /* no creds (early boot / kernel) — allow */
    if (c->euid == 0)
        return 1; /* root */
    return c->euid == seg->ds.shm_perm.uid || c->euid == seg->ds.shm_perm.cuid;
}

/* ── Global shared memory segments ── */

static struct shm_segment shm_segments[SHMMNI];
static u16 shm_seq = 0;

/* ── Attachments per process ── */

struct proc_attachments {
    usize pid;
    struct shm_attach attaches[SHM_MAX_ATTACH_PER_PROC];
};

#define MAX_PROC_ATTACH 32
static struct proc_attachments proc_attaches[MAX_PROC_ATTACH];

/* ── Forward declarations ── */

static struct proc_attachments *find_or_create_proc_attaches(usize pid);
static struct proc_attachments *find_proc_attaches(usize pid);
static int find_free_shmid(void);
static int find_proc_attach_slot(struct proc_attachments *pa);

/* ── Initialization ── */

void shm_init(void)
{
    memset(shm_segments, 0, sizeof(shm_segments));
    memset(proc_attaches, 0, sizeof(proc_attaches));
    console_write("shm: initialized (max ");
    console_write_dec(SHMMNI);
    console_write(" segments, ");
    console_write_dec(g_resource_caps.shmmax_bytes / 1024);
    console_write(" KB max size)\n");
}

/* ── Helpers (all callers hold shm_lock) ── */

static int find_free_shmid(void)
{
    for (int i = 0; i < SHMMNI; i++) {
        if (!shm_segments[i].used) return i;
    }
    return -1;
}

static struct proc_attachments *find_proc_attaches(usize pid)
{
    for (int i = 0; i < MAX_PROC_ATTACH; i++) {
        if (proc_attaches[i].pid == pid) return &proc_attaches[i];
    }
    return 0;
}

static struct proc_attachments *find_or_create_proc_attaches(usize pid)
{
    /* Find existing */
    for (int i = 0; i < MAX_PROC_ATTACH; i++) {
        if (proc_attaches[i].pid == pid) return &proc_attaches[i];
    }
    /* Find free slot */
    for (int i = 0; i < MAX_PROC_ATTACH; i++) {
        if (proc_attaches[i].pid == 0) {
            proc_attaches[i].pid = pid;
            memset(proc_attaches[i].attaches, 0, sizeof(proc_attaches[i].attaches));
            return &proc_attaches[i];
        }
    }
    return 0;
}

static int find_proc_attach_slot(struct proc_attachments *pa)
{
    for (int i = 0; i < SHM_MAX_ATTACH_PER_PROC; i++) {
        if (!pa->attaches[i].used) return i;
    }
    return -1;
}

/* ── shmget: Create or find a shared memory segment ── */

int shmget(u32 key, usize size, int shmflg)
{
    if (size < SHMMIN) size = SHMMIN;
    if (size > g_resource_caps.shmmax_bytes) return -1;

    int create = (shmflg & IPC_CREAT) != 0;
    int excl   = (shmflg & IPC_EXCL) != 0;

    int npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > (int)(g_resource_caps.shmmax_bytes / PAGE_SIZE)) return -1;

    usize pid = scheduler_get_pid();

    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);

    /* First, look for existing segment with this key */
    for (int i = 0; i < SHMMNI; i++) {
        if (shm_segments[i].used && shm_segments[i].key == key) {
            spin_unlock_irqrestore(&shm_lock, flags);
            if (create && excl) return -1; /* IPC_EXCL and exists */
            return i;                      /* Return existing shmid */
        }
    }

    if (!create) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -1; /* Doesn't exist and IPC_CREAT not specified */
    }

    /* Create new segment */
    int shmid = find_free_shmid();
    if (shmid < 0) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -1;
    }

    struct shm_segment *seg = &shm_segments[shmid];
    memset(seg, 0, sizeof(*seg));
    seg->used = 1;
    seg->key = key;

    /* Allocate the page-frame table up front (M77: segment size is a runtime
     * cap now, so the table is per-segment heap, not a fixed array). */
    seg->physical_pages = kmalloc((usize)npages * sizeof(u64));
    if (!seg->physical_pages) {
        seg->used = 0;
        spin_unlock_irqrestore(&shm_lock, flags);
        return -1;
    }

    /* Allocate physical pages. ponytail: pmm_alloc runs under shm_lock — fine
     * for typical small segments; a multi-MiB segment holds the lock across a
     * long alloc loop. Split with a not-ready flag if that ever matters. */
    for (int p = 0; p < npages; p++) {
        seg->physical_pages[p] = pmm_alloc_frame();
        if (seg->physical_pages[p] == 0) {
            for (int q = 0; q < p; q++) {
                pmm_free_frame(seg->physical_pages[q]);
            }
            kfree(seg->physical_pages);
            seg->physical_pages = 0;
            seg->used = 0;
            spin_unlock_irqrestore(&shm_lock, flags);
            return -1;
        }
    }

    /* Fill in shmid_ds */
    seg->ds.shm_perm.key = key;
    seg->ds.shm_perm.uid = 0;    /* root */
    seg->ds.shm_perm.gid = 0;    /* root */
    seg->ds.shm_perm.cuid = pid;
    seg->ds.shm_perm.cgid = 0;
    seg->ds.shm_perm.mode = SHM_R | SHM_W;
    seg->ds.shm_perm.seq = shm_seq++;
    seg->ds.shm_segsz = size;
    seg->ds.shm_atime = 0;
    seg->ds.shm_dtime = 0;
    seg->ds.shm_ctime = 0;
    seg->ds.shm_cpid = (u16)pid;
    seg->ds.shm_lpid = (u16)pid;
    seg->ds.shm_nattch = 0;
    seg->ds.shm_npages = npages;
    seg->page_count = npages;

    spin_unlock_irqrestore(&shm_lock, flags);

    console_write("shm: created segment id=");
    console_write_dec(shmid);
    console_write(" key=0x");
    console_write_hex32(key);
    console_write(" size=");
    console_write_dec(size);
    console_write(" pages=");
    console_write_dec(npages);
    console_write("\n");

    return shmid;
}

/* ── shmat: Attach shared memory segment ── */

void *shmat(int shmid, const void *shmaddr, int shmflg)
{
    (void)shmaddr; /* We ignore requested addr for simplicity (SHM_RND moot) */

    if (shmid < 0 || shmid >= SHMMNI) return (void *)-1;

    usize pid = scheduler_get_pid();

    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);

    struct shm_segment *seg = &shm_segments[shmid];
    if (!seg->used) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return (void *)-1;
    }

    struct proc_attachments *pa = find_or_create_proc_attaches(pid);
    if (!pa) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return (void *)-1;
    }

    int slot = find_proc_attach_slot(pa);
    if (slot < 0) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return (void *)-1; /* Too many attachments */
    }

    /* Reserve the slot and bump shm_nattch BEFORE dropping the lock: while
     * shm_nattch>0 the segment cannot be IPC_RMID'd, so its pages stay valid
     * while we map them below without the lock. */
    int npages = seg->page_count;
    pa->attaches[slot].used = 1;
    pa->attaches[slot].shmid = shmid;
    pa->attaches[slot].virtual_addr = 0; /* finalized after the mapping */
    seg->ds.shm_nattch++;
    seg->ds.shm_lpid = (u16)pid;

    spin_unlock_irqrestore(&shm_lock, flags);

    /* Find a free virtual address for mapping (per-process, no shm_lock). */
    u64 vaddr = vm_find_free_area(current_task, (u64)npages * PAGE_SIZE);
    if (vaddr == (u64)-1)
        goto fail_unreserve;

    /* SHM_RDONLY: map without VMM_WRITABLE so writes fault (POSIX read-only
     * attach), instead of always mapping writable. */
    u64 map_flags = VMM_USER | VMM_SHARED | VMM_PRESENT;
    if (!(shmflg & SHM_RDONLY))
        map_flags |= VMM_WRITABLE;

    /* Map all pages into user virtual space. seg->physical_pages is stable —
     * the reserved shm_nattch keeps the segment alive. */
    for (int p = 0; p < npages; p++) {
        u64 page_vaddr = vaddr + (u64)p * PAGE_SIZE;
        /* VMM_SHARED bypasses CoW on fork. */
        vmm_map_page(page_vaddr, seg->physical_pages[p], map_flags);
        /* Explicitly increment physical frame refcount for this new mapping */
        pmm_ref_frame(seg->physical_pages[p]);
    }

    /* Create VMA for this region */
    struct vm_area *vma = kmalloc(sizeof(struct vm_area));
    if (!vma) {
        for (int p = 0; p < npages; p++)
            vmm_unmap_page(vaddr + (u64)p * PAGE_SIZE);
        goto fail_unreserve;
    }
    vma->start = vaddr;
    vma->end = vaddr + (u64)npages * PAGE_SIZE;
    vma->prot = (shmflg & SHM_RDONLY) ? PROT_READ : (PROT_READ | PROT_WRITE);
    vma->flags = MAP_SHARED;
    vma->node = 0;
    vma->offset = 0;

    /* Insert VMA into current_task */
    struct vm_area **prev = &current_task->vma_list;
    struct vm_area *curr = current_task->vma_list;
    while (curr && curr->start < vaddr) {
        prev = &curr->next;
        curr = curr->next;
    }
    vma->next = curr;
    *prev = vma;

    /* Finalize the attach record under the lock. */
    spin_lock_irqsave(&shm_lock, &flags);
    pa->attaches[slot].virtual_addr = vaddr;
    seg->ds.shm_atime = 0; /* Would use a timestamp */
    spin_unlock_irqrestore(&shm_lock, flags);

    console_write("shm: attached id=");
    console_write_dec(shmid);
    console_write(" @ 0x");
    console_write_hex64(vaddr);
    console_write("\n");

    return (void *)(usize)vaddr;

fail_unreserve:
    /* Roll back the reservation made above. */
    spin_lock_irqsave(&shm_lock, &flags);
    pa->attaches[slot].used = 0;
    pa->attaches[slot].shmid = 0;
    if (seg->ds.shm_nattch > 0)
        seg->ds.shm_nattch--;
    spin_unlock_irqrestore(&shm_lock, flags);
    return (void *)-1;
}

/* ── shmdt: Detach shared memory segment ── */

int shmdt(const void *shmaddr)
{
    usize pid = scheduler_get_pid();

    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);

    struct proc_attachments *pa = find_proc_attaches(pid);
    if (!pa) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -1;
    }

    int shmid = -1;
    u64 vaddr = 0, size = 0;
    for (int i = 0; i < SHM_MAX_ATTACH_PER_PROC; i++) {
        if (pa->attaches[i].used &&
            pa->attaches[i].virtual_addr == (u64)(usize)shmaddr) {
            shmid = pa->attaches[i].shmid;
            struct shm_segment *seg = &shm_segments[shmid];
            vaddr = pa->attaches[i].virtual_addr;
            size = (u64)seg->page_count * PAGE_SIZE;

            /* Release the bookkeeping under the lock; the unmap happens after. */
            pa->attaches[i].used = 0;
            pa->attaches[i].shmid = 0;
            pa->attaches[i].virtual_addr = 0;
            if (seg->ds.shm_nattch > 0)
                seg->ds.shm_nattch--;
            seg->ds.shm_dtime = 0;
            break;
        }
    }

    spin_unlock_irqrestore(&shm_lock, flags);

    if (shmid < 0)
        return -1; /* not attached at that address */

    /* Unmap pages (drops the per-mapping frame ref) and delete the VMA —
     * done outside the lock so the TLB shootdown does not run with it held. */
    for (u64 v = vaddr; v < vaddr + size; v += PAGE_SIZE)
        vmm_unmap_page(v);
    vma_delete_range(current_task, vaddr, vaddr + size);

    console_write("shm: detached id=");
    console_write_dec(shmid);
    console_write("\n");

    return 0;
}

/* ── shmctl: Shared memory control ── */

int shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
    if (shmid < 0 || shmid >= SHMMNI) return -1;

    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);

    struct shm_segment *seg = &shm_segments[shmid];
    if (!seg->used) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -1;
    }

    int rc = -1;
    int removed = 0;

    switch (cmd) {
    case IPC_RMID:
        /* Only the owner/creator or root may remove the segment (POSIX). */
        if (!shm_caller_may_control(seg)) break;
        /* Remove segment if no attachments */
        if (seg->ds.shm_nattch > 0) break;

        /* Free all physical pages (pmm_free has no TLB shootdown). */
        for (int p = 0; p < seg->page_count; p++) {
            pmm_free_frame(seg->physical_pages[p]);
            seg->physical_pages[p] = 0;
        }
        kfree(seg->physical_pages);
        seg->physical_pages = 0;
        memset(seg, 0, sizeof(*seg));
        removed = 1;
        rc = 0;
        break;

    case IPC_STAT:
        if (buf) {
            *buf = seg->ds;
            rc = 0;
        }
        break;

    case IPC_SET:
        /* Only the owner/creator or root may change ownership/permissions. */
        if (!shm_caller_may_control(seg)) break;
        if (buf) {
            seg->ds.shm_perm.uid = buf->shm_perm.uid;
            seg->ds.shm_perm.gid = buf->shm_perm.gid;
            seg->ds.shm_perm.mode = buf->shm_perm.mode;
            seg->ds.shm_ctime = 0;
            rc = 0;
        }
        break;

    default:
        break;
    }

    spin_unlock_irqrestore(&shm_lock, flags);

    if (removed) {
        console_write("shm: removed id=");
        console_write_dec(shmid);
        console_write("\n");
    }
    return rc;
}

/* ── Get process attachments (legacy accessor; caller does not hold the lock) ── */

struct shm_attach *shm_get_process_attaches(usize pid)
{
    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);
    struct proc_attachments *pa = find_proc_attaches(pid);
    struct shm_attach *res = pa ? pa->attaches : 0;
    spin_unlock_irqrestore(&shm_lock, flags);
    return res;
}

/* ── Account a process's attachments at address-space teardown ──
 *
 * Called from user_address_space_cleanup() — the single chokepoint that
 * tears down a user address space on voluntary exit, signal kill (OOM) and
 * execve. The teardown itself unmaps the VMM_SHARED pages and drops the
 * refcounted frames, so this only undoes the BOOKKEEPING: decrement
 * shm_nattch for every still-attached segment and free the per-process slot.
 * It touches no page tables, so it is safe in the reaper's context (the dying
 * task's address space, not current_task's). Idempotent: a second call for a
 * pid whose slot is already freed is a no-op. */
void shm_account_exit(usize pid)
{
    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);

    struct proc_attachments *pa = find_proc_attaches(pid);
    if (pa) {
        for (int i = 0; i < SHM_MAX_ATTACH_PER_PROC; i++) {
            if (!pa->attaches[i].used) continue;
            int shmid = pa->attaches[i].shmid;
            if (shmid >= 0 && shmid < SHMMNI && shm_segments[shmid].used &&
                shm_segments[shmid].ds.shm_nattch > 0)
                shm_segments[shmid].ds.shm_nattch--;
        }
        pa->pid = 0;
        memset(pa->attaches, 0, sizeof(pa->attaches));
    }

    spin_unlock_irqrestore(&shm_lock, flags);
}

/* ── Account inherited attachments across fork ── */

void shm_fork_inherit(usize parent_pid, usize child_pid)
{
    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);

    struct proc_attachments *ppa = find_proc_attaches(parent_pid);
    if (!ppa) {
        spin_unlock_irqrestore(&shm_lock, flags); /* parent had no attachments */
        return;
    }

    /* fork clones the address space 1:1, so the child's shm vaddrs match the
     * parent's. Mirror each attach into a child slot and count it in
     * shm_nattch so IPC_RMID stays blocked until the child also detaches. */
    struct proc_attachments *cpa = 0;
    for (int i = 0; i < SHM_MAX_ATTACH_PER_PROC; i++) {
        if (!ppa->attaches[i].used) continue;

        if (!cpa) {
            cpa = find_or_create_proc_attaches(child_pid);
            if (!cpa) break; /* attach-table full — best effort */
        }
        int slot = find_proc_attach_slot(cpa);
        if (slot < 0) break;

        int shmid = ppa->attaches[i].shmid;
        cpa->attaches[slot].used = 1;
        cpa->attaches[slot].shmid = shmid;
        cpa->attaches[slot].virtual_addr = ppa->attaches[i].virtual_addr;

        if (shmid >= 0 && shmid < SHMMNI && shm_segments[shmid].used)
            shm_segments[shmid].ds.shm_nattch++;
    }

    spin_unlock_irqrestore(&shm_lock, flags);
}
