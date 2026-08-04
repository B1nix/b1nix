#ifndef B1NIX_PCI_H
#define B1NIX_PCI_H

#include <b1nix/types.h>

struct pci_device_info {
	u8 bus;
	u8 slot;
	u8 func;
	u16 vendor_id;
	u16 device_id;
	u8 class_code;
	u8 subclass;
	u8 prog_if;
	u8 irq_line;
};

void pci_init(void);
u32 pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset);
u16 pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset);
u8 pci_config_read8(u8 bus, u8 slot, u8 func, u8 offset);
void pci_config_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 value);
void pci_config_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 value);
void pci_config_write8(u8 bus, u8 slot, u8 func, u8 offset, u8 value);
int pci_find_device(u16 vendor_id, u16 device_id, struct pci_device_info *info);
int pci_find_class(u8 class_code, u8 subclass, u8 index, struct pci_device_info *info);
u32 pci_get_vram_size(u16 vendor_id, u16 device_id);

/* ── M98 T3: modern PCI ───────────────────────────────────────────── */

/* Config-space register numbers used by the helpers below. */
#define PCI_CFG_COMMAND      0x04
#define PCI_CFG_STATUS       0x06
#define PCI_CFG_HEADER_TYPE  0x0E
#define PCI_CFG_BAR0         0x10
#define PCI_CFG_CAP_PTR      0x34

#define PCI_CMD_IO_SPACE     0x0001
#define PCI_CMD_MEM_SPACE    0x0002
#define PCI_CMD_BUS_MASTER   0x0004
#define PCI_CMD_INTX_DISABLE 0x0400

#define PCI_STATUS_CAP_LIST  0x0010

#define PCI_CAP_ID_PM        0x01
#define PCI_CAP_ID_MSI       0x05
#define PCI_CAP_ID_VENDOR    0x09
#define PCI_CAP_ID_PCIE      0x10
#define PCI_CAP_ID_MSIX      0x11

#define PCI_EXT_CAP_ID_AER   0x0001
#define PCI_EXT_CAP_ID_ATS   0x000F

#define PCI_MAX_BARS 6

/* One decoded Base Address Register. `index` is the BAR number it starts at;
 * a 64-bit memory BAR consumes index and index+1 (the second is reported with
 * valid == 0 so a caller iterating 0..5 never double-counts it). */
struct pci_bar {
	u64 base;        /* CPU-visible base address (or I/O port number) */
	u64 size;        /* region size in bytes, always a power of two */
	u8  index;
	u8  is_io;       /* 1 = I/O space, 0 = memory space */
	u8  is_64bit;    /* memory BAR is 64-bit wide */
	u8  prefetchable;
	u8  valid;       /* 0 = BAR unimplemented, or the upper half of a 64-bit BAR */
};

/*
 * Decode and size one BAR. Sizing writes all-ones to the BAR and reads the
 * mask back, so the device's memory/IO decode is disabled for the duration and
 * the original value is restored before returning — the caller sees no change.
 * Returns 0 on success (check bar->valid), -1 on a bad argument.
 */
int pci_bar_read(u8 bus, u8 slot, u8 func, u8 index, struct pci_bar *bar);

/* Decode every BAR of a function into out[PCI_MAX_BARS]. Returns the number of
 * implemented (valid) BARs. */
int pci_bar_enumerate(u8 bus, u8 slot, u8 func, struct pci_bar *out);

/* Set bits in the command register (idempotent). Returns the register value
 * read back from the device afterwards, so callers can verify the bits stuck —
 * some functions hard-wire capabilities they do not implement. */
u16 pci_command_set(u8 bus, u8 slot, u8 func, u16 bits);
u16 pci_command_clear(u8 bus, u8 slot, u8 func, u16 bits);
/* Enable bus mastering (DMA). Returns 1 if the bit reads back set. */
int pci_enable_bus_master(u8 bus, u8 slot, u8 func);
/* Enable memory and I/O space decoding. Returns the command register. */
u16 pci_enable_decode(u8 bus, u8 slot, u8 func);

/* Walk the standard capability list. Returns the config offset of the first
 * capability with the given id, or 0 when absent (or the function reports no
 * capability list at all). The walk is bounded and rejects cycles. */
u8 pci_find_capability(u8 bus, u8 slot, u8 func, u8 cap_id);

/*
 * PCI Express extended capabilities live at config offsets 0x100.. and are only
 * reachable through memory-mapped ECAM, not the 0xCF8/0xCFC window. Returns the
 * extended-config offset of the capability, or 0 when it is absent or ECAM is
 * unavailable on this machine (pci_ecam_base() == 0).
 */
u8 pci_ecam_available(void);
u64 pci_ecam_base(void);
u32 pci_ecam_read32(u8 bus, u8 slot, u8 func, u16 offset);
void pci_ecam_write32(u8 bus, u8 slot, u8 func, u16 offset, u32 value);
u16 pci_find_ext_capability(u8 bus, u8 slot, u8 func, u16 cap_id);

/*
 * MSI / MSI-X.
 *
 * A message interrupt carries no line: the device writes the vector straight to
 * the local APIC, so nothing is routed through the IOAPIC and there is no line
 * to mask or to share. The driver claims a vector from the dedicated MSI range
 * with msi_alloc_vector() (kernel/include/b1nix/irq.h), programs it here, and
 * its handler is called by msi_dispatch from the IRQ entry path. Both functions
 * take the vector itself, not a legacy line number, and set INTX_DISABLE —
 * MSI and INTx are mutually exclusive by the spec.
 */
/* The PCI-to-PCI bridge `bus` sits behind, if any. Returns 0 and fills the
 * bridge's location, or -1 when the bus is not behind a bridge. */
int pci_bridge_for_bus(u8 bus, u8 *out_bus, u8 *out_slot, u8 *out_func);

/* ACS: does this bridge currently keep the devices below it apart? Read-only —
 * grouping asks this, and asking must not change the machine. */
int pci_acs_isolating(u8 bus, u8 slot, u8 func);

/* Turn the peer-forwarding controls off, so the devices below can be separated.
 * A policy decision: it also pushes device-to-device traffic up through the
 * root complex. Returns 1 when the bridge now isolates. */
int pci_acs_enable(u8 bus, u8 slot, u8 func);
int pci_acs_disable(u8 bus, u8 slot, u8 func);

/* ARI: functions are numbered across the bus rather than 8 per device. */
int pci_has_ari(u8 bus, u8 slot, u8 func);
int pci_has_sriov(u8 bus, u8 slot, u8 func);

int pci_msi_enable(u8 bus, u8 slot, u8 func, u8 vector);
void pci_msi_disable(u8 bus, u8 slot, u8 func);
/* Read back the address/data pair actually programmed into the device. Returns
 * 0 on success, -1 when the function has no MSI capability. */
int pci_msi_readback(u8 bus, u8 slot, u8 func, u64 *out_addr, u16 *out_data,
                     int *out_enabled);

/* Number of MSI-X vectors the function implements, or -1 when it has no MSI-X
 * capability. */
int pci_msix_table_size(u8 bus, u8 slot, u8 func);
/* Map the MSI-X vector table (from the BAR its capability points at), program
 * entry `entry` to deliver `vector` to the BSP, unmask it and set the
 * capability's global MSI-X enable. Returns 0 on success. */
int pci_msix_enable(u8 bus, u8 slot, u8 func, u32 entry, u8 vector);
/* Same, with the message supplied by the caller — what interrupt remapping
 * needs, since there the address names a table entry and the data carries no
 * vector. */
int pci_msix_enable_msg(u8 bus, u8 slot, u8 func, u32 entry, u64 addr,
                        u32 data);
/* Read one programmed MSI-X table entry back out of the device's table. */
int pci_msix_entry_readback(u8 bus, u8 slot, u8 func, u32 entry, u64 *out_addr,
                            u32 *out_data, u32 *out_vector_ctrl);
/* Write a saved entry back verbatim (used to leave a probed device exactly as
 * it was found). */
int pci_msix_entry_restore(u8 bus, u8 slot, u8 func, u32 entry, u64 addr,
                           u32 data, u32 vector_ctrl);
void pci_msix_disable(u8 bus, u8 slot, u8 func);

/*
 * Intel graphics stolen memory.
 *
 * The iGPU's stolen ranges are described by the *host bridge* (00:00.0), not by
 * the GPU function: BDSM (0x5C) is the base of Data Stolen Memory and BGSM
 * (0x70) the base of the GTT Stolen Memory, with the size encoded in the GMCH
 * Graphics Control register GGC (0x50). Returns 0 and fills the struct when an
 * Intel host bridge is present and reports a stolen region, -1 otherwise (which
 * is the expected answer under QEMU, whose 82441FX/Q35 host bridges implement
 * neither register).
 */
struct pci_intel_stolen {
	u64 dsm_base;  /* BDSM: base of data stolen memory */
	u64 gsm_base;  /* BGSM: base of GTT stolen memory */
	u64 dsm_size;  /* decoded from GGC.GMS */
	u64 gsm_size;  /* decoded from GGC.GGMS */
	u16 ggc;       /* raw GGC register, for diagnostics */
};
int pci_intel_stolen_read(struct pci_intel_stolen *out);

/* The decode on its own: GGC/BDSM/BGSM in, bases and sizes out, no hardware
 * touched. Returns 0 when the registers describe a stolen region and -1 when
 * they describe none. Split out because the GMS/GGMS arithmetic can only be
 * driven by values a real iGPU supplies, so it is tested directly instead of
 * being unreachable everywhere except on Intel graphics. */
int pci_intel_stolen_decode(u16 ggc, u32 bdsm, u32 bgsm,
                            struct pci_intel_stolen *out);

/* M98 in-kernel self-test (BAR sizing, capability walk, bus master, MSI/MSI-X
 * programming readback, stolen memory). No-op outside b1nix.test=1. */
void pci_selftest(void);

#endif
