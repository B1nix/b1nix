#include <string.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/shm.h>
#include <b1nix/panic.h>

/* ── Global shared memory segments ── */

static struct shm_segment shm_segments[SHMMNI];
static u16 shm_seq = 0;

/* ── Attachments per process ── */

struct proc_attachments {
    usize pid;
    struct shm_attach attaches[SHM_MAX_ATTACH_PER_PROC];
};

#define MAX_PROC_ATTACH 16
static struct proc_attachments proc_attaches[MAX_PROC_ATTACH];

/* ── Forward declarations ── */

static struct proc_attachments *find_or_create_proc_attaches(usize pid);
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
    console_write_dec(SHMMAX / 1024);
    console_write(" KB max size)\n");
}

/* ── Helpers ── */

static int find_free_shmid(void)
{
    for (int i = 0; i < SHMMNI; i++) {
        if (!shm_segments[i].used) return i;
    }
    return -1;
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
    if (size > SHMMAX) return -1;

    int create = (shmflg & IPC_CREAT) != 0;
    int excl   = (shmflg & IPC_EXCL) != 0;

    /* First, look for existing segment with this key */
    for (int i = 0; i < SHMMNI; i++) {
        if (shm_segments[i].used && shm_segments[i].key == key) {
            if (create && excl) return -1; /* IPC_EXCL and exists */
            return i; /* Return existing shmid */
        }
    }

    if (!create) return -1; /* Doesn't exist and IPC_CREAT not specified */

    /* Create new segment */
    int shmid = find_free_shmid();
    if (shmid < 0) return -1;

    struct shm_segment *seg = &shm_segments[shmid];
    memset(seg, 0, sizeof(*seg));
    seg->used = 1;
    seg->key = key;

    usize pid = scheduler_get_pid();

    /* Calculate number of pages needed */
    int npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > (int)(SHMMAX / PAGE_SIZE)) {
        seg->used = 0;
        return -1;
    }

    /* Allocate physical pages */
    for (int p = 0; p < npages; p++) {
        seg->physical_pages[p] = pmm_alloc_frame();
        if (seg->physical_pages[p] == 0) {
            /* Free already allocated pages on failure */
            for (int q = 0; q < p; q++) {
                pmm_free_frame(seg->physical_pages[q]);
            }
            seg->used = 0;
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
    (void)shmaddr; /* We ignore requested addr for simplicity */
    (void)shmflg;  /* SHM_RDONLY/SHM_RND not yet honored */

    if (shmid < 0 || shmid >= SHMMNI) return (void *)-1;
    if (!shm_segments[shmid].used) return (void *)-1;

    struct shm_segment *seg = &shm_segments[shmid];
    usize pid = scheduler_get_pid();

    struct proc_attachments *pa = find_or_create_proc_attaches(pid);
    if (!pa) return (void *)-1;

    int slot = find_proc_attach_slot(pa);
    if (slot < 0) return (void *)-1; /* Too many attachments */

    /* Find a free virtual address for mapping */
    u64 vaddr = vm_find_free_area(current_task, seg->page_count * PAGE_SIZE);
    if (vaddr == (u64)-1) return (void *)-1;

    /* Map all pages into user virtual space */
    int npages = seg->page_count;
    for (int p = 0; p < npages; p++) {
        u64 page_vaddr = vaddr + p * PAGE_SIZE;
        /* Map with VMM_SHARED to bypass CoW on fork */
        vmm_map_page(page_vaddr, seg->physical_pages[p], VMM_WRITABLE | VMM_USER | VMM_SHARED | VMM_PRESENT);
        /* Explicitly increment physical frame refcount for this new mapping */
        pmm_ref_frame(seg->physical_pages[p]);
    }

    /* Create VMA for this region */
    struct vm_area *vma = kmalloc(sizeof(struct vm_area));
    if (!vma) {
        for (int p = 0; p < npages; p++) {
            vmm_unmap_page(vaddr + p * PAGE_SIZE);
        }
        return (void *)-1;
    }
    vma->start = vaddr;
    vma->end = vaddr + npages * PAGE_SIZE;
    vma->prot = PROT_READ | PROT_WRITE;
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

    /* Record attachment */
    pa->attaches[slot].used = 1;
    pa->attaches[slot].shmid = shmid;
    pa->attaches[slot].virtual_addr = vaddr;

    seg->ds.shm_nattch++;
    seg->ds.shm_atime = 0; /* Would use a timestamp */
    seg->ds.shm_lpid = (u16)pid;

    console_write("shm: attached id=");
    console_write_dec(shmid);
    console_write(" @ 0x");
    console_write_hex64(vaddr);
    console_write("\n");

    return (void *)(usize)vaddr;
}

/* ── shmdt: Detach shared memory segment ── */

int shmdt(const void *shmaddr)
{
    usize pid = scheduler_get_pid();
    struct proc_attachments *pa = find_or_create_proc_attaches(pid);
    if (!pa) return -1;

    for (int i = 0; i < SHM_MAX_ATTACH_PER_PROC; i++) {
        if (pa->attaches[i].used && 
            pa->attaches[i].virtual_addr == (u64)(usize)shmaddr) {
            
            int shmid = pa->attaches[i].shmid;
            struct shm_segment *seg = &shm_segments[shmid];
            
            u64 vaddr = pa->attaches[i].virtual_addr;
            u64 size = seg->page_count * PAGE_SIZE;

            /* Unmap pages and drop physical refcounts */
            for (u64 v = vaddr; v < vaddr + size; v += PAGE_SIZE) {
                vmm_unmap_page(v);
            }

            /* Delete VMA */
            vma_delete_range(current_task, vaddr, vaddr + size);

            /* Clear attach record */
            pa->attaches[i].used = 0;
            pa->attaches[i].shmid = 0;
            pa->attaches[i].virtual_addr = 0;

            seg->ds.shm_nattch--;
            seg->ds.shm_dtime = 0;

            console_write("shm: detached id=");
            console_write_dec(shmid);
            console_write("\n");

            return 0;
        }
    }

    return -1;
}

/* ── shmctl: Shared memory control ── */

int shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
    if (shmid < 0 || shmid >= SHMMNI) return -1;
    struct shm_segment *seg = &shm_segments[shmid];
    if (!seg->used) return -1;

    switch (cmd) {
    case IPC_RMID:
        /* Remove segment if no attachments */
        if (seg->ds.shm_nattch > 0) return -1;
        
        /* Free all physical pages */
        for (int p = 0; p < seg->page_count; p++) {
            pmm_free_frame(seg->physical_pages[p]);
            seg->physical_pages[p] = 0;
        }
        
        memset(seg, 0, sizeof(*seg));
        console_write("shm: removed id=");
        console_write_dec(shmid);
        console_write("\n");
        return 0;

    case IPC_STAT:
        if (buf) {
            *buf = seg->ds;
            return 0;
        }
        return -1;

    case IPC_SET:
        if (buf) {
            seg->ds.shm_perm.uid = buf->shm_perm.uid;
            seg->ds.shm_perm.gid = buf->shm_perm.gid;
            seg->ds.shm_perm.mode = buf->shm_perm.mode;
            seg->ds.shm_ctime = 0;
            return 0;
        }
        return -1;

    default:
        return -1;
    }
}

/* ── Get process attachments (for cleanup on process exit) ── */

struct shm_attach *shm_get_process_attaches(usize pid)
{
    for (int i = 0; i < MAX_PROC_ATTACH; i++) {
        if (proc_attaches[i].pid == pid) {
            return proc_attaches[i].attaches;
        }
    }
    return 0;
}

/* ── Detach all segments for a process (called on exit) ── */

void shm_detach_all(usize pid)
{
    struct shm_attach *attaches = shm_get_process_attaches(pid);
    if (!attaches) return;

    for (int i = 0; i < SHM_MAX_ATTACH_PER_PROC; i++) {
        if (attaches[i].used) {
            shmdt((const void *)(usize)attaches[i].virtual_addr);
        }
    }
}
