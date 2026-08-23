/* Lockdep-light implementation. See kernel/include/b1nix/lockdep.h.
 *
 * Translation-unit-level no-op when KERNEL_LOCKDEP is not defined — the
 * compiler still gets a .o so the Makefile entry stays stable across
 * configurations, but every public function body collapses to nothing.
 */

#include <lkpi/lock.h>
#include <b1nix/console.h>
#include <b1nix/lapic.h>
#include <b1nix/lockdep.h>
#include <b1nix/panic.h>
#include <b1nix/arch_x86_64.h>
#include <b1nix/klog.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>

#ifdef KERNEL_LOCKDEP

#define LOCKDEP_STACK_DEPTH  16

struct lockdep_cpu_state {
    /* Stack of currently-held lock levels, top at index `depth - 1`. */
    int levels[LOCKDEP_STACK_DEPTH];
    const char *names[LOCKDEP_STACK_DEPTH];
    int depth;
    /* Plain spinlocks this CPU currently holds, and where it took each. A
     * spinlock_t has no room for an owner, so the record lives beside the CPU
     * instead of inside the lock. */
    const void *spin_held[LOCKDEP_STACK_DEPTH];
    u64 spin_site[LOCKDEP_STACK_DEPTH];
};

/* Per-CPU state lives in a static array sized for the compile-time MAX_CPUS
 * ceiling. We can't put this in struct percpu because lockdep callers may
 * fire before percpu_init (during early bring-up); the BSP slot is
 * cpu_id 0 by convention, and APs each get their slot in ap_main. */
static struct lockdep_cpu_state g_lockdep[MAX_CPUS];

/* "Bequeathing" lock level (M28 #2 Variant A): the per-inode sleeping rwlock
 * can be released by a CPU other than the one that acquired it (its holder
 * blocks via scheduler_block_on and may resume on another CPU). Its
 * LOCKDEP_*_GLOBAL helpers bypass the per-CPU acquisition stack and do only
 * the inversion check on acquire. The BKL was the other bequeath client; it
 * was removed in M28 #7, so the inode lock is now the only one. No global
 * counter is bumped — a shared cache line here would perturb syscall density
 * for no detection benefit; the acquire-side order check is what matters. */

static int lockdep_self_cpu(void) {
    struct percpu *p = get_percpu();
    return p ? (int)p->cpu_id : 0;
}

void lockdep_acquire(int level, const char *name) {
    int cpu = lockdep_self_cpu();
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    struct lockdep_cpu_state *s = &g_lockdep[cpu];

    if (s->depth >= LOCKDEP_STACK_DEPTH) {
        console_write("LOCKDEP: held-lock stack overflow on cpu ");
        console_write_dec(cpu);
        console_write(" acquiring ");
        console_write(name ? name : "(null)");
        console_write("\n");
        lockdep_dump_cpu(cpu);
        panic("lockdep: stack overflow");
    }

    /* Strict-> inversion check. Same-level acquisition of two distinct lock
     * instances (e.g. rename locking parent and grandparent inodes in pointer
     * order) is legitimate and must be allowed; the DAG only constrains
     * cross-level order. Recursive re-acquisition of the SAME instance is
     * caught by each lock's own machinery (BKL bumps depth_owner, spinlocks
     * deadlock-detect via their cmpxchg loop). */
    if (s->depth > 0 && s->levels[s->depth - 1] > level) {
        console_write("LOCKDEP: ORDER INVERSION on cpu ");
        console_write_dec(cpu);
        console_write("\n  trying to acquire ");
        console_write(name ? name : "(null)");
        console_write(" (level ");
        console_write_dec((u32)level);
        console_write(")\n  while holding ");
        console_write(s->names[s->depth - 1] ? s->names[s->depth - 1] : "(null)");
        console_write(" (level ");
        console_write_dec((u32)s->levels[s->depth - 1]);
        console_write(")\n");
        lockdep_dump_cpu(cpu);
        panic("lockdep: inversion");
    }

    s->levels[s->depth] = level;
    s->names[s->depth] = name;
    s->depth++;
}

void lockdep_release(int level) {
    int cpu = lockdep_self_cpu();
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    struct lockdep_cpu_state *s = &g_lockdep[cpu];

    if (s->depth == 0) {
        console_write("LOCKDEP: release with empty stack on cpu ");
        console_write_dec(cpu);
        console_write(" level ");
        console_write_dec((u32)level);
        console_write("\n");
        panic("lockdep: empty-stack release");
    }

    int top = s->levels[s->depth - 1];
    if (top != level) {
        console_write("LOCKDEP: OUT-OF-ORDER RELEASE on cpu ");
        console_write_dec(cpu);
        console_write("\n  releasing level ");
        console_write_dec((u32)level);
        console_write("\n  but top is ");
        console_write(s->names[s->depth - 1] ? s->names[s->depth - 1] : "(null)");
        console_write(" (level ");
        console_write_dec((u32)top);
        console_write(")\n");
        lockdep_dump_cpu(cpu);
        panic("lockdep: out-of-order release");
    }

    s->depth--;
    s->names[s->depth] = 0;
}

void lockdep_acquire_global(int level, const char *name) {
    int cpu = lockdep_self_cpu();
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    /* Order check: a global-level acquire while the same CPU already
     * holds a higher per-CPU-stack level is still an inversion. The
     * release side we cannot check (a different CPU may release us, since
     * the inode lock's holder can resume elsewhere after blocking).
     *
     * No global counter is bumped: the bequeath client (the per-inode
     * sleeping rwlock) acquires on one CPU and may release on another, so an
     * atomic on a shared cache line here would just add contention for no
     * detection benefit. The acquire-side order check is the useful part. */
    struct lockdep_cpu_state *s = &g_lockdep[cpu];
    if (s->depth > 0 && s->levels[s->depth - 1] > level) {
        console_write("LOCKDEP: ORDER INVERSION on cpu ");
        console_write_dec(cpu);
        console_write(" (global acquire)\n  trying to acquire ");
        console_write(name ? name : "(null)");
        console_write(" (level ");
        console_write_dec((u32)level);
        console_write(")\n  while holding ");
        console_write(s->names[s->depth - 1] ?
                      s->names[s->depth - 1] : "(null)");
        console_write(" (level ");
        console_write_dec((u32)s->levels[s->depth - 1]);
        console_write(")\n");
        lockdep_dump_cpu(cpu);
        panic("lockdep: inversion (global)");
    }
}

void lockdep_release_global(int level, const char *name) {
    /* Intentionally no-op: any work here would either contend on a
     * shared cache line (defeating the bequeath relaxation) or require
     * per-task held-lock tracking. The matching acquire-side order check
     * is what we care about for race detection. */
    (void)level;
    (void)name;
}

void lockdep_dump_cpu(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    struct lockdep_cpu_state *s = &g_lockdep[cpu];
    console_write("  cpu ");
    console_write_dec(cpu);
    console_write(" held locks: ");
    if (s->depth == 0) {
        console_write("(none)\n");
    } else {
        for (int i = 0; i < s->depth; i++) {
            console_write(s->names[i] ? s->names[i] : "?");
            if (i + 1 < s->depth) console_write(" -> ");
        }
        console_write("\n");
    }
}

void lockdep_note_spin_acquire(const void *lock, u64 site) {
    int cpu = lockdep_self_cpu();

    if (cpu < 0 || cpu >= (int)MAX_CPUS)
        return;
    struct lockdep_cpu_state *st = &g_lockdep[cpu];
    for (int i = 0; i < LOCKDEP_STACK_DEPTH; i++) {
        if (!st->spin_held[i]) {
            st->spin_held[i] = lock;
            st->spin_site[i] = site;
            return;
        }
    }
    /* More nested spinlocks than there are slots: the oldest is the one most
     * likely still to matter, so drop this one rather than evict it. */
}

void lockdep_note_spin_release(const void *lock) {
    int cpu = lockdep_self_cpu();

    if (cpu < 0 || cpu >= (int)MAX_CPUS)
        return;
    struct lockdep_cpu_state *st = &g_lockdep[cpu];
    for (int i = 0; i < LOCKDEP_STACK_DEPTH; i++) {
        if (st->spin_held[i] == lock) {
            st->spin_held[i] = 0;
            st->spin_site[i] = 0;
            return;
        }
    }
}

int lockdep_spin_holder(const void *lock, int *cpu_out, u64 *site_out) {
    for (int c = 0; c < (int)MAX_CPUS; c++) {
        for (int i = 0; i < LOCKDEP_STACK_DEPTH; i++) {
            if (g_lockdep[c].spin_held[i] == lock) {
                if (cpu_out)
                    *cpu_out = c;
                if (site_out)
                    *site_out = g_lockdep[c].spin_site[i];
                return 1;
            }
        }
    }
    return 0;
}

void lockdep_dump_all(void) {
    for (int i = 0; i < g_max_cpus && i < MAX_CPUS; i++) {
        lockdep_dump_cpu(i);
    }
}

#endif  /* KERNEL_LOCKDEP */

/* ── Spin-lock lockup detector ──
 *
 * A CPU that spins forever on a spinlock (holder descheduled while holding it,
 * missing unlock on an error path, or an IRQ-unsafe lock re-entered from an
 * ISR) is invisible: the machine simply goes silent with no panic and no last
 * marker. spin_lock() calls this once its spin count crosses the threshold so
 * the failure names the lock, the CPU, the current task, and the caller.
 */
static int spin_owner_report_lkpi(volatile int *lock) {
	const struct lkpi_spinlock *l = (const struct lkpi_spinlock *)lock;

	if (l->owner_cpu < 0 || l->owner_cpu >= (int)MAX_CPUS || !l->acquired_at)
		return 0;
	console_write("\n  holder: cpu ");
	console_write_dec((u64)l->owner_cpu);
	console_write(" task ");
	console_write_dec(l->owner_task);
	console_write(" took it at 0x");
	console_write_hex64(l->acquired_at);
	ksym_print(l->acquired_at);
	return 1;
}

static void spin_owner_report(volatile int *lock) {
	int cpu = -1;
	u64 site = 0;

	if (spin_owner_report_lkpi(lock))
		return;
	if (lockdep_spin_holder((const void *)lock, &cpu, &site)) {
		console_write("\n  holder: cpu ");
		console_write_dec((u64)cpu);
		console_write(" took it at 0x");
		console_write_hex64(site);
		ksym_print(site);
		return;
	}
	console_write("\n  holder: not recorded (not a driver lock;"
	              " rebuild with LOCKDEP=1 to record plain spinlocks)");
}


void spin_lock_stuck(volatile int *lock, u64 caller) {
    console_bust_lock();
    console_write("\nSPINLOCK LOCKUP on cpu ");
    console_write_dec((u64)percpu_read(cpu_id));
    console_write(": lock=0x");
    console_write_hex64((u64)(usize)lock);
    console_write(" value=");
    console_write_dec((u64)(unsigned)*lock);
    /* And WHAT the lock is: an address alone names nothing, and every lockup
     * report so far has ended in guessing which structure it belonged to. The
     * heap knows the allocation it falls inside. */
    {
        extern void kheap_describe(u64 addr, const char *prefix);

        kheap_describe((u64)(usize)lock, " lock->");
    }
    console_write("\n  caller: 0x");
    console_write_hex64(caller);
    ksym_print(caller);
    {
        /* If this is a runqueue lock, name the CPU that has it. */
        extern void rq_describe_lock(const void *lock);
        rq_describe_lock((const void *)lock);
    }
    /*
     * The immediate caller is whichever inline wrapper took the lock, which
     * names the locking helper and not the code that wanted the lock. Walk the
     * frames so the site is in the report — a self-deadlock is only actionable
     * once you can see who already held it.
     */
    spin_owner_report(lock);
    console_write("\n  backtrace:");
    arch_backtrace((u64)(usize)__builtin_frame_address(0), caller);
    if (current_task) {
        console_write("\n  task: pid=");
        console_write_dec((u64)current_task->id);
        console_write(" name=");
        console_write(current_task->name ? current_task->name : "(none)");
    }
    console_write("\n");
    scheduler_dump_tasks();
    panic("spinlock lockup");
}
