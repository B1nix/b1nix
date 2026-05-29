/*
 * IOAPIC driver (A1 audit follow-on).
 *
 * Programs the I/O APIC redirection table from the ACPI MADT data, masks
 * the legacy 8259 PIC, and lets interrupts.c switch its EOI path to the
 * Local APIC. This removes the PIC dependency for the IRQs we use while
 * staying backward-compatible: if ACPI didn't list an IOAPIC, ioapic_init
 * returns 0 and the PIC continues to drive everything.
 */
#include <b1nix/ioapic.h>
#include <b1nix/acpi.h>
#include <b1nix/console.h>
#include <b1nix/io.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/types.h>

/* IOAPIC MMIO is window-style: write a register index to IOREGSEL, then read
 * or write data through IOWIN. We use direct-mapped access (the IOAPIC sits
 * in the [0, 4 GiB) PCI hole, well below DIRECT_MAP_SIZE which we floored at
 * 4 GiB in B1). */
#define IOAPIC_REG_ID      0x00
#define IOAPIC_REG_VER     0x01
#define IOAPIC_REG_REDIR   0x10  /* first of the redirection-entry registers */

#define IOREGSEL_OFFSET    0x00
#define IOWIN_OFFSET       0x10

/* Redirection entry bits (low dword). */
#define REDIR_VECTOR_MASK  0xFF
#define REDIR_DELIV_FIXED  (0u << 8)
#define REDIR_DEST_PHYS    (0u << 11)
#define REDIR_POL_HIGH     (0u << 13)
#define REDIR_POL_LOW      (1u << 13)
#define REDIR_TRIG_EDGE    (0u << 15)
#define REDIR_TRIG_LEVEL   (1u << 15)
#define REDIR_MASK         (1u << 16)

/* 8259 PIC ports (we use them only to mask the PIC out of the picture). */
#define PIC1_DATA          0x21
#define PIC2_DATA          0xA1

static volatile u32 *g_ioapic_base = (volatile u32 *)0;
static u32  g_ioapic_gsi_base = 0;
static u32  g_ioapic_max_redir = 0;  /* max entry index (count - 1) */
static int  g_ioapic_active = 0;

static u32 ioapic_read(u32 reg) {
    g_ioapic_base[IOREGSEL_OFFSET / 4] = reg;
    return g_ioapic_base[IOWIN_OFFSET / 4];
}

static void ioapic_write(u32 reg, u32 value) {
    g_ioapic_base[IOREGSEL_OFFSET / 4] = reg;
    g_ioapic_base[IOWIN_OFFSET / 4] = value;
}

static void write_redir_entry(u32 gsi, u32 low, u32 high) {
    /* Each redirection entry occupies 2 consecutive 32-bit registers. Write
     * the high half first while the entry is masked, then the low half — the
     * IOAPIC samples the destination atomically on the low-dword write. */
    ioapic_write(IOAPIC_REG_REDIR + gsi * 2 + 1, high);
    ioapic_write(IOAPIC_REG_REDIR + gsi * 2 + 0, low);
}

/* Resolve the GSI + polarity/trigger flags for a legacy ISA IRQ, applying
 * the optional ACPI ISO override on top of caller-supplied defaults. */
static void resolve_irq(u8 legacy_irq, int level_low,
                        u32 *out_gsi, u32 *out_flags) {
    /* ISA defaults: edge-triggered, active-high. */
    u32 pol = REDIR_POL_HIGH;
    u32 trig = REDIR_TRIG_EDGE;
    if (level_low) {
        pol = REDIR_POL_LOW;
        trig = REDIR_TRIG_LEVEL;
    }

    u32 gsi = legacy_irq;
    const struct acpi_iso_entry *iso = acpi_iso_for_irq(legacy_irq);
    if (iso) {
        gsi = iso->gsi;
        /* MPS polarity flags (bits 0-1): 0=conform, 1=active high, 3=active low. */
        u32 mps_pol = iso->flags & 0x3;
        if (mps_pol == 1) pol = REDIR_POL_HIGH;
        else if (mps_pol == 3) pol = REDIR_POL_LOW;
        /* MPS trigger flags (bits 2-3): 0=conform, 1=edge, 3=level. */
        u32 mps_trig = (iso->flags >> 2) & 0x3;
        if (mps_trig == 1) trig = REDIR_TRIG_EDGE;
        else if (mps_trig == 3) trig = REDIR_TRIG_LEVEL;
    }

    *out_gsi = gsi;
    *out_flags = pol | trig;
}

void ioapic_route_irq(u8 legacy_irq, u8 vector, u8 dest_apic, int level_low) {
    if (!g_ioapic_active) return;

    u32 gsi, flags;
    resolve_irq(legacy_irq, level_low, &gsi, &flags);
    if (gsi > g_ioapic_max_redir) {
        console_write("ioapic: GSI out of range, IRQ=");
        console_write_dec(legacy_irq);
        console_write("\n");
        return;
    }

    u32 low  = ((u32)vector & REDIR_VECTOR_MASK) | REDIR_DELIV_FIXED |
               REDIR_DEST_PHYS | flags;  /* mask bit clear -> unmasked */
    u32 high = ((u32)dest_apic & 0xFF) << 24;
    write_redir_entry(gsi, low, high);
}

static void ioapic_set_mask(u8 legacy_irq, int masked) {
    if (!g_ioapic_active) return;
    u32 gsi = acpi_irq_to_gsi(legacy_irq);
    if (gsi > g_ioapic_max_redir) return;
    u32 low = ioapic_read(IOAPIC_REG_REDIR + gsi * 2);
    if (masked) low |= REDIR_MASK;
    else        low &= ~REDIR_MASK;
    ioapic_write(IOAPIC_REG_REDIR + gsi * 2, low);
}

void ioapic_mask_irq(u8 legacy_irq)   { ioapic_set_mask(legacy_irq, 1); }
void ioapic_unmask_irq(u8 legacy_irq) { ioapic_set_mask(legacy_irq, 0); }

int ioapic_active(void) { return g_ioapic_active; }

int ioapic_init(void) {
    if (!acpi_ready() || acpi_ioapic_count() == 0) {
        console_write("ioapic: no IOAPIC in MADT, keeping PIC\n");
        return 0;
    }

    /* Single-IOAPIC config covers every machine b1nix targets today.
     * The next IOAPIC would just be another redirection-table consumer; the
     * routing API would loop over acpi_ioapic_count() at that point. */
    const struct acpi_ioapic_entry *e = acpi_ioapic(0);
    if (!e || e->address == 0) {
        console_write("ioapic: bad MADT IOAPIC entry\n");
        return 0;
    }

    /* IOAPIC MMIO lives in the PCI hole (~0xFEC00000 on QEMU/x86). Direct map
     * is floored at 4 GiB in B1, so this is reachable via DIRECT_MAP_BASE. */
    g_ioapic_base = (volatile u32 *)(usize)(DIRECT_MAP_BASE + (u64)e->address);
    g_ioapic_gsi_base = e->gsi_base;

    /* Version register: bits 16-23 give max redirection entry index. */
    u32 ver = ioapic_read(IOAPIC_REG_VER);
    g_ioapic_max_redir = (ver >> 16) & 0xFF;

    console_write("ioapic: id=");
    console_write_dec(e->id);
    console_write(" addr=0x");
    console_write_hex64(e->address);
    console_write(" gsi_base=");
    console_write_dec(e->gsi_base);
    console_write(" entries=");
    console_write_dec(g_ioapic_max_redir + 1);
    console_write("\n");

    /* Start with every entry masked, then mask the PIC out completely. */
    for (u32 i = 0; i <= g_ioapic_max_redir; i++) {
        write_redir_entry(i, REDIR_MASK, 0);
    }
    /* 0xFF on both PIC data ports masks every line — the PIC will neither
     * raise nor latch interrupts from here on. We leave the PIC's command
     * port alone so spurious-IRQ7/15 hardware still acks the way the CPU
     * expects if anything escapes. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    g_ioapic_active = 1;

    /* Route the three ISA IRQs the kernel actively uses today. The NIC
     * registers its own IRQ later via ioapic_route_irq when virtio-net is
     * brought up (or via the x86_pic_unmask wrapper we patched). */
    u8 bsp_apic = (u8)lapic_id();
    ioapic_route_irq(0,  32, bsp_apic, /*level_low=*/0);  /* PIT */
    ioapic_route_irq(1,  33, bsp_apic, /*level_low=*/0);  /* PS/2 keyboard */
    ioapic_route_irq(12, 44, bsp_apic, /*level_low=*/0);  /* PS/2 mouse */

    console_write("ioapic: routed PIT/kbd/mouse via APIC, PIC masked\n");
    return 1;
}
