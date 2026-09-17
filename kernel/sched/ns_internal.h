/* State shared by the namespace core (namespace.c), PID namespaces
 * (pid_namespace.c) and user namespaces (user_namespace.c). Nothing outside
 * those three files includes this. */
#ifndef B1NIX_NS_INTERNAL_H
#define B1NIX_NS_INTERNAL_H

#include <b1nix/namespace.h>
#include <b1nix/spinlock.h>
#include <b1nix/user_namespace.h>

/* One lock for every table below. Every critical section is a table update or
 * a short walk; nothing sleeps or allocates under it. */
extern spinlock_t ns_lock;

/* Bookkeeping common to every kind. */
struct ns_slot {
  u8 used;   /* allocated */
  u8 zombie; /* no references left; the reaper still has to tear it down */
  u8 entered; /* a task has been a member (time namespaces lock their offsets) */
  u32 refs;
  u32 owner; /* owning user namespace */
  u64 inum;  /* nsfs inode number, never reused */
};

extern struct ns_slot *const ns_slots[NS_KIND_COUNT];
extern const u32 ns_max[NS_KIND_COUNT];

/* Non-zero once any namespace other than an initial one has been created. */
extern int ns_any;

/* Per-task membership, indexed by the task's slot. A row that is not `used`
 * means "every initial namespace". Each non-zero id holds one reference. */
struct ns_row {
  u8 used;
  u16 id[NS_KIND_COUNT]; /* NS_USER is carried in the task's cred instead */
  u16 pid_children;      /* pid_ns_for_children */
  u16 time_children;     /* time_ns_for_children */
  /* Namespaces clone(2) prepared for the next child, each holding a
   * reference until namespace_fork_inherit hands it over. */
  u16 child[NS_KIND_COUNT];
  u8 child_valid;
};

/* Allocate a slot of `kind`, owned by user namespace `owner` (a reference on
 * the owner is taken). Returns the id with refs = 1, or 0 when the table is
 * full. Caller holds ns_lock. */
u32 ns_alloc_locked(int kind, u32 owner);
void ns_get_locked(int kind, u32 id);
/* Drop a reference; frees the slot (or queues it for the reaper). Caller holds
 * ns_lock. */
void ns_put_locked(int kind, u32 id);

/* Called by ns_put_locked when a PID or user namespace dies. */
void pidns_release_locked(u32 id);
void userns_release_locked(u32 id);
/* Set up the kind-specific state of a freshly allocated namespace. */
void pidns_init_locked(u32 id, u32 parent);
int userns_init_locked(u32 id, u32 parent, const struct cred *creator);

/* PID-namespace membership of a task (pid_namespace.c). */
void pidns_enter_locked(u32 ns, struct task *t, int is_thread);
void pidns_task_reaped_locked(usize gid);
void pidns_task_exit_locked(struct task *t, u32 ns);
u32 pidns_level_locked(u32 ns);
u32 pidns_parent_locked(u32 ns);
int pidns_is_ancestor_locked(u32 ancestor, u32 of);
int pidns_dying_locked(u32 ns);
/* Kill the members of namespaces whose init has exited. Not under ns_lock. */
void pidns_run_zaps(void);

/* Put a credential into user namespace `ns` with the full capability set in
 * it (unshare(CLONE_NEWUSER), setns into a user namespace, a clone child).
 * Takes over a reference the caller holds on `ns`. Caller holds ns_lock. */
void cred_enter_userns_locked(struct cred *c, u32 ns);

/* User-namespace facts the core needs (user_namespace.c). */
u32 userns_level_locked(u32 ns);
u32 userns_parent_locked(u32 ns);

/* The row of a task, or 0 for a task outside the table (a per-CPU idle task). */
struct ns_row *ns_row_of(const struct task *t);

#endif
