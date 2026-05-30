/* Lockdep-light implementation. See kernel/include/b1nix/lockdep.h.
 *
 * Translation-unit-level no-op when KERNEL_LOCKDEP is not defined — the
 * compiler still gets a .o so the Makefile entry stays stable across
 * configurations, but every public function body collapses to nothing.
 */

#include <b1nix/console.h>
#include <b1nix/lapic.h>
#include <b1nix/lockdep.h>
#include <b1nix/panic.h>

#ifdef KERNEL_LOCKDEP

#define LOCKDEP_STACK_DEPTH  16

struct lockdep_cpu_state {
    /* Stack of currently-held lock levels, top at index `depth - 1`. */
    int levels[LOCKDEP_STACK_DEPTH];
    const char *names[LOCKDEP_STACK_DEPTH];
    int depth;
};

/* Per-CPU state lives in a static array sized for the compile-time MAX_CPUS
 * ceiling. We can't put this in struct percpu because lockdep callers may
 * fire before percpu_init (during early bring-up); the BSP slot is
 * cpu_id 0 by convention, and APs each get their slot in ap_main. */
static struct lockdep_cpu_state g_lockdep[MAX_CPUS];

/* "Bequeathing" lock levels (M28 #2 Variant A): BKL and the per-inode
 * sleeping rwlock can be released by a CPU other than the one that
 * acquired them. Their LOCKDEP_*_GLOBAL helpers bypass the per-CPU
 * acquisition stack and do only the inversion check on acquire — that
 * suffices to detect lock-discipline mistakes T4 might introduce
 * without bumping a globally-contended counter (cache-line ping-pong
 * perturbs syscall density enough to tickle the documented stack-
 * corruption race; see docs/m28-t4-blocker.md). */

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
     * release side we cannot check (different CPU may release us).
     *
     * No global counter is bumped: under the M24b bequeath model the
     * BKL is acquired on one CPU and released on another constantly,
     * so an atomic on a shared cache line here perturbs syscall density
     * enough to tickle the documented stack-corruption race in M14
     * (see docs/m28-t4-blocker.md). The order check is sufficient as a
     * detector for the lock-discipline mistakes T4 might introduce; the
     * stack-corruption races surface as GP/PF exceptions independently. */
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

void lockdep_dump_all(void) {
    for (int i = 0; i < g_max_cpus && i < MAX_CPUS; i++) {
        lockdep_dump_cpu(i);
    }
}

#endif  /* KERNEL_LOCKDEP */
