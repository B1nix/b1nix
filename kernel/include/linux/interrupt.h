/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_INTERRUPT_H
#define LKPI_LINUX_INTERRUPT_H
#include <linux/irqreturn.h>
#include <linux/types.h>
/* Handler registration is not wired up yet: b1nix installs interrupt handlers
 * through its own IDT/IOAPIC paths, and which of those a DRM driver should use
 * is M102's question, not the core's. Declared so the core compiles; the first
 * driver that needs a real request_irq gets one then. */
typedef irqreturn_t (*irq_handler_t)(int, void *);
#define IRQF_SHARED 0x00000080
int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
                const char *name, void *dev);
void free_irq(unsigned int irq, void *dev);
static inline void disable_irq(unsigned int irq) { (void)irq; }
static inline void enable_irq(unsigned int irq) { (void)irq; }

/* ── tasklets ────────────────────────────────────────────────────
 *
 * A callback scheduled from an interrupt handler and run later, with the one
 * guarantee the callers depend on: the same tasklet is never running twice at
 * once, however many CPUs schedule it. i915 runs its execlists submission this
 * way, and that serialisation is what lets the submission code touch the
 * engine's queue without a lock of its own.
 *
 * Underneath is b1nix's workqueue, which owns a single kernel thread and runs
 * items one at a time in submission order — so the serialisation is free, and
 * queue_work is safe from an interrupt handler, which is the other requirement.
 * The difference from Linux is context: upstream runs tasklets in softirq, with
 * interrupts enabled but ahead of every thread, and these run in a kernel
 * thread. Later, therefore, and a driver measuring latency from the interrupt
 * will see it. Nothing here may sleep in a tasklet callback regardless — the
 * callers do not, and upstream's contract says they must not.
 */
#include <linux/atomic.h>
#include <linux/workqueue.h>

struct tasklet_struct;
typedef void (*tasklet_callback_t)(struct tasklet_struct *t);

#define TASKLET_STATE_SCHED 0  /* queued, not yet run */
#define TASKLET_STATE_RUN   1  /* a CPU is inside the callback */

struct tasklet_struct {
	struct work_struct work;
	tasklet_callback_t callback;
	void (*func)(unsigned long);  /* the older form, still used in places */
	unsigned long data;
	/* An unsigned long, not a bitfield or an int: i915 reads it with test_bit,
	 * which takes exactly this type. */
	unsigned long state;
	/* Nesting depth of tasklet_disable, as atomic_t because imported code
	 * increments it directly with atomic_fetch_inc. Non-zero means a schedule
	 * is recorded but not run — which is why enable has to re-queue anything
	 * that arrived while it was held. */
	atomic_t count;
};

void tasklet_setup(struct tasklet_struct *t, tasklet_callback_t callback);
void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long),
                  unsigned long data);
void tasklet_schedule(struct tasklet_struct *t);
void tasklet_kill(struct tasklet_struct *t);
void tasklet_disable_nosync(struct tasklet_struct *t);
void tasklet_disable(struct tasklet_struct *t);
void tasklet_enable(struct tasklet_struct *t);
void tasklet_unlock_wait(struct tasklet_struct *t);
int tasklet_trylock(struct tasklet_struct *t);
void tasklet_unlock(struct tasklet_struct *t);

/* There is one queue, so "high priority" is the same queue. Naming it
 * separately would suggest an ordering that does not exist. */
#define tasklet_hi_schedule(t) tasklet_schedule(t)

static inline int tasklet_is_enabled(struct tasklet_struct *t)
{ return t && atomic_read(&t->count) == 0; }
static inline int tasklet_is_scheduled(struct tasklet_struct *t)
{ return t && (t->state & (1UL << TASKLET_STATE_SCHED)) != 0; }
/* tasklet_is_locked is deliberately NOT defined here: i915 defines its own
 * under !CONFIG_SMP, and providing a second one is a redefinition rather than a
 * missing piece. tasklet_trylock/tasklet_unlock above are what it is built on. */
void tasklet_unlock_spin_wait(struct tasklet_struct *t);

/* ── bottom halves ───────────────────────────────────────────────
 *
 * Upstream disables softirq processing on this CPU around a region that must
 * not be re-entered by one. b1nix has no softirqs — the equivalent work runs in
 * a kernel thread — so there is nothing to disable, and preemption is what such
 * a region is actually protected against here.
 */
static inline void local_bh_disable(void) {}
static inline void local_bh_enable(void) {}


/* Recover the containing structure from a tasklet, the way container_of does
 * for a work_struct. Named by upstream after the callback signature it pairs
 * with: tasklet_setup hands the callback the tasklet, and this is how the
 * callback gets back to its owner. */
#define from_tasklet(var, callback_tasklet, tasklet_fieldname) \
	container_of(callback_tasklet, __typeof__(*var), tasklet_fieldname)


/* Wait for any in-flight handler for this IRQ to finish. b1nix runs handlers
 * with interrupts disabled on the CPU taking them and has no per-IRQ in-flight
 * counter to wait on; by the time a caller reaches here on a different CPU the
 * handler it cares about has either run or not been raised. */
static inline void synchronize_irq(unsigned int irq) { (void)irq; }
static inline void synchronize_hardirq(unsigned int irq) { (void)irq; }

#endif
