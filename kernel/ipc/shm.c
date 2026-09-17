/* System V shared memory — shmget(2), shmat(2), shmdt(2), shmctl(2).
 *
 * A segment is a set of physical frames every attach maps. It belongs to the
 * IPC namespace it was created in (a key or an id finds only segments of the
 * caller's namespace) and carries SysV permissions, checked on every get,
 * attach and stat.
 *
 * IPC_RMID on a segment that is still attached does not free it: the segment
 * is marked SHM_DEST, loses its key (so no new shmget finds it) and goes away
 * at its last detach — which is what every program that removes a segment
 * right after attaching it relies on.
 */

#include <string.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/namespace.h>
#include <b1nix/resource_caps.h>
#include <b1nix/sched.h>
#include <b1nix/shm.h>
#include <b1nix/spinlock.h>
#include <b1nix/sysv_ipc.h>
#include <b1nix/uidgid.h>
#include <b1nix/panic.h>
#include <b1nix/vfs.h>

/* Global lock for the shm_segments[] and proc_attaches[] tables. Held only
 * around bookkeeping (segment/slot allocation, shm_nattch, the attach table);
 * the per-process page-table work (vm_find_free_area / vmm_map_page /
 * vmm_unmap_page + TLB shootdown) is done OUTSIDE the lock so a shootdown
 * round-trip never runs with the lock held. A segment with shm_nattch > 0 is
 * never freed, which is what lets shmat map its pages after dropping the lock. */
static spinlock_t shm_lock = SPINLOCK_INIT;

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

static struct proc_attachments *find_proc_attaches(usize pid)
{
    for (int i = 0; i < MAX_PROC_ATTACH; i++) {
        if (proc_attaches[i].pid == pid) return &proc_attaches[i];
    }
    return 0;
}

static struct proc_attachments *find_or_create_proc_attaches(usize pid)
{
    struct proc_attachments *pa = find_proc_attaches(pid);
    if (pa) return pa;
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

/* The segment `shmid` names for a caller in namespace `ns`, or NULL. */
static struct shm_segment *seg_lookup(int shmid, u32 ns)
{
    if (shmid < 0 || shmid >= SHMMNI) return 0;
    struct shm_segment *seg = &shm_segments[shmid];
    if (!seg->used || seg->ns != ns) return 0;
    return seg;
}

/* Take the segment out of the table. Its frames are handed back to the caller
 * to free outside the lock. Frames still mapped somewhere carry their own
 * references, so dropping the segment's is always safe. */
static u64 *seg_release(struct shm_segment *seg, int *npages)
{
    u64 *pages = seg->physical_pages;
    *npages = seg->page_count;
    memset(seg, 0, sizeof(*seg));
    return pages;
}

static void free_pages_array(u64 *pages, int npages)
{
    if (!pages) return;
    for (int p = 0; p < npages; p++)
        if (pages[p]) pmm_free_frame(pages[p]);
    kfree(pages);
}

/* A detach or an exit dropped an attach: a segment marked for destruction is
 * gone once nothing has it attached. Caller holds shm_lock; returns frames to
 * free after the unlock (or NULL). */
static u64 *seg_maybe_destroy(struct shm_segment *seg, int *npages)
{
    *npages = 0;
    if (!seg->used || seg->ds.shm_nattch != 0 ||
        !(seg->ds.shm_perm.mode & SHM_DEST))
        return 0;
    return seg_release(seg, npages);
}

/* ── shmget: Create or find a shared memory segment ── */

int shmget(u32 key, usize size, int shmflg)
{
    int create = (shmflg & IPC_CREAT) != 0;
    int excl   = (shmflg & IPC_EXCL) != 0;
    u32 ns = ipc_current_ns();
    const struct cred *c = scheduler_get_current_cred();

    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);

    /* IPC_PRIVATE (key 0) always makes a new segment. */
    if (key != 0) {
        for (int i = 0; i < SHMMNI; i++) {
            struct shm_segment *seg = &shm_segments[i];
            if (!seg->used || seg->ns != ns || seg->key != key ||
                (seg->ds.shm_perm.mode & SHM_DEST))
                continue;
            int rc = i;
            if (create && excl)
                rc = -EEXIST;
            else if (size > seg->ds.shm_segsz)
                rc = -EINVAL;
            else if (ipc_check_perm(&seg->ds.shm_perm, ns,
                                    (u16)(shmflg & 0777)) != 0)
                rc = -EACCES;
            spin_unlock_irqrestore(&shm_lock, flags);
            return rc;
        }
        if (!create) {
            spin_unlock_irqrestore(&shm_lock, flags);
            return -ENOENT;
        }
    }

    if (size < SHMMIN || size > g_resource_caps.shmmax_bytes) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -EINVAL;
    }
    int npages = (int)((size + PAGE_SIZE - 1) / PAGE_SIZE);

    int shmid = -1;
    for (int i = 0; i < SHMMNI; i++)
        if (!shm_segments[i].used) { shmid = i; break; }
    if (shmid < 0) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -ENOSPC;
    }

    struct shm_segment *seg = &shm_segments[shmid];
    memset(seg, 0, sizeof(*seg));
    seg->used = 1; /* claimed; the frames are allocated below */
    seg->key = key;
    seg->ns = ns;
    /* Hidden from lookups until it is complete. */
    seg->ds.shm_perm.mode = SHM_DEST;
    spin_unlock_irqrestore(&shm_lock, flags);

    u64 *pages = kzalloc((usize)npages * sizeof(u64));
    int ok = pages != 0;
    for (int p = 0; ok && p < npages; p++) {
        pages[p] = pmm_alloc_frame();
        if (!pages[p]) ok = 0;
    }
    if (!ok) {
        free_pages_array(pages, npages);
        spin_lock_irqsave(&shm_lock, &flags);
        memset(seg, 0, sizeof(*seg));
        spin_unlock_irqrestore(&shm_lock, flags);
        return -ENOMEM;
    }

    usize pid = scheduler_get_pid();
    spin_lock_irqsave(&shm_lock, &flags);
    seg->physical_pages = pages;
    seg->page_count = npages;
    seg->ds.shm_perm.key = key;
    seg->ds.shm_perm.uid = seg->ds.shm_perm.cuid = c ? c->euid : 0;
    seg->ds.shm_perm.gid = seg->ds.shm_perm.cgid = c ? c->egid : 0;
    seg->ds.shm_perm.seq = shm_seq++;
    seg->ds.shm_segsz = size;
    seg->ds.shm_ctime = vfs_get_unix_time();
    seg->ds.shm_cpid = pid;
    seg->ds.shm_lpid = 0;
    seg->ds.shm_nattch = 0;
    seg->ds.shm_npages = (u32)npages;
    seg->ds.shm_perm.mode = (u16)(shmflg & 0777); /* published */
    spin_unlock_irqrestore(&shm_lock, flags);
    return shmid;
}

/* ── shmat: Attach shared memory segment ── */

void *shmat(int shmid, const void *shmaddr, int shmflg)
{
    (void)shmaddr; /* the kernel picks the address */

    usize pid = scheduler_get_pid();
    u32 ns = ipc_current_ns();

    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);

    struct shm_segment *seg = seg_lookup(shmid, ns);
    if (!seg || !seg->physical_pages) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return (void *)(isize)-EINVAL;
    }
    u16 acc = (shmflg & SHM_RDONLY) ? 0400 : 0600;
    if (ipc_check_perm(&seg->ds.shm_perm, ns, acc) != 0) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return (void *)(isize)-EACCES;
    }

    struct proc_attachments *pa = find_or_create_proc_attaches(pid);
    int slot = pa ? find_proc_attach_slot(pa) : -1;
    if (slot < 0) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return (void *)(isize)-EMFILE;
    }

    /* Reserve the slot and bump shm_nattch BEFORE dropping the lock: while
     * shm_nattch > 0 the segment is never freed, so its pages stay valid while
     * we map them below without the lock. */
    int npages = seg->page_count;
    u64 *pages = seg->physical_pages;
    pa->attaches[slot].used = 1;
    pa->attaches[slot].shmid = shmid;
    pa->attaches[slot].virtual_addr = 0; /* finalized after the mapping */
    seg->ds.shm_nattch++;
    seg->ds.shm_lpid = pid;

    spin_unlock_irqrestore(&shm_lock, flags);

    u64 vaddr = vm_find_free_area(current_task, (u64)npages * PAGE_SIZE);
    if (vaddr == (u64)-1)
        goto fail_unreserve;

    u64 map_flags = VMM_USER | VMM_SHARED | VMM_PRESENT;
    if (!(shmflg & SHM_RDONLY))
        map_flags |= VMM_WRITABLE;

    for (int p = 0; p < npages; p++) {
        u64 page_vaddr = vaddr + (u64)p * PAGE_SIZE;
        /* VMM_SHARED bypasses CoW on fork. */
        vmm_map_page(page_vaddr, pages[p], map_flags);
        pmm_ref_frame(pages[p]);
    }

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
    vma->special = 0;
    vma->pkey = 0;
    vma_insert(current_task, vma);

    spin_lock_irqsave(&shm_lock, &flags);
    pa->attaches[slot].virtual_addr = vaddr;
    shm_segments[shmid].ds.shm_atime = vfs_get_unix_time();
    spin_unlock_irqrestore(&shm_lock, flags);
    return (void *)(usize)vaddr;

fail_unreserve:
    spin_lock_irqsave(&shm_lock, &flags);
    pa->attaches[slot].used = 0;
    pa->attaches[slot].shmid = 0;
    if (shm_segments[shmid].ds.shm_nattch > 0)
        shm_segments[shmid].ds.shm_nattch--;
    int dn;
    u64 *dead = seg_maybe_destroy(&shm_segments[shmid], &dn);
    spin_unlock_irqrestore(&shm_lock, flags);
    free_pages_array(dead, dn);
    return (void *)(isize)-ENOMEM;
}

/* ── shmdt: Detach shared memory segment ── */

int shmdt(const void *shmaddr)
{
    usize pid = scheduler_get_pid();

    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);

    struct proc_attachments *pa = find_proc_attaches(pid);
    int shmid = -1;
    u64 vaddr = 0, size = 0;
    u64 *dead = 0;
    int dn = 0;
    for (int i = 0; pa && i < SHM_MAX_ATTACH_PER_PROC; i++) {
        if (pa->attaches[i].used &&
            pa->attaches[i].virtual_addr == (u64)(usize)shmaddr) {
            shmid = pa->attaches[i].shmid;
            struct shm_segment *seg = &shm_segments[shmid];
            vaddr = pa->attaches[i].virtual_addr;
            size = (u64)seg->page_count * PAGE_SIZE;
            pa->attaches[i].used = 0;
            pa->attaches[i].shmid = 0;
            pa->attaches[i].virtual_addr = 0;
            if (seg->ds.shm_nattch > 0)
                seg->ds.shm_nattch--;
            seg->ds.shm_dtime = vfs_get_unix_time();
            seg->ds.shm_lpid = pid;
            dead = seg_maybe_destroy(seg, &dn);
            break;
        }
    }

    spin_unlock_irqrestore(&shm_lock, flags);

    if (shmid < 0)
        return -EINVAL; /* not attached at that address */

    /* Unmap (drops the per-mapping frame references) and delete the VMA —
     * outside the lock so the TLB shootdown does not run with it held. */
    for (u64 v = vaddr; v < vaddr + size; v += PAGE_SIZE)
        vmm_unmap_page(v);
    vma_delete_range(current_task, vaddr, vaddr + size);
    free_pages_array(dead, dn);
    return 0;
}

/* ── shmctl: Shared memory control ── */

#define SHM_LOCK   11
#define SHM_UNLOCK 12

int shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
    u32 ns = ipc_current_ns();
    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);

    struct shm_segment *seg = seg_lookup(shmid, ns);
    if (!seg || !seg->physical_pages) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -EINVAL;
    }

    int rc = -EINVAL;
    u64 *dead = 0;
    int dn = 0;

    switch (cmd) {
    case IPC_RMID:
        if (!ipc_may_control(&seg->ds.shm_perm, ns)) { rc = -EPERM; break; }
        /* Gone from the key space now; the frames go at the last detach. */
        seg->ds.shm_perm.mode |= SHM_DEST;
        seg->ds.shm_perm.key = 0;
        seg->key = 0;
        dead = seg_maybe_destroy(seg, &dn);
        rc = 0;
        break;

    case IPC_STAT:
        if (ipc_check_perm(&seg->ds.shm_perm, ns, 0400) != 0) { rc = -EACCES; break; }
        if (buf) {
            *buf = seg->ds;
            rc = 0;
        }
        break;

    case IPC_SET:
        if (!ipc_may_control(&seg->ds.shm_perm, ns)) { rc = -EPERM; break; }
        if (buf) {
            seg->ds.shm_perm.uid = buf->shm_perm.uid;
            seg->ds.shm_perm.gid = buf->shm_perm.gid;
            seg->ds.shm_perm.mode = (u16)((seg->ds.shm_perm.mode & ~0777) |
                                          (buf->shm_perm.mode & 0777));
            seg->ds.shm_ctime = vfs_get_unix_time();
            rc = 0;
        }
        break;

    case SHM_LOCK:
    case SHM_UNLOCK:
        /* The frames of a segment are never swapped out, so a locked segment
         * is what every segment already is; only the permission is checked. */
        rc = ipc_may_control(&seg->ds.shm_perm, ns) ? 0 : -EPERM;
        break;

    default:
        break;
    }

    spin_unlock_irqrestore(&shm_lock, flags);
    free_pages_array(dead, dn);
    return rc;
}

int shm_stat_index(int idx, struct shmid_ds *out)
{
    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);
    struct shm_segment *seg = seg_lookup(idx, ipc_current_ns());
    int rc = -EINVAL;
    if (seg && seg->physical_pages && out) {
        *out = seg->ds;
        rc = 0;
    }
    spin_unlock_irqrestore(&shm_lock, flags);
    return rc;
}

/* The namespace is gone, so nothing can find its segments again. An attached
 * one stays until its last detach, as after IPC_RMID. */
void shm_ns_destroy(u32 ns)
{
    for (int i = 0; i < SHMMNI; i++) {
        u64 flags;
        spin_lock_irqsave(&shm_lock, &flags);
        struct shm_segment *seg = &shm_segments[i];
        u64 *dead = 0;
        int dn = 0;
        if (seg->used && seg->ns == ns && seg->physical_pages) {
            seg->ds.shm_perm.mode |= SHM_DEST;
            seg->ds.shm_perm.key = 0;
            seg->key = 0;
            dead = seg_maybe_destroy(seg, &dn);
        }
        spin_unlock_irqrestore(&shm_lock, flags);
        free_pages_array(dead, dn);
    }
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
 * shm_nattch for every still-attached segment (destroying one marked for it)
 * and free the per-process slot. Idempotent. */
void shm_account_exit(usize pid)
{
    u64 *dead[SHM_MAX_ATTACH_PER_PROC];
    int dn[SHM_MAX_ATTACH_PER_PROC];
    int ndead = 0;
    u64 flags;
    spin_lock_irqsave(&shm_lock, &flags);

    struct proc_attachments *pa = find_proc_attaches(pid);
    if (pa) {
        for (int i = 0; i < SHM_MAX_ATTACH_PER_PROC; i++) {
            if (!pa->attaches[i].used) continue;
            int shmid = pa->attaches[i].shmid;
            if (shmid >= 0 && shmid < SHMMNI && shm_segments[shmid].used &&
                shm_segments[shmid].ds.shm_nattch > 0) {
                shm_segments[shmid].ds.shm_nattch--;
                int n = 0;
                u64 *d = seg_maybe_destroy(&shm_segments[shmid], &n);
                if (d) {
                    dead[ndead] = d;
                    dn[ndead++] = n;
                }
            }
        }
        pa->pid = 0;
        memset(pa->attaches, 0, sizeof(pa->attaches));
    }

    spin_unlock_irqrestore(&shm_lock, flags);
    for (int i = 0; i < ndead; i++)
        free_pages_array(dead[i], dn[i]);
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
     * shm_nattch so the segment stays until the child also detaches. */
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
