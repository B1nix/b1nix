#ifndef B1NIX_NVME_H
#define B1NIX_NVME_H

#include <b1nix/types.h>

// PCI class code for NVMe
#define NVME_PCI_CLASS    0x01
#define NVME_PCI_SUBCLASS 0x08
#define NVME_PCI_PROG_IF  0x02

// NVMe registers (BAR0, BAR1)
struct nvme_registers {
    u64 cap;            // 0x00  Controller Capabilities
    u32 vs;             // 0x08  Version
    u32 intms;          // 0x0C  Interrupt Mask Set
    u32 intmc;          // 0x10  Interrupt Mask Clear
    u32 cc;             // 0x14  Controller Configuration
    u32 rsv0;           // 0x18  Reserved
    u32 csts;           // 0x1C  Controller Status
    u32 nssr;           // 0x20  NVM Subsystem Reset
    u32 aqa;            // 0x24  Admin Queue Attributes
    u64 asq;            // 0x28  Admin Submission Queue Base
    u64 acq;            // 0x30  Admin Completion Queue Base
    u8  rsv1[0x1000 - 0x38]; // 0x38 - 0xFFF
} __attribute__((packed));

// CAP register bits
#define NVME_CAP_MPSMIN_SHIFT 48
#define NVME_CAP_MPSMAX_SHIFT 52
#define NVME_CAP_DSTRD_SHIFT  32
#define NVME_CAP_TO_SHIFT     24
#define NVME_CAP_CSS_SHIFT    37
#define NVME_CAP_CSS_NVM      (1 << 37)

// CC register bits
#define NVME_CC_EN         (1 << 0)
#define NVME_CC_CSS_SHIFT  4
#define NVME_CC_CSS_NVM    (0 << 4)  // NVM command set
#define NVME_CC_MPS_SHIFT  7
#define NVME_CC_AMS_SHIFT  11
#define NVME_CC_SHN_SHIFT  14
#define NVME_CC_IOSQES     6
#define NVME_CC_IOCQES     4

// CSTS register bits
#define NVME_CSTS_RDY      (1 << 0)
#define NVME_CSTS_CFS      (1 << 1)
#define NVME_CSTS_SHST_SHIFT 2
#define NVME_CSTS_NSSRO    (1 << 4)
#define NVME_CSTS_PP       (1 << 5)

// AQA register bits
#define NVME_AQA_ASQS_SHIFT 0
#define NVME_AQA_ACQS_SHIFT 16

// Submission Queue Entry (SQE)
struct nvme_sqe {
    u32 cdw0;    // Command DWord 0 (opcode, fuse, etc.)
    u32 nsid;    // Namespace Identifier
    u64 rsv0;
    u64 mptr;    // Metadata Pointer
    u64 prp1;    // Physical Region Page 1
    u64 prp2;    // Physical Region Page 2
    u32 cdw10;
    u32 cdw11;
    u32 cdw12;
    u32 cdw13;
    u32 cdw14;
    u32 cdw15;
} __attribute__((packed));

// Completion Queue Entry (CQE)
struct nvme_cqe {
    u32 cdw0;
    u32 rsv0;
    u16 sq_head;
    u16 sq_id;
    u16 cid;
    u16 status;
} __attribute__((packed));

// NVMe command opcodes
#define NVME_CMD_ADMIN_DELETE_SQ    0x00
#define NVME_CMD_ADMIN_CREATE_SQ    0x01
#define NVME_CMD_ADMIN_DELETE_CQ    0x04
#define NVME_CMD_ADMIN_CREATE_CQ    0x05
#define NVME_CMD_ADMIN_IDENTIFY     0x06
#define NVME_CMD_ADMIN_SET_FEATURES 0x09

#define NVME_CMD_IO_READ            0x02
#define NVME_CMD_IO_WRITE           0x01
#define NVME_CMD_IO_FLUSH           0x00

// Identify CNS values
#define NVME_IDENTIFY_CNS_NS        0x00  // Namespace
#define NVME_IDENTIFY_CNS_CTRL      0x01  // Controller

struct nvme_power_state_desc {
    u16  mp;
    u16  rsv0;
    u32  enlat;
    u32  exlat;
    u8   rrt;
    u8   rrl;
    u8   rwt;
    u8   rwl;
    u16  rsv1[5];
} __attribute__((packed));

struct nvme_identify_ctrl {
    u16  vid;
    u16  ssvid;
    char sn[20];
    char mn[40];
    char fr[8];
    u8   rab;
    u8   ieee[3];
    u8   cmic;
    u8   mdts;
    u16  cntlid;
    u32  ver;
    u32  rtd3r;
    u32  rtd3e;
    u32  oaes;
    u32  ctratt;
    u8   rsv0[156];
    u16  oacs;
    u8   acl;
    u8   aerl;
    u8   frmw;
    u8   lpa;
    u8   elpe;
    u8   npss;
    u8   avscc;
    u8   apsta;
    u16  wctemp;
    u16  cctemp;
    u16  mtfa;
    u32  hmpre;
    u32  hmmin;
    u8   tnvmcap[16];
    u8   unvmcap[16];
    u32  rpmbs[4];
    u16  edstt;
    u8   dsto;
    u8   fwug;
    u16  kas;
    u16  hctma;
    u16  mntmt;
    u16  mxtmt;
    u32  sanicap;
    u8   rsv1[168];
    u8   sqes;
    u8   cqes;
    u16  maxcmd;
    u32  nn;
    u16  oncs;
    u16  fuses;
    u8   fna;
    u8   vwc;
    u16  awun;
    u16  awupf;
    u8   nvscc;
    u8   rsv2;
    u16  acwu;
    u8   rsv3[170];
    u8   vs[1344];
    struct nvme_power_state_desc psd[32];
    u8   vs2[1024];
} __attribute__((packed));

struct nvme_identify_ns {
    u64  nsze;
    u64  ncap;
    u64  nuse;
    u8   nsfeat;
    u8   nlbaf;
    u8   flbas;
    u8   mc;
    u8   dpc;
    u8   dps;
    u8   nmic;
    u8   rescap;
    u8   fpi;
    u8   dlfeat;
    u16  nawun;
    u16  nawupf;
    u16  nacwu;
    u16  nabsn;
    u16  nabo;
    u16  nabspf;
    u16  noiob;
    u8   nvmcap[16];
    u16  npwg;
    u16  npwa;
    u16  npdg;
    u16  npda;
    u16  nows;
    u8   rsvd74[18];
    u32  anagrpid;
    u8   rsvd96[3];
    u8   nsattr;
    u16  nvmsetid;
    u16  endgid;
    u8   nguid[16];
    u8   eui64[8];
    struct {
        u16  ms;
        u8   ds;
        u8   rp;
    } __attribute__((packed)) lbaf[64];
    u8   vs[3712];
} __attribute__((packed));

void nvme_init(void);

/* M98: prove an MSI-X message the controller sends is delivered to the vector
 * this driver owns. Issues a real read and checks the handler ran. Emits
 * M98-DRV-SMOKE markers; no-op outside b1nix.test=1. */
void nvme_msix_selftest(void);

/* M100b: run the controller inside its own IOMMU domain with only its queues
 * and the transfer's buffer mapped, and read a block through it. */
void nvme_iommu_selftest(void);

/* M100c: take away the interrupt remapping entry the controller's message
 * names, and see the unit refuse the interrupt that claims it. */
int nvme_ir_rejection_probe(void);

#endif // B1NIX_NVME_H
