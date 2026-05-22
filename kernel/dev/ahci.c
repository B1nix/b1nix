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
};

static struct ahci_port_state ports[AHCI_MAX_PORTS];

static u64 ahci_pci_bar5 = 0;



static int ahci_port_read(struct ahci_port_state *port, u64 lba, u32 count,
                          void *buffer) {
  if (!port->present)
    return -1;

  volatile struct ahci_port *p = &port->abar->ports[port->port_num];
  struct ahci_cmd_header *cmd_hdr = &port->cmd_list[0];
  struct ahci_cmd_table *cmd_table = port->cmd_table;

  // Wait for command list to be not running
  int timeout = 1000000;
  while ((p->cmd & AHCI_PxCMD_CR) && timeout > 0) {
    __asm__ volatile("pause");
    timeout--;
  }
  if (timeout == 0)
    return -1;

  // Setup command header
  memset(cmd_hdr, 0, sizeof(struct ahci_cmd_header));
  cmd_hdr->cfis_len = sizeof(struct fis_reg_h2d) / 4;
  cmd_hdr->write = 0; // Read
  cmd_hdr->prdtl = 1; // One PRD entry

  // Setup PRD
  struct ahci_prdt_entry *prdt = &cmd_table->prdt[0];
  memset(prdt, 0, sizeof(struct ahci_prdt_entry));
  u64 phys_buf = vmm_virt_to_phys(buffer);
  prdt->dba = (u32)(phys_buf & 0xFFFFFFFF);
  prdt->dbau = (u32)((phys_buf >> 32) & 0xFFFFFFFF);
  prdt->dbc = (count * 512 - 1) |
              (1ULL << 31); // byte count, I=1 (interrupt on completion)

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

  // Wait for completion
  timeout = 10000000;
  while ((p->ci & 1) && timeout > 0) {
    scheduler_yield();
    timeout--;
  }

  if (timeout == 0) {
    console_write("ahci: port ");
    console_write_dec(port->port_num);
    console_write(" read timeout\n");
    return -1;
  }

  // Check for errors
  u32 tfd = p->tfd;
  if (tfd & 0x01) { // ERR bit
    console_write("ahci: port ");
    console_write_dec(port->port_num);
    console_write(" read error tfd=0x");
    console_write_hex32(tfd);
    console_write("\n");
    return -1;
  }

  return (int)count;
}

static int ahci_port_write(struct ahci_port_state *port, u64 lba, u32 count,
                           const void *buffer) {
  if (!port->present)
    return -1;

  volatile struct ahci_port *p = &port->abar->ports[port->port_num];
  struct ahci_cmd_header *cmd_hdr = &port->cmd_list[1]; // Use slot 1 for writes
  struct ahci_cmd_table *cmd_table =
      (struct ahci_cmd_table *)((u8 *)port->cmd_table +
                                4096); // Separate table for slot 1

  int timeout = 1000000;
  while ((p->cmd & AHCI_PxCMD_CR) && timeout > 0) {
    __asm__ volatile("pause");
    timeout--;
  }
  if (timeout == 0)
    return -1;

  memset(cmd_hdr, 0, sizeof(struct ahci_cmd_header));
  cmd_hdr->cfis_len = sizeof(struct fis_reg_h2d) / 4;
  cmd_hdr->write = 1; // Write
  cmd_hdr->prdtl = 1;

  struct ahci_prdt_entry *prdt = &cmd_table->prdt[0];
  memset(prdt, 0, sizeof(struct ahci_prdt_entry));
  u64 phys_buf = vmm_virt_to_phys((void *)buffer);
  prdt->dba = (u32)(phys_buf & 0xFFFFFFFF);
  prdt->dbau = (u32)((phys_buf >> 32) & 0xFFFFFFFF);
  prdt->dbc = (count * 512 - 1) | (1ULL << 31);

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

  timeout = 10000000;
  while ((p->ci & (1 << 1)) && timeout > 0) {
    __asm__ volatile("pause");
    timeout--;
  }

  if (timeout == 0) {
    console_write("ahci: port ");
    console_write_dec(port->port_num);
    console_write(" write timeout\n");
    return -1;
  }

  // Flush cache
  memset(cmd_table->cfis, 0, 64);
  struct fis_reg_h2d *flush_fis = (struct fis_reg_h2d *)cmd_table->cfis;
  flush_fis->fis_type = FIS_TYPE_REG_H2D;
  flush_fis->c = 1;
  flush_fis->command = ATA_CMD_FLUSH_CACHE;
  flush_fis->device = (1 << 6);

  cmd_hdr->write = 1;
  cmd_hdr->prdtl = 0; // No data for flush
  memset(prdt, 0, sizeof(struct ahci_prdt_entry));

  p->ci = (1 << 1);
  timeout = 10000000;
  while ((p->ci & (1 << 1)) && timeout > 0) {
    __asm__ volatile("pause");
    timeout--;
  }

  u32 tfd = p->tfd;
  if (tfd & 0x01) {
    return -1;
  }

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
  while ((p->cmd & AHCI_PxCMD_CR) && timeout > 0) {
    __asm__ volatile("pause");
    timeout--;
  }
  if (timeout == 0)
    return -1;

  memset(cmd_hdr, 0, sizeof(struct ahci_cmd_header));
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

  timeout = 10000000;
  while ((p->ci & 1) && timeout > 0) {
    scheduler_yield();
    timeout--;
  }
  if (timeout == 0) {
    return -1;
  }

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

  // Map ABAR into kernel's virtual address space via direct map
  u64 abar_virt = vmm_direct_map_base() + ahci_pci_bar5;
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
