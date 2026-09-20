/* SPDX-License-Identifier: GPL-2.0-only */
/* User namespaces (M123): id maps and namespace-relative capabilities.
 *
 * See <b1nix/user_namespace.h> for the model. The maps are written once, from
 * /proc/<pid>/uid_map and gid_map, and never change afterwards, so a
 * translation reads them without taking a lock: the extents are filled in
 * before the count that makes them visible is published.
 *
 * Every extent stores its lower range as KERNEL ids. Writing a map converts the
 * lower ids from the parent namespace's view, so translating through any
 * nesting depth is one lookup — as in Linux.
 */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/uidgid.h>
#include <stdio.h>
#include <string.h>

#include "ns_internal.h"

struct id_extent {
  u32 first; /* first id inside the namespace */
  u32 lower; /* first kernel id it maps to */
  u32 count;
};

struct id_map {
  u32 nr; /* published last; 0 = not written */
  struct id_extent ext[USERNS_MAP_EXTENTS];
};

struct userns_data {
  u32 parent;
  u32 level;
  u32 owner_kuid;
  u32 owner_kgid;
  struct id_map map[3]; /* USERNS_UID_MAP, USERNS_GID_MAP, USERNS_PROJID_MAP */
  u8 setgroups_allowed;
  /* Whether the creator held CAP_SETFCAP in the parent: without it the new
   * namespace may not map the parent's root, or a file capability written
   * inside could be honoured outside. */
  u8 parent_could_setfcap;
};

static struct userns_data userns[NS_MAX_USER];
static int userns_ready;

static void userns_ensure_init(void) {
  if (userns_ready)
    return;
  userns_ready = 1;
  for (int m = 0; m < 3; m++) {
    userns[0].map[m].ext[0].first = 0;
    userns[0].map[m].ext[0].lower = 0;
    userns[0].map[m].ext[0].count = 0xFFFFFFFFu;
    userns[0].map[m].nr = 1;
  }
  userns[0].setgroups_allowed = 1;
  userns[0].parent_could_setfcap = 1;
}

u32 cred_userns(const struct cred *c) { return c ? c->user_ns : 0; }

u32 userns_level_locked(u32 ns) {
  return ns < NS_MAX_USER ? userns[ns].level : 0;
}

u32 userns_parent_locked(u32 ns) {
  return ns < NS_MAX_USER ? userns[ns].parent : 0;
}

u32 userns_owner_kuid(u32 ns) {
  return (ns && ns < NS_MAX_USER) ? userns[ns].owner_kuid : 0;
}

/* ── translation ────────────────────────────────────────────────────────── */

static u32 map_down(const struct id_map *m, u32 id, u32 count) {
  u32 nr = __atomic_load_n(&m->nr, __ATOMIC_RELAXED);
  if (id == 0xFFFFFFFFu)
    return 0xFFFFFFFFu;
  for (u32 i = 0; i < nr; i++) {
    const struct id_extent *e = &m->ext[i];
    u32 last = id + count - 1;
    if (last < id)
      return 0xFFFFFFFFu;
    if (id >= e->first && last - e->first < e->count)
      return e->lower + (id - e->first);
  }
  return 0xFFFFFFFFu;
}

static u32 map_up(const struct id_map *m, u32 kid) {
  u32 nr = __atomic_load_n(&m->nr, __ATOMIC_RELAXED);
  if (kid == 0xFFFFFFFFu)
    return 0xFFFFFFFFu;
  for (u32 i = 0; i < nr; i++) {
    const struct id_extent *e = &m->ext[i];
    if (kid >= e->lower && kid - e->lower < e->count)
      return e->first + (kid - e->lower);
  }
  return 0xFFFFFFFFu;
}

u32 make_kuid(u32 ns, u32 uid) {
  if (ns == 0 || ns >= NS_MAX_USER)
    return uid;
  return map_down(&userns[ns].map[USERNS_UID_MAP], uid, 1);
}

u32 make_kgid(u32 ns, u32 gid) {
  if (ns == 0 || ns >= NS_MAX_USER)
    return gid;
  return map_down(&userns[ns].map[USERNS_GID_MAP], gid, 1);
}

u32 from_kuid(u32 ns, u32 kuid) {
  if (ns == 0 || ns >= NS_MAX_USER)
    return kuid;
  return map_up(&userns[ns].map[USERNS_UID_MAP], kuid);
}

u32 from_kgid(u32 ns, u32 kgid) {
  if (ns == 0 || ns >= NS_MAX_USER)
    return kgid;
  return map_up(&userns[ns].map[USERNS_GID_MAP], kgid);
}

u32 from_kuid_munged(u32 ns, u32 kuid) {
  u32 v = from_kuid(ns, kuid);
  return v == UID_INVALID ? UID_OVERFLOW : v;
}

u32 from_kgid_munged(u32 ns, u32 kgid) {
  u32 v = from_kgid(ns, kgid);
  return v == GID_INVALID ? GID_OVERFLOW : v;
}

int kuid_has_mapping(u32 ns, u32 kuid) {
  return from_kuid(ns, kuid) != UID_INVALID;
}

int kgid_has_mapping(u32 ns, u32 kgid) {
  return from_kgid(ns, kgid) != GID_INVALID;
}

static u32 current_userns(void) {
  return cred_userns(scheduler_get_current_cred());
}

u32 current_make_kuid(u32 uid) { return make_kuid(current_userns(), uid); }
u32 current_make_kgid(u32 gid) { return make_kgid(current_userns(), gid); }
u32 current_from_kuid(u32 kuid) {
  return from_kuid_munged(current_userns(), kuid);
}
u32 current_from_kgid(u32 kgid) {
  return from_kgid_munged(current_userns(), kgid);
}

/* ── capabilities ───────────────────────────────────────────────────────── */

int userns_is_ancestor(u32 ancestor, u32 ns) {
  for (int depth = 0; depth <= NS_MAX_LEVEL + 1; depth++) {
    if (ns == ancestor)
      return 1;
    if (ns == 0 || ns >= NS_MAX_USER)
      return 0;
    ns = userns[ns].parent;
  }
  return 0;
}

/* Linux cap_capable(). A credential's capability sets are meaningful in its
 * own namespace. Walking up from the target: reaching the credential's
 * namespace answers from its sets; passing a namespace whose parent is the
 * credential's and whose owner is the credential's euid answers "everything";
 * climbing to (or above) the credential's own level without meeting it answers
 * "nothing". */
int ns_capable_cred(const struct cred *c, u32 ns, int cap) {
  if (!c || cap < 0 || cap > CAP_LAST || ns >= NS_MAX_USER)
    return 0;
  u32 mine = c->user_ns;
  for (int depth = 0; depth <= NS_MAX_LEVEL + 1; depth++) {
    if (ns == mine)
      return (c->cap_effective & (1ULL << cap)) != 0;
    if (userns[ns].level <= userns[mine].level)
      return 0;
    u32 parent = userns[ns].parent;
    if (parent == mine && c->euid == userns[ns].owner_kuid)
      return 1;
    ns = parent;
  }
  return 0;
}

int ns_capable(u32 ns, int cap) {
  return ns_capable_cred(scheduler_get_current_cred(), ns, cap);
}

int net_ns_capable(int cap) {
  return ns_capable(namespace_owner(NS_NET, namespace_net_current()), cap);
}

int capable_wrt_inode_uidgid(const struct cred *c, u32 kuid, u32 kgid,
                             int cap) {
  if (!c)
    return 0;
  u32 ns = c->user_ns;
  return ns_capable_cred(c, ns, cap) && kuid_has_mapping(ns, kuid) &&
         kgid_has_mapping(ns, kgid);
}

int cred_inode_owner_or_capable(const struct cred *c, u32 kuid, u32 kgid) {
  (void)kgid;
  if (!c)
    return 0;
  if (c->fsuid == kuid)
    return 1;
  u32 ns = c->user_ns;
  return kuid_has_mapping(ns, kuid) && ns_capable_cred(c, ns, CAP_FOWNER);
}

int userns_may_setgroups(u32 ns) {
  if (ns == 0 || ns >= NS_MAX_USER)
    return 1;
  return __atomic_load_n(&userns[ns].map[USERNS_GID_MAP].nr,
                         __ATOMIC_RELAXED) != 0 &&
         userns[ns].setgroups_allowed;
}

/* ── lifetime ───────────────────────────────────────────────────────────── */

void cred_enter_userns_locked(struct cred *c, u32 ns) {
  if (!c) {
    ns_put_locked(NS_USER, ns);
    return;
  }
  u32 old = c->user_ns;
  c->user_ns = ns;
  ns_put_locked(NS_USER, old);
  /* Linux set_cred_user_ns(): the creator of (or a task entering) a user
   * namespace holds every capability in it, and nothing it could carry across
   * an exec is inherited from outside. */
  c->securebits = 0;
  c->cap_inheritable = 0;
  c->cap_ambient = 0;
  c->cap_permitted = CAP_FULL_SET;
  c->cap_effective = CAP_FULL_SET;
  c->cap_bounding = CAP_FULL_SET;
}

int userns_init_locked(u32 id, u32 parent, const struct cred *creator) {
  userns_ensure_init();
  struct userns_data *u = &userns[id];
  memset(u, 0, sizeof(*u));
  u->parent = parent;
  u->level = userns[parent].level + 1;
  u->owner_kuid = creator ? creator->euid : 0;
  u->owner_kgid = creator ? creator->egid : 0;
  u->setgroups_allowed = userns[parent].setgroups_allowed;
  u->parent_could_setfcap =
      creator ? (u8)ns_capable_cred(creator, parent, CAP_SETFCAP) : 1;
  return 0;
}

void userns_release_locked(u32 id) {
  if (id && id < NS_MAX_USER)
    memset(&userns[id], 0, sizeof(userns[id]));
}

int userns_create(const struct cred *creator) {
  if (!creator)
    return -EPERM;
  userns_ensure_init();
  u32 parent = creator->user_ns;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  int rc;
  if (userns[parent].level + 1 > NS_MAX_LEVEL) {
    rc = -EUSERS;
  } else if (!kuid_has_mapping(parent, creator->euid) ||
             !kgid_has_mapping(parent, creator->egid)) {
    /* The owner must be nameable in the parent, or nobody there could ever
     * be told who owns the new namespace. */
    rc = -EPERM;
  } else {
    u32 id = ns_alloc_locked(NS_USER, parent);
    if (!id) {
      rc = -ENOSPC;
    } else {
      userns_init_locked(id, parent, creator);
      rc = (int)id;
    }
  }
  spin_unlock_irqrestore(&ns_lock, f);
  return rc;
}

/* ── /proc/<pid>/uid_map, gid_map, projid_map, setgroups ────────────────── */

static u32 target_userns(const struct task *t) {
  return (t && t->cred) ? t->cred->user_ns : 0;
}

int userns_map_render(const struct task *target, int which, char *buf,
                      usize len) {
  if (which < 0 || which > 2 || !buf || len == 0)
    return -EINVAL;
  userns_ensure_init();
  u32 ns = target_userns(target);
  /* The lower ids are shown in the reader's namespace — or, for a reader
   * looking at its own namespace, in the parent's, which is the only view in
   * which "what does my root map to" has an answer. */
  u32 lower_ns = current_userns();
  if (lower_ns == ns && ns != 0)
    lower_ns = userns[ns].parent;
  const struct id_map *m = &userns[ns].map[which];
  u32 nr = __atomic_load_n(&m->nr, __ATOMIC_RELAXED);
  usize pos = 0;
  for (u32 i = 0; i < nr && pos < len; i++) {
    const struct id_extent *e = &m->ext[i];
    u32 lower = which == USERNS_GID_MAP ? from_kgid(lower_ns, e->lower)
                                        : from_kuid(lower_ns, e->lower);
    if (which == USERNS_PROJID_MAP)
      lower = lower_ns == 0 ? e->lower
                            : map_up(&userns[lower_ns].map[USERNS_PROJID_MAP],
                                     e->lower);
    pos += (usize)snprintf(buf + pos, len - pos, "%10u %10u %10u\n", e->first,
                           lower, e->count);
  }
  return (int)(pos < len ? pos : len);
}

static int parse_u32(const char **pp, const char *end, u32 *out) {
  const char *p = *pp;
  while (p < end && (*p == ' ' || *p == '\t'))
    p++;
  if (p >= end || *p < '0' || *p > '9')
    return -EINVAL;
  u64 v = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    v = v * 10 + (u64)(*p - '0');
    if (v > 0xFFFFFFFFull)
      return -EINVAL;
    p++;
  }
  *out = (u32)v;
  *pp = p;
  return 0;
}

static int ranges_overlap(u32 a, u32 an, u32 b, u32 bn) {
  u32 alast = a + an - 1, blast = b + bn - 1;
  return a <= blast && b <= alast;
}

/* Linux verify_root_map(): mapping the parent's root (lower id 0) needs
 * CAP_SETFCAP over the parent — held at creation time if the writer is inside
 * the namespace itself, held now otherwise. */
static int verify_root_map(const struct cred *c, u32 ns,
                           const struct id_map *nm) {
  int maps_root = 0;
  for (u32 i = 0; i < nm->nr; i++)
    if (nm->ext[i].lower == 0)
      maps_root = 1;
  if (!maps_root)
    return 1;
  if (c->user_ns == ns)
    return userns[ns].parent_could_setfcap;
  return ns_capable_cred(c, userns[ns].parent, CAP_SETFCAP);
}

/* Linux new_idmap_permitted(), with the opener and the writer being the same
 * credential (this procfs has no per-open credential). */
static int new_idmap_permitted(const struct cred *c, u32 ns, int which,
                               const struct id_map *nm) {
  int cap_setid = which == USERNS_UID_MAP   ? CAP_SETUID
                  : which == USERNS_GID_MAP ? CAP_SETGID
                                            : -1;
  u32 parent = userns[ns].parent;
  if (cap_setid == CAP_SETUID && !verify_root_map(c, ns, nm))
    return 0;
  /* The unprivileged case: the owner maps exactly its own id. */
  if (nm->nr == 1 && nm->ext[0].count == 1 &&
      userns[ns].owner_kuid == c->euid) {
    u32 id = nm->ext[0].lower;
    if (cap_setid == CAP_SETUID && make_kuid(parent, id) == c->euid)
      return 1;
    /* A gid map written without privilege is only safe once setgroups(2) is
     * off for good: otherwise the process could drop a group it was denied
     * access through. */
    if (cap_setid == CAP_SETGID && !userns[ns].setgroups_allowed &&
        make_kgid(parent, id) == c->egid)
      return 1;
  }
  if (cap_setid < 0)
    return 1;
  return ns_capable_cred(c, parent, cap_setid);
}

isize userns_map_write(const struct task *target, int which, const char *buf,
                       usize len) {
  if (which < 0 || which > 2 || !buf)
    return -EINVAL;
  userns_ensure_init();
  const struct cred *c = scheduler_get_current_cred();
  if (!c)
    return -EPERM;
  u32 ns = target_userns(target);
  if (ns == 0)
    return -EPERM; /* the initial namespace's maps are fixed */
  if (c->user_ns != ns && c->user_ns != userns[ns].parent)
    return -EPERM;
  if (len >= 4096)
    return -EINVAL;

  struct id_map *nm = kzalloc(sizeof(*nm));
  if (!nm)
    return -ENOMEM;
  const char *p = buf, *end = buf + len;
  isize rc = 0;
  while (p < end) {
    const char *eol = p;
    while (eol < end && *eol != '\n')
      eol++;
    const char *q = p;
    while (q < eol && (*q == ' ' || *q == '\t'))
      q++;
    if (q == eol) { /* blank line */
      p = eol + 1;
      continue;
    }
    struct id_extent e;
    if (parse_u32(&q, eol, &e.first) || parse_u32(&q, eol, &e.lower) ||
        parse_u32(&q, eol, &e.count)) {
      rc = -EINVAL;
      goto out;
    }
    while (q < eol && (*q == ' ' || *q == '\t'))
      q++;
    if (q != eol || e.count == 0 || e.first + e.count - 1 < e.first ||
        e.lower + e.count - 1 < e.lower || nm->nr >= USERNS_MAP_EXTENTS) {
      rc = -EINVAL;
      goto out;
    }
    for (u32 i = 0; i < nm->nr; i++)
      if (ranges_overlap(nm->ext[i].first, nm->ext[i].count, e.first,
                         e.count) ||
          ranges_overlap(nm->ext[i].lower, nm->ext[i].count, e.lower,
                         e.count)) {
        rc = -EINVAL;
        goto out;
      }
    nm->ext[nm->nr++] = e;
    p = eol + 1;
  }
  if (nm->nr == 0) {
    rc = -EINVAL;
    goto out;
  }

  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  struct id_map *m = &userns[ns].map[which];
  int cap_setid = which == USERNS_UID_MAP   ? CAP_SETUID
                  : which == USERNS_GID_MAP ? CAP_SETGID
                                            : -1;
  if (m->nr != 0) {
    rc = -EPERM; /* one successful write per map, ever */
  } else if (cap_setid >= 0 && !ns_capable_cred(c, ns, CAP_SYS_ADMIN)) {
    rc = -EPERM;
  } else if (!new_idmap_permitted(c, ns, which, nm)) {
    rc = -EPERM;
  } else {
    /* The lower ids were written in the parent's terms; store kernel ids. Each
     * lower range has to lie inside one extent of the parent's map. */
    u32 parent = userns[ns].parent;
    for (u32 i = 0; i < nm->nr && rc == 0; i++) {
      u32 k = parent == 0 ? nm->ext[i].lower
                          : map_down(&userns[parent].map[which],
                                     nm->ext[i].lower, nm->ext[i].count);
      if (k == 0xFFFFFFFFu)
        rc = -EPERM;
      else
        nm->ext[i].lower = k;
    }
    if (rc == 0) {
      for (u32 i = 0; i < nm->nr; i++)
        m->ext[i] = nm->ext[i];
      __atomic_store_n(&m->nr, nm->nr, __ATOMIC_RELAXED);
      rc = (isize)len;
    }
  }
  spin_unlock_irqrestore(&ns_lock, f);
out:
  kfree(nm);
  return rc;
}

int userns_setgroups_render(const struct task *target, char *buf, usize len) {
  userns_ensure_init();
  u32 ns = target_userns(target);
  return snprintf(buf, len, "%s\n",
                  userns[ns].setgroups_allowed ? "allow" : "deny");
}

isize userns_setgroups_write(const struct task *target, const char *buf,
                             usize len) {
  userns_ensure_init();
  const struct cred *c = scheduler_get_current_cred();
  u32 ns = target_userns(target);
  if (!c || !ns_capable_cred(c, ns, CAP_SYS_ADMIN))
    return -EPERM;
  usize n = len;
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' ||
                   buf[n - 1] == '\t'))
    n--;
  int allow;
  if (n == 5 && memcmp(buf, "allow", 5) == 0)
    allow = 1;
  else if (n == 4 && memcmp(buf, "deny", 4) == 0)
    allow = 0;
  else
    return -EINVAL;
  u64 f;
  spin_lock_irqsave(&ns_lock, &f);
  isize rc = (isize)len;
  if (allow) {
    /* Re-enabling setgroups after it was denied is never allowed. */
    if (!userns[ns].setgroups_allowed)
      rc = -EPERM;
  } else {
    /* Denying it after the gid map already enabled it is not allowed either. */
    if (userns[ns].map[USERNS_GID_MAP].nr != 0)
      rc = -EPERM;
    else
      userns[ns].setgroups_allowed = 0;
  }
  spin_unlock_irqrestore(&ns_lock, f);
  return rc;
}
