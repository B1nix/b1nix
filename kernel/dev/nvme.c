#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/nvme.h>
#include <b1nix/pci.h>
#include <b1nix/sched.h>
#include <string.h>

#define NVME_MAX_QUEUE_SIZE 64
#define NVME_PAGE_SIZE 4096

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
};

static struct nvme_device nvme;

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
    // CDW11: IEN (bit 1) | PC (bit 0)
    sqe.cdw11 = (1 << 0); // PC = 1
    
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
    
    // Wait for completion on CQ1. Returning on timeout would let the caller
    // free or reuse PRP-list/data buffers while the device still owns them.
    u64 spins = 0;
    for (;;) {
        volatile u32 *cq_hdb = (volatile u32 *)((u64)(usize)dev->regs + 0x1000 + 12); // CQ1
        u16 cq_head = dev->io_cq_head;
        
        struct nvme_cqe *cqe = &dev->io_cq[cq_head];
        if (cqe->status != 0xFFFF) {
            u16 status = cqe->status;
            cqe->status = 0xFFFF; // Reset status on consume
            
            cq_head = (cq_head + 1) % NVME_MAX_QUEUE_SIZE;
            dev->io_cq_head = cq_head;
            *cq_hdb = cq_head;
            
            if ((status & 0xFFFE) != 0) {
                console_write("nvme: io cmd error status=0x");
                console_write_hex32(status);
                console_write("\n");
                return -1;
            }
            return 0;
        }
        if (spins == 10000000ULL) {
            nvme_wait_note("io");
        }
        scheduler_yield();
        spins++;
    }
}

static int nvme_io_transfer(struct nvme_device *nd, u64 lba, u32 count, void *buffer, int is_write)
{
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
    
    // Enable bus master and memory space
    u16 command = pci_config_read16(pci_info.bus, pci_info.slot, pci_info.func, 0x04);
    command |= 0x06;
    pci_config_write16(pci_info.bus, pci_info.slot, pci_info.func, 0x04, command);
    
    // Read BAR0 (64-bit MMIO base)
    u32 bar0_low = pci_config_read32(pci_info.bus, pci_info.slot, pci_info.func, 0x10);
    u32 bar0_high = pci_config_read32(pci_info.bus, pci_info.slot, pci_info.func, 0x14);
    u64 bar0 = (u64)bar0_low | ((u64)bar0_high << 32);
    bar0 &= 0xFFFFFFFFFFFFFFF0ULL;
    
    console_write("nvme: BAR0 at 0x");
    console_write_hex64(bar0);
    console_write("\n");
    
    // Map registers via direct map
    u64 regs_virt = vmm_direct_map_base() + bar0;
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
