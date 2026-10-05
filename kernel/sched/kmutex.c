/* SPDX-License-Identifier: GPL-2.0-only */
/* kernel/sched/kmutex.c — a sleeping mutex that knows its owner (see
 * <b1nix/kmutex.h>). */

#include <b1nix/kmutex.h>
#include <b1nix/klog.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>

void scheduler_kcrit_enter(void);
void scheduler_kcrit_leave(void);

static int kmutex_try(kmutex_t *m, int self) {
    int expected = 0;

    return __atomic_compare_exchange_n(&m->owner, &expected, self, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void kmutex_taken(kmutex_t *m, u64 site) {
    scheduler_kcrit_enter();
    spin_note_at(m, SPIN_NOTE_SLEEP_ACQUIRE, site);
}

void kmutex_lock(kmutex_t *m) {
    u64 site = (u64)(usize)__builtin_return_address(0);
    int self = spin_owner_self();
    struct task *t = current_task;

    KASSERT(self == SPIN_OWNER_NOTASK || m->owner != self,
            "kmutex %p taken recursively from %p", (void *)m, (void *)site);
    if (t) {
        KASSERT(t->spin_held == 0,
                "kmutex %p taken with %d native spinlock(s) held, from %p",
                (void *)m, t->spin_held, (void *)site);
        KASSERT(t->irq_nest == 0, "kmutex %p taken in an interrupt handler",
                (void *)m);
    }
    while (!kmutex_try(m, self)) {
        if (!scheduler_can_block()) {
            /* Early boot or interrupts off: nothing can be slept on. */
            scheduler_yield();
            continue;
        }
        __atomic_add_fetch(&m->waiters, 1, __ATOMIC_SEQ_CST);
        scheduler_wait_prepare(m);
        if (kmutex_try(m, self)) {
            scheduler_wait_cancel();
            __atomic_sub_fetch(&m->waiters, 1, __ATOMIC_SEQ_CST);
            break;
        }
        scheduler_wait_commit();
        __atomic_sub_fetch(&m->waiters, 1, __ATOMIC_SEQ_CST);
    }
    kmutex_taken(m, site);
}

int kmutex_trylock(kmutex_t *m) {
    int self = spin_owner_self();

    KASSERT(self == SPIN_OWNER_NOTASK || m->owner != self,
            "kmutex %p tried recursively", (void *)m);
    if (!kmutex_try(m, self))
        return 0;
    kmutex_taken(m, (u64)(usize)__builtin_return_address(0));
    return 1;
}

void kmutex_unlock(kmutex_t *m) {
    int self = spin_owner_self();
    int owner = m->owner;

    KASSERT(owner != 0, "kmutex %p released while not held", (void *)m);
    KASSERT(owner == self || self == SPIN_OWNER_NOTASK ||
                owner == SPIN_OWNER_NOTASK,
            "kmutex %p released by a task that does not hold it", (void *)m);
    spin_note(m, SPIN_NOTE_SLEEP_RELEASE);
    __atomic_store_n(&m->owner, 0, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&m->waiters, __ATOMIC_SEQ_CST))
        scheduler_wake_all(m);
    scheduler_kcrit_leave();
}

void kmutex_assert_held(kmutex_t *m) {
    int self = spin_owner_self();

    KASSERT(self == SPIN_OWNER_NOTASK || m->owner == self,
            "kmutex %p is not held by the caller (%p)", (void *)m,
            __builtin_return_address(0));
}
