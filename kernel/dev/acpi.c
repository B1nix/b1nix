/*
 * ACPI table parser (RSDP -> RSDT/XSDT -> MADT).
 *
 * Goal (audit item A1): replace CPUID-only CPU enumeration and the assumption
 * of a legacy PIC with the real, firmware-described topology. This module only
 * READS the BIOS tables; smp_boot_aps consumes the result.
 *
 * Memory access: every physical address dereferenced here is below 4 GiB
 * (RSDP search regions, RSDT/XSDT, SDT bodies — all reside in low BIOS/ACPI
 * RAM), well inside the [0, DIRECT_MAP_SIZE) window mapped by vmm_init. So we
 * always read through DIRECT_MAP_BASE + phys. Callers must invoke acpi_init()
 * after vmm_init() (direct_map_ready == 1) and before lapic_init().
 */
#include <b1nix/acpi.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/types.h>
#include <string.h>

static int g_acpi_ready = 0;
static u64 g_lapic_addr = 0;

static struct acpi_cpu_entry    g_cpus[ACPI_MAX_CPUS];
static int                      g_cpu_count = 0;

static struct acpi_ioapic_entry g_ioapics[ACPI_MAX_IOAPICS];
static int                      g_ioapic_count = 0;

static struct acpi_iso_entry    g_isos[ACPI_MAX_ISOS];
static int                      g_iso_count = 0;

/* ── helpers ───────────────────────────────────────────────────── */

static inline void *phys_to_virt(u64 phys) {
    return (void *)(usize)(DIRECT_MAP_BASE + phys);
}

static int acpi_checksum_ok(const void *p, usize len) {
    const u8 *b = (const u8 *)p;
    u8 sum = 0;
    for (usize i = 0; i < len; i++)
        sum = (u8)(sum + b[i]);
    return sum == 0;
}

static int sig_eq(const char *a, const char *b, usize n) {
    for (usize i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* ── RSDP search ───────────────────────────────────────────────── */

static const struct acpi_rsdp_v1 *try_rsdp_at(u64 phys) {
    /* The signature is 8 bytes at 16-byte alignment. */
    const struct acpi_rsdp_v1 *r = (const struct acpi_rsdp_v1 *)phys_to_virt(phys);
    if (!sig_eq(r->signature, "RSD PTR ", 8))
        return (const struct acpi_rsdp_v1 *)0;
    /* v1 checksum covers the first 20 bytes. */
    if (!acpi_checksum_ok(r, sizeof(struct acpi_rsdp_v1)))
        return (const struct acpi_rsdp_v1 *)0;
    if (r->revision >= 2) {
        const struct acpi_rsdp_v2 *r2 = (const struct acpi_rsdp_v2 *)r;
        /* Bound the length so a corrupt firmware entry can't make us read off
         * the end of the BIOS region. */
        if (r2->length < sizeof(*r2) || r2->length > 4096)
            return (const struct acpi_rsdp_v1 *)0;
        if (!acpi_checksum_ok(r2, r2->length))
            return (const struct acpi_rsdp_v1 *)0;
    }
    return r;
}

static const struct acpi_rsdp_v1 *find_rsdp(void) {
    /* 1) EBDA: BDA at phys 0x40E holds a real-mode segment; shifting left 4
     *    gives the EBDA base. Search the first 1 KiB at 16-byte stride. */
    u16 ebda_seg = *(volatile u16 *)phys_to_virt(0x40E);
    u64 ebda_base = ((u64)ebda_seg) << 4;
    if (ebda_base >= 0x80000 && ebda_base < 0xA0000) {
        for (u64 p = ebda_base; p < ebda_base + 1024; p += 16) {
            const struct acpi_rsdp_v1 *r = try_rsdp_at(p);
            if (r) return r;
        }
    }
    /* 2) The legacy BIOS region 0xE0000-0xFFFFF. */
    for (u64 p = 0xE0000; p < 0x100000; p += 16) {
        const struct acpi_rsdp_v1 *r = try_rsdp_at(p);
        if (r) return r;
    }
    return (const struct acpi_rsdp_v1 *)0;
}

/* ── MADT parser ───────────────────────────────────────────────── */

static void add_cpu(u8 apic_id, u8 uid, int enabled) {
    if (g_cpu_count >= ACPI_MAX_CPUS)
        return;
    g_cpus[g_cpu_count].apic_id = apic_id;
    g_cpus[g_cpu_count].processor_uid = uid;
    g_cpus[g_cpu_count].enabled = (u8)(enabled ? 1 : 0);
    g_cpu_count++;
}

static void parse_madt(const struct acpi_madt *madt) {
    g_lapic_addr = madt->lapic_address;

    const u8 *cur = (const u8 *)madt + sizeof(struct acpi_madt);
    const u8 *end = (const u8 *)madt + madt->header.length;

    while (cur + sizeof(struct acpi_madt_entry_hdr) <= end) {
        const struct acpi_madt_entry_hdr *h = (const struct acpi_madt_entry_hdr *)cur;
        if (h->length < sizeof(*h) || cur + h->length > end)
            break;  /* malformed — stop, don't crash */

        switch (h->type) {
        case ACPI_MADT_TYPE_LAPIC: {
            const struct acpi_madt_lapic *l = (const struct acpi_madt_lapic *)cur;
            int en = (l->flags & ACPI_MADT_LAPIC_ENABLED) ||
                     (l->flags & ACPI_MADT_LAPIC_ONLINE_CAPABLE);
            add_cpu(l->apic_id, l->processor_uid, en);
            break;
        }
        case ACPI_MADT_TYPE_IOAPIC: {
            const struct acpi_madt_ioapic *io = (const struct acpi_madt_ioapic *)cur;
            if (g_ioapic_count < ACPI_MAX_IOAPICS) {
                g_ioapics[g_ioapic_count].id = io->ioapic_id;
                g_ioapics[g_ioapic_count].address = io->ioapic_address;
                g_ioapics[g_ioapic_count].gsi_base = io->gsi_base;
                g_ioapic_count++;
            }
            break;
        }
        case ACPI_MADT_TYPE_ISO: {
            const struct acpi_madt_iso *iso = (const struct acpi_madt_iso *)cur;
            if (g_iso_count < ACPI_MAX_ISOS) {
                g_isos[g_iso_count].source = iso->source;
                g_isos[g_iso_count].gsi = iso->gsi;
                g_isos[g_iso_count].flags = iso->flags;
                g_iso_count++;
            }
            break;
        }
        case ACPI_MADT_TYPE_LAPIC_OVERRIDE: {
            /* 64-bit override of the LAPIC physical address. The fields after
             * the 2-byte header are: 2 bytes reserved, then u64 address. */
            if (h->length >= 12) {
                u64 addr = *(const u64 *)(cur + 4);
                g_lapic_addr = addr;
            }
            break;
        }
        default:
            break;  /* ignore types we don't model yet */
        }
        cur += h->length;
    }
}

/* ── SDT walker ────────────────────────────────────────────────── */

static const struct acpi_sdt_header *map_sdt(u64 phys) {
    const struct acpi_sdt_header *h;
    if (phys < DIRECT_MAP_SIZE) {
        /* Low ACPI RAM — already covered by the direct map (the common case). */
        h = (const struct acpi_sdt_header *)phys_to_virt(phys);
    } else {
        /* Firmware on a large-RAM machine (or QEMU with a big -m) places the
         * RSDT/XSDT and their tables just below 4 GiB, beyond the direct map.
         * Map the header first to read the length, then map the whole table
         * into the MMIO window. (mmio mappings are never reclaimed, but ACPI
         * init touches only a handful of small tables once at boot.) */
        h = (const struct acpi_sdt_header *)vmm_map_mmio(phys, sizeof(*h),
                                                         VMM_PRESENT);
        if (!h)
            return (const struct acpi_sdt_header *)0;
        u32 len = h->length;
        if (len < sizeof(*h) || len > 65536)
            return (const struct acpi_sdt_header *)0;
        h = (const struct acpi_sdt_header *)vmm_map_mmio(phys, len, VMM_PRESENT);
        if (!h)
            return (const struct acpi_sdt_header *)0;
    }
    if (h->length < sizeof(*h))
        return (const struct acpi_sdt_header *)0;
    /* Bound the table length (largest legitimate tables are a few KB). */
    if (h->length > 65536)
        return (const struct acpi_sdt_header *)0;
    if (!acpi_checksum_ok(h, h->length))
        return (const struct acpi_sdt_header *)0;
    return h;
}

static const struct acpi_madt *find_madt_via_rsdt(u64 rsdt_phys) {
    const struct acpi_sdt_header *rsdt = map_sdt(rsdt_phys);
    if (!rsdt) return (const struct acpi_madt *)0;
    if (!sig_eq(rsdt->signature, "RSDT", 4)) return (const struct acpi_madt *)0;

    int n = (int)((rsdt->length - sizeof(*rsdt)) / sizeof(u32));
    const u32 *entries = (const u32 *)((const u8 *)rsdt + sizeof(*rsdt));
    for (int i = 0; i < n; i++) {
        const struct acpi_sdt_header *t = map_sdt((u64)entries[i]);
        if (t && sig_eq(t->signature, "APIC", 4))
            return (const struct acpi_madt *)t;
    }
    return (const struct acpi_madt *)0;
}

static const struct acpi_madt *find_madt_via_xsdt(u64 xsdt_phys) {
    const struct acpi_sdt_header *xsdt = map_sdt(xsdt_phys);
    if (!xsdt) return (const struct acpi_madt *)0;
    if (!sig_eq(xsdt->signature, "XSDT", 4)) return (const struct acpi_madt *)0;

    int n = (int)((xsdt->length - sizeof(*xsdt)) / sizeof(u64));
    /* XSDT entries are 8-byte unaligned in the table — read byte-by-byte. */
    const u8 *base = (const u8 *)xsdt + sizeof(*xsdt);
    for (int i = 0; i < n; i++) {
        u64 phys = 0;
        for (int b = 0; b < 8; b++)
            phys |= ((u64)base[i * 8 + b]) << (b * 8);
        /* XSDT can point above 4 GiB on real hardware, which the 32-bit kernel
         * cannot address at all (no PAE); skip those. Tables between the direct
         * map and 4 GiB are reachable via map_sdt's MMIO fallback. */
        if (phys > 0xFFFFFFFFULL)
            continue;
        const struct acpi_sdt_header *t = map_sdt(phys);
        if (t && sig_eq(t->signature, "APIC", 4))
            return (const struct acpi_madt *)t;
    }
    return (const struct acpi_madt *)0;
}

/* ── Generic table lookup ──────────────────────────────────────── */

/* Root table addresses captured at acpi_init so later subsystems (M98's PCI
 * ECAM, which needs MCFG) can find their own tables without re-scanning for the
 * RSDP. Zero when ACPI was not found. */
static u64 g_rsdt_phys;
static u64 g_xsdt_phys;

const struct acpi_sdt_header *acpi_find_table(const char *signature) {
    if (!signature)
        return (const struct acpi_sdt_header *)0;

    if (g_xsdt_phys) {
        const struct acpi_sdt_header *xsdt = map_sdt(g_xsdt_phys);
        if (xsdt && sig_eq(xsdt->signature, "XSDT", 4)) {
            int n = (int)((xsdt->length - sizeof(*xsdt)) / sizeof(u64));
            const u8 *base = (const u8 *)xsdt + sizeof(*xsdt);
            for (int i = 0; i < n; i++) {
                u64 phys = 0;
                for (int b = 0; b < 8; b++)
                    phys |= ((u64)base[i * 8 + b]) << (b * 8);
                if (phys > 0xFFFFFFFFULL)
                    continue;
                const struct acpi_sdt_header *t = map_sdt(phys);
                if (t && sig_eq(t->signature, signature, 4))
                    return t;
            }
        }
    }

    if (g_rsdt_phys) {
        const struct acpi_sdt_header *rsdt = map_sdt(g_rsdt_phys);
        if (rsdt && sig_eq(rsdt->signature, "RSDT", 4)) {
            int n = (int)((rsdt->length - sizeof(*rsdt)) / sizeof(u32));
            const u32 *entries = (const u32 *)((const u8 *)rsdt + sizeof(*rsdt));
            for (int i = 0; i < n; i++) {
                const struct acpi_sdt_header *t = map_sdt((u64)entries[i]);
                if (t && sig_eq(t->signature, signature, 4))
                    return t;
            }
        }
    }

    return (const struct acpi_sdt_header *)0;
}

/* ── Public API ────────────────────────────────────────────────── */

int acpi_init(void) {
    const struct acpi_rsdp_v1 *rsdp = find_rsdp();
    if (!rsdp) {
        console_write("acpi: no RSDP found (legacy boot path)\n");
        return 0;
    }

    console_write("acpi: RSDP rev=");
    console_write_dec(rsdp->revision);
    console_write("\n");

    const struct acpi_madt *madt = (const struct acpi_madt *)0;
    g_rsdt_phys = rsdp->rsdt_address;
    if (rsdp->revision >= 2) {
        const struct acpi_rsdp_v2 *r2 = (const struct acpi_rsdp_v2 *)rsdp;
        if (r2->xsdt_address && r2->xsdt_address <= 0xFFFFFFFFULL)
            g_xsdt_phys = r2->xsdt_address;
        if (r2->xsdt_address)
            madt = find_madt_via_xsdt(r2->xsdt_address);
    }
    if (!madt && rsdp->rsdt_address)
        madt = find_madt_via_rsdt(rsdp->rsdt_address);

    if (!madt) {
        console_write("acpi: MADT not found\n");
        return 0;
    }

    parse_madt(madt);
    g_acpi_ready = 1;

    console_write("acpi: lapic_addr=0x");
    console_write_hex64(g_lapic_addr);
    console_write(" cpus=");
    console_write_dec(g_cpu_count);
    console_write(" ioapics=");
    console_write_dec(g_ioapic_count);
    console_write(" isos=");
    console_write_dec(g_iso_count);
    console_write("\n");
    return 1;
}

int acpi_ready(void) {
    return g_acpi_ready;
}

int acpi_cpu_count(void) {
    return g_cpu_count;
}

const struct acpi_cpu_entry *acpi_cpu(int idx) {
    if (idx < 0 || idx >= g_cpu_count)
        return (const struct acpi_cpu_entry *)0;
    return &g_cpus[idx];
}

u64 acpi_lapic_address(void) {
    return g_lapic_addr;
}

int acpi_ioapic_count(void) {
    return g_ioapic_count;
}

const struct acpi_ioapic_entry *acpi_ioapic(int idx) {
    if (idx < 0 || idx >= g_ioapic_count)
        return (const struct acpi_ioapic_entry *)0;
    return &g_ioapics[idx];
}

u32 acpi_irq_to_gsi(u8 legacy_irq) {
    for (int i = 0; i < g_iso_count; i++)
        if (g_isos[i].source == legacy_irq)
            return g_isos[i].gsi;
    return legacy_irq;
}

const struct acpi_iso_entry *acpi_iso_for_irq(u8 legacy_irq) {
    for (int i = 0; i < g_iso_count; i++)
        if (g_isos[i].source == legacy_irq)
            return &g_isos[i];
    return (const struct acpi_iso_entry *)0;
}
