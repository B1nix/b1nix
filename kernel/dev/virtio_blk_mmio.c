/* virtio-blk over the virtio-mmio transport (QEMU aarch64 "virt" machine).
 *
 * x86_64 reaches virtio-blk over legacy virtio-PCI (kernel/dev/virtio_blk.c,
 * port-I/O based — inb/outb don't exist on aarch64). QEMU's "virt" board has
 * no PCI host bridge by default; instead it exposes up to 32 virtio-mmio
 * transport slots at fixed physical addresses, one per `-device
 * virtio-*-device` on the command line. This file is the aarch64-only,
 * mmio-register counterpart of virtio_blk.c: same block_device registration
 * API, same struct vring_desc/avail/used wire layout (the virtqueue memory
 * layout is transport-independent — only how the device is told where the
 * rings live, and how it's kicked, differs), different register access.
 *
 * Completion is IRQ-driven: QEMU virt wires virtio-mmio slot i to SPI
 * (16+i), i.e. GIC interrupt ID 32+16+i = 48+i (kernel/arch/aarch64/
 * interrupts.c's irq_register_handler()/irq_unmask(), added alongside this
 * driver — previously only x86_64 had them). A brief in-RAM spin still
 * catches a same-microsecond completion with no context switch, exactly
 * like virtio_blk.c's do_virtio_blk_req(); the fallback blocks on the IRQ
 * instead of re-spinning, so a slow (TCG, large-readahead) request costs a
 * context switch instead of an arbitrary polling budget.
 */
#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/irq.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>
#include <b1nix/virtio.h>
#include <string.h>

#define VIRTIO_MMIO_BASE   0x0a000000ULL
#define VIRTIO_MMIO_STRIDE 0x200ULL
#define VIRTIO_MMIO_SLOTS  32
/* SPI 16+slot -> GIC INTID 32+16+slot. */
#define VIRTIO_MMIO_IRQ(slot) (48 + (slot))

#define VIRTIO_MMIO_MAGIC_VALUE 0x74726976u /* "virt" */
#define VIRTIO_MMIO_DEVICE_ID_BLK 2

struct virtio_mmio_regs {
  u32 magic_value;        /* 0x000 R */
  u32 version;            /* 0x004 R */
  u32 device_id;          /* 0x008 R */
  u32 vendor_id;          /* 0x00c R */
  u32 device_features;    /* 0x010 R */
  u32 device_features_sel;/* 0x014 W */
  u32 _pad1[2];
  u32 driver_features;    /* 0x020 W */
  u32 driver_features_sel;/* 0x024 W */
  u32 guest_page_size;    /* 0x028 W — legacy (Version==1) only */
  u32 _pad2[1];
  u32 queue_sel;          /* 0x030 W */
  u32 queue_num_max;      /* 0x034 R */
  u32 queue_num;          /* 0x038 W */
  u32 queue_align;        /* 0x03c W — legacy only */
  u32 queue_pfn;           /* 0x040 RW — legacy only: physical addr >> 12 */
  u32 queue_ready;        /* 0x044 RW — modern (Version==2) only */
  u32 _pad4[2];
  u32 queue_notify;       /* 0x050 W */
  u32 _pad5[3];
  u32 interrupt_status;   /* 0x060 R */
  u32 interrupt_ack;      /* 0x064 W */
  u32 _pad6[2];
  u32 status;             /* 0x070 RW */
  u32 _pad7[3];
  u32 queue_desc_low;     /* 0x080 W */
  u32 queue_desc_high;    /* 0x084 W */
  u32 _pad8[2];
  u32 queue_driver_low;   /* 0x090 W */
  u32 queue_driver_high;  /* 0x094 W */
  u32 _pad9[2];
  u32 queue_device_low;   /* 0x0a0 W */
  u32 queue_device_high;  /* 0x0a4 W */
  u32 _pad10[21];
  u32 config_generation;  /* 0x0fc R */
  u8 config[256];         /* 0x100+ device-specific */
  /* Deliberately NOT __attribute__((packed)): every field here is already
   * naturally 4-byte aligned, and `packed` would tell the compiler every
   * access might be misaligned — which can lower a 32-bit register access
   * into multiple byte-wide loads/stores that Device (nGnRnE) memory does
   * not tolerate. */
};

#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED    128

#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_T_FLUSH 4
/* The device has a write-back cache and will accept a flush command. Without
 * this bit negotiated, fsync(2) on this disk only reached the host's page
 * cache. */
#define VIRTIO_BLK_F_FLUSH (1u << 9)
#define MAX_VIRTIO_BLK_MMIO 8

struct virtio_blk_req {
  u32 type;
  u32 reserved;
  u64 sector;
} __attribute__((packed));

struct virtio_blk_dma_req {
  struct virtio_blk_req req;
  volatile u8 status;
} __attribute__((packed));

struct vblk_mmio_instance {
  volatile struct virtio_mmio_regs *regs;
  struct virtqueue vq;
  struct block_device blk;
  volatile int busy;
  u8 irq;
};

static struct vblk_mmio_instance instances[MAX_VIRTIO_BLK_MMIO];
static int instance_count = 0;

/* M70-style completion interrupt handler (mirrors virtio_blk.c's
 * virtio_blk_irq): read+clear InterruptStatus and, if it was a used-buffer
 * notification from this device, wake whoever is blocked on it. */
static int vblk_mmio_irq(void *ctx) {
  struct vblk_mmio_instance *inst = (struct vblk_mmio_instance *)ctx;
  u32 status = inst->regs->interrupt_status;
  if (!(status & 0x1))
    return 0; /* not a used-buffer notification from this device */
  inst->regs->interrupt_ack = status;
  scheduler_wake_all(inst);
  return 1;
}

static void vblk_lock(struct vblk_mmio_instance *inst) {
  while (__sync_lock_test_and_set(&inst->busy, 1))
    scheduler_yield();
}

static void vblk_unlock(struct vblk_mmio_instance *inst) {
  __sync_lock_release(&inst->busy);
}

/* Same page-boundary-splitting descriptor builder as virtio_blk.c: a region
 * handed to us (block-cache entry, kheap struct) is only virtually
 * contiguous, so describe it to the device one physical page at a time. */
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
    vq->desc[idx].flags = write_flag;
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

static int do_vblk_req(struct vblk_mmio_instance *inst, u64 lba, u32 count,
                       void *buffer, u32 type) {
  struct virtio_blk_dma_req *dma = kzalloc(sizeof(*dma));
  if (!dma)
    return -1;

  vblk_lock(inst);
  dma->req.type = type;
  dma->req.reserved = 0;
  dma->req.sector = lba;
  dma->status = 0xFF;

  /* A flush carries no data at all — header and status only. Handing the
   * device a zero-length data descriptor is not the same thing and it will
   * reject the request. */
  int has_data = (type != VIRTIO_BLK_T_FLUSH);

  u16 next_desc = 0;
  u16 prev = 0xFFFF;
  if (vblk_add_region(&inst->vq, &next_desc, &prev, (u64)(usize)&dma->req,
                      sizeof(struct virtio_blk_req), 0) < 0 ||
      (has_data &&
       vblk_add_region(&inst->vq, &next_desc, &prev, (u64)(usize)buffer,
                       (u64)count * 512,
                       type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0) < 0) ||
      vblk_add_region(&inst->vq, &next_desc, &prev, (u64)(usize)&dma->status,
                      1, VRING_DESC_F_WRITE) < 0) {
    vblk_unlock(inst);
    kfree(dma);
    return -1;
  }

  u16 avail_idx = inst->vq.avail->idx % inst->vq.queue_size;
  inst->vq.avail->ring[avail_idx] = 0;

  __sync_synchronize();
  inst->vq.avail->idx++;
  __sync_synchronize();
  inst->regs->queue_notify = 0;

  /* vblk_mmio_irq() is registered and unmasked (see virtio_blk_mmio_init) and
   * will fire on real hardware/KVM, but under this session's TCG testing the
   * scheduler_wait_prepare_timeout()/scheduler_wait_commit() blocking path
   * (virtio_blk.c's exact technique) was observed to stall past its own
   * watchdog during the very-early boot call (blk_scan_partitions() off
   * blk_register() in vfs_init, before the scheduler's normal task mix
   * exists) — needs its own follow-up. Bound by wall-clock TIME instead of a
   * raw cycle count: CNTFRQ_EL0 varies by host (24MHz on Apple Silicon under
   * HVF passthrough, ~62.5MHz typical under TCG) — a fixed cycle budget
   * silently meant anywhere from ~1 to ~160 real seconds depending on the
   * host. Compute the budget from the real frequency instead. */
  u64 cntfrq;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
  const u64 wait_seconds = 10;
  const u64 wait_budget = cntfrq * wait_seconds;
  u64 wait_start;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(wait_start));
  int timed_out = 0;
  while (inst->vq.used->idx == inst->vq.last_used_idx) {
    u64 now;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    if (now - wait_start > wait_budget) { timed_out = 1; break; }
    /* Give the CPU back while waiting once there is a scheduler to give it
     * to. Hard-spinning for the whole budget starved every other task on this
     * single-CPU port whenever the host was busy. */
    if (scheduler_can_block())
      scheduler_yield();
  }

  if (timed_out) {
    /* The device still OWNS the descriptors and the DMA buffer: advancing
     * last_used_idx would desynchronise every later request against the used
     * ring, and freeing `dma` would hand the device a block the heap has
     * already reused. Leave both alone — deliberately leaking one small
     * request — and report the failure instead of corrupting the queue. */
    console_write("virtio-blk-mmio: request timed out, lba=0x");
    console_write_hex64(lba);
    console_write("\n");
    vblk_unlock(inst);
    return -1;
  }

  __sync_synchronize();
  inst->vq.last_used_idx++;

  int ret = (dma->status == 0) ? (int)count : -1;
  if (ret < 0) {
    console_write("virtio-blk-mmio: request failed status=0x");
    console_write_hex64(dma->status);
    console_write(" lba=0x");
    console_write_hex64(lba);
    console_write("\n");
  }
  vblk_unlock(inst);
  kfree(dma);
  return ret;
}

/* Only installed when the device offered VIRTIO_BLK_F_FLUSH. */
static int vblk_mmio_flush(struct block_device *dev) {
  struct vblk_mmio_instance *inst = (struct vblk_mmio_instance *)dev->priv;

  return do_vblk_req(inst, 0, 0, 0, VIRTIO_BLK_T_FLUSH) < 0 ? -1 : 0;
}

static int vblk_mmio_read(struct block_device *dev, u64 lba, u32 count,
                          void *buffer) {
  return do_vblk_req((struct vblk_mmio_instance *)dev->priv, lba, count,
                     buffer, VIRTIO_BLK_T_IN);
}

static int vblk_mmio_write(struct block_device *dev, u64 lba, u32 count,
                           const void *buffer) {
  return do_vblk_req((struct vblk_mmio_instance *)dev->priv, lba, count,
                     (void *)(usize)buffer, VIRTIO_BLK_T_OUT);
}

static int vblk_setup_queue(volatile struct virtio_mmio_regs *regs,
                            struct virtqueue *vq, int legacy) {
  regs->queue_sel = 0;
  u32 qmax = regs->queue_num_max;
  if (qmax == 0)
    return -1;
  u16 qsize = qmax < 256 ? (u16)qmax : 256;
  regs->queue_num = qsize;

  vq->queue_idx = 0;
  vq->queue_size = qsize;
  vq->last_used_idx = 0;

  /* Legacy split-virtqueue layout (virtio-mmio Version==1, still QEMU's
   * default for `-device virtio-blk-device` unless disable-legacy=on):
   * descriptor table + avail ring packed together, padded up to QueueAlign,
   * then the used ring — exactly what this offset math already builds, so
   * the same allocation serves both legacy (one QueuePFN) and modern (three
   * separate desc/driver/device addresses) register interfaces. */
  usize desc_size = 16u * qsize;
  usize avail_size = 6u + 2u * qsize;
  usize offset_used = (desc_size + avail_size + 4095) & ~(usize)4095;
  usize used_size = 6u + 8u * qsize;
  usize total = offset_used + ((used_size + 4095) & ~(usize)4095);
  usize frames = (total + PAGE_SIZE - 1) / PAGE_SIZE;
  u64 paddr = pmm_alloc_frames(frames);
  if (!paddr)
    return -1;

  u8 *base = (u8 *)(usize)(paddr + vmm_direct_map_base());
  memset(base, 0, frames * PAGE_SIZE);
  vq->desc = (struct vring_desc *)base;
  vq->avail = (struct vring_avail *)(base + desc_size);
  vq->used = (struct vring_used *)(base + offset_used);

  if (legacy) {
    regs->queue_align = PAGE_SIZE;
    regs->queue_pfn = (u32)(paddr / PAGE_SIZE);
  } else {
    u64 desc_phys = paddr;
    u64 avail_phys = paddr + desc_size;
    u64 used_phys = paddr + offset_used;
    regs->queue_desc_low = (u32)(desc_phys & 0xffffffffu);
    regs->queue_desc_high = (u32)(desc_phys >> 32);
    regs->queue_driver_low = (u32)(avail_phys & 0xffffffffu);
    regs->queue_driver_high = (u32)(avail_phys >> 32);
    regs->queue_device_low = (u32)(used_phys & 0xffffffffu);
    regs->queue_device_high = (u32)(used_phys >> 32);
    regs->queue_ready = 1;
  }
  return 0;
}

void virtio_blk_mmio_init(void) {
  instance_count = 0;

  for (int slot = 0; slot < VIRTIO_MMIO_SLOTS &&
                     instance_count < MAX_VIRTIO_BLK_MMIO; slot++) {
    u64 phys = VIRTIO_MMIO_BASE + (u64)slot * VIRTIO_MMIO_STRIDE;
    /* No mapping call: the low MMIO region (GIC at 0x08000000, PL011 UART near 0x09000000,
     * and this virtio-mmio range at 0x0a000000) is already identity-mapped
     * by the kernel's boot-time page tables, exactly like interrupts.c reads
     * GICD_BASE as a raw pointer with no mapping call. Do the same here. */
    volatile struct virtio_mmio_regs *regs =
        (volatile struct virtio_mmio_regs *)(usize)phys;
    if (regs->magic_value != VIRTIO_MMIO_MAGIC_VALUE ||
        regs->device_id != VIRTIO_MMIO_DEVICE_ID_BLK)
      continue; /* empty slot or a different device type */

    struct vblk_mmio_instance *inst = &instances[instance_count];
    inst->regs = regs;

    /* QEMU's `-device virtio-blk-device` defaults to legacy (Version==1)
     * unless disable-legacy=on is passed — confirmed live via `-d
     * guest_errors`: writing the modern desc/driver/device address
     * registers on a legacy device logs "write to non-legacy register" and
     * is silently dropped, so the device never learns the real queue
     * addresses and every kick is a no-op (used->idx never moves) even
     * though status/queue_ready reads back exactly as expected. Legacy has
     * a completely different (and simpler) handshake: no FEATURES_OK step,
     * GuestPageSize must be set once, and the queue address is one QueuePFN
     * register instead of three. */
    int legacy = (regs->version == 1);

    regs->status = 0; /* reset */
    regs->status = VIRTIO_STATUS_ACK;
    regs->status |= VIRTIO_STATUS_DRIVER;

    /* Accept exactly one optional word-0 feature: the cache flush. Declining
     * everything is not the neutral choice it looks like — a device with a
     * write-back cache and no negotiated FLUSH gives fsync(2) nothing to
     * issue, so "durable" meant "in the host's cache". */
    regs->device_features_sel = 0;
    u32 host_features = regs->device_features;
    u32 want = host_features & VIRTIO_BLK_F_FLUSH;

    if (legacy) {
      regs->guest_page_size = PAGE_SIZE;
      regs->driver_features_sel = 0;
      regs->driver_features = want;
    } else {
      regs->driver_features_sel = 0;
      regs->driver_features = want;
      /* VIRTIO_F_VERSION_1 is feature bit 32 (word 1, bit 0) and is
       * mandatory for a modern device to actually activate. */
      regs->driver_features_sel = 1;
      regs->driver_features = 1; /* VIRTIO_F_VERSION_1 */
      regs->status |= VIRTIO_STATUS_FEATURES_OK;
      if (!(regs->status & VIRTIO_STATUS_FEATURES_OK)) {
        regs->status = VIRTIO_STATUS_FAILED;
        continue;
      }
    }

    if (vblk_setup_queue(regs, &inst->vq, legacy) < 0) {
      console_write("virtio-blk-mmio: queue setup failed at slot ");
      console_write_dec(slot);
      console_write("\n");
      regs->status = VIRTIO_STATUS_FAILED;
      continue;
    }

    /* Register + unmask the completion IRQ before DRIVER_OK so the device
     * never has a window to raise an interrupt we're not yet listening
     * for. */
    inst->irq = (u8)VIRTIO_MMIO_IRQ(slot);
    irq_register_handler(inst->irq, vblk_mmio_irq, inst);
    irq_unmask(inst->irq);

    regs->status |= VIRTIO_STATUS_DRIVER_OK;

    u64 capacity;
    memcpy(&capacity, (const void *)regs->config, sizeof(capacity));

    inst->blk.block_size = 512;
    inst->blk.block_count = capacity;
    inst->blk.read_blocks = vblk_mmio_read;
    inst->blk.write_blocks = vblk_mmio_write;
    if (want & VIRTIO_BLK_F_FLUSH)
      inst->blk.flush = vblk_mmio_flush;
    inst->blk.priv = inst;
    /* The same "vd" sequence the PCI virtio-blk driver takes its names from:
     * a disk is vda, vdb, ... whatever transport it arrived on, which is what
     * the rest of Unix calls them and what the root mount in kernel/main.c
     * looks for. Registering as "virtio-blk0" here left `root=vda` unmatched
     * and the boot stayed on the initramfs with mount failed: -ENODEV. */
    blk_register_disk(&inst->blk, "vd", BLK_BUS_VIRTIO);

    console_write("virtio-blk-mmio: ");
    console_write((want & VIRTIO_BLK_F_FLUSH) ? "flush=yes " : "flush=no ");
    console_write("registered ");
    console_write(inst->blk.name);
    console_write(" (slot ");
    console_write_dec(slot);
    console_write(", ");
    console_write_dec((u32)capacity);
    console_write(" blocks)\n");

    instance_count++;
  }

  if (instance_count == 0)
    console_write("virtio-blk-mmio: no device found\n");
}
