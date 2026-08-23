/* Network protocol registry (M96).
 *
 * A tiny publish/dispatch layer that lets the IPv6 datapath, Neighbour
 * Discovery and the SNTP client live in loadable modules while the core stack
 * keeps a single, stable set of call sites.
 */

#include <b1nix/errno.h>
#include <b1nix/netproto.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <string.h>

static struct net_proto *proto_list;
static spinlock_t proto_lock = SPINLOCK_INIT;

/* Dispatch guard. A protocol module is unloaded while the net daemon may be
 * anywhere inside one of its hooks; the hooks send packets, so they cannot run
 * under a spinlock. Instead every dispatcher brackets its iteration with this
 * counter, and proto_unregister — after unlinking, so no new dispatch can find
 * the entry — waits for the count to drain before returning. The module's exit
 * path only frees its text once that has happened. */
static volatile int proto_dispatch_depth;

static void proto_dispatch_enter(void) {
  __atomic_fetch_add(&proto_dispatch_depth, 1, __ATOMIC_ACQUIRE);
}

static void proto_dispatch_leave(void) {
  __atomic_fetch_sub(&proto_dispatch_depth, 1, __ATOMIC_RELEASE);
}

int proto_register(struct net_proto *proto) {
  if (!proto || !proto->name)
    return -EINVAL;
  u64 flags;
  spin_lock_irqsave(&proto_lock, &flags);
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p == proto || strcmp(p->name, proto->name) == 0) {
      spin_unlock_irqrestore(&proto_lock, flags);
      return -EEXIST;
    }
  }
  proto->next = proto_list;
  proto_list = proto;
  spin_unlock_irqrestore(&proto_lock, flags);
  return 0;
}

void proto_unregister(struct net_proto *proto) {
  if (!proto)
    return;
  u64 flags;
  spin_lock_irqsave(&proto_lock, &flags);
  struct net_proto **pp = &proto_list;
  while (*pp) {
    if (*pp == proto) {
      *pp = proto->next;
      proto->next = 0;
      break;
    }
    pp = &(*pp)->next;
  }
  spin_unlock_irqrestore(&proto_lock, flags);

  /* Wait out any dispatcher that entered before the unlink. */
  while (__atomic_load_n(&proto_dispatch_depth, __ATOMIC_ACQUIRE) > 0)
    scheduler_yield();
}

struct net_proto *proto_find(const char *name) {
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (strcmp(p->name, name) == 0)
      return p;
  }
  return 0;
}

usize proto_count(void) {
  usize n = 0;
  for (struct net_proto *p = proto_list; p; p = p->next)
    n++;
  return n;
}

const char *proto_name_at(usize index) {
  usize n = 0;
  for (struct net_proto *p = proto_list; p; p = p->next, n++) {
    if (n == index)
      return p->name;
  }
  return 0;
}

/* A snapshot of the list, taken under the lock that writers hold.
 *
 * The dispatch depth below counts readers so an unregistering module knows
 * when its text is safe to free. It does NOT exclude a writer: proto_register
 * mutates proto_list under proto_lock, and a walk holding only the counter can
 * be in the middle of the list while that happens. The IOMMU instance panicked
 * on it in one boot out of two -- a #GP calling p->reset() through a pointer
 * that was not a function -- and because it is a coin flip it also lands on
 * whatever change happens to be under test, which cost a wrong attribution
 * tonight.
 *
 * Copying the pointers under the lock and calling the callbacks afterwards
 * fixes the walk without holding a spinlock across a callback that may sleep.
 * The depth counter is still taken around the calls, because it is what keeps
 * an entry's text alive while its callback runs.
 */
#define PROTO_SNAPSHOT_MAX 16

static unsigned proto_snapshot(struct net_proto **out, unsigned max) {
  unsigned n = 0;
  u64 flags;

  spin_lock_irqsave(&proto_lock, &flags);
  for (struct net_proto *p = proto_list; p && n < max; p = p->next)
    out[n++] = p;
  spin_unlock_irqrestore(&proto_lock, flags);
  return n;
}

int proto_deliver_ether(u16 ether_type, const void *data, usize size) {
  struct net_proto *snap[PROTO_SNAPSHOT_MAX];
  int delivered = 0;
  unsigned n;

  proto_dispatch_enter();
  n = proto_snapshot(snap, PROTO_SNAPSHOT_MAX);
  for (unsigned i = 0; i < n; i++) {
    if (snap[i]->ether_type == ether_type && snap[i]->receive) {
      snap[i]->receive(data, size);
      delivered = 1;
    }
  }
  proto_dispatch_leave();
  return delivered;
}

void net_proto_tick(u64 now_ticks) {
  struct net_proto *snap[PROTO_SNAPSHOT_MAX];
  unsigned n;

  proto_dispatch_enter();
  n = proto_snapshot(snap, PROTO_SNAPSHOT_MAX);
  for (unsigned i = 0; i < n; i++)
    if (snap[i]->tick)
      snap[i]->tick(now_ticks);
  proto_dispatch_leave();
}

void net_proto_reset(void) {
  struct net_proto *snap[PROTO_SNAPSHOT_MAX];
  unsigned n;

  proto_dispatch_enter();
  n = proto_snapshot(snap, PROTO_SNAPSHOT_MAX);
  for (unsigned i = 0; i < n; i++)
    if (snap[i]->reset)
      snap[i]->reset();
  proto_dispatch_leave();
}

void net_proto_selftest(void) {
  struct net_proto *snap[PROTO_SNAPSHOT_MAX];
  unsigned n;

  proto_dispatch_enter();
  n = proto_snapshot(snap, PROTO_SNAPSHOT_MAX);
  for (unsigned i = 0; i < n; i++)
    if (snap[i]->selftest)
      snap[i]->selftest();
  proto_dispatch_leave();
}

int net_proto_ipv6_available(void) {
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->send6)
      return 1;
  }
  return 0;
}

void net_proto_ipv6_send(struct in6_addr_k dst, u8 next_header,
                         const void *payload, usize size) {
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->send6) {
      p->send6(dst, next_header, payload, size);
      break;
    }
  }
  proto_dispatch_leave();
}

void net_proto_icmp6_unreach(struct in6_addr_k dst, u8 code, const void *quoted,
                             usize quoted_len) {
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->icmp6_unreach) {
      p->icmp6_unreach(dst, code, quoted, quoted_len);
      break;
    }
  }
  proto_dispatch_leave();
}

void ndp_dispatch_receive(struct in6_addr_k src, struct in6_addr_k dst, u8 type,
                          const void *data, usize size) {
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->icmp6) {
      p->icmp6(src, dst, type, data, size);
      break;
    }
  }
  proto_dispatch_leave();
}

void ndp_dispatch_mld_join(struct in6_addr_k addr) {
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->mld_join) {
      p->mld_join(addr);
      break;
    }
  }
  proto_dispatch_leave();
}

int ndp_dispatch_resolve(struct in6_addr_k ip, struct mac_addr *mac,
                         struct netdev *dev) {
  int rc = 0;
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->resolve6) {
      rc = p->resolve6(ip, mac, dev);
      break;
    }
  }
  proto_dispatch_leave();
  return rc;
}

int net_proto_neigh6_available(void) {
  int ok = 0;
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->neigh_dump || p->neigh_set || p->neigh_del) {
      ok = 1;
      break;
    }
  }
  proto_dispatch_leave();
  return ok;
}

usize ndp_dispatch_neigh_dump(struct neigh_info *out, usize max) {
  usize n = 0;
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->neigh_dump) {
      n = p->neigh_dump(out, max);
      break;
    }
  }
  proto_dispatch_leave();
  return n;
}

int ndp_dispatch_neigh_set(struct in6_addr_k ip, struct mac_addr mac,
                           int permanent) {
  /* No module to hold the entry means IPv6 neighbours genuinely cannot be
   * administered — the same answer the kernel gave before this hook existed. */
  int rc = -EAFNOSUPPORT;
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->neigh_set) {
      rc = p->neigh_set(ip, mac, permanent);
      break;
    }
  }
  proto_dispatch_leave();
  return rc;
}

int ndp_dispatch_neigh_del(struct in6_addr_k ip) {
  int rc = -EAFNOSUPPORT;
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->neigh_del) {
      rc = p->neigh_del(ip);
      break;
    }
  }
  proto_dispatch_leave();
  return rc;
}
