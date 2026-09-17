/* What every System V IPC object shares: the namespace it lives in and the
 * permission rules Linux's ipc/util.c applies to it (M123). */

#include <b1nix/errno.h>
#include <b1nix/mqueue.h>
#include <b1nix/namespace.h>
#include <b1nix/sched.h>
#include <b1nix/sysv_ipc.h>
#include <b1nix/uidgid.h>
#include <b1nix/user_namespace.h>

u32 ipc_current_ns(void) { return namespace_current_id(NS_IPC); }

static int cred_in_group(const struct cred *c, u32 kgid) {
  if (c->egid == kgid)
    return 1;
  for (int i = 0; i < c->ngroups && i < MAX_GROUPS; i++)
    if (c->groups[i] == kgid)
      return 1;
  return 0;
}

int ipc_check_perm(const struct ipc_perm *perm, u32 ns, u16 flag) {
  const struct cred *c = scheduler_get_current_cred();
  if (!c)
    return 0; /* kernel context */
  u16 requested = (u16)((flag >> 6) | (flag >> 3) | flag);
  u16 granted = perm->mode;
  if (c->euid == perm->cuid || c->euid == perm->uid)
    granted >>= 6;
  else if (cred_in_group(c, perm->cgid) || cred_in_group(c, perm->gid))
    granted >>= 3;
  if ((requested & ~granted & 0007) &&
      !ns_capable_cred(c, namespace_owner(NS_IPC, ns), CAP_IPC_OWNER))
    return -EACCES;
  return 0;
}

int ipc_may_control(const struct ipc_perm *perm, u32 ns) {
  const struct cred *c = scheduler_get_current_cred();
  if (!c)
    return 1;
  return c->euid == perm->cuid || c->euid == perm->uid ||
         ns_capable_cred(c, namespace_owner(NS_IPC, ns), CAP_SYS_ADMIN);
}

/* Called by the namespace reaper when the last reference to an IPC namespace
 * is gone: nothing can name its objects any more. */
void ipc_ns_destroy(u32 ns) {
  shm_ns_destroy(ns);
  sysv_sem_ns_destroy(ns);
  sysv_msg_ns_destroy(ns);
  mqueue_ns_destroy(ns);
}
