#include <b1nix/arch.h>
#include <b1nix/blk.h>
#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/io.h>
#include <b1nix/irq.h>
#include <b1nix/klog.h>
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
/* One tick, not fifty.
 *
 * The comment above calls this "never reached on a healthy device", and that
 * assumption set it to half a second. It is reached: under a browser's write
 * load a request regularly finishes without the waiter being woken, and every
 * one of those cost 500 ms — while holding this instance's lock, so the whole
 * system queued behind it. The bound belongs at the scale of the device's own
 * latency (microseconds), where a missed wake-up costs one tick instead of
 * stalling every task that wants the disk. */
#define VIRTIO_BLK_IO_WATCHDOG_TICKS 1

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_BLK_DEVICE_ID 0x1001

/* Request types (the `type` field of the request header). */
#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_T_FLUSH 4
#define VIRTIO_BLK_T_DISCARD 11
#define VIRTIO_BLK_T_WRITE_ZEROES 13

/* Feature bits, legacy (32-bit) feature word. Note these are NOT the request
 * type numbers above — VIRTIO_BLK_T_FLUSH is request type 4 but is gated on
 * feature bit 9. */
#define VIRTIO_BLK_F_SEG_MAX (1u << 2)
#define VIRTIO_BLK_F_GEOMETRY (1u << 4)
#define VIRTIO_BLK_F_RO (1u << 5)
#define VIRTIO_BLK_F_BLK_SIZE (1u << 6)
#define VIRTIO_BLK_F_FLUSH (1u << 9)
#define VIRTIO_BLK_F_DISCARD (1u << 13)
#define VIRTIO_BLK_F_WRITE_ZEROES (1u << 14)

/* Device configuration space. On a legacy virtio-PCI device without MSI-X it
 * starts at BAR0 + 0x14; every offset below is relative to that. */
#define VIRTIO_BLK_CONFIG_CAPACITY 0x14
#define VIRTIO_BLK_CONFIG_SEG_MAX (VIRTIO_BLK_CONFIG_CAPACITY + 12)
#define VIRTIO_BLK_CONFIG_BLK_SIZE (VIRTIO_BLK_CONFIG_CAPACITY + 20)
#define VIRTIO_BLK_CONFIG_MAX_DISCARD_SECTORS (VIRTIO_BLK_CONFIG_CAPACITY + 36)
#define VIRTIO_BLK_CONFIG_MAX_WZ_SECTORS (VIRTIO_BLK_CONFIG_CAPACITY + 48)

/* Support multiple virtio-blk devices */
#define MAX_VIRTIO_BLK 16

struct virtio_blk_instance {
  struct virtio_device dev;
  struct virtqueue vq;
  struct block_device blk;
  volatile int busy;
  /* What the device and the driver both agreed to, not what either wished for. */
  u32 features;
  /* Largest number of 512-byte blocks one request may carry. Derived from the
   * queue size and, when the device states one, its seg_max: a descriptor chain
   * longer than the device advertises is outside the contract even if QEMU
   * happens to tolerate it. */
  u32 max_blocks_per_req;
  /* 0 when the device did not offer WRITE ZEROES or stated no limit. */
  u32 max_write_zeroes_sectors;
  /* Same, for DISCARD. */
  u32 max_discard_sectors;
};

static struct virtio_blk_instance instances[MAX_VIRTIO_BLK];
static int instance_count = 0;

struct virtio_blk_req {
  u32 type;
  u32 reserved;
  u64 sector;
} __attribute__((packed));

/* Payload of a DISCARD / WRITE ZEROES request — the device reads this the way
 * it reads the data of an ordinary write. */
struct virtio_blk_zero_range {
  u64 sector;
  u32 num_sectors;
  u32 flags;
} __attribute__((packed));

struct virtio_blk_dma_req {
  struct virtio_blk_req req;
  struct virtio_blk_zero_range zero;
  volatile u8 status;
} __attribute__((packed));

/* Holding this lock is a kernel critical section in the strict sense: a task
 * killed while it holds the device never releases it, and every other task then
 * spins here forever. Tell the scheduler to keep a fatal signal pending until
 * the request is done — that is the whole reason the counter exists. */
static void virtio_blk_lock(struct virtio_blk_instance *inst) {
  /* The guard goes up AFTER the lock is held, not before.
   *
   * Waiting for the lock is not a critical section — nothing is held yet, and
   * a task killed here leaves nothing behind. Raising the guard around the wait
   * instead made a task that merely wanted the disk unkillable for as long as
   * somebody else had it: the fatal signal stayed pending, the loop kept
   * yielding, and `timeout` could not end the process it was watching. */
  while (__sync_lock_test_and_set(&inst->busy, 1)) {
    scheduler_yield();
  }
  scheduler_kcrit_enter();
}

static void virtio_blk_unlock(struct virtio_blk_instance *inst) {
  __sync_lock_release(&inst->busy);
  scheduler_kcrit_leave();
}

/* M70: completion interrupt handler. Runs in IRQ context — read the device ISR
 * (which deasserts the level-triggered line), and if this device posted a used
 * buffer, wake the task blocked in do_virtio_blk_req(). The instance pointer is
 * the wait channel. Returns 1 if the interrupt was ours (shared-line aware). */
/* Completion interrupts actually seen, and calls into this handler that were
 * not ours. A request that only ever finishes when the watchdog re-polls looks
 * identical to a slow device; these two counters are what tell the difference,
 * so the stall report prints them. */
static volatile u64 vblk_irq_completions;
static volatile u64 vblk_irq_foreign;
/* Stall reports already printed, and the two limits that bound them: a report
 * is worth making once the wait is clearly not ordinary latency, and worth
 * making only a handful of times. */
static volatile u64 vblk_stall_reports;
#define VIRTIO_BLK_STALL_REPORT_TICKS 200 /* 2 s at 10 ms a tick */
#define VIRTIO_BLK_STALL_REPORT_MAX   8

static int virtio_blk_irq(void *ctx) {
  struct virtio_blk_instance *inst = (struct virtio_blk_instance *)ctx;
  u8 isr = virtio_read_isr(&inst->dev); /* read clears the ISR status */
  if (!(isr & 0x1)) {
    vblk_irq_foreign++;
    return 0; /* not a used-buffer notification from this device */
  }
  vblk_irq_completions++;
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
    u64 phys = vmm_virt_to_phys((void *)(usize)vaddr);
    /* No translation means no descriptor. Physical 0 is the real-mode IVT, not
     * a buffer: publishing it would aim the device's DMA engine at the bottom
     * of memory and corrupt the machine rather than fail the I/O. A kernel
     * buffer that cannot be translated is a page-table problem upstream of this
     * driver — fail the request so the caller sees an error. */
    if (phys == 0) {
      static unsigned reported;
      if (reported < 8) {
        console_write("virtio-blk: untranslatable buffer at 0x");
        console_write_hex64(vaddr);
        console_write(" len ");
        console_write_dec(len);
        console_write(" — request failed\n");
        /* Whose buffer it was decides what this is. An address that was mapped
         * when the request was submitted and is not mapped now means the buffer
         * was freed under I/O still in flight; a buffer that was never mapped
         * means the caller passed an address it does not own. Name the caller
         * and the task so the two can be told apart. */
        console_write("  submitted by 0x");
        console_write_hex64((u64)(usize)__builtin_return_address(0));
        ksym_print((u64)(usize)__builtin_return_address(0));
        if (current_task) {
          console_write("\n  task ");
          console_write_dec(current_task->id);
          console_write(" ");
          console_write(current_task->name);
        }
        console_write("\n");
        reported++;
      }
      return -1;
    }
    u16 idx = (*next_desc)++;
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

volatile u64 vblk_reads, vblk_read_sectors, vblk_writes, vblk_write_sectors,
    vblk_flushes;

static int do_virtio_blk_req(struct virtio_blk_instance *inst, u64 lba,
                             u32 count, void *buffer, u32 type) {
  struct virtio_blk_dma_req *dma = kzalloc(sizeof(struct virtio_blk_dma_req));
  if (!dma)
    return -1;

  virtio_blk_lock(inst);
  dma->req.type = type;
  dma->req.reserved = 0;
  /* FLUSH and WRITE ZEROES carry no sector in the header — the range of a
   * WRITE ZEROES lives in its payload, and a FLUSH covers the whole device. */
  dma->req.sector = (type == VIRTIO_BLK_T_IN || type == VIRTIO_BLK_T_OUT) ? lba : 0;
  dma->status = 0xFF;

  /* What the middle (data) region of the chain describes, per request type:
   *   IN            — the caller's buffer, written by the device
   *   OUT           — the caller's buffer, read by the device
   *   FLUSH         — nothing at all
   *   WRITE_ZEROES  — the range descriptor, read by the device */
  void *data = buffer;
  u64 data_len = (u64)count * 512;
  u16 data_flags = (type == VIRTIO_BLK_T_IN) ? VRING_DESC_F_WRITE : 0;
  if (type == VIRTIO_BLK_T_FLUSH) {
    data = 0;
    data_len = 0;
  } else if (type == VIRTIO_BLK_T_WRITE_ZEROES ||
             type == VIRTIO_BLK_T_DISCARD) {
    dma->zero.sector = lba;
    dma->zero.num_sectors = count;
    /* flags: bit 0 (unmap) is defined for WRITE ZEROES only, and asking for it
     * would let the blocks read back as something other than zeroes. DISCARD
     * defines no flags at all, so 0 is right for both. */
    dma->zero.flags = 0;
    data = &dma->zero;
    data_len = sizeof(dma->zero);
    data_flags = 0;
  }

  /* Build the descriptor chain: header (device reads), data (device writes for
   * reads / reads for writes), status (device writes). Each region is split at
   * page boundaries so the device never crosses into an unrelated frame. The
   * chain head is descriptor 0 (next_desc starts at 0). */
  u16 next_desc = 0;
  u16 prev = 0xFFFF;
  if (vblk_add_region(&inst->vq, &next_desc, &prev, (u64)(usize)&dma->req,
                      sizeof(struct virtio_blk_req), 0) < 0 ||
      (data_len &&
       vblk_add_region(&inst->vq, &next_desc, &prev, (u64)(usize)data,
                       (u32)data_len, data_flags) < 0) ||
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
  /* Spin for about a millisecond before giving up on the fast path.
   *
   * Four thousand pauses is roughly thirty microseconds, and the device does
   * not answer that quickly: the request then blocks, and the shortest sleep
   * this kernel can take is one 10 ms tick. Two per cent of requests took that
   * sleep, and the ones whose completion interrupt went astray took thirty-two
   * of them — a third of a second each, which is what made a demand-paged fault
   * cost milliseconds instead of microseconds and made two runs of the same
   * workload differ a hundredfold.
   *
   * A busy wait of a millisecond is worth trading for a sleep of ten: it costs
   * at most a millisecond of one core, and it catches a completion that a lost
   * interrupt would otherwise leave for the watchdog. */
  /* Reads wait; writes and flushes do not.
   *
   * A read is a page the faulting thread cannot continue without, and the
   * measurement says spinning for it is right: the stalls went to zero and a
   * demand-paging fault went from milliseconds to 0.16 ms. A write or a flush
   * is nobody's critical path in the same way, and spinning a millisecond on
   * every one of them made fdatasync and ftruncate markedly worse — so those
   * keep the short spin and take the sleep. */
  /* Bounded in time, not in iterations.
   *
   * "Three hundred thousand pauses" is a number whose meaning changes with
   * every processor it runs on: what was a millisecond on the machine it was
   * tuned against is half that on a faster one and twice on a slower. The
   * clock this kernel already calibrates at boot says how long a wait actually
   * is, so the budget is expressed as one — a millisecond for a read, whose
   * requester cannot continue without it, and a short one for a write or a
   * flush, which is nobody's critical path and whose long spin made fdatasync
   * markedly worse. */
  {
    u64 budget_ns = (type == VIRTIO_BLK_T_IN) ? 1000000ull : 15000ull;
    u64 spin_start = arch_tsc_monotonic_ns();

    for (;;) {
      if (inst->vq.used->idx != inst->vq.last_used_idx)
        break;
      cpu_relax(); /* not a bare `pause`: x86-only mnemonic */
      /* The clock read is a counter read and a multiply, so it is checked
       * every few hundred pauses rather than on each one. */
      static const unsigned check_every = 256;
      static unsigned since_check;

      if (++since_check < check_every)
        continue;
      since_check = 0;
      if (arch_tsc_monotonic_ns() - spin_start >= budget_ns)
        break;
    }
  }
  /* What a request actually costs, summarised rather than sampled.
   *
   * The stall reports name individual slow requests but say nothing about the
   * common case, and the common case is what a browser's thousands of small
   * synchronous writes are made of. Count every request, how many were still
   * unfinished after the in-RAM spin, and how many scheduler ticks were spent
   * waiting for those — printed periodically so the cost of the disk can be
   * compared between runs instead of guessed at. */
  static volatile u64 vblk_reqs, vblk_slow, vblk_wait_ticks, vblk_sectors;
  u64 wait_start = scheduler_get_ticks();

  vblk_reqs++;
  vblk_sectors += count;
  /* Split by direction and by size: "a quarter of a million requests" says
   * nothing about which half of the system to fix, and an average of 738 bytes
   * per request could be a stream of single sectors or a mix of one huge read
   * with many tiny writes. */
  {
    extern volatile u64 vblk_reads, vblk_read_sectors, vblk_writes,
        vblk_write_sectors, vblk_flushes;

    if (type == VIRTIO_BLK_T_IN) {
      vblk_reads++;
      vblk_read_sectors += count;
    } else if (type == VIRTIO_BLK_T_FLUSH) {
      vblk_flushes++;
    } else {
      vblk_writes++;
      vblk_write_sectors += count;
    }
  }
  if (inst->vq.used->idx == inst->vq.last_used_idx)
    vblk_slow++;

  unsigned waits = 0;
  while (inst->vq.used->idx == inst->vq.last_used_idx) {
    if (!scheduler_can_block()) {
      scheduler_yield();
      continue;
    }
    /* A request the device never completes holds this instance's lock for as
     * long as it waits, and every other task then spins in virtio_blk_lock —
     * the whole system stops on one descriptor chain. The watchdog re-poll
     * alone cannot say why, so once the wait is clearly not ordinary latency,
     * describe the request and the queue indices that should have moved. */
    /* Report by ELAPSED TIME, not by poll count, and only a few times.
     *
     * In a context that cannot block, the loop below spins on yield, so a
     * request that takes a few milliseconds racks up dozens of "waits" and
     * printed a stall report for each one. Six thousand of those reports went
     * out of a 115200-baud serial port during one btrfs commit — the printing
     * itself then WAS the delay, and the message that exists to explain a
     * stall was manufacturing one. A real stall still says so. */
    if (scheduler_get_ticks() - wait_start > VIRTIO_BLK_STALL_REPORT_TICKS &&
        vblk_stall_reports < VIRTIO_BLK_STALL_REPORT_MAX && ++waits == 1) {
      vblk_stall_reports++;
      console_write("virtio-blk: request not completing: type ");
      console_write_dec(type);
      console_write(" lba ");
      console_write_dec(lba);
      console_write(" count ");
      console_write_dec(count);
      console_write(" descs ");
      console_write_dec(next_desc);
      console_write("\n  avail->idx ");
      console_write_dec(inst->vq.avail->idx);
      console_write(" used->idx ");
      console_write_dec(inst->vq.used->idx);
      console_write(" last_used ");
      console_write_dec(inst->vq.last_used_idx);
      console_write(" queue_size ");
      console_write_dec(inst->vq.queue_size);
      console_write("\n  irq ");
      console_write_dec(inst->dev.irq);
      console_write(" completions ");
      console_write_dec(vblk_irq_completions);
      console_write(" foreign ");
      console_write_dec(vblk_irq_foreign);
      console_write("\n");
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

  vblk_wait_ticks += scheduler_get_ticks() - wait_start;
  if ((vblk_reqs % 20000) == 0) {
    console_write("virtio-blk: ");
    console_write_dec(vblk_reqs);
    console_write(" requests, ");
    console_write_dec(vblk_slow);
    console_write(" needed a wait, ");
    console_write_dec(vblk_wait_ticks);
    console_write(" ticks spent waiting (10 ms each), ");
    console_write_dec(vblk_sectors / 2048);
    console_write(" MiB moved, ");
    console_write_dec(vblk_irq_completions);
    console_write(" completion interrupts; reads ");
    console_write_dec(vblk_reads);
    console_write("/");
    console_write_dec(vblk_read_sectors);
    console_write(" sectors, writes ");
    console_write_dec(vblk_writes);
    console_write("/");
    console_write_dec(vblk_write_sectors);
    console_write(" sectors, flushes ");
    console_write_dec(vblk_flushes);
    console_write("\n");
  }

  int ret = (dma->status != 0) ? -1
            : (type == VIRTIO_BLK_T_IN || type == VIRTIO_BLK_T_OUT) ? (int)count
                                                                    : 0;
  virtio_blk_unlock(inst);
  kfree(dma);
  return ret;
}

/* Split a transfer into requests no longer than the descriptor chain the device
 * agreed to accept, and run them in order. The block layer's bulk path asks for
 * up to 1536 blocks at once, which needs ~193 descriptors — more than the 128
 * segments QEMU states in seg_max. */
static int virtio_blk_rw(struct virtio_blk_instance *inst, u64 lba, u32 count,
                         void *buffer, u32 type) {
  u32 done = 0;
  while (done < count) {
    u32 chunk = count - done;
    if (chunk > inst->max_blocks_per_req)
      chunk = inst->max_blocks_per_req;
    if (do_virtio_blk_req(inst, lba + done, chunk,
                          (void *)((u8 *)buffer + (u64)done * 512), type) < 0)
      return -1;
    done += chunk;
  }
  return (int)count;
}

static int virtio_blk_read(struct block_device *dev, u64 lba, u32 count,
                           void *buffer) {
  struct virtio_blk_instance *inst = (struct virtio_blk_instance *)dev->priv;
  return virtio_blk_rw(inst, lba, count, buffer, VIRTIO_BLK_T_IN);
}

static int virtio_blk_write(struct block_device *dev, u64 lba, u32 count,
                            const void *buffer) {
  struct virtio_blk_instance *inst = (struct virtio_blk_instance *)dev->priv;
  return virtio_blk_rw(inst, lba, count, (void *)(usize)buffer,
                       VIRTIO_BLK_T_OUT);
}

/* Only installed when the device offered VIRTIO_BLK_F_FLUSH — a device without
 * it has no volatile write cache to empty, so its writes are already durable
 * once acknowledged. Called from the block layer's fsync/sync/umount path. */
static int virtio_blk_flush_op(struct block_device *dev) {
  struct virtio_blk_instance *inst = (struct virtio_blk_instance *)dev->priv;
  return do_virtio_blk_req(inst, 0, 0, 0, VIRTIO_BLK_T_FLUSH);
}

/* Only installed when the device offered VIRTIO_BLK_F_WRITE_ZEROES. Lets the
 * device zero a range itself instead of us DMA-ing a zero-filled buffer at it.
 * Callers must go through blk_zero_blocks(), which keeps the block cache
 * coherent with a write the cache never saw. */
/* M109 DISCARD. Same payload and the same one-command-at-a-time path as WRITE
 * ZEROES; the difference is only that the device is told the range is free
 * rather than told to make it read as zeroes. */
static int virtio_blk_discard_op(struct block_device *dev, u64 lba, u32 count) {
  struct virtio_blk_instance *inst = (struct virtio_blk_instance *)dev->priv;
  u32 done = 0;
  while (done < count) {
    u32 chunk = count - done;
    if (inst->max_discard_sectors && chunk > inst->max_discard_sectors)
      chunk = inst->max_discard_sectors;
    if (do_virtio_blk_req(inst, lba + done, chunk, 0, VIRTIO_BLK_T_DISCARD) < 0)
      return -1;
    done += chunk;
  }
  return 0;
}

static int virtio_blk_write_zeroes_op(struct block_device *dev, u64 lba,
                                      u32 count) {
  struct virtio_blk_instance *inst = (struct virtio_blk_instance *)dev->priv;
  u32 done = 0;
  while (done < count) {
    u32 chunk = count - done;
    if (inst->max_write_zeroes_sectors && chunk > inst->max_write_zeroes_sectors)
      chunk = inst->max_write_zeroes_sectors;
    if (do_virtio_blk_req(inst, lba + done, chunk, 0,
                          VIRTIO_BLK_T_WRITE_ZEROES) < 0)
      return -1;
    done += chunk;
  }
  return 0;
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
    inst->dev.irq = pci_intx_line(pci_info.bus, pci_info.slot, pci_info.func);

    // Reset device
    virtio_set_status(&inst->dev, 0);
    virtio_set_status(&inst->dev,
                      VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    /* Negotiate: take what the device offers, out of what this driver actually
     * implements. Writing 0 here — which is what we used to do — is not a
     * neutral choice: it declines the write cache flush the device is offering,
     * so there is no way to make a write durable, and it declines seg_max, so
     * we never learn how long a descriptor chain the device will accept. */
    u32 host_features = virtio_get_host_features(&inst->dev);
    u32 wanted = VIRTIO_BLK_F_SEG_MAX | VIRTIO_BLK_F_BLK_SIZE |
                 VIRTIO_BLK_F_RO | VIRTIO_BLK_F_FLUSH |
                 VIRTIO_BLK_F_DISCARD | VIRTIO_BLK_F_WRITE_ZEROES;
    inst->features = host_features & wanted;
    virtio_set_guest_features(&inst->dev, inst->features);
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

    /* Capacity is always counted in 512-byte sectors, whatever blk_size says,
     * and the sector number in a request always is too — so the block layer
     * keeps addressing this device in 512-byte units. */
    inst->blk.block_size = 512;
    inst->blk.block_count =
        (u64)inl((u16)(inst->dev.port_base + VIRTIO_BLK_CONFIG_CAPACITY)) |
        ((u64)inl((u16)(inst->dev.port_base +
                        VIRTIO_BLK_CONFIG_CAPACITY + 4))
         << 32);

    /* How long a chain may get. Every request spends one descriptor on the
     * header and one on the status byte; a data region of N bytes needs at most
     * N/4096 + 1 descriptors because it is split at physical page boundaries. */
    u32 max_data_desc = inst->vq.queue_size > 2 ? inst->vq.queue_size - 2u : 1u;
    if (inst->features & VIRTIO_BLK_F_SEG_MAX) {
      u32 seg_max =
          inl((u16)(inst->dev.port_base + VIRTIO_BLK_CONFIG_SEG_MAX));
      if (seg_max > 2 && seg_max - 2u < max_data_desc)
        max_data_desc = seg_max - 2u;
    }
    inst->max_blocks_per_req = max_data_desc > 1 ? (max_data_desc - 1u) * 8u : 1u;

    /* Publish what this queue can do so the block layer sizes its read-ahead
     * and its bulk transfers against this device instead of against a constant
     * picked for the smallest controller in the tree. The depth is 1 because
     * virtio_blk_lock() serialises the instance and every request reuses
     * descriptor 0 as its chain head — the virtqueue is deeper than that, the
     * driver is not, and saying so is what keeps the layers above honest. */
    inst->blk.limits.max_sectors = inst->max_blocks_per_req;
    inst->blk.limits.max_segments = max_data_desc;
    inst->blk.limits.queue_depth = 1;

    inst->blk.read_blocks = virtio_blk_read;
    inst->blk.write_blocks = virtio_blk_write;
    if (inst->features & VIRTIO_BLK_F_RO)
      inst->blk.write_blocks = 0; /* the device would reject every write */
    if (inst->features & VIRTIO_BLK_F_FLUSH)
      inst->blk.flush = virtio_blk_flush_op;
    if (inst->features & VIRTIO_BLK_F_DISCARD) {
      inst->max_discard_sectors =
          inl((u16)(inst->dev.port_base + VIRTIO_BLK_CONFIG_MAX_DISCARD_SECTORS));
      inst->blk.discard = virtio_blk_discard_op;
    }
    if (inst->features & VIRTIO_BLK_F_WRITE_ZEROES) {
      inst->max_write_zeroes_sectors =
          inl((u16)(inst->dev.port_base + VIRTIO_BLK_CONFIG_MAX_WZ_SECTORS));
      inst->blk.write_zeroes = virtio_blk_write_zeroes_op;
    }
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

    /* Say what was actually agreed, not what was asked for — a device that
     * withholds FLUSH is a device whose fsync cannot be trusted, and that has
     * to be visible in the log. */
    console_write("virtio-blk: features host=0x");
    console_write_hex32(host_features);
    console_write(" negotiated=0x");
    console_write_hex32(inst->features);
    console_write(inst->features & VIRTIO_BLK_F_FLUSH ? " flush=yes" : " flush=no");
    console_write(inst->features & VIRTIO_BLK_F_WRITE_ZEROES ? " write-zeroes=yes"
                                                             : " write-zeroes=no");
    console_write(inst->features & VIRTIO_BLK_F_DISCARD ? " discard=yes"
                                                        : " discard=no");
    console_write(" max-blocks=");
    console_write_dec(inst->max_blocks_per_req);
    if (inst->features & VIRTIO_BLK_F_BLK_SIZE) {
      console_write(" blk-size=");
      console_write_dec(inl((u16)(inst->dev.port_base + VIRTIO_BLK_CONFIG_BLK_SIZE)));
    }
    console_write("\n");

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
