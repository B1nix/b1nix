#include <b1nix/blk.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/amdvi.h>
#include <b1nix/iommu.h>
#include <b1nix/irq.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/nvme.h>
#include <b1nix/pci.h>
#include <lkpi/dma-mapping.h>
#include <b1nix/sched.h>
#include <string.h>

#define NVME_MAX_QUEUE_SIZE 64
#define NVME_PAGE_SIZE 4096

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
    
    struct block_device blk_dev;
    u16 cid_counter;
    volatile int io_busy; // yield-safe I/O-path mutex (see nvme_io_lock)

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

/* Yield-safe per-device I/O mutex. nvme_io_submit() rings the SQ doorbell then
 * spins on the CQ calling scheduler_yield(); the submission/completion queues,
 * io_sq_tail and io_cq_head are shared device state. Without serialization a
 * task that runs during that yield — e.g. swap_out()/swap_in() driven by
 * another process's page fault reaching this device via blk_*_cached() —
 * re-enters nvme_io_transfer() and corrupts the in-flight queue entry / head
 * tracking. Mirrors virtio_blk's and AHCI's busy-flag mutex: spin with
 * scheduler_yield() so no real spinlock is held across the DMA wait. */
static void nvme_io_lock(struct nvme_device *dev) {
    while (__sync_lock_test_and_set(&dev->io_busy, 1)) {
        scheduler_yield();
    }
}

static void nvme_io_unlock(struct nvme_device *dev) {
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
        __asm__ volatile("pause");
        timeout--;
    }
    return -1; // Timeout
}

static int nvme_admin_submit(struct nvme_device *dev, struct nvme_sqe *sqe)
{
    u16 tail = dev->admin_sq_tail;
    memcpy(&dev->admin_sq[tail], sqe, sizeof(struct nvme_sqe));
    tail = (tail + 1) % NVME_MAX_QUEUE_SIZE;
    dev->admin_sq_tail = tail;
    
    // Ring the doorbell
    volatile u32 *sq_tdb = (volatile u32 *)((u64)(usize)dev->regs + 0x1000);
    *sq_tdb = tail;
    
    // Wait for completion. Once the command is submitted, do not return while
    // the controller may still DMA into command buffers owned by the caller.
    u64 spins = 0;
    for (;;) {
        volatile u32 *cq_hdb = (volatile u32 *)((u64)(usize)dev->regs + 0x1000 + 4); // CQ0 head doorbell
        u16 cq_head = dev->admin_cq_head;
        
        // Check if there's a completion
        struct nvme_cqe *cqe = &dev->admin_cq[cq_head];
        if (cqe->status != 0xFFFF) {
            u16 status = cqe->status;
            cqe->status = 0xFFFF; // Reset status on consume
            
            // Update head
            cq_head = (cq_head + 1) % NVME_MAX_QUEUE_SIZE;
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
    sqe.cdw10 = ((NVME_MAX_QUEUE_SIZE - 1) << 16) | 1;
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
    sqe.cdw10 = ((NVME_MAX_QUEUE_SIZE - 1) << 16) | 1;
    // CDW11: CQID (bits 31:16) | PC (bit 0)
    sqe.cdw11 = (1 << 16) | (1 << 0); // CQID = 1, PC = 1
    
    return nvme_admin_submit(dev, &sqe);
}

static int nvme_io_submit(struct nvme_device *dev, struct nvme_sqe *sqe)
{
    u16 tail = dev->io_sq_tail;
    memcpy(&dev->io_sq[tail], sqe, sizeof(struct nvme_sqe));
    tail = (tail + 1) % NVME_MAX_QUEUE_SIZE;
    dev->io_sq_tail = tail;
    
    // Ring SQ1 doorbell
    volatile u32 *sq_tdb = (volatile u32 *)((u64)(usize)dev->regs + 0x1000 + 8); // SQ1
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
    volatile u32 *cq_hdb = (volatile u32 *)((u64)(usize)dev->regs + 0x1000 + 12); // CQ1
    u64 spins = 0;
    for (;;) {
        u16 cq_head = dev->io_cq_head;
        struct nvme_cqe *cqe = &dev->io_cq[cq_head];
        /* The CQ entry lives in host RAM (the controller DMAs the completion), so
         * polling it is a plain memory read, not an MMIO VM-exit. Spin on it
         * briefly before yielding: under KVM the command completes in
         * microseconds, and yielding on the first not-done pays a full scheduler
         * round-trip per command, capping throughput far below the device. */
        for (int s = 0; s < 4096 && cqe->status == 0xFFFF; s++)
            __asm__ volatile("pause");
        if (cqe->status != 0xFFFF) {
            u16 status = cqe->status;
            cqe->status = 0xFFFF; // Reset status on consume

            cq_head = (cq_head + 1) % NVME_MAX_QUEUE_SIZE;
            dev->io_cq_head = cq_head;
            *cq_hdb = cq_head;           // deasserts the controller's INTx line
            if (!dev->use_msix)
                dev->regs->intmc = (1u << 0); // re-enable vector 0 for the next I/O

            if ((status & 0xFFFE) != 0) {
                console_write("nvme: io cmd error status=0x");
                console_write_hex32(status);
                console_write("\n");
                return -1;
            }
            return 0;
        }

        // Fast path: brief in-RAM spin (no MMIO) to catch a quick completion.
        for (int i = 0; i < 4000; i++) {
            if (dev->io_cq[dev->io_cq_head].status != 0xFFFF)
                break;
            __asm__ volatile("pause");
        }
        if (dev->io_cq[dev->io_cq_head].status != 0xFFFF)
            continue;

        if (!scheduler_can_block()) {
            if (spins == 10000000ULL)
                nvme_wait_note("io");
            scheduler_yield();
            spins++;
            continue;
        }
        scheduler_wait_prepare_timeout(&dev->io_cq, NVME_IO_WATCHDOG_TICKS);
        if (dev->io_cq[dev->io_cq_head].status != 0xFFFF) {
            scheduler_wait_cancel();
            continue;
        }
        scheduler_wait_commit();
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
    if ((u64)count * 512 > NVME_PAGE_SIZE * (NVME_PAGE_SIZE / sizeof(u64)))
        return -1;
    nvme_io_lock(nd);
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    
    sqe.cdw0 = (is_write ? NVME_CMD_IO_WRITE : NVME_CMD_IO_READ) | (0 << 16);
    sqe.nsid = 1;
    
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
        console_write("nvme: no NVMe controller found\n");
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
    u8 nvme_irq_line = pci_config_read8(pci_info.bus, pci_info.slot, pci_info.func, 0x3C);
    
    // Read BAR0 (64-bit MMIO base)
    u32 bar0_low = pci_config_read32(pci_info.bus, pci_info.slot, pci_info.func, 0x10);
    u32 bar0_high = pci_config_read32(pci_info.bus, pci_info.slot, pci_info.func, 0x14);
    u64 bar0 = (u64)bar0_low | ((u64)bar0_high << 32);
    bar0 &= 0xFFFFFFFFFFFFFFF0ULL;
    
    console_write("nvme: BAR0 at 0x");
    console_write_hex64(bar0);
    console_write("\n");
    
    // Map the controller registers.
#ifdef __x86_64__
    // x86_64: the direct map already covers PCI MMIO BARs.
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
    
    console_write("nvme: cap=0x");
    console_write_hex64(cap);
    console_write(" timeout=");
    console_write_dec(to);
    console_write("\n");
    
    // Disable controller if enabled
    u32 cc = regs->cc;
    if (cc & NVME_CC_EN) {
        cc &= ~NVME_CC_EN;
        regs->cc = cc;
        if (nvme_wait_ready(regs, 0) < 0) {
            console_write("nvme: failed to disable\n");
            return;
        }
    }
    
    // Allocate admin submission queue (phys contiguous)
    nvme.phys_admin_sq = pmm_alloc_frames((NVME_MAX_QUEUE_SIZE * sizeof(struct nvme_sqe) + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE);
    nvme.admin_sq = (struct nvme_sqe *)(usize)(nvme.phys_admin_sq + vmm_direct_map_base());
    memset(nvme.admin_sq, 0, NVME_MAX_QUEUE_SIZE * sizeof(struct nvme_sqe));
    
    // Allocate admin completion queue
    nvme.phys_admin_cq = pmm_alloc_frames((NVME_MAX_QUEUE_SIZE * sizeof(struct nvme_cqe) + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE);
    nvme.admin_cq = (struct nvme_cqe *)(usize)(nvme.phys_admin_cq + vmm_direct_map_base());
    memset(nvme.admin_cq, 0, NVME_MAX_QUEUE_SIZE * sizeof(struct nvme_cqe));
    
    // Allocate I/O submission queue
    nvme.phys_io_sq = pmm_alloc_frames((NVME_MAX_QUEUE_SIZE * sizeof(struct nvme_sqe) + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE);
    nvme.io_sq = (struct nvme_sqe *)(usize)(nvme.phys_io_sq + vmm_direct_map_base());
    memset(nvme.io_sq, 0, NVME_MAX_QUEUE_SIZE * sizeof(struct nvme_sqe));
    
    // Allocate I/O completion queue
    nvme.phys_io_cq = pmm_alloc_frames((NVME_MAX_QUEUE_SIZE * sizeof(struct nvme_cqe) + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE);
    nvme.io_cq = (struct nvme_cqe *)(usize)(nvme.phys_io_cq + vmm_direct_map_base());
    memset(nvme.io_cq, 0, NVME_MAX_QUEUE_SIZE * sizeof(struct nvme_cqe));
    
    // Allocate identify buffer (phys contiguous, page aligned)
    // Need 2 pages: one for controller, one for namespace
    nvme.phys_identify_buf = pmm_alloc_frames(2);
    nvme.identify_ctrl = (struct nvme_identify_ctrl *)(usize)(nvme.phys_identify_buf + vmm_direct_map_base());
    nvme.identify_ns = (struct nvme_identify_ns *)(usize)(nvme.phys_identify_buf + NVME_PAGE_SIZE + vmm_direct_map_base());
    memset((void *)(usize)(nvme.phys_identify_buf + vmm_direct_map_base()), 0, 2 * NVME_PAGE_SIZE);
    
    for (int i = 0; i < NVME_MAX_QUEUE_SIZE; i++) {
        nvme.admin_cq[i].status = 0xFFFF;
        nvme.io_cq[i].status = 0xFFFF;
    }
    
    // Configure admin queue attributes
    u32 aqa = (NVME_MAX_QUEUE_SIZE - 1) | ((NVME_MAX_QUEUE_SIZE - 1) << 16);
    regs->aqa = aqa;
    regs->asq = nvme.phys_admin_sq;
    regs->acq = nvme.phys_admin_cq;
    
    // Configure and enable controller
    cc = NVME_CC_EN | NVME_CC_CSS_NVM | (0 << NVME_CC_MPS_SHIFT) | (NVME_CC_IOSQES << 16) | (NVME_CC_IOCQES << 20);
    regs->cc = cc;
    
    if (nvme_wait_ready(regs, 1) < 0) {
        console_write("nvme: failed to enable\n");
        return;
    }
    
    console_write("nvme: controller enabled\n");
    
    // Identify controller
    if (nvme_identify(&nvme, NVME_IDENTIFY_CNS_CTRL, 0, nvme.phys_identify_buf) < 0) {
        console_write("nvme: identify controller failed\n");
        return;
    }
    
    console_write("nvme: model=");
    console_write(nvme.identify_ctrl->mn);
    console_write(" sn=");
    console_write(nvme.identify_ctrl->sn);
    console_write("\n");
    
    nvme.namespace_count = nvme.identify_ctrl->nn;
    console_write("nvme: namespaces=");
    console_write_dec(nvme.namespace_count);
    console_write("\n");
    
    if (nvme.namespace_count == 0) {
        console_write("nvme: no namespaces\n");
        return;
    }
    
    // Identify namespace 1
    if (nvme_identify(&nvme, NVME_IDENTIFY_CNS_NS, 1, nvme.phys_identify_buf + NVME_PAGE_SIZE) < 0) {
        console_write("nvme: identify namespace 1 failed\n");
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
        console_write("nvme: create IO CQ failed\n");
        return;
    }
    console_write("nvme: IO CQ created\n");
    
    // Create I/O submission queue
    if (nvme_create_io_sq(&nvme) < 0) {
        console_write("nvme: create IO SQ failed\n");
        return;
    }
    console_write("nvme: IO SQ created\n");

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

    // Register block device
    nvme.blk_dev.name = "nvme0";
    nvme.blk_dev.block_size = 512; // Use 512-byte blocks for compatibility with blk cache
    nvme.blk_dev.block_count = nvme.namespace_size * (nvme.block_size / 512);
    nvme.blk_dev.read_blocks = nvme_blk_read;
    nvme.blk_dev.write_blocks = nvme_blk_write;
    nvme.blk_dev.priv = &nvme;
    blk_register(&nvme.blk_dev);
    
    console_write("nvme: registered nvme0 with ");
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
        console_write("M98-DRV-SMOKE: skip msi-delivery (no NVMe controller)\n");
        return;
    }
    if (!nvme.use_msix) {
        console_write("M98-DRV-SMOKE: skip msi-delivery (controller has no MSI-X)\n");
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
        armed = rb == 0 && data == (u32)nvme.msix_vector && (vctrl & 1u) == 0;
    }
    if (!armed) {
        console_write("M98-DRV-SMOKE: FAIL msi-delivery (entry 0 not armed for vector ");
        console_write_dec((u64)nvme.msix_vector);
        console_write(")\n");
        return;
    }

    u8 *buf = (u8 *)kmalloc(512);
    if (!buf) {
        console_write("M98-DRV-SMOKE: FAIL msi-delivery (no buffer)\n");
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
        console_write("M100C-SMOKE: ok ir-delivery (a remapped MSI-X reached its vector)\n");
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
static int unit_active(void) { return iommu_active() || amdvi_active(); }

static const char *unit_suite(void)
{
	return iommu_active() ? "M100B-SMOKE" : "M100D-SMOKE";
}

static int unit_map_identity(u64 phys, usize size, int writable)
{
	if (iommu_active())
		return iommu_map_identity(phys, size, writable);
	u64 base = phys & ~(u64)(NVME_PAGE_SIZE - 1);
	return amdvi_map(base, base, (usize)((phys - base) + size), writable);
}

static int unit_unmap(u64 base, usize size)
{
	if (iommu_active())
		return iommu_unmap(base, size);
	return amdvi_unmap(base, size);
}

static u64 unit_translate(u64 iova)
{
	return iommu_active() ? iommu_translate(iova) : amdvi_translate(iova);
}

static u32 unit_fault_count(void)
{
	return iommu_active() ? iommu_fault_count() : amdvi_fault_count();
}

static void unit_fault_clear(void)
{
	if (iommu_active())
		iommu_fault_clear();
	else
		amdvi_fault_clear();
}

static int unit_attach(void)
{
	if (iommu_active())
		return iommu_attach_device(nvme.pci_bus, nvme.pci_slot, nvme.pci_func);
	u16 bdf = (u16)((nvme.pci_bus << 8) | ((nvme.pci_slot & 0x1F) << 3) |
	                (nvme.pci_func & 7));
	return amdvi_attach_device(bdf);
}

static void unit_detach(void)
{
	if (iommu_active()) {
		iommu_detach_device(nvme.pci_bus, nvme.pci_slot, nvme.pci_func);
		return;
	}
	u16 bdf = (u16)((nvme.pci_bus << 8) | ((nvme.pci_slot & 0x1F) << 3) |
	                (nvme.pci_func & 7));
	amdvi_detach_device(bdf);
}

static int nvme_iommu_map_own_pages(void)
{
    usize sq_bytes = NVME_MAX_QUEUE_SIZE * sizeof(struct nvme_sqe);
    usize cq_bytes = NVME_MAX_QUEUE_SIZE * sizeof(struct nvme_cqe);
    if (unit_map_identity(nvme.phys_admin_sq, sq_bytes, 1) != 0 ||
        unit_map_identity(nvme.phys_admin_cq, cq_bytes, 1) != 0 ||
        unit_map_identity(nvme.phys_io_sq, sq_bytes, 1) != 0 ||
        unit_map_identity(nvme.phys_io_cq, cq_bytes, 1) != 0 ||
        unit_map_identity(nvme.phys_identify_buf, NVME_PAGE_SIZE, 1) != 0)
        return -1;
    return 0;
}

static void nvme_iommu_unmap_own_pages(void)
{
    usize sq_bytes = NVME_MAX_QUEUE_SIZE * sizeof(struct nvme_sqe);
    usize cq_bytes = NVME_MAX_QUEUE_SIZE * sizeof(struct nvme_cqe);
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
        iommu_fault_last(&fa, &fs, &fr);
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
