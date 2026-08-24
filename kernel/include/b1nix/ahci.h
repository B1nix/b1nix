#ifndef B1NIX_AHCI_H
#define B1NIX_AHCI_H

#include <b1nix/types.h>

#define AHCI_PCI_CLASS    0x01
#define AHCI_PCI_SUBCLASS 0x06
#define AHCI_PCI_PROG_IF  0x01

#define AHCI_CAP_NP       ((1 << 0) - 1) // Number of ports mask
#define AHCI_CAP_NP_SHIFT 0
#define AHCI_CAP_SSS      (1 << 27)      // Staggered Spin-up
#define AHCI_CAP_SAM      (1 << 18)      // Supports AHCI mode only

#define AHCI_GHC_AE       (1 << 31)      // AHCI Enable
#define AHCI_GHC_IE       (1 << 1)       // Interrupt Enable
#define AHCI_GHC_HR       (1 << 0)       // HBA Reset

#define AHCI_PxCMD_ST     (1 << 0)       // Start
#define AHCI_PxCMD_FRE    (1 << 4)       // FIS Receive Enable
#define AHCI_PxCMD_FR     (1 << 14)      // FIS Receive Running
#define AHCI_PxCMD_CR     (1 << 15)      // Command List Running
#define AHCI_PxCMD_SUD    (1 << 1)       // Spin-Up Device
#define AHCI_PxCMD_POD    (1 << 2)       // Power On Device

#define AHCI_PxSSTS_DET_NO_DEVICE    0
#define AHCI_PxSSTS_DET_PRESENT      (1 << 0)
#define AHCI_PxSSTS_DET_ESTABLISHED  (3 << 0)

#define AHCI_PxSSTS_IPM_ACTIVE       (1 << 8)

#define AHCI_PORT_INT_DHRS  (1 << 0)    // Device to Host Register FIS
#define AHCI_PORT_INT_PSS   (1 << 1)    // PIO Setup FIS
#define AHCI_PORT_INT_DPS   (1 << 5)    // Descriptor Processed
#define AHCI_PORT_INT_PRC   (1 << 22)   // PhyRdy Change
#define AHCI_PORT_INT_CPD   (1 << 30)   // Cold Presence Detect
#define AHCI_PORT_INT_IFE   (1 << 26)   // Interface Fatal Error
#define AHCI_PORT_INT_HBF   (1 << 25)   // Host Bus Fatal Error
#define AHCI_PORT_INT_TFRE  (1 << 24)   // Task File Error
#define AHCI_PORT_INT_OF    (1 << 23)   // Overflow

// SATA FIS types
#define FIS_TYPE_REG_H2D    0x27        // Register FIS - Host to Device
#define FIS_TYPE_REG_D2H    0x34        // Register FIS - Device to Host
#define FIS_TYPE_DMA_ACT    0x39        // DMA Activate FIS
#define FIS_TYPE_DMA_SETUP  0x41        // DMA Setup FIS
#define FIS_TYPE_DATA       0x46        // Data FIS
#define FIS_TYPE_PIO_SETUP  0x5F        // PIO Setup FIS
#define FIS_TYPE_DEV_BITS   0xA1        // Set Device Bits FIS

// ATA commands
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_DMA_EXT   0x35
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1

/* Port signatures (PxSIG). A port says what kind of device answered on it, and
 * the probe has to ask: IDENTIFY DEVICE sent to a packet device is aborted, and
 * on QEMU's ich9-ahci the port then never clears CI at all. */
#define AHCI_SIG_ATA    0x00000101u
#define AHCI_SIG_ATAPI  0xEB140101u
#define AHCI_SIG_SEMB   0xC33C0101u
#define AHCI_SIG_PM     0x96690101u
#define ATA_CMD_FLUSH_CACHE     0xE7
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA
/* DATA SET MANAGEMENT. The command itself only says "here is a list of ranges
 * and something to say about them"; the TRIM feature bit is what says the
 * something is "these are free". */
#define ATA_CMD_DATA_SET_MANAGEMENT 0x06
#define ATA_DSM_FEATURE_TRIM 0x01

#define HBA_PORT_CPD  (1 << 31)
#define HBA_PORT_IPM  (3 << 8)
#define HBA_PORT_DET  (7 << 0)

struct ahci_hba_mem {
    // Generic Host Control (0x00 - 0x2B)
    u32 cap;            // 0x00
    u32 ghc;            // 0x04
    u32 is;             // 0x08
    u32 pi;             // 0x0C
    u32 vs;             // 0x10
    u32 ccc_ctl;        // 0x14
    u32 ccc_pts;        // 0x18
    u32 em_loc;         // 0x1C
    u32 em_ctl;         // 0x20
    u32 cap2;           // 0x24
    u32 bohc;           // 0x28
    u8  rsv[0x60 - 0x2C]; // 0x2C - 0x5F
    u8  vendor[0x100 - 0x60]; // 0x60 - 0xFF

    // Port control registers (0x100 + port_num * 0x80)
    struct ahci_port {
        u32 clb;        // 0x00  Command List Base Address (lower)
        u32 clbu;       // 0x04  Command List Base Address (upper)
        u32 fb;         // 0x08  FIS Base Address (lower)
        u32 fbu;        // 0x0C  FIS Base Address (upper)
        u32 is;         // 0x10  Interrupt Status
        u32 ie;         // 0x14  Interrupt Enable
        u32 cmd;        // 0x18  Command and Status
        u32 rsv0;       // 0x1C  Reserved
        u32 tfd;        // 0x20  Task File Data
        u32 sig;        // 0x24  Signature
        u32 ssts;       // 0x28  SATA Status
        u32 sctl;       // 0x2C  SATA Control
        u32 serr;       // 0x30  SATA Error
        u32 sact;       // 0x34  SATA Active
        u32 ci;         // 0x38  Command Issue
        u32 sntf;       // 0x3C  SATA Notification
        u32 fbs;        // 0x40  FIS-based Switching Control
        u8  rsv1[0x70 - 0x44]; // 0x44 - 0x6F
        u8  vendor[0x80 - 0x70]; // 0x70 - 0x7F
    } __attribute__((packed)) ports[];
} __attribute__((packed));

struct ahci_cmd_header {
    u16  cfis_len : 5;   // CFIS length in DW
    u16  atapi    : 1;
    u16  write    : 1;   // 0=read, 1=write
    u16  prefetch : 1;
    u16  reset    : 1;
    u16  bist     : 1;
    u16  clear_busy : 1;
    u16  rsv0     : 1;
    u16  pmp      : 4;   // Port Multiplier Port
    u16  prdtl;          // Physical Region Descriptor Table Length
    u32  prdbc;          // Physical Region Descriptor Byte Count
    u32  ctba;           // Command Table Descriptor Base Address (lower)
    u32  ctbau;          // Command Table Descriptor Base Address (upper)
    u32  rsv1[4];        // Reserved
} __attribute__((packed));

struct ahci_prdt_entry {
    u32 dba;            // Data Base Address (lower)
    u32 dbau;           // Data Base Address (upper)
    u32 rsv0;
    u32 dbc;            // Byte count (22:0), I bit (31)
} __attribute__((packed));

struct ahci_cmd_table {
    u8  cfis[64];       // Command FIS
    u8  acmd[16];       // ATAPI command
    u8  rsv[48];
    struct ahci_prdt_entry prdt[1]; // Physical Region Descriptor Table
} __attribute__((packed));

// FIS - Register H2D
struct fis_reg_h2d {
    u8  fis_type;       // 0x27
    u8  pmport : 4;     // Port Multiplier
    u8  rsv0   : 3;
    u8  c      : 1;     // 1=command, 0=control
    u8  command;        // ATA command
    u8  feature_low;
    u8  lba0;
    u8  lba1;
    u8  lba2;
    u8  device;
    u8  lba3;
    u8  lba4;
    u8  lba5;
    u8  feature_high;
    u8  count_low;
    u8  count_high;
    u8  icc;
    u8  control;
    u8  rsv1[4];
} __attribute__((packed));

void ahci_init(void);

#endif // B1NIX_AHCI_H
