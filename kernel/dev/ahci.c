#include <b1nix/ahci.h>
#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/sched.h>
#include <string.h>

#define AHCI_MAX_PORTS 32
#define AHCI_COMMAND_SLOTS 32

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

static void ahci_wait_ci_clear(volatile struct ahci_port *p, u32 slot_mask,
                               const char *what, int port_num) {
  u64 spins = 0;
  while (p->ci & slot_mask) {
    /* Spin briefly on the CPU (plain `pause`, NO MMIO) before yielding. Each
     * p->ci read is an MMIO access = a VM-exit under KVM (~µs); polling it in a
     * tight loop costs more than the DMA itself. KVM completes the command in
     * microseconds, so this short register-only spin usually lets it finish
     * before the next MMIO check — avoiding both a flood of VM-exits AND a
     * scheduler round-trip. Yield only if it's genuinely still pending, so we
     * never hog the CPU or block the swap re-entrancy path. */
    for (int i = 0; i < 20000; i++)
      __asm__ volatile("pause");
    if (!(p->ci & slot_mask))
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
     * (blk.c's raw-device fast path chunks to 256 KiB to stay well under this.) */
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
  ahci_wait_ci_clear(p, 1, "read", port->port_num);

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

  ahci_wait_ci_clear(p, (1 << 1), "write", port->port_num);

  // Flush cache
  memset(cmd_table->cfis, 0, 64);
  struct fis_reg_h2d *flush_fis = (struct fis_reg_h2d *)cmd_table->cfis;
  flush_fis->fis_type = FIS_TYPE_REG_H2D;
  flush_fis->c = 1;
  flush_fis->command = ATA_CMD_FLUSH_CACHE;
  flush_fis->device = (1 << 6);

  cmd_hdr->write = 1;
  cmd_hdr->prdtl = 0; // No data for flush
  memset(&cmd_table->prdt[0], 0, sizeof(struct ahci_prdt_entry));

  p->ci = (1 << 1);
  ahci_wait_ci_clear(p, (1 << 1), "flush", port->port_num);

  u32 tfd = p->tfd;
  if (tfd & 0x01) {
    ahci_port_unlock(port);
    return -1;
  }

  ahci_port_unlock(port);
  return (int)count;
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
  cmd_hdr->prdtl = 1;

  struct ahci_prdt_entry *prdt = &cmd_table->prdt[0];
  memset(prdt, 0, sizeof(struct ahci_prdt_entry));
  u64 phys_buf = vmm_virt_to_phys(identify_buf);
  prdt->dba = (u32)(phys_buf & 0xFFFFFFFF);
  prdt->dbau = (u32)((phys_buf >> 32) & 0xFFFFFFFF);
  prdt->dbc = (512 - 1) | (1ULL << 31);

  memset(cmd_table->cfis, 0, 64);
  struct fis_reg_h2d *fis = (struct fis_reg_h2d *)cmd_table->cfis;
  fis->fis_type = FIS_TYPE_REG_H2D;
  fis->c = 1;
  fis->command = ATA_CMD_IDENTIFY;
  fis->device = 0;

  p->serr = p->serr;
  p->ci = 1;

  ahci_wait_ci_clear(p, 1, "identify", port->port_num);

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
  // First, enable memory space and bus mastering
  u16 command = pci_config_read16(pci.bus, pci.slot, pci.func, 0x04);
  command |= 0x06; // Bus Master + Memory Space
  pci_config_write16(pci.bus, pci.slot, pci.func, 0x04, command);

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
  u32 pi = ahci_bar->pi; // Ports Implemented

  console_write("ahci: cap=0x");
  console_write_hex32(cap);
  console_write(" n_ports=");
  console_write_dec(n_ports);
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

        char name_buf[16];
        // Build name like "sata0", "sata1", etc.
        name_buf[0] = 's';
        name_buf[1] = 'a';
        name_buf[2] = 't';
        name_buf[3] = 'a';
        name_buf[4] = '0' + (char)ahci_device_count;
        name_buf[5] = '\0';

        // Need persistent name storage
        char *persistent_name = kmalloc(8);
        memcpy(persistent_name, name_buf, 6);

        dev->name = persistent_name;
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
          }
          kfree(identify_buf);
        }

        dev->read_blocks = ahci_blk_read;
        dev->write_blocks = ahci_blk_write;
        dev->priv = &ports[i];
        blk_register(dev);

        console_write("ahci: registered ");
        console_write(name_buf);
        console_write("\n");
        ahci_device_count++;
      }
    }
  }

  console_write("ahci: initialized with ");
  console_write_dec(ahci_device_count);
  console_write(" devices\n");
}
