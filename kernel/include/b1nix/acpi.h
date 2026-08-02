#ifndef B1NIX_ACPI_H
#define B1NIX_ACPI_H

#include <b1nix/types.h>

/*
 * ACPI / MADT discovery (A1).
 *
 * Walks the BIOS-published ACPI tables (RSDP → RSDT/XSDT → MADT) and exposes
 * the hardware-discovered CPU + interrupt topology so the rest of the kernel
 * (smp_boot_aps, future IOAPIC routing) can stop hardcoding what CPUID + the
 * legacy PIC give us.
 *
 * Layout structs follow ACPI 6.x. Only the fields we need are typed; the rest
 * is left as opaque bytes (the parser walks length-prefixed records).
 */

/* ── RSDP (Root System Description Pointer) ── */
struct acpi_rsdp_v1 {
    char     signature[8];     /* "RSD PTR " */
    u8       checksum;
    char     oem_id[6];
    u8       revision;
    u32      rsdt_address;
} __attribute__((packed));

struct acpi_rsdp_v2 {
    struct acpi_rsdp_v1 v1;
    u32      length;
    u64      xsdt_address;
    u8       extended_checksum;
    u8       reserved[3];
} __attribute__((packed));

/* ── Generic System Description Table header ── */
struct acpi_sdt_header {
    char     signature[4];
    u32      length;
    u8       revision;
    u8       checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    u32      oem_revision;
    u32      creator_id;
    u32      creator_revision;
} __attribute__((packed));

/* ── MADT ("APIC" table) ── */
struct acpi_madt {
    struct acpi_sdt_header header;
    u32      lapic_address;
    u32      flags;             /* bit 0 = PCAT_COMPAT (8259 present) */
    /* Variable-length entry stream follows. */
} __attribute__((packed));

/* MADT entry types we care about (ACPI 6.x table 5-45). */
#define ACPI_MADT_TYPE_LAPIC            0x00
#define ACPI_MADT_TYPE_IOAPIC           0x01
#define ACPI_MADT_TYPE_ISO              0x02   /* Interrupt source override */
#define ACPI_MADT_TYPE_NMI              0x03
#define ACPI_MADT_TYPE_LAPIC_NMI        0x04
#define ACPI_MADT_TYPE_LAPIC_OVERRIDE   0x05
#define ACPI_MADT_TYPE_X2APIC           0x09

struct acpi_madt_entry_hdr {
    u8       type;
    u8       length;
} __attribute__((packed));

struct acpi_madt_lapic {
    struct acpi_madt_entry_hdr hdr;
    u8       processor_uid;
    u8       apic_id;
    u32      flags;             /* bit 0 = enabled, bit 1 = online-capable */
} __attribute__((packed));

#define ACPI_MADT_LAPIC_ENABLED         (1u << 0)
#define ACPI_MADT_LAPIC_ONLINE_CAPABLE  (1u << 1)

struct acpi_madt_ioapic {
    struct acpi_madt_entry_hdr hdr;
    u8       ioapic_id;
    u8       reserved;
    u32      ioapic_address;
    u32      gsi_base;
} __attribute__((packed));

struct acpi_madt_iso {
    struct acpi_madt_entry_hdr hdr;
    u8       bus;               /* always 0 (ISA) */
    u8       source;            /* legacy IRQ source */
    u32      gsi;               /* mapped GSI */
    u16      flags;             /* polarity/trigger MPS bits */
} __attribute__((packed));

/* Matched to lapic.h MAX_CPUS — bumped to 64 in the C3 audit pass so MADT
 * parsing doesn't silently truncate the CPU list on multi-socket boxes. */
#define ACPI_MAX_CPUS                   64
#define ACPI_MAX_IOAPICS                4
#define ACPI_MAX_ISOS                   24

struct acpi_cpu_entry {
    u8  apic_id;
    u8  processor_uid;
    u8  enabled;        /* 1 if MADT marks the CPU enabled OR online-capable */
};

struct acpi_ioapic_entry {
    u8  id;
    u32 address;
    u32 gsi_base;
};

struct acpi_iso_entry {
    u8  source;         /* legacy IRQ */
    u32 gsi;            /* GSI it actually fires on */
    u16 flags;
};

/*
 * acpi_init: scan RSDP, walk RSDT/XSDT, parse MADT.
 * Returns 1 on success (tables found AND parsed), 0 otherwise.
 * Safe to call before lapic_init(); does not touch the LAPIC.
 */
int acpi_init(void);

/* True after acpi_init() found a usable MADT. */
int acpi_ready(void);

/* Look up any ACPI table by its four-character signature ("MCFG", "FADT", ...)
 * in the RSDT/XSDT captured at acpi_init(). Returns a mapped, checksum-verified
 * header, or NULL when the table is absent or ACPI was never found. The mapping
 * is permanent (boot-time MMIO window), so the caller may keep the pointer. */
const struct acpi_sdt_header *acpi_find_table(const char *signature);

/* Discovered CPU count (BSP + enabled APs), 0 if ACPI unavailable. */
int acpi_cpu_count(void);

/* Get the i-th discovered CPU (0..acpi_cpu_count()-1). Returns NULL on OOB. */
const struct acpi_cpu_entry *acpi_cpu(int idx);

/* LAPIC base from MADT (0 if not discovered). Callers may still prefer the
 * IA32_APIC_BASE MSR — this is the firmware-declared default. */
u64 acpi_lapic_address(void);

/* IOAPIC enumeration (for the future IRQ-routing rewrite; not used yet). */
int acpi_ioapic_count(void);
const struct acpi_ioapic_entry *acpi_ioapic(int idx);

/* Interrupt Source Override lookup: given a legacy ISA IRQ, return the GSI it
 * actually fires on. If no override exists, returns the IRQ itself (identity). */
u32 acpi_irq_to_gsi(u8 legacy_irq);

/* Full ISO record for a legacy IRQ, NULL if no override. Callers that need
 * the polarity/trigger MPS flags (for IOAPIC redirection entries) use this. */
const struct acpi_iso_entry *acpi_iso_for_irq(u8 legacy_irq);

#endif /* B1NIX_ACPI_H */
