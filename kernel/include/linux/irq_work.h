/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_IRQ_WORK_H
#define LKPI_LINUX_IRQ_WORK_H
#include <linux/interrupt.h>
#include <linux/llist.h>
#include <linux/workqueue.h>

/*
 * Work deferred out of hard-interrupt context.
 *
 * Linux runs these from a self-IPI, so they execute with interrupts enabled but
 * still ahead of any thread. b1nix has no such context: the equivalent is the
 * workqueue, which runs on a kernel thread. That is later than upstream's, and
 * the difference matters for anything measuring latency from the interrupt —
 * so it is stated here rather than left to be discovered in a trace.
 */
struct irq_work {
	struct work_struct work;
	void (*func)(struct irq_work *);
	/* Upstream links pending items on a per-CPU list through this node, and
	 * imported code initialises it. The workqueue underneath carries its own
	 * linkage, so nothing here reads it — but it is a real member rather than
	 * a macro alias for `work`: a #define of a common field name rewrites
	 * every unrelated `node` in the tree, which is the trap that cost a day on
	 * `mutex` in M101. */
	struct __call_single_node { struct llist_node llist; u16 flags; } node;
};

void lkpi_irq_work_queue(struct irq_work *w);

#define __IRQ_WORK_INIT(_func) { .func = (_func) }
#define DEFINE_IRQ_WORK(name, _func) struct irq_work name = __IRQ_WORK_INIT(_func)

static inline void init_irq_work(struct irq_work *w, void (*func)(struct irq_work *))
{ w->func = func; }
static inline bool irq_work_queue(struct irq_work *w) { lkpi_irq_work_queue(w); return true; }
static inline void irq_work_sync(struct irq_work *w) { (void)w; }

#endif
