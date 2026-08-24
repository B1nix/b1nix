/*
 * TIOCCONS — send the kernel console somewhere else.
 *
 * `setconsole /dev/ttyS1` asks the kernel to print on that terminal instead of
 * on its own console. The catch is where the kernel prints from:
 * console_write() runs under a spinlock with interrupts disabled, and is
 * called from interrupt handlers and from the panic path. A device write can
 * sleep, so calling one from there would deadlock the machine at the first log
 * line — the redirection has to be handed to something that is allowed to
 * sleep.
 *
 * So the console pushes bytes into a ring and returns; a kernel thread drains
 * the ring into the target. Output arrives a moment late, and never at the
 * cost of the caller. If the ring overflows the oldest bytes are dropped and
 * the loss is reported once — a redirected console that silently swallows
 * output would be worse than one that says it fell behind.
 */

#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/vfs.h>
#include <string.h>

#define CR_RING 8192

static char cr_ring[CR_RING];
static volatile usize cr_head;  /* next write position */
static volatile usize cr_tail;  /* next read position */
static volatile int cr_dropped;
static volatile int cr_active;

/* The terminal the console is redirected to, with a reference held for as long
 * as the redirection lasts: the node must not be freed under the drain thread
 * because the process that set it up exited. */
static struct vfs_node *cr_target;
static spinlock_t cr_lock;
static int cr_thread_started;

static void console_redirect_drain(void *arg);

int console_redirect_set(struct vfs_node *node)
{
	u64 flags;
	struct vfs_node *old;

	spin_lock_irqsave(&cr_lock, &flags);
	old = cr_target;
	if (node)
		vfs_node_get(node);
	cr_target = node;
	cr_active = node ? 1 : 0;
	/* Start clean: bytes queued for the previous terminal are not this
	 * terminal's to print. */
	cr_head = cr_tail = 0;
	cr_dropped = 0;
	spin_unlock_irqrestore(&cr_lock, flags);

	if (old)
		vfs_node_put(old);

	if (node && !cr_thread_started) {
		cr_thread_started = 1;
		if (kthread_create("console-redirect", console_redirect_drain, 0) < 0) {
			cr_thread_started = 0;
			console_redirect_set(0);
			return -1;
		}
	}
	return 0;
}

/* Called from console_write with the console lock held and interrupts off:
 * this must not sleep, must not take a lock anyone else holds while sleeping,
 * and must be cheap when no redirection is set (which is always, normally). */
void console_redirect_push(const char *text)
{
	if (!cr_active || !text)
		return;

	for (usize i = 0; text[i] != '\0'; i++) {
		usize head = cr_head;
		usize next = (head + 1) % CR_RING;

		if (next == cr_tail) {
			cr_dropped = 1;
			return;
		}
		cr_ring[head] = text[i];
		cr_head = next;
	}
}

static void console_redirect_drain(void *arg)
{
	(void)arg;

	for (;;) {
		char batch[256];
		usize n = 0;
		struct vfs_node *target;
		u64 flags;

		spin_lock_irqsave(&cr_lock, &flags);
		target = cr_target;
		while (n < sizeof(batch) - 1 && cr_tail != cr_head) {
			batch[n++] = cr_ring[cr_tail];
			cr_tail = (cr_tail + 1) % CR_RING;
		}
		int dropped = cr_dropped;
		cr_dropped = 0;
		spin_unlock_irqrestore(&cr_lock, flags);

		if (n > 0 && target) {
			batch[n] = '\0';
			vfs_node_pwrite(target, batch, n, 0);
		}
		if (dropped && target)
			vfs_node_pwrite(target,
					"[console redirect: output lost]\n", 32, 0);

		if (n == 0)
			scheduler_block_on_timeout(&cr_ring, SCHED_MS_TO_TICKS(20));
	}
}
