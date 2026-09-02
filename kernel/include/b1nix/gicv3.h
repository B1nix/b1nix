#ifndef B1NIX_GICV3_H
#define B1NIX_GICV3_H

#include <b1nix/types.h>

/* GICv3 (kernel/arch/aarch64/gicv3.c). A board whose device tree reports a
 * GICv2 never enters any of this: gicv3_init() returns -1 and the v2 path in
 * kernel/arch/aarch64/interrupts.c stays in charge. */
int  gicv3_init(void);          /* 0 when a GICv3 was found and brought up */
int  gicv3_present(void);
void gicv3_cpu_init(void);      /* per-CPU: redistributor + ICC_* interface */
void gicv3_enable_irq(u32 irq);
u32  gicv3_ack(void);
void gicv3_eoi(u32 iar);
u32  gicv3_ack_peek(void);
u32  gicv3_isenabler0(void);
u32  gicv3_processor_number(void);
/* This CPU's redistributor base — the ITS addresses one by address. */
u64  gicv3_rdbase(void);
int  gicv3_lpi_enable(u64 prop_table, u32 id_bits, u64 pend_table);

/* The ITS (kernel/arch/aarch64/gicv3_its.c). Without one, this board has no
 * message-signalled interrupts at all. */
int  its_init(void);
int  its_ready(void);
/* Test mode: prove the controller can deliver an LPI on its own. */
void its_selftest(void);
int  its_vector_for_lpi(u32 lpi);
u32  its_lpi_for_vector(int vector);
u64  its_translater_phys(void);

#endif
