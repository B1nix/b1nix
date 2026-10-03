// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host interrupts for arm64 KVM (M131), over b1nix's GICv3 driver.
 *
 * KVM takes three per-CPU interrupts (the EL1 virtual and physical timers
 * and the vGIC maintenance interrupt), and it owns the GIC state of the
 * virtual timer while a guest runs: the interrupt stays active on the host
 * side, "forwarded" to the vCPU, until the guest deactivates it through its
 * list register. Linux numbers interrupts through irq domains; here a Linux
 * interrupt number is the GIC INTID, one descriptor each, and the chip calls
 * the GIC driver through the bridge.
 *
 * GICv4 direct injection is not driven (b1nix's ITS has no vPE support), so
 * KVM finds no GICv4 and never reaches the vLPI/vPE calls, and a hierarchy
 * over the GIC (for a GIC without hardware deactivation) cannot be built.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/arm-gic-v4.h>
#include <linux/irqchip/arm-vgic-info.h>
#include <linux/property.h>
#include <b1nix/kvm_bridge.h>

#define GIC_NR_IRQS 1020

static struct irq_domain gic_domain = { .name = "gicv3" };

static void gic_mask(struct irq_data *d)
{
	b1nix_kvm_irq_disable(d->hwirq);
}

static void gic_unmask(struct irq_data *d)
{
	b1nix_kvm_irq_enable(d->hwirq);
}

static int gic_get_state(struct irq_data *d, enum irqchip_irq_state which, bool *val)
{
	int state;

	if (which != IRQCHIP_STATE_PENDING && which != IRQCHIP_STATE_ACTIVE)
		return -EINVAL;
	if (b1nix_kvm_irq_state(d->hwirq, which == IRQCHIP_STATE_ACTIVE, &state))
		return -EINVAL;
	*val = state;
	return 0;
}

static int gic_set_state(struct irq_data *d, enum irqchip_irq_state which, bool val)
{
	if (which != IRQCHIP_STATE_PENDING && which != IRQCHIP_STATE_ACTIVE)
		return -EINVAL;
	return b1nix_kvm_irq_set_state(d->hwirq, which == IRQCHIP_STATE_ACTIVE, val) ?
		-EINVAL : 0;
}

/* A vCPU for the interrupt: its deactivation becomes the guest's, which
 * needs the GIC's split priority drop and deactivation (EOImode 1). */
static int gic_set_vcpu_affinity(struct irq_data *d, void *vcpu)
{
	if (!b1nix_kvm_gic_eoimode_split())
		return -EINVAL;
	if (vcpu)
		irqd_set_forwarded_to_vcpu(d);
	else
		irqd_clr_forwarded_to_vcpu(d);
	b1nix_kvm_irq_set_forwarded(d->hwirq, vcpu != NULL);
	return 0;
}

static struct irq_chip gic_chip = {
	.name			= "GICv3",
	.irq_mask		= gic_mask,
	.irq_unmask		= gic_unmask,
	.irq_get_irqchip_state	= gic_get_state,
	.irq_set_irqchip_state	= gic_set_state,
	.irq_set_vcpu_affinity	= gic_set_vcpu_affinity,
};

static struct irq_desc descs[GIC_NR_IRQS];

struct irq_desc *irq_to_desc(unsigned int irq)
{
	struct irq_desc *desc;

	if (irq >= GIC_NR_IRQS)
		return NULL;
	desc = &descs[irq];
	if (!desc->irq_data.chip) {
		desc->irq_data.irq = irq;
		desc->irq_data.hwirq = irq;
		desc->irq_data.domain = &gic_domain;
		desc->irq_data.chip = &gic_chip;
	}
	return desc;
}

struct irq_data *irq_get_irq_data(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	return desc ? &desc->irq_data : NULL;
}

/* The trigger the device tree gave the interrupt (set at start-up), as
 * IRQF_TRIGGER_* -- the same values as IRQ_TYPE_*. */
u32 irq_get_trigger_type(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	return desc ? desc->trigger : 0;
}

void lkpi_arm64_irq_set_trigger(unsigned int irq, u32 trigger)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (desc)
		desc->trigger = trigger & IRQF_TRIGGER_MASK;
}

void irq_set_status_flags(unsigned int irq, unsigned long set)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (desc)
		desc->status |= set;
}

void irq_clear_status_flags(unsigned int irq, unsigned long clr)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (desc)
		desc->status &= ~clr;
}

/* ── per-CPU interrupts ───────────────────────────────────────────────── */

/* From b1nix's IRQ dispatch, on the CPU that took the interrupt. */
static int percpu_irq_entry(void *ctx)
{
	struct irq_desc *desc = ctx;

	return desc->handler(desc->irq_data.irq,
			     this_cpu_ptr(desc->percpu_dev_id)) == IRQ_HANDLED;
}

int request_percpu_irq(unsigned int irq, irq_handler_t handler,
		       const char *devname, void __percpu *percpu_dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || !handler || desc->handler)
		return -EINVAL;
	desc->handler = handler;
	desc->percpu_dev_id = percpu_dev_id;
	desc->name = devname;
	if (b1nix_kvm_irq_register(irq, percpu_irq_entry, desc)) {
		desc->handler = NULL;
		return -EBUSY;
	}
	return 0;
}

void free_percpu_irq(unsigned int irq, void __percpu *percpu_dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || !desc->handler || desc->percpu_dev_id != percpu_dev_id)
		return;
	b1nix_kvm_irq_unregister(irq, percpu_irq_entry, desc);
	desc->handler = NULL;
	desc->percpu_dev_id = NULL;
}

/* This CPU's copy of a PPI. The GIC programs a PPI's trigger per CPU and
 * the architected timer and maintenance lines are level; nothing to set. */
void enable_percpu_irq(unsigned int irq, unsigned int type)
{
	(void)type;
	b1nix_kvm_irq_enable(irq);
}

void disable_percpu_irq(unsigned int irq)
{
	b1nix_kvm_irq_disable(irq);
}

void lkpi_gic_enable_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc)
		return;
	desc->irq_data.state &= ~IRQD_IRQ_DISABLED;
	b1nix_kvm_irq_enable(irq);
}

void lkpi_gic_disable_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc)
		return;
	desc->irq_data.state |= IRQD_IRQ_DISABLED;
	b1nix_kvm_irq_disable(irq);
}

/* ── chip state ───────────────────────────────────────────────────────── */

int irq_set_irqchip_state(unsigned int irq, enum irqchip_irq_state which, bool val)
{
	struct irq_data *d = irq_get_irq_data(irq);

	if (!d || !d->chip->irq_set_irqchip_state)
		return -EINVAL;
	return d->chip->irq_set_irqchip_state(d, which, val);
}

int irq_get_irqchip_state(unsigned int irq, enum irqchip_irq_state which, bool *state)
{
	struct irq_data *d = irq_get_irq_data(irq);

	if (!d || !d->chip->irq_get_irqchip_state)
		return -EINVAL;
	return d->chip->irq_get_irqchip_state(d, which, state);
}

int irq_set_vcpu_affinity(unsigned int irq, void *vcpu_info)
{
	struct irq_data *d = irq_get_irq_data(irq);

	if (!d || !d->chip->irq_set_vcpu_affinity)
		return -ENOSYS;
	return d->chip->irq_set_vcpu_affinity(d, vcpu_info);
}

/* Moving an interrupt between CPUs is GICv4 vPE doorbell business here;
 * b1nix routes every SPI to the boot CPU. */
int irq_set_affinity(unsigned int irq, const struct cpumask *cpumask)
{
	(void)irq;
	(void)cpumask;
	return -EINVAL;
}

/* A hierarchy's calls down to the parent chip. The GIC domain is flat, so
 * only a stacked chip -- which cannot be created here -- has a parent. */
void irq_chip_mask_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_mask(data);
}

void irq_chip_unmask_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_unmask(data);
}

void irq_chip_eoi_parent(struct irq_data *data)
{
	data = data->parent_data;
	if (data->chip->irq_eoi)
		data->chip->irq_eoi(data);
}

int irq_chip_set_type_parent(struct irq_data *data, unsigned int type)
{
	data = data->parent_data;
	return data->chip->irq_set_type ? data->chip->irq_set_type(data, type) : -ENOSYS;
}

int irq_chip_set_parent_state(struct irq_data *data, enum irqchip_irq_state which,
			      bool val)
{
	data = data->parent_data;
	return data->chip->irq_set_irqchip_state ?
		data->chip->irq_set_irqchip_state(data, which, val) : -ENOSYS;
}

/* ── domains ──────────────────────────────────────────────────────────── */

struct fwnode_handle *irq_domain_alloc_named_fwnode(const char *name)
{
	(void)name;
	return kzalloc(sizeof(struct fwnode_handle), GFP_KERNEL);
}

void irq_domain_free_fwnode(struct fwnode_handle *fwnode)
{
	kfree(fwnode);
}

struct irq_domain *irq_domain_create_hierarchy(struct irq_domain *parent,
					       unsigned int flags, unsigned int size,
					       struct fwnode_handle *fwnode,
					       const struct irq_domain_ops *ops,
					       void *host_data)
{
	(void)parent; (void)flags; (void)size; (void)fwnode; (void)ops; (void)host_data;
	pr_warn("kvm: no interrupt hierarchy over the GIC\n");
	return NULL;
}

int irq_domain_push_irq(struct irq_domain *domain, int virq, void *arg)
{
	(void)domain; (void)virq; (void)arg;
	return -ENOSYS;
}

int irq_domain_set_hwirq_and_chip(struct irq_domain *domain, unsigned int virq,
				  irq_hw_number_t hwirq, const struct irq_chip *chip,
				  void *chip_data)
{
	(void)domain; (void)virq; (void)hwirq; (void)chip; (void)chip_data;
	return -ENOSYS;
}

unsigned int irq_find_mapping(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	(void)domain; (void)hwirq;
	return 0;
}

int irq_domain_activate_irq(struct irq_data *d, bool reserve)
{
	(void)d; (void)reserve;
	return -ENOSYS;
}

void irq_domain_deactivate_irq(struct irq_data *d)
{
	(void)d;
}

/* ── GICv4, not driven ────────────────────────────────────────────────── */

bool gic_cpuif_has_vsgi(void) { return false; }
int its_alloc_vcpu_irqs(struct its_vm *vm) { (void)vm; return -ENXIO; }
void its_free_vcpu_irqs(struct its_vm *vm) { (void)vm; }
int its_make_vpe_resident(struct its_vpe *vpe, bool g0en, bool g1en)
{ (void)vpe; (void)g0en; (void)g1en; return -ENXIO; }
int its_make_vpe_non_resident(struct its_vpe *vpe, bool db) { (void)vpe; (void)db; return -ENXIO; }
int its_commit_vpe(struct its_vpe *vpe) { (void)vpe; return -ENXIO; }
int its_invall_vpe(struct its_vpe *vpe) { (void)vpe; return -ENXIO; }
int its_map_vlpi(int irq, struct its_vlpi_map *map) { (void)irq; (void)map; return -ENXIO; }
int its_get_vlpi(int irq, struct its_vlpi_map *map) { (void)irq; (void)map; return -ENXIO; }
void its_unmap_vlpi(int irq) { (void)irq; }
int its_prop_update_vlpi(int irq, u8 config, bool inv) { (void)irq; (void)config; (void)inv; return -ENXIO; }
int its_prop_update_vsgi(int irq, u8 priority, bool group) { (void)irq; (void)priority; (void)group; return -ENXIO; }

/* ── what the GIC driver tells KVM ────────────────────────────────────── */

/* The vGIC's view of the host GIC, handed over the way Linux's GICv3 driver
 * does at probe (vgic_set_kvm_info): a GICv3 with its maintenance interrupt,
 * hardware deactivation, no GICv2 compatibility frame, no GICv4. */
int lkpi_arm64_gic_kvm_info(void)
{
	struct gic_kvm_info info = { .type = GIC_V3 };

	if (!b1nix_kvm_gic_v3())
		return -ENODEV;
	info.maint_irq = b1nix_kvm_gic_maint_irq();
	if (!info.maint_irq)
		return -ENODEV;
	lkpi_arm64_irq_set_trigger(info.maint_irq, b1nix_kvm_gic_maint_irq_flags());
	vgic_set_kvm_info(&info);
	return 0;
}
