#include <b1nix/ahci.h>
#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/irq.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/sched.h>
#include <string.h>

#define AHCI_MAX_PORTS 32

/* M70: watchdog deadline (10 ms scheduler ticks) for a blocked AHCI I/O wait.
 * The completion IRQ wakes the waiter on the common path; this only bounds a
 * lost interrupt to a re-poll. Never reached on a healthy controller. */
#define AHCI_IO_WATCHDOG_TICKS 50

static struct block_device ahci_devices[AHCI_MAX_PORTS];
static int ahci_device_count = 0;

// Store BAR5 (ABAR) base
static volatile struct ahci_hba_mem *ahci_bar = 0;

// Per-port state
struct ahci_port_state {
  volatile struct ahci_hba_mem *abar;
  int port_num;
  struct ahci_cmd_header *cmd_list;
  struct ahci_cmd_table *cmd_table;
  u8 *fis_base;
  u64 phys_cmd_list;
  u64 phys_cmd_table;
  u64 phys_fis;
  int present;
  volatile int busy; // yield-safe per-port I/O mutex (see ahci_port_lock)
};

static struct ahci_port_state ports[AHCI_MAX_PORTS];

/* Yield-safe per-port mutex. The read (slot 0) and write (slot 1) paths share
 * the port's command list / command tables / FIS area and the port registers
 * (ci, serr, tfd), and ahci_wait_ci_clear() calls scheduler_yield() while a
 * command is in flight. Without serialization a second task that runs during
 * that yield — e.g. swap_out()/swap_in() driven by another process's page
 * fault, which reaches this same port via blk_*_cached() → ahci_blk_* — would
 * re-enter ahci_port_write()/read() and rewrite the in-flight command table,
 * corrupting the command QEMU is still reading (mismatched FIS sector count vs
 * PRDT byte count → ide_dma_cb assert) and silently dropping the swap I/O. This
 * mirrors virtio_blk's busy-flag mutex: spin with scheduler_yield() so we never
 * hold a real spinlock across the blocking DMA wait (CLAUDE.md: never sleep/
 * yield holding a spinlock). __sync_lock_test_and_set gives the SMP-safe
 * atomic claim. */
static void ahci_port_lock(struct ahci_port_state *port) {
  while (__sync_lock_test_and_set(&port->busy, 1)) {
    scheduler_yield();
  }
}

static void ahci_port_unlock(struct ahci_port_state *port) {
  __sync_lock_release(&port->busy);
}

static u64 ahci_pci_bar5 = 0;

/* M70: AHCI completion interrupt handler. Runs in IRQ context. The HBA latches a
 * per-port pending bit in the global IS register; for each port flagged, ack the
 * port-level PxIS (RW1C) and wake the task blocked in ahci_wait_ci_clear() on
 * that port's channel, then ack the global IS. The port_state pointer is the
 * wait channel. Returns 1 if any port was pending (shared-line aware). */
static int ahci_irq(void *ctx) {
  volatile struct ahci_hba_mem *abar = (volatile struct ahci_hba_mem *)ctx;
  u32 is = abar->is;
  if (!is)
    return 0; /* another device on a shared INTx line */
  for (int i = 0; i < AHCI_MAX_PORTS; i++) {
    if (is & (1u << i)) {
      volatile struct ahci_port *p = &abar->ports[i];
      p->is = p->is; /* RW1C: clear the port-level interrupt status */
      if (ports[i].present)
        scheduler_wake_all(&ports[i]);
    }
  }
  abar->is = is; /* RW1C: clear the global pending bits we serviced */
  return 1;
}

/* Wait for the command-issue bit(s) in slot_mask to clear. Once issued we must
 * never return while the DMA may still target the caller's buffer, so this only
 * ever returns when the slot is genuinely complete.
 *
 * Fast path: a brief no-MMIO CPU spin then a single ci read — KVM completes in
 * microseconds, so this usually finishes without a VM-exit storm or a context
 * switch. Slow path: block until ahci_irq() wakes us; scheduler_wait_prepare_
 * timeout re-checks ci after publishing BLOCKED (so a completion racing the
 * block is never lost) and the watchdog deadline re-polls if an interrupt is
 * ever lost. Before the scheduler is live (boot-time identify) or from an
 * IRQs-off caller, fall back to the original cooperative yield-poll. */
static void ahci_wait_ci_clear(struct ahci_port_state *port,
                               volatile struct ahci_port *p, u32 slot_mask,
                               const char *what, int port_num) {
  /* Fast path: FINE-grained spin+check (a single huge spin overshoots the µs KVM
   * completion — origin/main found a 20000-pause spin is ~1 ms and capped
   * throughput ~200 MB/s). A handful of short spin+check rounds detect a
   * just-completed command almost immediately with no context switch. */
  for (int round = 0; round < 16; round++) {
    for (int i = 0; i < 256; i++)
      __asm__ volatile("pause");
    if (!(p->ci & slot_mask))
      return;
  }

  u64 spins = 0;
  while (p->ci & slot_mask) {
    if (!scheduler_can_block()) {
      /* Early boot (no scheduler) / IRQs-off caller: same fine-grained poll,
       * then a cooperative yield — never the interrupt-driven block. */
      int detected = 0;
      for (int round = 0; round < 16 && !detected; round++) {
        for (int i = 0; i < 256; i++)
          __asm__ volatile("pause");
        if (!(p->ci & slot_mask))
          detected = 1;
      }
      if (detected)
        break;
      if (spins == 10000000ULL) {
        console_write("ahci: port ");
        console_write_dec(port_num);
        console_write(" ");
        console_write(what);
        console_write(" still pending after timeout; waiting to preserve DMA buffer lifetime\n");
      }
      scheduler_yield();
      spins++;
      continue;
    }
    /* Normal path: block until ahci_irq() wakes us; the watchdog deadline
     * re-checks ci so a lost interrupt degrades to a re-poll, never a wedge. */
    scheduler_wait_prepare_timeout(port, AHCI_IO_WATCHDOG_TICKS);
    if (!(p->ci & slot_mask)) {
      scheduler_wait_cancel();
      break;
    }
    scheduler_wait_commit();
  }
}



/* Build the PRDT for a (possibly multi-page) DMA buffer.
 *
 * A single PRD entry describes a PHYSICALLY contiguous region. DMA buffers are
 * frequently kernel-heap allocations (e.g. the block cache, whose per-entry
 * 512-byte data field straddles 4 KiB page boundaries because sizeof(struct
 * block_buffer) is not a power of two) — and adjacent heap *pages* are not
 * necessarily adjacent *frames*. Emitting one PRD entry per page-contiguous
 * segment (translating each segment's virtual base with vmm_virt_to_phys)
 * prevents a transfer that crosses a page boundary from spilling into the
 * wrong physical frame — which previously corrupted unrelated frames (page
 * tables, etc.) and triple-faulted the kernel. Returns the PRD entry count
 * (-> cmd_hdr->prdtl). The command table is page-sized, leaving room for far
 * more entries than any real transfer (count<=8 -> at most a few pages).
 */
/* prdt[] lives in the page-sized command table starting at offset 128:
 * (4096 - 128) / sizeof(struct ahci_prdt_entry=16) = 248 usable entries. */
#define AHCI_PRDT_MAX_ENTRIES 248

static int ahci_build_prdt(struct ahci_cmd_table *cmd_table, void *buffer,
                           u32 total_bytes) {
  u64 vaddr = (u64)(usize)buffer;
  u64 remaining = total_bytes;
  int n = 0;
  while (remaining > 0) {
    /* The command table is page-sized; prdt[] starts at offset 128, so it holds
     * (4096-128)/16 = 248 entries. Refuse rather than overflow into adjacent
     * memory if a caller ever asks for a transfer with more page-segments.
     * (blk.c chunks every transfer to this port's published max_sectors, which
     * is derived from this very limit, so it never gets here.) */
    if (n >= AHCI_PRDT_MAX_ENTRIES)
      return -1;
    u64 page_off = vaddr & (PAGE_SIZE - 1);
    u64 seg = PAGE_SIZE - page_off; /* bytes to the end of this page */
    if (seg > remaining)
      seg = remaining;
    u64 phys = vmm_virt_to_phys((void *)(usize)vaddr);
    struct ahci_prdt_entry *e = &cmd_table->prdt[n];
    memset(e, 0, sizeof(*e));
    e->dba = (u32)(phys & 0xFFFFFFFF);
    e->dbau = (u32)((phys >> 32) & 0xFFFFFFFF);
    e->dbc = (u32)(seg - 1); /* byte count - 1 */
    vaddr += seg;
    remaining -= seg;
    n++;
  }
  if (n == 0) { /* defensive: zero-length transfer */
    memset(&cmd_table->prdt[0], 0, sizeof(struct ahci_prdt_entry));
    n = 1;
  }
  cmd_table->prdt[n - 1].dbc |= (1u << 31); /* I=1 on the final entry */
  return n;
}

static int ahci_port_read(struct ahci_port_state *port, u64 lba, u32 count,
                          void *buffer) {
  if (!port->present)
    return -1;

  ahci_port_lock(port);

  volatile struct ahci_port *p = &port->abar->ports[port->port_num];
  struct ahci_cmd_header *cmd_hdr = &port->cmd_list[0];
  struct ahci_cmd_table *cmd_table = port->cmd_table;

  // Wait for device to not be busy
  int timeout = 1000000;
  while ((p->tfd & 0x88) && timeout > 0) {
    __asm__ volatile("pause");
    timeout--;
  }
  if (timeout == 0) {
    ahci_port_unlock(port);
    return -1;
  }

  // Setup command header
  u32 ctba = cmd_hdr->ctba;
  u32 ctbau = cmd_hdr->ctbau;
  memset(cmd_hdr, 0, sizeof(struct ahci_cmd_header));
  cmd_hdr->ctba = ctba;
  cmd_hdr->ctbau = ctbau;
  cmd_hdr->cfis_len = sizeof(struct fis_reg_h2d) / 4;
  cmd_hdr->write = 0; // Read

  // Setup PRD(s) — split across physical page boundaries (see ahci_build_prdt).
  int prdt_n = ahci_build_prdt(cmd_table, buffer, count * 512);
  if (prdt_n < 0) { ahci_port_unlock(port); return -1; }
  cmd_hdr->prdtl = (u16)prdt_n;

  // Setup FIS
  memset(cmd_table->cfis, 0, 64);
  struct fis_reg_h2d *fis = (struct fis_reg_h2d *)cmd_table->cfis;
  fis->fis_type = FIS_TYPE_REG_H2D;
  fis->c = 1; // Command
  fis->command = ATA_CMD_READ_DMA_EXT;
  fis->lba0 = (u8)(lba & 0xFF);
  fis->lba1 = (u8)((lba >> 8) & 0xFF);
  fis->lba2 = (u8)((lba >> 16) & 0xFF);
  fis->device = (1 << 6); // LBA mode
  fis->lba3 = (u8)((lba >> 24) & 0xFF);
  fis->lba4 = (u8)((lba >> 32) & 0xFF);
  fis->lba5 = (u8)((lba >> 40) & 0xFF);
  fis->count_low = (u8)(count & 0xFF);
  fis->count_high = (u8)((count >> 8) & 0xFF);

  // Clear SATA error register before starting
  p->serr = p->serr;

  // Issue command
  p->ci = 1;

  // Wait for completion. Once issued, never return while DMA may still target
  // the caller's buffer.
  ahci_wait_ci_clear(port, p, 1, "read", port->port_num);

  // Check for errors
  u32 tfd = p->tfd;
  if (tfd & 0x01) { // ERR bit
    console_write("ahci: port ");
    console_write_dec(port->port_num);
    console_write(" read error tfd=0x");
    console_write_hex32(tfd);
    console_write("\n");
    ahci_port_unlock(port);
    return -1;
  }

  ahci_port_unlock(port);
  return (int)count;
}

static int ahci_port_write(struct ahci_port_state *port, u64 lba, u32 count,
                           const void *buffer) {
  if (!port->present)
    return -1;

  ahci_port_lock(port);

  volatile struct ahci_port *p = &port->abar->ports[port->port_num];
  struct ahci_cmd_header *cmd_hdr = &port->cmd_list[1]; // Use slot 1 for writes
  struct ahci_cmd_table *cmd_table =
      (struct ahci_cmd_table *)((u8 *)port->cmd_table +
                                4096); // Separate table for slot 1

  int timeout = 1000000;
  while ((p->tfd & 0x88) && timeout > 0) {
    __asm__ volatile("pause");
    timeout--;
  }
  if (timeout == 0) {
    ahci_port_unlock(port);
    return -1;
  }

  u32 ctba = cmd_hdr->ctba;
  u32 ctbau = cmd_hdr->ctbau;
  memset(cmd_hdr, 0, sizeof(struct ahci_cmd_header));
  cmd_hdr->ctba = ctba;
  cmd_hdr->ctbau = ctbau;
  cmd_hdr->cfis_len = sizeof(struct fis_reg_h2d) / 4;
  cmd_hdr->write = 1; // Write

  // Setup PRD(s) — split across physical page boundaries (see ahci_build_prdt).
  int prdt_nw = ahci_build_prdt(cmd_table, (void *)buffer, count * 512);
  if (prdt_nw < 0) { ahci_port_unlock(port); return -1; }
  cmd_hdr->prdtl = (u16)prdt_nw;

  memset(cmd_table->cfis, 0, 64);
  struct fis_reg_h2d *fis = (struct fis_reg_h2d *)cmd_table->cfis;
  fis->fis_type = FIS_TYPE_REG_H2D;
  fis->c = 1;
  fis->command = ATA_CMD_WRITE_DMA_EXT;
  fis->lba0 = (u8)(lba & 0xFF);
  fis->lba1 = (u8)((lba >> 8) & 0xFF);
  fis->lba2 = (u8)((lba >> 16) & 0xFF);
  fis->device = (1 << 6);
  fis->lba3 = (u8)((lba >> 24) & 0xFF);
  fis->lba4 = (u8)((lba >> 32) & 0xFF);
  fis->lba5 = (u8)((lba >> 40) & 0xFF);
  fis->count_low = (u8)(count & 0xFF);
  fis->count_high = (u8)((count >> 8) & 0xFF);

  p->serr = p->serr;
  p->ci = (1 << 1); // Issue slot 1

  ahci_wait_ci_clear(port, p, (1 << 1), "write", port->port_num);

  u32 tfd = p->tfd;
  if (tfd & 0x01) { // ERR bit
    console_write("ahci: port ");
    console_write_dec(port->port_num);
    console_write(" write error tfd=0x");
    console_write_hex32(tfd);
    console_write("\n");
    ahci_port_unlock(port);
    return -1;
  }

  ahci_port_unlock(port);
  return (int)count;
}

/* Push the disk's write-back cache out to the platters.
 *
 * This is now the ONLY place b1nix issues a cache flush to a SATA disk. It used
 * to follow every single WRITE DMA EXT, which is not a barrier — it is the
 * absence of a write-back cache: the disk could neither merge nor reorder
 * anything, and every write cost two full command round-trips instead of one.
 * It bought the filesystems no ordering either, because every ext2/3/4 write
 * (journal blocks included) lands in the block cache first and reaches the disk
 * only when that cache is drained, in whatever order the drain picks.
 *
 * The barrier belongs where a caller actually asks for durability: fsync(2),
 * sync(2) and umount all funnel through blk_cache_flush()/blk_sync_all(), which
 * drain the dirty block-cache entries into the driver and then call this. */
static int ahci_port_flush(struct ahci_port_state *port) {
  if (!port->present)
    return -1;

  ahci_port_lock(port);

  volatile struct ahci_port *p = &port->abar->ports[port->port_num];
  struct ahci_cmd_header *cmd_hdr = &port->cmd_list[1]; /* slot 1, as writes */
  struct ahci_cmd_table *cmd_table =
      (struct ahci_cmd_table *)((u8 *)port->cmd_table + 4096);

  int timeout = 1000000;
  while ((p->tfd & 0x88) && timeout > 0) {
    __asm__ volatile("pause");
    timeout--;
  }
  if (timeout == 0) {
    ahci_port_unlock(port);
    return -1;
  }

  u32 ctba = cmd_hdr->ctba;
  u32 ctbau = cmd_hdr->ctbau;
  memset(cmd_hdr, 0, sizeof(struct ahci_cmd_header));
  cmd_hdr->ctba = ctba;
  cmd_hdr->ctbau = ctbau;
  cmd_hdr->cfis_len = sizeof(struct fis_reg_h2d) / 4;
  cmd_hdr->write = 1;
  cmd_hdr->prdtl = 0; /* no data transfer */
  memset(&cmd_table->prdt[0], 0, sizeof(struct ahci_prdt_entry));

  memset(cmd_table->cfis, 0, 64);
  struct fis_reg_h2d *fis = (struct fis_reg_h2d *)cmd_table->cfis;
  fis->fis_type = FIS_TYPE_REG_H2D;
  fis->c = 1;
  /* FLUSH CACHE EXT is the 48-bit-addressing form of the same command; every
   * device that accepts READ/WRITE DMA EXT — which is all we ever issue —
   * supports it. */
  fis->command = ATA_CMD_FLUSH_CACHE_EXT;
  fis->device = (1 << 6);

  p->serr = p->serr;
  p->ci = (1 << 1);
  ahci_wait_ci_clear(port, p, (1 << 1), "flush", port->port_num);

  u32 tfd = p->tfd;
  if (tfd & 0x01) {
    console_write("ahci: port ");
    console_write_dec(port->port_num);
    console_write(" flush error tfd=0x");
    console_write_hex32(tfd);
    console_write("\n");
    ahci_port_unlock(port);
    return -1;
  }

  ahci_port_unlock(port);
  return 0;
}

/* M109 TRIM: ATA DATA SET MANAGEMENT with the TRIM feature. The payload is a
 * list of 8-byte entries — 48-bit LBA in the low bits, a 16-bit sector count
 * above it — padded out to whole 512-byte blocks, sent to the device as an
 * ordinary DMA write. Only installed for a disk whose IDENTIFY says word 169
 * bit 0, so a drive that cannot trim reports "not supported" rather than
 * failing a command it never claimed.
 *
 * The entry count per block is 64, and each entry covers up to 65535 sectors,
 * so one 512-byte block describes 4 M sectors — more than any single call from
 * the block layer, but the loop is written for the general case anyway. */
#define AHCI_TRIM_ENTRIES_PER_BLOCK 64
#define AHCI_TRIM_MAX_SECTORS_PER_ENTRY 65535u

static int ahci_port_trim(struct ahci_port_state *port, u64 lba, u32 count) {
  if (!port->present || count == 0)
    return -1;

  u64 *ranges = kzalloc(512);
  if (!ranges)
    return -1;

  int rc = 0;
  u32 done = 0;
  while (done < count && rc == 0) {
    memset(ranges, 0, 512);
    int n = 0;
    while (n < AHCI_TRIM_ENTRIES_PER_BLOCK && done < count) {
      u32 chunk = count - done;
      if (chunk > AHCI_TRIM_MAX_SECTORS_PER_ENTRY)
        chunk = AHCI_TRIM_MAX_SECTORS_PER_ENTRY;
      ranges[n++] = ((lba + done) & 0x0000FFFFFFFFFFFFull) |
                    ((u64)chunk << 48);
      done += chunk;
    }

    ahci_port_lock(port);
    volatile struct ahci_port *p = &port->abar->ports[port->port_num];
    struct ahci_cmd_header *cmd_hdr = &port->cmd_list[1];
    struct ahci_cmd_table *cmd_table =
        (struct ahci_cmd_table *)((u8 *)port->cmd_table + 4096);

    int timeout = 1000000;
    while ((p->tfd & 0x88) && timeout > 0) {
      __asm__ volatile("pause");
      timeout--;
    }
    if (timeout == 0) {
      ahci_port_unlock(port);
      rc = -1;
      break;
    }

    u32 ctba = cmd_hdr->ctba;
    u32 ctbau = cmd_hdr->ctbau;
    memset(cmd_hdr, 0, sizeof(struct ahci_cmd_header));
    cmd_hdr->ctba = ctba;
    cmd_hdr->ctbau = ctbau;
    cmd_hdr->cfis_len = sizeof(struct fis_reg_h2d) / 4;
    cmd_hdr->write = 1; /* host to device */

    int prdt_nw = ahci_build_prdt(cmd_table, ranges, 512);
    if (prdt_nw < 0) {
      ahci_port_unlock(port);
      rc = -1;
      break;
    }
    cmd_hdr->prdtl = (u16)prdt_nw;

    memset(cmd_table->cfis, 0, 64);
    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)cmd_table->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_DATA_SET_MANAGEMENT;
    fis->feature_low = ATA_DSM_FEATURE_TRIM;
    fis->device = (1 << 6);
    /* COUNT is the number of 512-byte range blocks, not sectors. */
    fis->count_low = 1;
    fis->count_high = 0;

    p->serr = p->serr;
    p->ci = (1 << 1);
    ahci_wait_ci_clear(port, p, (1 << 1), "trim", port->port_num);

    u32 tfd = p->tfd;
    ahci_port_unlock(port);
    if (tfd & 0x01) {
      console_write("ahci: port ");
      console_write_dec(port->port_num);
      console_write(" trim error tfd=0x");
      console_write_hex32(tfd);
      console_write("\n");
      rc = -1;
    }
  }

  kfree(ranges);
  return rc;
}

static int ahci_blk_discard(struct block_device *dev, u64 lba, u32 count) {
  struct ahci_port_state *port = (struct ahci_port_state *)dev->priv;
  if (!port)
    return -1;
  return ahci_port_trim(port, lba, count);
}

static int ahci_blk_read(struct block_device *dev, u64 lba, u32 count,
                         void *buffer) {
  struct ahci_port_state *port = (struct ahci_port_state *)dev->priv;
  return ahci_port_read(port, lba, count, buffer);
}

static int ahci_blk_write(struct block_device *dev, u64 lba, u32 count,
                          const void *buffer) {
  struct ahci_port_state *port = (struct ahci_port_state *)dev->priv;
  return ahci_port_write(port, lba, count, buffer);
}

static int ahci_blk_flush(struct block_device *dev) {
  struct ahci_port_state *port = (struct ahci_port_state *)dev->priv;
  return ahci_port_flush(port);
}

static void ahci_port_init(struct ahci_port_state *port,
                           volatile struct ahci_hba_mem *abar, int num) {
  port->abar = abar;
  port->port_num = num;
  port->present = 0;

  volatile struct ahci_port *p = &abar->ports[num];

  u32 ssts = p->ssts;
  u32 det = ssts & 0x07;        // HBA_PORT_DET mask
  u32 ipm = (ssts >> 8) & 0x03; // HBA_PORT_IPM shift

  if (det != 3) {
    return; // No device present (need DET=3 for PHY comms established)
  }
  if (ipm != 1) {
    return; // Not active (need IPM=1 for active)
  }

  console_write("ahci: port ");
  console_write_dec(num);
  console_write(" device detected\n");

  // Make sure engine is stopped before writing configuration registers
  p->cmd &= ~AHCI_PxCMD_ST;
  p->cmd &= ~AHCI_PxCMD_FRE;
  int init_timeout = 1000000;
  while (init_timeout > 0) {
    if (!(p->cmd & AHCI_PxCMD_CR) && !(p->cmd & AHCI_PxCMD_FR)) {
      break;
    }
    __asm__ volatile("pause");
    init_timeout--;
  }

  // Allocate command list (1K aligned)
  port->phys_cmd_list = pmm_alloc_frames(1);
  port->cmd_list = (struct ahci_cmd_header *)(usize)(port->phys_cmd_list + vmm_direct_map_base());
  memset(port->cmd_list, 0, PAGE_SIZE);

  // Allocate FIS receive area (256 bytes, must be 256-byte aligned, but page is
  // fine)
  port->phys_fis = pmm_alloc_frames(1);
  port->fis_base = (u8 *)(usize)(port->phys_fis + vmm_direct_map_base());
  memset(port->fis_base, 0, PAGE_SIZE);

  // Allocate command table (must be 128-byte aligned)
  port->phys_cmd_table = pmm_alloc_frames(2); // 2 pages for read + write tables
  port->cmd_table = (struct ahci_cmd_table *)(usize)(port->phys_cmd_table + vmm_direct_map_base());
  memset(port->cmd_table, 0, 2 * PAGE_SIZE);

  // Point to command list (phys addr needs to be in specific format)
  p->clb = (u32)(port->phys_cmd_list & 0xFFFFFFFF);
  p->clbu = (u32)((port->phys_cmd_list >> 32) & 0xFFFFFFFF);

  // Point to FIS receive area
  p->fb = (u32)(port->phys_fis & 0xFFFFFFFF);
  p->fbu = (u32)((port->phys_fis >> 32) & 0xFFFFFFFF);

  // Write command table physical address into command header slot 0
  // ctba = lower 32 bits, ctbau = upper 32 bits (actually ctba=slot0,
  // ctba+16=slot1)
  port->cmd_list[0].ctba = (u32)(port->phys_cmd_table & 0xFFFFFFFF);
  port->cmd_list[0].ctbau = (u32)((port->phys_cmd_table >> 32) & 0xFFFFFFFF);
  port->cmd_list[1].ctba =
      (u32)((port->phys_cmd_table + PAGE_SIZE) & 0xFFFFFFFF);
  port->cmd_list[1].ctbau =
      (u32)(((port->phys_cmd_table + PAGE_SIZE) >> 32) & 0xFFFFFFFF);

  // Enable FIS receive and start
  p->cmd |= AHCI_PxCMD_FRE;
  p->cmd |= AHCI_PxCMD_ST;

  /* M70: clear any stale port interrupt status (RW1C) and enable per-port
   * interrupts so a command completion raises the controller's INTx line. The
   * global GHC.IE bit and the IRQ handler registration are set up once in
   * ahci_init() after all ports are configured. */
  p->is = p->is;
  p->ie = 0xFFFFFFFF;

  port->present = 1;

  console_write("ahci: port ");
  console_write_dec(num);
  console_write(" ready\n");
}

static int ahci_port_identify(struct ahci_port_state *port, u16 *identify_buf) {
  if (!port->present)
    return -1;

  volatile struct ahci_port *p = &port->abar->ports[port->port_num];
  struct ahci_cmd_header *cmd_hdr = &port->cmd_list[0];
  struct ahci_cmd_table *cmd_table = port->cmd_table;

  int timeout = 1000000;
  while ((p->tfd & 0x88) && timeout > 0) {
    __asm__ volatile("pause");
    timeout--;
  }
  if (timeout == 0)
    return -1;

  u32 ctba = cmd_hdr->ctba;
  u32 ctbau = cmd_hdr->ctbau;
  memset(cmd_hdr, 0, sizeof(struct ahci_cmd_header));
  cmd_hdr->ctba = ctba;
  cmd_hdr->ctbau = ctbau;
  cmd_hdr->cfis_len = sizeof(struct fis_reg_h2d) / 4;
  cmd_hdr->write = 0;

  /* identify_buf is a kzalloc(512) heap buffer, not guaranteed page-aligned —
   * a single raw PRD entry (translating only its starting address) silently
   * corrupts whatever physical frame follows the buffer's page whenever the
   * 512 bytes straddle a page boundary (see ahci_build_prdt's comment: this
   * exact bug previously corrupted unrelated frames via the block-I/O path). */
  int prdt_n = ahci_build_prdt(cmd_table, identify_buf, 512);
  if (prdt_n < 0)
    return -1;
  cmd_hdr->prdtl = (u16)prdt_n;

  memset(cmd_table->cfis, 0, 64);
  struct fis_reg_h2d *fis = (struct fis_reg_h2d *)cmd_table->cfis;
  fis->fis_type = FIS_TYPE_REG_H2D;
  fis->c = 1;
  fis->command = ATA_CMD_IDENTIFY;
  fis->device = 0;

  p->serr = p->serr;
  p->ci = 1;

  ahci_wait_ci_clear(port, p, 1, "identify", port->port_num);

  u32 tfd = p->tfd;
  if (tfd & 0x01) {
    return -1;
  }
  return 0;
}

void ahci_init(void) {
  struct pci_device_info pci;
  int found = pci_find_class(AHCI_PCI_CLASS, AHCI_PCI_SUBCLASS, 0, &pci);
  if (!found) {
    // Check for generic mass storage class
    for (int i = 0; i < 8; i++) {
      if (pci_find_class(0x01, i, 0, &pci)) {
        found = 1;
        break;
      }
    }
  }

  if (!found) {
    console_write("ahci: no SATA controller found\n");
    return;
  }

  console_write("ahci: found controller v=0x");
  console_write_hex32(pci.vendor_id);
  console_write(" d=0x");
  console_write_hex32(pci.device_id);
  console_write(" prog_if=0x");
  console_write_hex32(pci.prog_if);
  console_write("\n");

  // Check if we're in IDE mode (prog_if != 0x01 for AHCI)
  if (pci.prog_if != AHCI_PCI_PROG_IF) {
    console_write("ahci: controller not in AHCI mode (prog_if=0x");
    console_write_hex32(pci.prog_if);
    console_write(")\n");
    // Some QEMU AHCI controllers report prog_if=0, still try AHCI
  }

  // Get BAR5 (ABAR - AHCI Base Address Register)
  // First, enable memory space and bus mastering; clear the INTx Disable bit
  // (bit 10) so the controller can raise legacy interrupts (M70).
  u16 command = pci_config_read16(pci.bus, pci.slot, pci.func, 0x04);
  command |= 0x06;     // Bus Master + Memory Space
  command &= ~0x0400;  // clear Interrupt Disable
  pci_config_write16(pci.bus, pci.slot, pci.func, 0x04, command);

  // Legacy interrupt line for the controller (M70: completion IRQ).
  u8 ahci_irq_line = pci_config_read8(pci.bus, pci.slot, pci.func, 0x3C);

  u32 bar5_low = pci_config_read32(pci.bus, pci.slot, pci.func, 0x24);
  u32 bar5_high = pci_config_read32(pci.bus, pci.slot, pci.func, 0x28);

  ahci_pci_bar5 = (u64)bar5_low | ((u64)bar5_high << 32);
  ahci_pci_bar5 &= 0xFFFFFFFFFFFFFFF0ULL; // Lower 4 bits are flags

  console_write("ahci: ABAR at 0x");
  console_write_hex64(ahci_pci_bar5);
  console_write("\n");

  // If BAR5 is 0, controller is in IDE mode — skip AHCI init
  if (ahci_pci_bar5 == 0) {
    console_write("ahci: ABAR is zero (likely IDE mode), skipping\n");
    return;
  }

  // Map ABAR into kernel's virtual address space.
#ifdef __x86_64__
  // x86_64: the direct map spans >=4 GB and already covers PCI MMIO BARs.
  u64 abar_virt = vmm_direct_map_base() + ahci_pci_bar5;
#else
  // 32-bit: the direct map only covers low RAM (<=1 GB), but the ABAR lives at
  // ~4 GB of MMIO space. Computing vmm_direct_map_base()+ABAR there overflows
  // the 32-bit address and aliases to junk (cap reads back 0 -> "0 devices").
  // Map it explicitly into the MMIO window instead.
  u64 abar_virt = (u64)(usize)vmm_map_mmio(ahci_pci_bar5,
                                           sizeof(struct ahci_hba_mem),
                                           VMM_WRITABLE | VMM_PCD);
#endif
  ahci_bar = (volatile struct ahci_hba_mem *)(usize)abar_virt;

  // Check capabilities
  u32 cap = ahci_bar->cap;
  u32 n_ports = (cap & 0x1F) + 1;
  /* CAP.NCS (bits 12:8) is the command-slot count this controller supports —
   * 32 on QEMU. The driver issues one command at a time, so it uses two of
   * them; reading it is what lets the log say what the hardware offered rather
   * than repeat a constant that was never checked against anything. */
  u32 n_cmd_slots = ((cap >> 8) & 0x1F) + 1;
  u32 pi = ahci_bar->pi; // Ports Implemented

  console_write("ahci: cap=0x");
  console_write_hex32(cap);
  console_write(" n_ports=");
  console_write_dec(n_ports);
  console_write(" n_cmd_slots=");
  console_write_dec(n_cmd_slots);
  console_write(" pi=0x");
  console_write_hex32(pi);
  console_write("\n");

  // Perform HBA reset
  ahci_bar->ghc |= AHCI_GHC_HR;
  int timeout = 1000000;
  while ((ahci_bar->ghc & AHCI_GHC_HR) && timeout > 0) {
    __asm__ volatile("pause");
    timeout--;
  }

  // Enable AHCI
  ahci_bar->ghc |= AHCI_GHC_AE;

  ahci_device_count = 0;
  memset(ports, 0, sizeof(ports));

  // Initialize each implemented port
  for (int i = 0; i < 32 && i < (int)n_ports; i++) {
    if (pi & (1 << i)) {
      ahci_port_init(&ports[i], ahci_bar, i);
      if (ports[i].present) {
        // Register as block device
        struct block_device *dev = &ahci_devices[ahci_device_count];

        dev->block_size = 512;
        dev->block_count = 0;

        u16 *identify_buf = kzalloc(512);
        if (identify_buf) {
          if (ahci_port_identify(&ports[i], identify_buf) == 0) {
            u64 sectors = 0;
            // Check if LBA48 is supported: word 83 bit 10 is 1
            if (identify_buf[83] & (1 << 10)) {
              sectors = ((u64)identify_buf[100]) |
                        ((u64)identify_buf[101] << 16) |
                        ((u64)identify_buf[102] << 32) |
                        ((u64)identify_buf[103] << 48);
            } else {
              sectors = ((u64)identify_buf[60]) |
                        ((u64)identify_buf[61] << 16);
            }
            dev->block_count = sectors;
            /* Word 169 bit 0: the drive has DATA SET MANAGEMENT with TRIM.
             * Word 217: 1 means non-rotating media; anything else is either a
             * rotation rate or "not reported", and Linux treats both as
             * rotating, so BLKROTATIONAL says the same here. */
            if (identify_buf[169] & 0x0001)
              dev->discard = ahci_blk_discard;
            dev->rotational = (identify_buf[217] == 0x0001) ? 0 : 1;
          }
          kfree(identify_buf);
        }

        dev->read_blocks = ahci_blk_read;
        dev->write_blocks = ahci_blk_write;
        dev->flush = ahci_blk_flush;
        dev->priv = &ports[i];
        /* What one command to this port can carry, published for the block
         * layer to clamp its read-ahead and its bulk transfers against. The
         * segment count is the PRDT the page-sized command table holds; the
         * sector count is one page-segment fewer, because a buffer that does
         * not start on a page boundary spends its first entry on the head of a
         * page. The controller reports NCS command slots (CAP bits 12:8, so
         * QEMU's 32) but this driver serialises on port->busy and uses two of
         * them, so the depth it can honestly claim is 1. */
        dev->limits.max_segments = AHCI_PRDT_MAX_ENTRIES;
        dev->limits.max_sectors = (AHCI_PRDT_MAX_ENTRIES - 1) * (PAGE_SIZE / 512);
        dev->limits.queue_depth = 1;
        /* The block layer names it: the next free sd* in the one SCSI-disk
         * sequence AHCI shares with USB mass storage. */
        blk_register_disk(dev, "sd", BLK_BUS_ATA);
        if (!dev->name)
          continue;

        console_write("ahci: trim=");
        console_write(dev->discard ? "yes" : "no");
        console_write(dev->rotational ? " rotational=yes\n" : " rotational=no\n");

        console_write("ahci: registered ");
        console_write(dev->name);
        console_write("\n");
        ahci_device_count++;
      }
    }
  }

  /* M70: enable interrupt-driven completion. Per-port PxIE was set in
   * ahci_port_init; now clear the global interrupt status, enable GHC.IE, then
   * register the handler and unmask the line. Doing this last means any stale
   * IS bits from boot-time identify are acked by the first delivered IRQ rather
   * than lost, and no IRQ reaches the CPU until the handler is in place. */
  if (ahci_device_count > 0) {
    ahci_bar->is = ahci_bar->is; /* RW1C: clear stale global pending bits */
    ahci_bar->ghc |= AHCI_GHC_IE;
    irq_register_handler(ahci_irq_line, ahci_irq, (void *)ahci_bar);
    irq_unmask(ahci_irq_line);
  }

  console_write("ahci: initialized with ");
  console_write_dec(ahci_device_count);
  console_write(" devices\n");
}
