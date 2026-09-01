#include <b1nix/arch.h>
#include <b1nix/kprintf.h>
#include <b1nix/blk.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/amdvi.h>
#include <b1nix/iommu.h>
#if defined(__aarch64__)
#include <b1nix/smmuv3.h>
#else
/* The SMMUv3 is an arm64 unit and kernel/dev/smmuv3.c is not built for x86_64.
 * These stand in for it so the unit indirection below reads the same on both
 * arches. Every one of them is unreachable here: unit_smmu_active() is a
 * compile-time zero on this arch. */
static inline int smmuv3_active(void) { return 0; }
static inline int smmuv3_map(u64 iova, u64 phys, usize size, int writable)
{ (void)iova; (void)phys; (void)size; (void)writable; return -1; }
static inline int smmuv3_unmap(u64 iova, usize size)
{ (void)iova; (void)size; return -1; }
static inline u64 smmuv3_translate(u64 iova) { (void)iova; return 0; }
static inline u32 smmuv3_fault_count(void) { return 0; }
static inline void smmuv3_fault_clear(void) {}
static inline int smmuv3_attach_device(u8 bus, u8 slot, u8 func)
{ (void)bus; (void)slot; (void)func; return -1; }
static inline void smmuv3_detach_device(u8 bus, u8 slot, u8 func)
{ (void)bus; (void)slot; (void)func; }
static inline void smmuv3_fault_last(u64 *addr, u32 *sid, u8 *type)
{ if (addr) *addr = 0; if (sid) *sid = 0; if (type) *type = 0; }
#endif
#include <b1nix/irq.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/nvme.h>
#include <b1nix/pci.h>
#if defined(__aarch64__)
#include <b1nix/gicv3.h>
#endif
#include <lkpi/dma-mapping.h>
#include <b1nix/sched.h>
#include <string.h>

/* Ceiling on the queue depth this driver will ask a controller for. Linux uses
 * 1024 entries per NVMe queue by default and clamps that to CAP.MQES; the depth
 * actually used here is min(MQES, this, `b1nix.nvme-queue-depth=N`). 1024 SQ
 * entries is 64 KiB and 1024 CQ entries 16 KiB of physically contiguous memory,
 * both claimed once at probe time when memory is still unfragmented. */
#define NVME_MAX_QUEUE_SIZE 1024
#define NVME_PAGE_SIZE 4096

/* The one namespace this driver drives. It is also the "n<N>" half of the
 * block-device name (nvme0n1). */
#define NVME_NSID 1

/* M70: watchdog deadline (10 ms scheduler ticks) for a blocked NVMe I/O wait.
 * The completion IRQ wakes the waiter on the common path; this only bounds a
 * lost interrupt to a re-poll. Never reached on a healthy controller. */
#define NVME_IO_WATCHDOG_TICKS 50

// NVMe device state
struct nvme_device {
    volatile struct nvme_registers *regs;
    
    // Admin queues
    struct nvme_sqe *admin_sq;
    struct nvme_cqe *admin_cq;
    u64 phys_admin_sq;
    u64 phys_admin_cq;
    u16 admin_sq_tail;
    u16 admin_cq_head;
    
    // I/O queues
    struct nvme_sqe *io_sq;
    struct nvme_cqe *io_cq;
    u64 phys_io_sq;
    u64 phys_io_cq;
    u16 io_sq_tail;
    u16 io_cq_head;
    
    // Buffer for identify data
    struct nvme_identify_ctrl *identify_ctrl;
    struct nvme_identify_ns *identify_ns;
    u64 phys_identify_buf;
    
    u32 namespace_count;
    u64 namespace_size; // In blocks
    u32 block_size;

    /* Queue geometry as the controller reports it, not as this file guesses
     * it: depth from CAP.MQES, doorbell spacing from CAP.DSTRD, largest single
     * transfer from the identify data's MDTS. */
    u32 queue_size;
    u32 db_stride;   /* bytes between consecutive doorbell registers */
    u32 max_sectors; /* largest single command, in 512-byte sectors */
    
    struct block_device blk_dev;
    char blk_name[16];
    u16 cid_counter;
    volatile int io_busy; // yield-safe I/O-path mutex (see nvme_io_lock)
    volatile u64 io_owner; // pid holding io_busy, for the stall report

    /* M98: message-signalled completions. use_msix is set once the controller's
     * MSI-X table entry 0 is programmed with msix_vector; until then the driver
     * is on the legacy INTx line. irq_hits counts handler entries so the
     * self-test can tell a delivered interrupt from a watchdog re-poll. */
    int use_msix;
    int msix_vector;
    int ir_handle; /* M100c: interrupt remapping entry, -1 when not remapped */
    u8 pci_bus, pci_slot, pci_func;
    volatile u32 irq_hits;
};

static struct nvme_device nvme;

/* Controllers bound so far — the "nvme<N>" half of the block-device name. */
static usize nvme_controller_count;

/* Yield-safe per-device I/O mutex. nvme_io_submit() rings the SQ doorbell then
 * spins on the CQ calling scheduler_yield(); the submission/completion queues,
 * io_sq_tail and io_cq_head are shared device state. Without serialization a
 * task that runs during that yield — e.g. swap_out()/swap_in() driven by
 * another process's page fault reaching this device via blk_*_cached() —
 * re-enters nvme_io_transfer() and corrupts the in-flight queue entry / head
 * tracking. Mirrors virtio_blk's and AHCI's busy-flag mutex: spin with
 * scheduler_yield() so no real spinlock is held across the DMA wait. */
static void nvme_io_lock(struct nvme_device *dev) {
    u64 waits = 0;

    while (__sync_lock_test_and_set(&dev->io_busy, 1)) {
        /* A yield-spin on a flag nothing ever releases is indistinguishable
         * from a wedged machine: the waiter stays READY, prints nothing, and
         * the lane dies on the harness timeout with no evidence at all. Name
         * the task that took it and never gave it back. */
        if (++waits == 200000ULL) {
            console_write("nvme: io mutex held by pid ");
            console_write_dec(dev->io_owner);
            console_write(" for too long; waiter pid ");
            console_write_dec(current_task ? (u64)current_task->id : 0);
            console_write("\n");
        }
        scheduler_yield();
    }
    dev->io_owner = current_task ? (u64)current_task->id : 0;
}

static void nvme_io_unlock(struct nvme_device *dev) {
    dev->io_owner = 0;
    __sync_lock_release(&dev->io_busy);
}

/* M70: I/O completion interrupt handler. Runs in IRQ context. The controller's
 * INTx line stays asserted until the host advances the CQ head doorbell, so a
 * level-triggered handler that only woke the waiter would re-fire in a storm
 * until the waiter ran. To avoid that, mask our interrupt vector here; the
 * waiter unmasks it (intmc) after consuming the CQE and ringing the head
 * doorbell. The io_cq pointer is the wait channel. Returns 1 if an I/O
 * completion is pending for this device (shared-line aware). */
static int nvme_irq(void *ctx) {
    struct nvme_device *dev = (struct nvme_device *)ctx;
    /* Count the message, not the work. On MSI-X the vector belongs to this
     * controller alone, so reaching this handler IS a delivered message —
     * including when the waiter's fast CQ spin already consumed the completion
     * and reset the slot, which is the common case under KVM and would
     * otherwise make a delivered interrupt look like no interrupt at all. On
     * the shared INTx line the same cannot be said, so there the count happens
     * only once the completion is confirmed to be ours. */
    if (dev->use_msix)
        __atomic_fetch_add(&dev->irq_hits, 1u, __ATOMIC_RELAXED);
    if (dev->io_cq[dev->io_cq_head].status == 0xFFFF)
        return dev->use_msix; /* our message; completion already consumed */
    if (!dev->use_msix)
        __atomic_fetch_add(&dev->irq_hits, 1u, __ATOMIC_RELAXED);
    /* INTMS/INTMC are the pin-based and MSI mask registers; the NVMe spec says
     * they are not used with MSI-X, whose masking lives in the vector table.
     * The storm they guard against is a level-triggered one, which an MSI-X
     * message — an edge, sent once per completion — cannot produce. */
    if (!dev->use_msix)
        dev->regs->intms = (1u << 0); /* mask vector 0 until the waiter consumes */
    scheduler_wake_all(&dev->io_cq);
    return 1;
}

static void nvme_wait_note(const char *queue_name)
{
    console_write("nvme: ");
    console_write(queue_name);
    console_write(" command still pending after timeout; waiting to preserve DMA buffer lifetime\n");
}



static int nvme_wait_ready(volatile struct nvme_registers *regs, int ready)
{
    int timeout = 10000000;
    while (timeout > 0) {
        u32 csts = regs->csts;
        int rdy = (csts & NVME_CSTS_RDY) ? 1 : 0;
        if (rdy == ready) return 0;
        cpu_relax();
        timeout--;
    }
    return -1; // Timeout
}

/* Doorbell for queue `qid` — submission tail when is_cq is 0, completion head
 * when it is 1. The spacing between doorbells is 4 << CAP.DSTRD bytes; the
 * fixed +4/+8/+12 offsets this used to hardcode are only correct for the
 * DSTRD == 0 controllers QEMU emulates, and mis-address every other one. */
static inline volatile u32 *nvme_doorbell(struct nvme_device *dev, u32 qid,
                                          int is_cq)
{
    u64 base = (u64)(usize)dev->regs + 0x1000;
    return (volatile u32 *)(usize)(base +
                                   (u64)(2u * qid + (u32)(is_cq ? 1 : 0)) *
                                       dev->db_stride);
}

static int nvme_admin_submit(struct nvme_device *dev, struct nvme_sqe *sqe)
{
    u16 tail = dev->admin_sq_tail;
    memcpy(&dev->admin_sq[tail], sqe, sizeof(struct nvme_sqe));
    tail = (u16)((tail + 1) % dev->queue_size);
    dev->admin_sq_tail = tail;
    
    /* The submission entry must be visible to the controller BEFORE the
     * doorbell that tells it to go and read it. Nothing ordered the two on this
     * driver, which is invisible on x86 (stores are ordered) and on TCG (one
     * thread), and is a hang on aarch64 under a real hypervisor: the device
     * thread sees the doorbell, reads a stale queue slot, and no completion is
     * ever posted — the waiter then polls a CQE that will never arrive. Same
     * barrier virtio's notify path has taken since it was written. */
    __sync_synchronize();
    volatile u32 *sq_tdb = nvme_doorbell(dev, 0, 0);
    *sq_tdb = tail;
    
    // Wait for completion. Once the command is submitted, do not return while
    // the controller may still DMA into command buffers owned by the caller.
    u64 spins = 0;
    for (;;) {
        volatile u32 *cq_hdb = nvme_doorbell(dev, 0, 1); // CQ0 head doorbell
        u16 cq_head = dev->admin_cq_head;
        
        // Check if there's a completion
        struct nvme_cqe *cqe = &dev->admin_cq[cq_head];
        if (cqe->status != 0xFFFF) {
            /* The status word is the flag; the rest of the entry (and whatever
             * the command DMA'd) must not be read from before it. */
            __sync_synchronize();
            u16 status = cqe->status;
            cqe->status = 0xFFFF; // Reset status on consume
            
            // Update head
            cq_head = (u16)((cq_head + 1) % dev->queue_size);
            dev->admin_cq_head = cq_head;
            *cq_hdb = cq_head;
            
            if ((status & 0xFFFE) != 0) { // Check bits 15:1 for error (bit 0 is Phase Tag)
                console_write("nvme: admin cmd error status=0x");
                console_write_hex32(status);
                console_write("\n");
                return -1;
            }
            return 0;
        }
        if (spins == 10000000ULL) {
            nvme_wait_note("admin");
        }
        scheduler_yield();
        spins++;
    }
}

static int nvme_identify(struct nvme_device *dev, u8 cns, u32 nsid, u64 phys_buf)
{
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    
    sqe.cdw0 = NVME_CMD_ADMIN_IDENTIFY | (0 << 16); // opcode | fuse
    sqe.nsid = nsid;
    sqe.prp1 = phys_buf;
    sqe.cdw10 = cns; // CNS
    
    return nvme_admin_submit(dev, &sqe);
}

static int nvme_create_io_cq(struct nvme_device *dev)
{
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    
    sqe.cdw0 = NVME_CMD_ADMIN_CREATE_CQ | (0 << 16);
    sqe.prp1 = dev->phys_io_cq;
    
    // CDW10: QSIZE (bits 31:16) | QID (bits 15:0)
    sqe.cdw10 = ((dev->queue_size - 1u) << 16) | 1;
    // CDW11: IV (bits 31:16, vector 0) | IEN (bit 1) | PC (bit 0)
    // M70: IEN=1 so a completion on this CQ raises interrupt vector 0.
    sqe.cdw11 = (1 << 1) | (1 << 0);

    return nvme_admin_submit(dev, &sqe);
}

static int nvme_create_io_sq(struct nvme_device *dev)
{
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    
    sqe.cdw0 = NVME_CMD_ADMIN_CREATE_SQ | (0 << 16);
    sqe.prp1 = dev->phys_io_sq;
    
    // CDW10: QSIZE (bits 31:16) | QID (bits 15:0)
    sqe.cdw10 = ((dev->queue_size - 1u) << 16) | 1;
    // CDW11: CQID (bits 31:16) | PC (bit 0)
    sqe.cdw11 = (1 << 16) | (1 << 0); // CQID = 1, PC = 1
    
    return nvme_admin_submit(dev, &sqe);
}

/* How much I/O this controller has actually done. A flush that is merely SLOW
 * and one that is stuck on a lost completion look identical from outside -- the
 * lane goes quiet either way -- and these two counters, sampled twice by the
 * guest watchdog, tell them apart. */
u64 g_nvme_io_submits;
u64 g_nvme_io_completions;

static int nvme_io_submit(struct nvme_device *dev, struct nvme_sqe *sqe)
{
    u16 tail = dev->io_sq_tail;
    memcpy(&dev->io_sq[tail], sqe, sizeof(struct nvme_sqe));
    tail = (u16)((tail + 1) % dev->queue_size);
    dev->io_sq_tail = tail;
    
    // Ring SQ1 doorbell (see nvme_admin_submit for why the barrier is here)
    __sync_synchronize();
    volatile u32 *sq_tdb = nvme_doorbell(dev, 1, 0); // SQ1
    *sq_tdb = tail;
    
    // Wait for completion on CQ1. Returning early would let the caller free or
    // reuse PRP-list/data buffers while the device still owns them, so this only
    // returns once the CQE is genuinely posted.
    //
    // M70: block until nvme_irq() wakes us instead of busy-yielding. The CQE
    // lives in RAM (the controller DMAs it), so the predicate is a cheap memory
    // read; a brief spin catches the sub-µs KVM completion, then we park on the
    // io_cq channel with a watchdog deadline. nvme_irq() masks our vector to
    // avoid a level-triggered storm; we unmask it (intmc) right after ringing
    // the head doorbell on consume. Early boot / IRQs-off callers yield-poll.
    volatile u32 *cq_hdb = nvme_doorbell(dev, 1, 1); // CQ1
    u64 wait_start_tick = scheduler_get_uptime_ticks();
    int reported = 0;

    __atomic_fetch_add(&g_nvme_io_submits, 1, __ATOMIC_RELAXED);
    for (;;) {
        u16 cq_head = dev->io_cq_head;
        struct nvme_cqe *cqe = &dev->io_cq[cq_head];
        /* The CQ entry lives in host RAM (the controller DMAs the completion), so
         * polling it is a plain memory read, not an MMIO VM-exit. Spin on it
         * briefly before yielding: under KVM the command completes in
         * microseconds, and yielding on the first not-done pays a full scheduler
         * round-trip per command, capping throughput far below the device. */
        for (int s = 0; s < 4096 && cqe->status == 0xFFFF; s++)
            cpu_relax();
        if (cqe->status != 0xFFFF) {
            __sync_synchronize(); /* same acquire as the admin queue */
            u16 status = cqe->status;
            cqe->status = 0xFFFF; // Reset status on consume

            cq_head = (u16)((cq_head + 1) % dev->queue_size);
            dev->io_cq_head = cq_head;
            *cq_hdb = cq_head;           // deasserts the controller's INTx line
            if (!dev->use_msix)
                dev->regs->intmc = (1u << 0); // re-enable vector 0 for the next I/O

            __atomic_fetch_add(&g_nvme_io_completions, 1, __ATOMIC_RELAXED);
            if ((status & 0xFFFE) != 0) {
                console_write("nvme: io cmd error status=0x");
                console_write_hex32(status);
                console_write("\n");
                return -1;
            }
            return 0;
        }

        for (int i = 0; i < 4000; i++) {
            if (dev->io_cq[dev->io_cq_head].status != 0xFFFF)
                break;
            cpu_relax();
        }
        while (dev->io_cq[dev->io_cq_head].status == 0xFFFF) {
            for (int i = 0; i < 256; i++)
                cpu_relax();
            if (dev->io_cq[dev->io_cq_head].status != 0xFFFF)
                break;
            /* A completion that never arrives is an unbounded yield-poll: the
             * task stays READY forever and the whole lane looks wedged with no
             * evidence at all. Say who we are waiting for, once, with the state
             * that distinguishes the three ways this ends: the controller
             * faulted (CSTS.CFS), the command was never fetched (SQ tail vs a
             * quiet controller), or a completion landed somewhere other than
             * the head we are watching. */
            /* Report on WALL CLOCK, not on a spin count.
             *
             * A count cannot describe this loop: every iteration ends in
             * scheduler_yield(), which costs a whole tick once the scheduler is
             * up (so 200,000 meant ~2000 seconds, twenty times the harness's
             * patience -- the lane always died with this never printed) and
             * costs nothing at all before it is (so a low count fires during
             * boot, when the completion is simply a few microseconds away).
             * Five seconds is far beyond any honest NVMe command and means the
             * same thing in both regimes.
             *
             * Measured in scheduler TICKS, not in arch_tsc_monotonic_ns(): that
             * clock is already known to advance slower than the tick on this
             * platform (see the ceiling syscall_sleep_timespec has to carry for
             * exactly that reason), and a deadline built on it never arrived --
             * the task sat here 65 seconds with this check silent. */
            if (!reported &&
                scheduler_get_uptime_ticks() - wait_start_tick > 500 /* 5 s */) {
                reported = 1;
                nvme_wait_note("io");
                {
                    u16 last = (u16)((dev->io_sq_tail + dev->queue_size - 1) %
                                     dev->queue_size);
                    struct nvme_sqe *out = &dev->io_sq[last];
                    console_write("nvme: outstanding opc=0x");
                    console_write_hex32(out->cdw0 & 0xff);
                    console_write(" nsid=");
                    console_write_dec(out->nsid);
                    console_write(" slba=");
                    console_write_dec(((u64)out->cdw11 << 32) | out->cdw10);
                    console_write(" nlb=");
                    console_write_dec((u64)out->cdw12 + 1);
                    console_write(" prp1=0x");
                    console_write_hex64(out->prp1);
                    console_write(" prp2=0x");
                    console_write_hex64(out->prp2);
                    console_write("\n");
                }
                console_write("nvme: csts=0x");
                console_write_hex32(dev->regs->csts);
                console_write(" sq_tail=");
                console_write_dec(dev->io_sq_tail);
                console_write(" cq_head=");
                console_write_dec(dev->io_cq_head);
                console_write(" irq_hits=");
                console_write_dec(dev->irq_hits);
                console_write(" cq_status=");
                for (u16 q = 0; q < dev->queue_size && q < 8; q++) {
                    console_write("0x");
                    console_write_hex32(dev->io_cq[q].status);
                    console_write(" ");
                }
                console_write("\n");
            }
            scheduler_yield();
        }
    }
}

static int nvme_io_transfer(struct nvme_device *nd, u64 lba, u32 count, void *buffer, int is_write)
{
    /* NLB is zero-based (cdw12 = count - 1), so count == 0 would underflow to
     * 0xFFFFFFFF and request a 4 GiB transfer the device DMAs over unrelated
     * memory. The PRP list (one page of u64 entries) also caps the page count;
     * reject transfers larger than it can describe (R4-9). */
    if (count == 0)
        return -1;
    if (count > nd->max_sectors)
        return -1;
    nvme_io_lock(nd);
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    
    sqe.cdw0 = (is_write ? NVME_CMD_IO_WRITE : NVME_CMD_IO_READ) | (0 << 16);
    sqe.nsid = NVME_NSID;
    
    u64 phys_addr = vmm_virt_to_phys(buffer);
    sqe.prp1 = phys_addr;
    
    u64 offset = phys_addr & (NVME_PAGE_SIZE - 1);
    u64 bytes_to_transfer = (u64)count * 512;
    u64 prp_list_phys = 0;
    
    if (offset + bytes_to_transfer <= NVME_PAGE_SIZE) {
        sqe.prp2 = 0;
    } else {
        u64 rem_bytes = bytes_to_transfer - (NVME_PAGE_SIZE - offset);
        if (rem_bytes <= NVME_PAGE_SIZE) {
            sqe.prp2 = vmm_virt_to_phys((void *)((usize)buffer + (NVME_PAGE_SIZE - offset)));
        } else {
            int num_pages = (offset + bytes_to_transfer + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE;
            prp_list_phys = pmm_alloc_frames(1);
            u64 *prp_list = (u64 *)(usize)(prp_list_phys + vmm_direct_map_base());
            memset(prp_list, 0, NVME_PAGE_SIZE);
            for (int i = 1; i < num_pages; i++) {
                u64 page_virt_addr = (usize)buffer + (NVME_PAGE_SIZE - offset) + (i - 1) * NVME_PAGE_SIZE;
                prp_list[i - 1] = vmm_virt_to_phys((void *)page_virt_addr);
            }
            sqe.prp2 = prp_list_phys;
        }
    }
    
    sqe.cdw10 = (u32)(lba & 0xFFFFFFFF);
    sqe.cdw11 = (u32)((lba >> 32) & 0xFFFFFFFF);
    sqe.cdw12 = count - 1;
    
    int ret = nvme_io_submit(nd, &sqe);
    if (prp_list_phys) {
        pmm_free_frame(prp_list_phys);
    }
    nvme_io_unlock(nd);
    return ret == 0 ? (int)count : -1;
}

static int nvme_blk_read(struct block_device *dev, u64 lba, u32 count, void *buffer)
{
    struct nvme_device *nd = (struct nvme_device *)dev->priv;
    if (!nd) return -1;
    return nvme_io_transfer(nd, lba, count, buffer, 0);
}

static int nvme_blk_write(struct block_device *dev, u64 lba, u32 count, const void *buffer)
{
    struct nvme_device *nd = (struct nvme_device *)dev->priv;
    if (!nd) return -1;
    return nvme_io_transfer(nd, lba, count, (void *)buffer, 1);
}

/* Commit the namespace's volatile write cache. Writes are acknowledged as soon
 * as the controller has them, so without this an fsync(2) that returned success
 * could still lose data on a power cut. Issued only from the block layer's
 * fsync/sync/umount path — never per write. */
static int nvme_blk_flush(struct block_device *dev)
{
    struct nvme_device *nd = (struct nvme_device *)dev->priv;
    if (!nd) return -1;

    nvme_io_lock(nd);
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0 = NVME_CMD_IO_FLUSH | (0 << 16);
    sqe.nsid = NVME_NSID;
    int ret = nvme_io_submit(nd, &sqe);
    nvme_io_unlock(nd);
    return ret == 0 ? 0 : -1;
}

/* M109 discard: Dataset Management with the Deallocate bit — the NVMe spelling
 * of TRIM. One range covers any length the block layer can ask for (the range's
 * block count is a full 32 bits and is not zero-based), so the list is always a
 * single entry and always fits in prp1's one page. Only installed when the
 * controller's ONCS says DSM exists. */
static int nvme_blk_discard(struct block_device *dev, u64 lba, u32 count)
{
    struct nvme_device *nd = (struct nvme_device *)dev->priv;
    if (!nd || count == 0) return -1;

    u64 list_phys = pmm_alloc_frames(1);
    if (!list_phys) return -1;
    struct nvme_dsm_range *range =
        (struct nvme_dsm_range *)(usize)(list_phys + vmm_direct_map_base());
    memset(range, 0, NVME_PAGE_SIZE);
    range->cattr = 0;
    range->nlb = count;
    range->slba = lba;

    nvme_io_lock(nd);
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0 = NVME_CMD_IO_DSM | (0 << 16);
    sqe.nsid = NVME_NSID;
    sqe.prp1 = list_phys;
    sqe.cdw10 = 0;        /* number of ranges, zero-based: one range */
    sqe.cdw11 = 1u << 2;  /* AD: deallocate */
    int ret = nvme_io_submit(nd, &sqe);
    nvme_io_unlock(nd);

    pmm_free_frame(list_phys);
    return ret == 0 ? 0 : -1;
}

void nvme_init(void)
{
    struct pci_device_info pci_info;
    int found = 0;
    
    // Find NVMe controller by class/subclass/prog_if
    for (u16 bus = 0; bus < 256 && !found; bus++) {
        for (u8 slot = 0; slot < 32 && !found; slot++) {
            u16 vendor = pci_config_read16((u8)bus, slot, 0, 0);
            if (vendor == 0xFFFF) continue;
            
            u8 class = pci_config_read8((u8)bus, slot, 0, 0x0B);
            u8 subclass = pci_config_read8((u8)bus, slot, 0, 0x0A);
            u8 prog_if = pci_config_read8((u8)bus, slot, 0, 0x09);
            
            if (class == NVME_PCI_CLASS && subclass == NVME_PCI_SUBCLASS && prog_if == NVME_PCI_PROG_IF) {
                pci_info.bus = (u8)bus;
                pci_info.slot = slot;
                pci_info.func = 0;
                pci_info.vendor_id = vendor;
                pci_info.device_id = pci_config_read16((u8)bus, slot, 0, 2);
                pci_info.class_code = class;
                pci_info.subclass = subclass;
                pci_info.prog_if = prog_if;
                found = 1;
            }
        }
    }
    
    if (!found) {
        k_info("nvme", "no NVMe controller found");
        return;
    }
    
    console_write("nvme: found controller v=0x");
    console_write_hex32(pci_info.vendor_id);
    console_write(" d=0x");
    console_write_hex32(pci_info.device_id);
    console_write("\n");
    
    // Enable bus master and memory space; clear INTx Disable (bit 10) so the
    // controller can raise legacy interrupts (M70).
    u16 command = pci_config_read16(pci_info.bus, pci_info.slot, pci_info.func, 0x04);
    command |= 0x06;
    command &= ~0x0400;
    pci_config_write16(pci_info.bus, pci_info.slot, pci_info.func, 0x04, command);

    // Legacy interrupt line for the controller (M70: completion IRQ).
    u8 nvme_irq_line = pci_intx_line(pci_info.bus, pci_info.slot, pci_info.func);
    
    // Read BAR0 (64-bit MMIO base)
    u32 bar0_low = pci_config_read32(pci_info.bus, pci_info.slot, pci_info.func, 0x10);
    u32 bar0_high = pci_config_read32(pci_info.bus, pci_info.slot, pci_info.func, 0x14);
    u64 bar0 = (u64)bar0_low | ((u64)bar0_high << 32);
    bar0 &= 0xFFFFFFFFFFFFFFF0ULL;
    
    console_write("nvme: BAR0 at 0x");
    console_write_hex64(bar0);
    console_write("\n");
    
    // Map the controller registers.
#if defined(__x86_64__) || defined(__aarch64__)
    /* x86_64: the direct map already covers PCI MMIO BARs. aarch64: the same
     * is true — build_kernel_half maps the board's PCIe MMIO window as Device
     * memory, and vmm_direct_map_base() is 0 there, so this is the identity
     * address. Going through vmm_map_mmio instead returned NULL (it is a stub
     * on that arch) and the first register read faulted at address 0. */
    u64 regs_virt = vmm_direct_map_base() + bar0;
#else
    // 32-bit: BAR0 lives at ~4 GB of MMIO space, above the 1 GB direct map, so
    // vmm_direct_map_base()+BAR0 overflows the 32-bit address (cap reads 0 ->
    // "failed to enable"). Map it into the dedicated MMIO window. The doorbell
    // registers extend past the fixed header, so map a few pages.
    u64 regs_virt = (u64)(usize)vmm_map_mmio(bar0, 0x2000,
                                             VMM_WRITABLE | VMM_PCD);
#endif
    volatile struct nvme_registers *regs = (volatile struct nvme_registers *)(usize)regs_virt;
    
    memset(&nvme, 0, sizeof(nvme));
    nvme.regs = regs;
    
    // Read capabilities
    u64 cap = regs->cap;
    u32 to = (u32)((cap >> NVME_CAP_TO_SHIFT) & 0xFF);
    /* CAP.MQES is zero-based and names the deepest queue this controller
     * accepts; CAP.DSTRD names the spacing of the doorbell registers. Both were
     * previously assumed. `b1nix.nvme-queue-depth=N` overrides the depth. */
    u32 mqes = (u32)(cap & 0xFFFFu) + 1u;
    nvme.db_stride = 4u << ((u32)(cap >> NVME_CAP_DSTRD_SHIFT) & 0xFu);
    u32 depth = mqes < NVME_MAX_QUEUE_SIZE ? mqes : NVME_MAX_QUEUE_SIZE;
    char qbuf[16];
    if (bootinfo_get_kv("b1nix.nvme-queue-depth", qbuf, sizeof(qbuf)) && qbuf[0]) {
        u32 v = 0;
        for (const char *cp = qbuf; *cp >= '0' && *cp <= '9'; cp++)
            v = v * 10u + (u32)(*cp - '0');
        if (v >= 2u && v <= mqes)
            depth = v;
    }
    if (depth < 2u)
        depth = 2u; /* a queue of one entry can never be non-full */
    nvme.queue_size = depth;
    
    console_write("nvme: cap=0x");
    console_write_hex64(cap);
    console_write(" timeout=");
    console_write_dec(to);
    console_write(" mqes=");
    console_write_dec(mqes);
    console_write(" queue_depth=");
    console_write_dec(nvme.queue_size);
    console_write(" dstrd_bytes=");
    console_write_dec(nvme.db_stride);
    console_write("\n");
    
    // Disable controller if enabled
    u32 cc = regs->cc;
    if (cc & NVME_CC_EN) {
        cc &= ~NVME_CC_EN;
        regs->cc = cc;
        if (nvme_wait_ready(regs, 0) < 0) {
            k_err("nvme", "failed to disable");
            return;
        }
    }
    
    /* Allocate the four queues (phys contiguous). A deep queue asks for a
     * physically contiguous run — 1024 SQ entries is sixteen frames — and a
     * machine that cannot spare one must still get a working controller, so a
     * failed reservation halves the depth and tries again rather than leaving a
     * null queue behind, which is what the old single-page allocation could
     * never hit and therefore never checked for. */
    for (;;) {
        usize sq_frames = (nvme.queue_size * sizeof(struct nvme_sqe) + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE;
        usize cq_frames = (nvme.queue_size * sizeof(struct nvme_cqe) + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE;
        nvme.phys_admin_sq = pmm_alloc_frames(sq_frames);
        nvme.phys_admin_cq = pmm_alloc_frames(cq_frames);
        nvme.phys_io_sq = pmm_alloc_frames(sq_frames);
        nvme.phys_io_cq = pmm_alloc_frames(cq_frames);
        if (nvme.phys_admin_sq && nvme.phys_admin_cq && nvme.phys_io_sq &&
            nvme.phys_io_cq)
            break;
        /* The allocator frees one frame at a time; give back whatever the
         * partial round did reserve. */
        for (usize f = 0; f < sq_frames; f++) {
            if (nvme.phys_admin_sq)
                pmm_free_frame(nvme.phys_admin_sq + (u64)f * NVME_PAGE_SIZE);
            if (nvme.phys_io_sq)
                pmm_free_frame(nvme.phys_io_sq + (u64)f * NVME_PAGE_SIZE);
        }
        for (usize f = 0; f < cq_frames; f++) {
            if (nvme.phys_admin_cq)
                pmm_free_frame(nvme.phys_admin_cq + (u64)f * NVME_PAGE_SIZE);
            if (nvme.phys_io_cq)
                pmm_free_frame(nvme.phys_io_cq + (u64)f * NVME_PAGE_SIZE);
        }
        nvme.phys_admin_sq = nvme.phys_admin_cq = 0;
        nvme.phys_io_sq = nvme.phys_io_cq = 0;
        if (nvme.queue_size <= 2u) {
            k_err("nvme", "cannot reserve queues");
            return;
        }
        nvme.queue_size /= 2u;
        console_write("nvme: queue reservation failed, retrying at depth ");
        console_write_dec(nvme.queue_size);
        console_write("\n");
    }
    nvme.admin_sq = (struct nvme_sqe *)(usize)(nvme.phys_admin_sq + vmm_direct_map_base());
    memset(nvme.admin_sq, 0, nvme.queue_size * sizeof(struct nvme_sqe));
    nvme.admin_cq = (struct nvme_cqe *)(usize)(nvme.phys_admin_cq + vmm_direct_map_base());
    memset(nvme.admin_cq, 0, nvme.queue_size * sizeof(struct nvme_cqe));
    nvme.io_sq = (struct nvme_sqe *)(usize)(nvme.phys_io_sq + vmm_direct_map_base());
    memset(nvme.io_sq, 0, nvme.queue_size * sizeof(struct nvme_sqe));
    nvme.io_cq = (struct nvme_cqe *)(usize)(nvme.phys_io_cq + vmm_direct_map_base());
    memset(nvme.io_cq, 0, nvme.queue_size * sizeof(struct nvme_cqe));
    
    // Allocate identify buffer (phys contiguous, page aligned)
    // Need 2 pages: one for controller, one for namespace
    nvme.phys_identify_buf = pmm_alloc_frames(2);
    nvme.identify_ctrl = (struct nvme_identify_ctrl *)(usize)(nvme.phys_identify_buf + vmm_direct_map_base());
    nvme.identify_ns = (struct nvme_identify_ns *)(usize)(nvme.phys_identify_buf + NVME_PAGE_SIZE + vmm_direct_map_base());
    memset((void *)(usize)(nvme.phys_identify_buf + vmm_direct_map_base()), 0, 2 * NVME_PAGE_SIZE);
    
    for (u32 i = 0; i < nvme.queue_size; i++) {
        nvme.admin_cq[i].status = 0xFFFF;
        nvme.io_cq[i].status = 0xFFFF;
    }
    
    // Configure admin queue attributes
    u32 aqa = (nvme.queue_size - 1u) | ((nvme.queue_size - 1u) << 16);
    regs->aqa = aqa;
    regs->asq = nvme.phys_admin_sq;
    regs->acq = nvme.phys_admin_cq;
    
    // Configure and enable controller
    cc = NVME_CC_EN | NVME_CC_CSS_NVM | (0 << NVME_CC_MPS_SHIFT) | (NVME_CC_IOSQES << 16) | (NVME_CC_IOCQES << 20);
    regs->cc = cc;
    
    if (nvme_wait_ready(regs, 1) < 0) {
        k_err("nvme", "failed to enable");
        return;
    }
    
    k_info("nvme", "controller enabled");
    
    // Identify controller
    if (nvme_identify(&nvme, NVME_IDENTIFY_CNS_CTRL, 0, nvme.phys_identify_buf) < 0) {
        k_err("nvme", "identify controller failed");
        return;
    }
    
    console_write("nvme: model=");
    console_write(nvme.identify_ctrl->mn);
    console_write(" sn=");
    console_write(nvme.identify_ctrl->sn);
    console_write("\n");
    
    /* MDTS is the largest transfer the controller accepts, expressed as a
     * power-of-two multiple of the minimum page size (4 KiB here, since CC.MPS
     * is programmed to 0); 0 means the controller states no limit. Whatever it
     * says, this driver can only describe one PRP list page — 512 entries of
     * 4 KiB, i.e. 2 MiB — because it never chains a second list, so the smaller
     * of the two is the honest ceiling. Linux does the same min() in
     * nvme_set_queue_limits(). */
    {
        u64 prp_limit = (u64)NVME_PAGE_SIZE * (NVME_PAGE_SIZE / sizeof(u64));
        u64 limit = prp_limit;
        u8 mdts = nvme.identify_ctrl->mdts;
        if (mdts && mdts < 32) {
            u64 dev_limit = (u64)NVME_PAGE_SIZE << mdts;
            if (dev_limit < limit)
                limit = dev_limit;
        }
        nvme.max_sectors = (u32)(limit / 512);
        console_write("nvme: mdts=");
        console_write_dec(mdts);
        console_write(" max_transfer=");
        console_write_dec(limit / 1024);
        console_write(" KiB\n");
    }

    nvme.namespace_count = nvme.identify_ctrl->nn;
    console_write("nvme: namespaces=");
    console_write_dec(nvme.namespace_count);
    console_write("\n");
    
    if (nvme.namespace_count == 0) {
        k_info("nvme", "no namespaces");
        return;
    }
    
    // Identify the namespace this driver drives
    if (nvme_identify(&nvme, NVME_IDENTIFY_CNS_NS, NVME_NSID, nvme.phys_identify_buf + NVME_PAGE_SIZE) < 0) {
        k_err("nvme", "identify namespace 1 failed");
        return;
    }
    
    struct nvme_identify_ns *ns = nvme.identify_ns;
    u8 flbas = ns->flbas & 0xF;
    u16 lbads = ns->lbaf[flbas].ds;
    
    nvme.namespace_size = ns->nsze;
    nvme.block_size = 1 << lbads;
    
    console_write("nvme: nsze=");
    console_write_dec(ns->nsze);
    console_write(" block_size=");
    console_write_dec(nvme.block_size);
    console_write("\n");
    
    // Create I/O completion queue
    if (nvme_create_io_cq(&nvme) < 0) {
        k_err("nvme", "create IO CQ failed");
        return;
    }
    k_info("nvme", "IO CQ created");
    
    // Create I/O submission queue
    if (nvme_create_io_sq(&nvme) < 0) {
        k_err("nvme", "create IO SQ failed");
        return;
    }
    k_info("nvme", "IO SQ created");

    /* M70: enable interrupt-driven I/O completion. The IO CQ was created with
     * IEN set (vector 0); unmask that vector (intmc) and register the handler,
     * then unmask the controller's INTx line at the IOAPIC. Done after the IO
     * queues exist so the first delivered completion is serviceable. */
    /* M98: prefer MSI-X. A message interrupt is written straight to the local
     * APIC, so there is no shared line to arbitrate and no level to deassert —
     * which is why the INTMS/INTMC dance above is skipped on this path. The
     * vector comes from the dedicated MSI range and is owned by this driver
     * alone. If anything fails, fall back to the legacy line: the completion
     * wait re-polls the CQ on a watchdog deadline either way, so a controller
     * that never delivers costs latency, not correctness. */
    nvme.ir_handle = -1;
    nvme.pci_bus = pci_info.bus;
    nvme.pci_slot = pci_info.slot;
    nvme.pci_func = pci_info.func;
    if (pci_msix_table_size(pci_info.bus, pci_info.slot, pci_info.func) > 0) {
        int vec = msi_alloc_vector(nvme_irq, &nvme);
        int programmed = -1;
        if (vec > 0 && iommu_ir_active()) {
            /* M100c: with remapping on, the message names an entry this kernel
             * owns and the unit supplies the vector from it. The entry is bound
             * to this controller's requester id, so the same message from
             * anything else is refused. */
            u16 source = (u16)((pci_info.bus << 8) |
                               ((pci_info.slot & 0x1F) << 3) |
                               (pci_info.func & 7));
            int handle = iommu_ir_alloc((u8)vec, lapic_id(), source);
            if (handle >= 0) {
                programmed = pci_msix_enable_msg(pci_info.bus, pci_info.slot,
                                                 pci_info.func, 0,
                                                 iommu_ir_message_address(handle),
                                                 iommu_ir_message_data(handle));
                if (programmed == 0)
                    nvme.ir_handle = handle;
                else
                    iommu_ir_free(handle);
            }
        }
        if (vec > 0 && programmed != 0)
            programmed = pci_msix_enable(pci_info.bus, pci_info.slot,
                                         pci_info.func, 0, (u8)vec);
        if (vec > 0 && programmed == 0) {
            nvme.use_msix = 1;
            nvme.msix_vector = vec;
            console_write("nvme: MSI-X completions on vector ");
            console_write_dec((u64)vec);
            if (nvme.ir_handle >= 0) {
                console_write(" through remap entry ");
                console_write_dec((u64)nvme.ir_handle);
            }
            console_write("\n");
        } else if (vec > 0) {
            msi_free_vector(vec);
        }
    }
    if (!nvme.use_msix) {
        nvme.regs->intmc = (1u << 0);
        irq_register_handler(nvme_irq_line, nvme_irq, &nvme);
        irq_unmask(nvme_irq_line);
    }

    // Register block device under its Linux name: controller index, namespace
    // id. This driver drives namespace 1 of the controller it bound to, and the
    // controller index comes from the enumeration counter, so a second one
    // would register as nvme1n1 with nothing here to edit.
    blk_nvme_name(nvme_controller_count, NVME_NSID, nvme.blk_name,
                  sizeof(nvme.blk_name));
    nvme_controller_count++;
    nvme.blk_dev.name = nvme.blk_name;
    nvme.blk_dev.bus = BLK_BUS_NVME;
    nvme.blk_dev.block_size = 512; // Use 512-byte blocks for compatibility with blk cache
    nvme.blk_dev.block_count = nvme.namespace_size * (nvme.block_size / 512);
    nvme.blk_dev.read_blocks = nvme_blk_read;
    nvme.blk_dev.write_blocks = nvme_blk_write;
    nvme.blk_dev.flush = nvme_blk_flush;
    /* Deallocate only if the controller says it has the command. A DSM sent to
     * a controller that never claimed it comes back as an invalid opcode, and
     * blkdiscard would then report an I/O error instead of the truth, which is
     * that this device does not do discard. */
    if (nvme.identify_ctrl && (nvme.identify_ctrl->oncs & NVME_ONCS_DSM))
        nvme.blk_dev.discard = nvme_blk_discard;
    nvme.blk_dev.priv = &nvme;
    /* Publish the controller's own limits so the block layer's read-ahead and
     * bulk transfers are cut to this device's MDTS rather than to a constant
     * chosen for the smallest controller in the tree. The segment count is the
     * PRP list's capacity. The depth is 1: the queue is `queue_size` deep, but
     * nvme_io_lock() serialises submit-and-wait, so one command is in flight. */
    nvme.blk_dev.limits.max_sectors = nvme.max_sectors;
    nvme.blk_dev.limits.max_segments = NVME_PAGE_SIZE / sizeof(u64);
    nvme.blk_dev.limits.queue_depth = 1;
    blk_register(&nvme.blk_dev);
    
    console_write("nvme: dataset-management=");
    console_write(nvme.blk_dev.discard ? "yes" : "no");
    console_write("\n");

    console_write("nvme: registered ");
    console_write(nvme.blk_dev.name);
    console_write(" with ");
    console_write_dec(nvme.blk_dev.block_count);
    console_write(" blocks\n");
}

/* ── M98: MSI-X delivery self-test ──────────────────────────────────
 *
 * The programming of an MSI/MSI-X capability is checked elsewhere by reading
 * the device's own registers back. This checks the other half — that a message
 * the *device* writes actually arrives — and it is the only way to check it:
 * the interrupt has to be raised by hardware, so the test issues a real read
 * command and looks at whether the handler ran.
 *
 * `irq_hits` is incremented only inside nvme_irq(), which the CPU reaches only
 * by taking the vector this driver programmed into MSI-X table entry 0. The
 * completion path deliberately does NOT depend on the interrupt (it re-polls
 * the CQ on a watchdog deadline), so a passing read proves nothing on its own —
 * the counter moving is what proves delivery.
 */
void nvme_msix_selftest(void)
{
    if (!bootinfo_has_flag("b1nix.test=1"))
        return;
    if (!nvme.regs) {
        k_info(NULL, "M98-DRV-SMOKE: skip msi-delivery (no NVMe controller)");
        return;
    }
    if (!nvme.use_msix) {
        k_info(NULL, "M98-DRV-SMOKE: skip msi-delivery (controller has no MSI-X)");
        return;
    }

    /* The vector the device was told to send, read back out of its own table,
     * must be the one this driver owns — otherwise a hit would prove delivery
     * of somebody else's message. */
    u64 addr = 0;
    u32 data = 0, vctrl = 0xFFFFFFFFu;
    int rb = pci_msix_entry_readback(nvme.pci_bus, nvme.pci_slot, nvme.pci_func,
                                     0, &addr, &data, &vctrl);
    int armed;
    if (nvme.ir_handle >= 0) {
        /* M100c: with interrupt remapping the message names a table entry and
         * carries no vector; the vector lives in that entry, and the unit
         * supplies it. Check the pair the device was given, and that the entry
         * it names is the one holding our vector. */
        u8 entry_vector = 0;
        armed = rb == 0 && (vctrl & 1u) == 0 &&
                addr == iommu_ir_message_address(nvme.ir_handle) &&
                data == iommu_ir_message_data(nvme.ir_handle) &&
                iommu_ir_entry_read(nvme.ir_handle, &entry_vector, 0, 0) == 0 &&
                entry_vector == (u8)nvme.msix_vector;
    } else {
        /* The data word is the vector on x86 and an ITS EventID on aarch64, so
         * ask the arch what it programmed rather than assuming either. */
        u64 want_addr = 0;
        u32 want_data = 0;

        armed = rb == 0 && (vctrl & 1u) == 0 &&
                arch_msi_expected(nvme.msix_vector, &want_addr, &want_data) == 0 &&
                addr == want_addr && data == want_data;
    }
    if (!armed) {
        console_write("M98-DRV-SMOKE: FAIL msi-delivery (entry 0 not armed for vector ");
        console_write_dec((u64)nvme.msix_vector);
        console_write(")\n");
        return;
    }

    u8 *buf = (u8 *)kmalloc(512);
    if (!buf) {
        k_info(NULL, "M98-DRV-SMOKE: FAIL msi-delivery (no buffer)");
        return;
    }

    u32 before = __atomic_load_n(&nvme.irq_hits, __ATOMIC_RELAXED);

    /* read_blocks returns the block count it transferred, not 0. */
    int rc = nvme_blk_read(&nvme.blk_dev, 0, 1, buf);
    /* The handler runs on this CPU in interrupt context; by the time the read
     * returns it has either run or the wait fell back to its watchdog re-poll.
     * Give the former a bounded chance to be observed rather than sampling the
     * counter the instant the doorbell settles. */
    u32 after = before;
    for (int i = 0; i < 200; i++) {
        after = __atomic_load_n(&nvme.irq_hits, __ATOMIC_RELAXED);
        if (after != before)
            break;
        scheduler_yield();
    }
    kfree(buf);

    if (rc != 1) {
        console_write("M98-DRV-SMOKE: FAIL msi-delivery (read returned ");
        console_write_dec((u64)(i64)rc);
        console_write(")\n");
        return;
    }
    if (after == before) {
        console_write("M98-DRV-SMOKE: FAIL msi-delivery (no interrupt on vector ");
        console_write_dec((u64)nvme.msix_vector);
        console_write(")\n");
        return;
    }
    if (nvme.ir_handle >= 0)
        k_info(NULL, "M100C-SMOKE: ok ir-delivery (a remapped MSI-X reached its vector)");
    console_write("M98-DRV-SMOKE: ok msi-delivery vector=");
    console_write_dec((u64)nvme.msix_vector);
    console_write(" hits=");
    console_write_dec((u64)(after - before));
    console_write("\n");
}

/* ── M100b: NVMe through the IOMMU ──────────────────────────────────
 *
 * The controller is moved out of the identity domain into one of its own, and
 * exactly the pages it legitimately touches are mapped there: its queues, its
 * PRP list and the buffer of the transfer under way. Everything else in memory
 * stops existing as far as this device is concerned — which is the property an
 * IOMMU is for, and the reason this is done with a device that really DMAs
 * rather than with a bridge that does not.
 *
 * The mappings are identity (device address == physical address) so the driver
 * keeps programming the addresses it always did; what changes is that those
 * addresses now have to be *granted*.
 */
/* ── whichever remapping unit this machine has ──────────────────────
 *
 * The two units answer the same questions in different registers and table
 * formats, and exactly one of them exists on any given boot. The checks below
 * are about the controller, not about whose silicon is translating, so they go
 * through this thin indirection rather than being written twice — and the
 * marker they print names the milestone the active unit belongs to.
 */
#if defined(__aarch64__)
/* The third unit is this architecture's own: an ARM SMMUv3 (kernel/dev/smmuv3.c).
 * It is reached through the same indirection and answers the same questions —
 * only the milestone the marker names changes, because the hardware being
 * proven is not VT-d and must not claim VT-d's marker. */
#define unit_iommu_active() 0
#define unit_amdvi_active() 0
#define unit_smmu_active()  smmuv3_active()
#else
#define unit_iommu_active() iommu_active()
#define unit_amdvi_active() amdvi_active()
#define unit_smmu_active()  0
#endif

static int unit_active(void)
{
	return unit_iommu_active() || unit_amdvi_active() || unit_smmu_active();
}

static const char *unit_suite(void)
{
	if (unit_smmu_active())
		return "M100E-SMOKE";
	return unit_iommu_active() ? "M100B-SMOKE" : "M100D-SMOKE";
}

static int unit_map_identity(u64 phys, usize size, int writable)
{
	u64 base = phys & ~(u64)(NVME_PAGE_SIZE - 1);

	if (unit_iommu_active())
		return iommu_map_identity(phys, size, writable);
	if (unit_smmu_active())
		return smmuv3_map(base, base, (usize)((phys - base) + size), writable);
	return amdvi_map(base, base, (usize)((phys - base) + size), writable);
}

static int unit_unmap(u64 base, usize size)
{
	if (unit_iommu_active())
		return iommu_unmap(base, size);
	if (unit_smmu_active())
		return smmuv3_unmap(base, size);
	return amdvi_unmap(base, size);
}

static u64 unit_translate(u64 iova)
{
	if (unit_iommu_active())
		return iommu_translate(iova);
	if (unit_smmu_active())
		return smmuv3_translate(iova);
	return amdvi_translate(iova);
}

static u32 unit_fault_count(void)
{
	if (unit_iommu_active())
		return iommu_fault_count();
	if (unit_smmu_active())
		return smmuv3_fault_count();
	return amdvi_fault_count();
}

static void unit_fault_clear(void)
{
	if (unit_iommu_active())
		iommu_fault_clear();
	else if (unit_smmu_active())
		smmuv3_fault_clear();
	else
		amdvi_fault_clear();
}

static int unit_attach(void)
{
	u16 bdf = (u16)((nvme.pci_bus << 8) | ((nvme.pci_slot & 0x1F) << 3) |
	                (nvme.pci_func & 7));

	if (unit_iommu_active())
		return iommu_attach_device(nvme.pci_bus, nvme.pci_slot, nvme.pci_func);
	if (unit_smmu_active())
		return smmuv3_attach_device(nvme.pci_bus, nvme.pci_slot, nvme.pci_func);
	return amdvi_attach_device(bdf);
}

static void unit_detach(void)
{
	u16 bdf = (u16)((nvme.pci_bus << 8) | ((nvme.pci_slot & 0x1F) << 3) |
	                (nvme.pci_func & 7));

	if (unit_iommu_active()) {
		iommu_detach_device(nvme.pci_bus, nvme.pci_slot, nvme.pci_func);
		return;
	}
	if (unit_smmu_active()) {
		smmuv3_detach_device(nvme.pci_bus, nvme.pci_slot, nvme.pci_func);
		return;
	}
	amdvi_detach_device(bdf);
}

/* The last fault the active unit recorded, however it spells one. */
static void unit_fault_last(u64 *addr, u16 *src, u8 *reason)
{
	if (unit_smmu_active()) {
		u32 sid = 0;
		u8 type = 0;

		smmuv3_fault_last(addr, &sid, &type);
		if (src)
			*src = (u16)sid;
		if (reason)
			*reason = type;
		return;
	}
	iommu_fault_last(addr, src, reason);
}

static int nvme_iommu_map_own_pages(void)
{
    usize sq_bytes = nvme.queue_size * sizeof(struct nvme_sqe);
    usize cq_bytes = nvme.queue_size * sizeof(struct nvme_cqe);
    if (unit_map_identity(nvme.phys_admin_sq, sq_bytes, 1) != 0 ||
        unit_map_identity(nvme.phys_admin_cq, cq_bytes, 1) != 0 ||
        unit_map_identity(nvme.phys_io_sq, sq_bytes, 1) != 0 ||
        unit_map_identity(nvme.phys_io_cq, cq_bytes, 1) != 0 ||
        unit_map_identity(nvme.phys_identify_buf, NVME_PAGE_SIZE, 1) != 0)
        return -1;
#if defined(__aarch64__)
    /* The device's interrupt is a memory write too, and behind an SMMU it is
     * translated like any other: the ITS's translation register has to be in
     * the domain or the MSI faults instead of arriving. x86 has no equivalent
     * line here because its message goes to the local APIC, which is not
     * behind the DMA remapping unit. */
    if (unit_smmu_active() && its_ready()) {
        u64 doorbell = its_translater_phys();

        if (doorbell &&
            unit_map_identity(doorbell & ~(u64)(NVME_PAGE_SIZE - 1),
                              NVME_PAGE_SIZE, 1) != 0)
            return -1;
    }
#endif
    return 0;
}

static void nvme_iommu_unmap_own_pages(void)
{
    usize sq_bytes = nvme.queue_size * sizeof(struct nvme_sqe);
    usize cq_bytes = nvme.queue_size * sizeof(struct nvme_cqe);
    unit_unmap(nvme.phys_admin_sq & ~(u64)(NVME_PAGE_SIZE - 1), sq_bytes);
    unit_unmap(nvme.phys_admin_cq & ~(u64)(NVME_PAGE_SIZE - 1), cq_bytes);
    unit_unmap(nvme.phys_io_sq & ~(u64)(NVME_PAGE_SIZE - 1), sq_bytes);
    unit_unmap(nvme.phys_io_cq & ~(u64)(NVME_PAGE_SIZE - 1), cq_bytes);
    unit_unmap(nvme.phys_identify_buf & ~(u64)(NVME_PAGE_SIZE - 1),
                NVME_PAGE_SIZE);
}

void nvme_iommu_selftest(void)
{
    if (!bootinfo_has_flag("b1nix.test=1"))
        return;
    if (!unit_active())
        return; /* the suite for the missing unit already says so */
    if (!nvme.regs) {
        console_write(unit_suite());
        console_write(": skip nvme-translated (no controller)\n");
        return;
    }

    /* A reference read from the identity domain, so the comparison later is
     * against data this machine really holds and not against a constant. */
    u8 *ref = (u8 *)kmalloc(512);
    u8 *buf = (u8 *)kmalloc(512);
    if (!ref || !buf) {
        if (ref) kfree(ref);
        if (buf) kfree(buf);
        console_write(unit_suite()); console_write(": FAIL nvme-translated detail=1\n");
        return;
    }
    if (nvme_blk_read(&nvme.blk_dev, 0, 1, ref) != 1) {
        kfree(ref); kfree(buf);
        console_write(unit_suite()); console_write(": FAIL nvme-translated detail=2\n");
        return;
    }

    /* Let the reference read settle before the domain changes under the
     * controller. The command has completed, but a posted write from it can
     * still be in flight, and it would land in the new domain and fault for a
     * transfer nobody is testing. (That is what an intermittent fault here
     * turned out to be.) */
    for (int i = 0; i < 20; i++)
        scheduler_yield();

    if (unit_attach() != 0) {
        kfree(ref); kfree(buf);
        console_write(unit_suite());
        console_write(": FAIL nvme-translated detail=3\n");
        return;
    }

    int ok = 1;
    u64 data_phys = vmm_virt_to_phys(buf);
    /* Grant the buffer page by page, translating each one on its own. A
     * kernel-heap buffer is contiguous in virtual addresses and need not be in
     * physical ones, so a transfer that straddles a page boundary reaches a
     * frame that has no relation to the first — including one *below* it. A
     * mapping that assumed contiguity granted the wrong second page, and the
     * controller's straddling read faulted intermittently, depending on where
     * the allocator had put the buffer. */
    if (nvme_iommu_map_own_pages() != 0)
        ok = 0;
    if (ok) {
        for (usize off = 0; off < 2 * NVME_PAGE_SIZE; off += NVME_PAGE_SIZE) {
            u64 p1 = vmm_virt_to_phys((u8 *)buf + off);
            u64 p2 = vmm_virt_to_phys((u8 *)ref + off);
            if ((p1 && unit_map_identity(p1, 1, 1) != 0) ||
                (p2 && unit_map_identity(p2, 1, 1) != 0)) {
                ok = 0;
                break;
            }
        }
    }

    /* Nothing else is mapped for this controller: an address it was not given
     * does not translate. */
    u64 stray = pmm_alloc_frame();
    if (ok && stray && unit_translate(stray) != 0)
        ok = 0;

    /* A quiescent window first: if the unit faults while this test issues
     * nothing, the traffic is somebody else's and the measurement below would
     * be blaming the wrong transfer. */
    unit_fault_clear();
    for (int i = 0; i < 40; i++)
        scheduler_yield();
    u32 idle_faults = unit_fault_count();

    unit_fault_clear();
    memset(buf, 0, 512);
    int rc = ok ? nvme_blk_read(&nvme.blk_dev, 0, 1, buf) : -1;
    u32 faults = unit_fault_count();

    if (rc != 1 || memcmp(buf, ref, 512) != 0)
        ok = 0;
    /* Every address the controller emitted was one of the mapped ones, or the
     * unit would have recorded a fault. */
    if (faults != 0 || idle_faults != 0)
        ok = 0;

    for (usize off = 0; off < 2 * NVME_PAGE_SIZE; off += NVME_PAGE_SIZE) {
        u64 p1 = vmm_virt_to_phys((u8 *)buf + off);
        u64 p2 = vmm_virt_to_phys((u8 *)ref + off);
        if (p1)
            unit_unmap(p1 & ~(u64)(NVME_PAGE_SIZE - 1), 1);
        if (p2)
            unit_unmap(p2 & ~(u64)(NVME_PAGE_SIZE - 1), 1);
    }
    nvme_iommu_unmap_own_pages();
    unit_detach();
    if (stray)
        pmm_free_frame(stray);

    if (ok) {
        console_write(unit_suite()); console_write(": ok nvme-translated (read through its own domain, no faults)\n");
    } else {
        u64 fa = 0; u16 fs = 0; u8 fr = 0;
        unit_fault_last(&fa, &fs, &fr);
        console_write(unit_suite()); console_write(": FAIL nvme-translated rc=");
        console_write_dec((u64)(i64)rc);
        console_write(" faults=");
        console_write_dec((u64)faults);
        console_write(" src=0x");
        console_write_hex32((u32)fs);
        console_write(" addr=0x");
        console_write_hex64(fa);
        console_write(" reason=0x");
        console_write_hex32((u32)fr);
        console_write(" idle_faults=");
        console_write_dec((u64)idle_faults);
        console_write(" asq=0x");
        console_write_hex64(nvme.phys_admin_sq);
        console_write(" acq=0x");
        console_write_hex64(nvme.phys_admin_cq);
        console_write(" isq=0x");
        console_write_hex64(nvme.phys_io_sq);
        console_write(" icq=0x");
        console_write_hex64(nvme.phys_io_cq);
        console_write(" idbuf=0x");
        console_write_hex64(nvme.phys_identify_buf);
        console_write(" data=0x");
        console_write_hex64(data_phys);
        console_write(" ref=0x");
        console_write_hex64(vmm_virt_to_phys(ref));
        console_write(" srcdev=0x");
        console_write_hex32(((u32)pci_config_read16((u8)(fs >> 8),
                                                    (u8)((fs >> 3) & 0x1F),
                                                    (u8)(fs & 7), 0) << 16) |
                            pci_config_read16((u8)(fs >> 8),
                                              (u8)((fs >> 3) & 0x1F),
                                              (u8)(fs & 7), 2));
        console_write("\n");
    }
    kfree(ref);
    kfree(buf);
}

/* ── M100c: an interrupt nobody programmed an entry for ─────────────
 *
 * The device keeps the message it was given; the entry that message names is
 * taken away. Its next completion therefore claims a handle the unit has no
 * entry for, and the unit has to refuse it — the interrupt does not arrive and
 * the refusal is recorded. The I/O itself still finishes, because the wait
 * loop polls the completion queue in memory and never depended on the
 * interrupt for correctness; that is what makes this safe to do to a live
 * storage controller.
 *
 * Returns 1 when the interrupt was refused and recorded, 0 when it was not,
 * -1 when there is nothing to ask.
 */
int nvme_ir_rejection_probe(void)
{
    if (!nvme.regs || !nvme.use_msix || nvme.ir_handle < 0 || !iommu_ir_active())
        return -1;

    u8 *buf = (u8 *)kmalloc(512);
    if (!buf)
        return -1;

    int handle = nvme.ir_handle;
    u16 source = (u16)((nvme.pci_bus << 8) | ((nvme.pci_slot & 0x1F) << 3) |
                       (nvme.pci_func & 7));

    /* Take the entry away, leaving the device pointing at it. */
    iommu_ir_free(handle);
    iommu_fault_clear();

    u32 before = __atomic_load_n(&nvme.irq_hits, __ATOMIC_RELAXED);
    int rc = nvme_blk_read(&nvme.blk_dev, 0, 1, buf);
    u32 after = before;
    for (int i = 0; i < 100; i++) {
        after = __atomic_load_n(&nvme.irq_hits, __ATOMIC_RELAXED);
        if (after != before)
            break;
        scheduler_yield();
    }
    u32 faults = iommu_fault_count();

    /* Put it back and confirm the controller is interrupting again, so the
     * rest of the boot is not left on the polling path. */
    int restored = iommu_ir_alloc((u8)nvme.msix_vector, lapic_id(), source);
    if (restored >= 0) {
        nvme.ir_handle = restored;
        pci_msix_enable_msg(nvme.pci_bus, nvme.pci_slot, nvme.pci_func, 0,
                            iommu_ir_message_address(restored),
                            iommu_ir_message_data(restored));
    }
    u32 again_before = __atomic_load_n(&nvme.irq_hits, __ATOMIC_RELAXED);
    (void)nvme_blk_read(&nvme.blk_dev, 0, 1, buf);
    u32 again_after = again_before;
    for (int i = 0; i < 100; i++) {
        again_after = __atomic_load_n(&nvme.irq_hits, __ATOMIC_RELAXED);
        if (again_after != again_before)
            break;
        scheduler_yield();
    }
    kfree(buf);

    if (rc != 1)
        return 0; /* the read must still have completed, by polling */
    if (after != before)
        return 0; /* the interrupt got through anyway */
    if (faults == 0)
        return 0; /* refused silently is not refused */
    if (again_after == again_before)
        return 0; /* and it must work again once the entry is back */
    return 1;
}
