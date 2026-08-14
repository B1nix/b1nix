#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/io.h>
#include <b1nix/irq.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>
#include <b1nix/virtio.h>
#include <string.h>

/* M70: watchdog deadline (in 10 ms scheduler ticks) for a blocked I/O wait. The
 * completion IRQ wakes the waiter on the common path; this only bounds the stall
 * if an interrupt is ever lost, degrading to a periodic re-poll instead of a
 * wedge. Never reached on a healthy device. */
#define VIRTIO_BLK_IO_WATCHDOG_TICKS 50

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_BLK_DEVICE_ID 0x1001

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_CONFIG_CAPACITY 0x14

/* Support multiple virtio-blk devices */
#define MAX_VIRTIO_BLK 16

struct virtio_blk_instance {
  struct virtio_device dev;
  struct virtqueue vq;
  struct block_device blk;
  volatile int busy;
};

static struct virtio_blk_instance instances[MAX_VIRTIO_BLK];
static int instance_count = 0;

struct virtio_blk_req {
  u32 type;
  u32 reserved;
  u64 sector;
} __attribute__((packed));

struct virtio_blk_dma_req {
  struct virtio_blk_req req;
  volatile u8 status;
} __attribute__((packed));

static void virtio_blk_lock(struct virtio_blk_instance *inst) {
  while (__sync_lock_test_and_set(&inst->busy, 1)) {
    scheduler_yield();
  }
}

static void virtio_blk_unlock(struct virtio_blk_instance *inst) {
  __sync_lock_release(&inst->busy);
}

/* M70: completion interrupt handler. Runs in IRQ context — read the device ISR
 * (which deasserts the level-triggered line), and if this device posted a used
 * buffer, wake the task blocked in do_virtio_blk_req(). The instance pointer is
 * the wait channel. Returns 1 if the interrupt was ours (shared-line aware). */
static int virtio_blk_irq(void *ctx) {
  struct virtio_blk_instance *inst = (struct virtio_blk_instance *)ctx;
  u8 isr = virtio_read_isr(&inst->dev); /* read clears the ISR status */
  if (!(isr & 0x1))
    return 0; /* not a used-buffer notification from this device */
  scheduler_wake_all(inst);
  return 1;
}

/* Append one logical buffer region to the virtqueue descriptor chain, split at
 * physical page boundaries.
 *
 * The device treats each descriptor's (addr, len) as a physically contiguous
 * span. But the buffers we are handed — block-cache entries that live in the
 * klarge arena, kheap structs — are only *virtually* contiguous; consecutive
 * virtual pages map to unrelated physical frames. Describing a region that
 * crosses a page boundary with a single (vmm_virt_to_phys(start), len) makes
 * the device run off the end of the first frame into whatever physical frame
 * follows it — frequently an unrelated kernel page table. (Observed: a 512-byte
 * cache read straddling a klarge page overran the data frame into the adjacent
 * klarge page-directory frame, zeroing a PDE; with other layouts it zeroed the
 * LAPIC leaf PT — a #PF on the next lapic_eoi. It also silently corrupted large
 * ELF reads, e.g. /persist/bin/make, producing a #GP in early userspace.)
 *
 * Splitting at page boundaries yields one descriptor per physically-contiguous
 * chunk. *next_desc is the next free descriptor slot; *prev is the previous
 * chain link (0xFFFF for the very first region). Returns -1 if the chain would
 * overflow the queue. */
static int vblk_add_region(struct virtqueue *vq, u16 *next_desc, u16 *prev,
                           u64 vaddr, u32 len, u16 write_flag) {
  u32 rem = len;
  while (rem > 0) {
    if (*next_desc >= vq->queue_size)
      return -1;
    u16 idx = (*next_desc)++;
    u64 phys = vmm_virt_to_phys((void *)(usize)vaddr);
    u32 chunk = (u32)(4096 - (vaddr & 4095));
    if (chunk > rem)
      chunk = rem;
    vq->desc[idx].addr = phys;
    vq->desc[idx].len = chunk;
    vq->desc[idx].flags = write_flag; /* F_NEXT is added once a successor links in */
    vq->desc[idx].next = 0;
    if (*prev != 0xFFFF) {
      vq->desc[*prev].flags |= VRING_DESC_F_NEXT;
      vq->desc[*prev].next = idx;
    }
    *prev = idx;
    vaddr += chunk;
    rem -= chunk;
  }
  return 0;
}

static int do_virtio_blk_req(struct virtio_blk_instance *inst, u64 lba,
                             u32 count, void *buffer, u32 type) {
  struct virtio_blk_dma_req *dma = kzalloc(sizeof(struct virtio_blk_dma_req));
  if (!dma)
    return -1;

  virtio_blk_lock(inst);
  dma->req.type = type;
  dma->req.reserved = 0;
  dma->req.sector = lba;
  dma->status = 0xFF;

  /* Build the descriptor chain: header (device reads), data (device writes for
   * reads / reads for writes), status (device writes). Each region is split at
   * page boundaries so the device never crosses into an unrelated frame. The
   * chain head is descriptor 0 (next_desc starts at 0). */
  u16 next_desc = 0;
  u16 prev = 0xFFFF;
  if (vblk_add_region(&inst->vq, &next_desc, &prev, (u64)(usize)&dma->req,
                      sizeof(struct virtio_blk_req), 0) < 0 ||
      vblk_add_region(&inst->vq, &next_desc, &prev, (u64)(usize)buffer,
                      (u64)count * 512,
                      type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0) < 0 ||
      vblk_add_region(&inst->vq, &next_desc, &prev, (u64)(usize)&dma->status, 1,
                      VRING_DESC_F_WRITE) < 0) {
    virtio_blk_unlock(inst);
    kfree(dma);
    return -1;
  }

  u16 avail_idx = inst->vq.avail->idx % inst->vq.queue_size;
  inst->vq.avail->ring[avail_idx] = 0; /* chain head is descriptor 0 */

  __sync_synchronize();
  inst->vq.avail->idx++;
  __sync_synchronize();

  virtq_kick(&inst->dev, &inst->vq);

  /* Wait for the device to post the used buffer. Fast path: a brief in-RAM spin
   * (used->idx is DMA'd into memory, no MMIO) catches the sub-µs KVM completion
   * with no context switch. Slow path: block until virtio_blk_irq() wakes us;
   * scheduler_wait_prepare_timeout re-checks the predicate after publishing
   * BLOCKED so a completion racing the block is never lost, and the watchdog
   * deadline bounds a genuinely-lost interrupt to a re-poll. Before the
   * scheduler is live (early-boot root mount) we cooperatively yield-poll. */
  for (int i = 0; i < 4000; i++) {
    if (inst->vq.used->idx != inst->vq.last_used_idx)
      break;
    __asm__ volatile("pause");
  }
  while (inst->vq.used->idx == inst->vq.last_used_idx) {
    if (!scheduler_can_block()) {
      scheduler_yield();
      continue;
    }
    scheduler_wait_prepare_timeout(inst, VIRTIO_BLK_IO_WATCHDOG_TICKS);
    if (inst->vq.used->idx != inst->vq.last_used_idx) {
      scheduler_wait_cancel();
      break;
    }
    scheduler_wait_commit();
  }

  __sync_synchronize();
  inst->vq.last_used_idx++;

  int ret = (dma->status == 0) ? (int)count : -1;
  virtio_blk_unlock(inst);
  kfree(dma);
  return ret;
}

static int virtio_blk_read(struct block_device *dev, u64 lba, u32 count,
                           void *buffer) {
  struct virtio_blk_instance *inst = (struct virtio_blk_instance *)dev->priv;
  return do_virtio_blk_req(inst, lba, count, buffer, VIRTIO_BLK_T_IN);
}

static int virtio_blk_write(struct block_device *dev, u64 lba, u32 count,
                            const void *buffer) {
  struct virtio_blk_instance *inst = (struct virtio_blk_instance *)dev->priv;
  return do_virtio_blk_req(inst, lba, count, (void *)(usize)buffer,
                           VIRTIO_BLK_T_OUT);
}

/* Find nth virtio-blk device on PCI bus */
static int virtio_find_nth_device(int n, struct pci_device_info *info) {
  int found = 0;
  for (u16 bus = 0; bus < 256 && found <= n; bus++) {
    for (u8 slot = 0; slot < 32 && found <= n; slot++) {
      u16 vendor = pci_config_read16((u8)bus, slot, 0, 0);
      if (vendor == 0xFFFF)
        continue;

      u8 header_type = pci_config_read8((u8)bus, slot, 0, 0x0E);
      u8 max_func = (header_type & 0x80) ? 8 : 1;

      for (u8 func = 0; func < max_func; func++) {
        vendor = pci_config_read16((u8)bus, slot, func, 0);
        if (vendor == 0xFFFF)
          continue;

        u16 dev = pci_config_read16((u8)bus, slot, func, 2);
        if (vendor == VIRTIO_VENDOR_ID && dev == VIRTIO_BLK_DEVICE_ID) {
          if (found == n) {
            if (info) {
              info->bus = (u8)bus;
              info->slot = slot;
              info->func = func;
              info->vendor_id = vendor;
              info->device_id = dev;
              info->class_code = pci_config_read8((u8)bus, slot, func, 0x0B);
              info->subclass = pci_config_read8((u8)bus, slot, func, 0x0A);
              info->prog_if = pci_config_read8((u8)bus, slot, func, 0x09);
            }
            return 1;
          }
          found++;
        }
      }
    }
  }
  return 0;
}

void virtio_blk_init(void) {
  instance_count = 0;
  int dev_idx = 0;

  while (instance_count < MAX_VIRTIO_BLK) {
    struct pci_device_info pci_info;
    if (!virtio_find_nth_device(dev_idx, &pci_info))
      break;

    struct virtio_blk_instance *inst = &instances[instance_count];

    // Read BAR0 to get I/O port base (legacy virtio)
    u32 bar0 =
        pci_config_read32(pci_info.bus, pci_info.slot, pci_info.func, 0x10);
    if ((bar0 & 1) == 0) {
      dev_idx++;
      continue;
    }

    inst->dev.port_base = (u16)(bar0 & ~3);
    inst->dev.irq =
        pci_config_read8(pci_info.bus, pci_info.slot, pci_info.func, 0x3C);

    // Reset device
    virtio_set_status(&inst->dev, 0);
    virtio_set_status(&inst->dev,
                      VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    virtio_set_guest_features(&inst->dev, 0);
    virtio_set_status(&inst->dev, VIRTIO_STATUS_ACKNOWLEDGE |
                                      VIRTIO_STATUS_DRIVER |
                                      VIRTIO_STATUS_FEATURES_OK);

    if (!virtq_init(&inst->dev, 0, &inst->vq)) {
      console_write("virtio-blk: failed to initialize virtqueue for device ");
      console_write_dec(dev_idx);
      console_write("\n");
      dev_idx++;
      continue;
    }
    /* M70: leave the avail ring's interrupt enabled (no VRING_AVAIL_F_NO_INTERRUPT)
     * so the device raises a completion IRQ instead of us busy-polling used->idx.
     * Register the handler and unmask the line before DRIVER_OK so we never miss
     * the first completion. */
    inst->vq.avail->flags = 0;
    irq_register_handler(inst->dev.irq, virtio_blk_irq, inst);
    irq_unmask(inst->dev.irq);

    virtio_set_status(&inst->dev,
                      virtio_get_status(&inst->dev) | VIRTIO_STATUS_DRIVER_OK);

    inst->blk.block_size = 512;
    inst->blk.block_count =
        (u64)inl((u16)(inst->dev.port_base + VIRTIO_BLK_CONFIG_CAPACITY)) |
        ((u64)inl((u16)(inst->dev.port_base +
                        VIRTIO_BLK_CONFIG_CAPACITY + 4))
         << 32);
    inst->blk.read_blocks = virtio_blk_read;
    inst->blk.write_blocks = virtio_blk_write;
    inst->blk.priv = inst;
    /* Virtio disks are named the way every other Unix names them: vda, vdb, ...
     * out of the block layer's "vd" sequence. */
    blk_register_disk(&inst->blk, "vd", BLK_BUS_VIRTIO);
    if (!inst->blk.name) {
      dev_idx++;
      continue;
    }

    console_write("virtio-blk: registered ");
    console_write(inst->blk.name);
    console_write(" (PCI ");
    console_write_dec(pci_info.bus);
    console_write(":");
    console_write_dec(pci_info.slot);
    console_write(".");
    console_write_dec(pci_info.func);
    console_write(")\n");

    instance_count++;
    dev_idx++;
  }

  if (instance_count == 0) {
    console_write("virtio-blk: no device found\n");
  } else {
    console_write("virtio-blk: initialized ");
    console_write_dec(instance_count);
    console_write(" devices\n");
  }
}
